#include "laptimer.h"
#include "trackmanager.h"
#include "webhook.h"

#include "debug.h"

#ifdef ESP32S3
#include "rgbled.h"
extern RgbLed* g_rgbLed;
#endif

// Kalman filter tuning (standard convention):
// Q = process noise variance: how much RSSI can change per sample.
//     Higher Q → filter tracks fast changes more closely (less lag, more noise).
// R = measurement noise variance: how noisy the ADC reading is.
//     Higher R → filter trusts the sensor less (more smoothing, more lag).
const float kalman_Q = 3.0f;   // RSSI can move ~2 units/sample (variance = 5)
const float kalman_R = 16.0f;   // ADC noise rejection — raise to smooth, lower for faster response

// Race debug output (Serial) — throttled so it doesn't overwhelm.
// Set to 0 to compile out the periodic race debug print.
#ifndef LAPTIMER_RACE_DEBUG
#define LAPTIMER_RACE_DEBUG 1
#endif

static const uint32_t kRaceDebugPeriodMs = 100;  // 10 Hz

// Debounce: require consecutive samples at/above enter before peak tracking
// kEnterHoldSamplesMin: now read from config (conf->getEnterHoldSamples())

// Moving average window (must match rssi_window[] size in laptimer.h)
static const uint8_t kMaWindow = 7;

// EMA low-pass tuning:
// alpha closer to 0 = stronger smoothing (slower response)
// alpha closer to 1 = weaker smoothing (faster response)
static const float kEmaAlpha = 0.15f;

// NEW: reject one-sample "teleport" drops/rises with a step limiter.
// This is NOT a low-pass; it only clamps absurd per-sample jumps.
// Lower = stricter (less likely to false-trigger), Higher = more responsive.
static const int kMaxStepPerSample = 12;

// kExitConfirmSamples: now read from config (conf->getExitConfirmSamples())

// Gate re-arm latch: require this many consecutive samples of the THRESHOLD-SMOOTH
// signal below exitT before the next gate entry is allowed.  With the smooth signal
// doing most of the heavy lifting, a small count is sufficient.
static const uint8_t kGateCloseRequired = 10;

// Asymmetric IIR for the threshold-smooth signal:
//   Rise alpha — tracks increases quickly so gate entry is detected promptly.
//   Fall alpha — tracks decreases moderately so noise dips within a single pass
//                do not pull the smooth signal below exit threshold, but the
//                signal still recovers between consecutive passes.
//
// Fall alpha is constant (independent of Bessel level).  The threshold-smooth
// only needs to reject brief within-pass dips (typically < 50 samples).  Tau
// of ~200 samples (~200 ms at 1 kHz) rejects those comfortably while still
// recovering between fast laps (≥ 1 s).  Slower fall alphas were causing
// consecutive fast passes to merge into a single detection.
static const float kThreshRiseAlpha = 0.05f;    // fast rise (tau ≈  20 samples)
static const float kThreshFallAlpha = 0.005f;   // moderate fall (tau ≈ 200 samples)

// ── Bessel coefficient table for the post-stage slider ─────────────────────
// Levels 4 (100 Hz), 7 (50 Hz), 10 (20 Hz) are the original RotorHazard
// designs.  In-between levels are linearly interpolated in coefficient space —
// not strictly Bessel, but DC gain stays at 1.0 (verified) and smoothing
// strength is monotone.  Level 0 bypasses the filter entirely.
struct BesselCoef { float b0, a1, a2; };
static const BesselCoef kBesselTable[11] = {
    { 0.0f,         0.0f,        0.0f       }, // 0: off (bypass — never read)
    { 0.21010f,    -0.06030f,   0.21980f   }, // 1: extrapolated very light
    { 0.17020f,    -0.12060f,   0.43950f   }, // 2: extrapolated light
    { 0.13030f,    -0.18100f,   0.65930f   }, // 3: extrapolated light-moderate
    { 0.09053999669813994622f, -0.24114073878907091308f, 0.87898075199651115597f }, // 4: 100 Hz
    { 0.07013f,    -0.32673f,   1.04631f   }, // 5: interp 4→7 (1/3)
    { 0.04967f,    -0.41244f,   1.21364f   }, // 6: interp 4→7 (2/3)
    { 0.02921062558939069298f, -0.49774398476624526211f, 1.38090148240868249019f }, // 7: 50 Hz
    { 0.02134f,    -0.58446f,   1.49930f   }, // 8: interp 7→10 (1/3)
    { 0.01346f,    -0.67117f,   1.61770f   }, // 9: interp 7→10 (2/3)
    { 0.00559344020910809616f, -0.75788377219702429688f, 1.73551001136059190877f }  // 10: 20 Hz
};

void LapTimer::init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook) {
    conf = config;
    rx = rx5808;
    buz = buzzer;
    led = l;
    webhooks = webhook;

    filter.setProcessNoise(kalman_Q);
    filter.setMeasurementNoise(kalman_R);

    selectedTrack = nullptr;
    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;

    stop();
    lapCount = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));
    memset(rssi, 0, sizeof(rssi));
    memset(rssi_window, 0, sizeof(rssi_window));
    rssi_window_index = 0;

    // V1 filter state
    memset(v1KHist, 0, sizeof(v1KHist));
    v1KHistIdx  = 0;
    v1Ema       = NAN;
    v1OutInit   = false;
    v1OutPrev   = 0;

    // V2 filter state
    v2Bv[0] = v2Bv[1] = v2Bv[2] = 0.0f;
    v2PeakDurationMs = 0;

    // Debug/state init
    lastRawRssi = 0;
    lastKalmanRssi = 0;
    lastAvgRssi = 0;
    prevAvgRssi = 0;
    lastRaceDebugPrintMs = 0;
    enteredGate = false;
    gateExited = true;
    gateCloseCount = 0;
    enterHoldSamples = 0;

    _threshSmooth    = 128.0f;
    _threshSmoothOut = 128;
#if RSSI_STREAM_ENABLED
    _lastStreamMs    = 0;
    _streamCount     = 0;
    _streamCountMs   = 0;
#endif
}

void LapTimer::start() {
    DEBUG("\n=== RACE STARTED ===\n");
    DEBUG("Current Thresholds:\n");
    DEBUG("  Enter RSSI: %u\n", conf->getEnterRssi());
    DEBUG("  Exit RSSI: %u\n", conf->getExitRssi());
    DEBUG("  Min Lap Time: %u ms\n", conf->getMinLapMs());
    DEBUG("\nCurrent RSSI: %u\n", rssi[rssiCount]);
    DEBUG("====================\n\n");

    // Reset all filter state so a new race starts from a clean slate.
    filter = KalmanFilter();
    filter.setProcessNoise(kalman_Q);
    filter.setMeasurementNoise(kalman_R);

    memset(v1KHist, 0, sizeof(v1KHist));
    v1KHistIdx = 0;
    v1Ema      = NAN;
    v1OutInit  = false;
    v1OutPrev  = 0;

    // v2Bv is seeded below from the current rawRssi reading; do not zero here.
    v2PeakDurationMs = 0;

    lapCount = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));

    raceStartTimeMs = millis();
    startTimeMs = raceStartTimeMs;
    state = RUNNING;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;

    gateExited = true;   // Gate 1 may open immediately at race start
    gateCloseCount = 0;
    enteredGate = false;
    enterHoldSamples = 0;

    // Seed both the threshold-smooth filter and the Bessel post-stage IIR at
    // the ambient RSSI level so there is no artificial ramp-up at race start.
    // For the biquad form used in besselStep (numerator 1+2z⁻¹+z⁻², DC gain 1),
    // the steady-state value of bv equals input/4.  Seeding bv = seed/4 makes
    // the filter output `seed` from sample 0 instead of ramping up from 0.
    {
        uint8_t seed = rx->readRssi();
        _threshSmooth    = (float)seed;
        _threshSmoothOut = seed;
        const float bvSeed = (float)seed / 4.0f;
        v2Bv[0] = v2Bv[1] = v2Bv[2] = bvSeed;
    }
#if RSSI_STREAM_ENABLED
    _lastStreamMs  = 0;
    _streamCount   = 0;
    _streamCountMs = 0;
#endif
    prevAvgRssi = 0;
    lastRaceDebugPrintMs = 0;

    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;

    buz->beep(500);
    led->on(500);

#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashGreen();
#endif

    if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookRaceStart()) {
        webhooks->triggerRaceStart();
    }
}

bool LapTimer::isRunning() const {
    return state == RUNNING || state == WAITING;
}

uint32_t LapTimer::getElapsedMs() const {
    return isRunning() ? (millis() - raceStartTimeMs) : 0;
}

uint8_t LapTimer::getLapCount() const { return lapCount; }

uint32_t LapTimer::getLapTimeAt(uint8_t index) const {
    if (index >= LAPTIMER_LAP_HISTORY) return 0;
    return lapTimes[index];
}

void LapTimer::stop() {
    DEBUG("LapTimer stopped\n");
    state = STOPPED;
    rssiCount = 0;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    startTimeMs = 0;

    gateExited = true;
    gateCloseCount = 0;
    enteredGate = false;
    enterHoldSamples = 0;

    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;
    // lapCount and lapTimes are intentionally preserved so they survive page refresh
    // after the race ends. They are reset when the next race starts().
    buz->beep(500);
    led->on(500);

#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashReset();
#endif

    if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookRaceStop()) {
        webhooks->triggerRaceStop();
    }
}

// ---------------------------------------------------------------------------
// Bessel IIR low-pass filter helpers (ported from RotorHazard).
// Coefficients are for a 2nd-order Bessel filter at the given cutoff.
// DC gain = 1 (verified analytically).  Output is clamped to [0, 255].
// State is held in the caller's v2Bv[3] array.
// ---------------------------------------------------------------------------
static inline uint8_t besselStep(float bv[3], float b0, float a1, float a2, uint8_t x)
{
    bv[0] = bv[1];
    bv[1] = bv[2];
    bv[2] = b0 * (float)x + a1 * bv[0] + a2 * bv[1];
    float out = bv[0] + bv[2] + 2.0f * bv[1];
    if (out < 0.0f)   return 0;
    if (out > 255.0f) return 255;
    return (uint8_t)lroundf(out);
}

void LapTimer::handleLapTimerUpdate(uint32_t currentTimeMs) {
#if RSSI_LOGGING_ENABLED
    snapshot.lapEvent = false;
#endif

    // --- Stage 1: raw RSSI ---
    uint8_t rawRssi = rx->readRssi();

    uint8_t out;
    uint8_t kalman_filtered = 0;
    uint8_t ma = 0;

    // ===== Pre-filter stage (Filter Mode) =====
    uint8_t preFiltered;
    if (conf->getFilterMode() == 1) {
        // V2 — RotorHazard: raw passthrough (Bessel is now an independent post-stage)
        preFiltered = rawRssi;
        // No Kalman / MA stages in V2 — leave their telemetry slots at 0 so
        // logs and snapshots reflect the actual pipeline that ran.
        lastKalmanRssi = 0;
        lastAvgRssi    = 0;
    } else {
        // V1 — FPVRaceOne: 5-stage pipeline (Kalman → Median → MA → EMA → Step limiter)

        // Stage 2: Kalman
        kalman_filtered = (uint8_t)round(filter.filter(rawRssi, 0));

        // Stage 2.5: Median-of-3
        v1KHist[v1KHistIdx] = kalman_filtered;
        v1KHistIdx = (v1KHistIdx + 1) % 3;
        uint8_t a = v1KHist[0], b = v1KHist[1], c = v1KHist[2];
        uint8_t median_kal =
            (a > b) ? ((b > c) ? b : ((a > c) ? c : a))
                    : ((a > c) ? a : ((b > c) ? c : b));

        // Stage 3: Moving average
        rssi_window[rssi_window_index] = median_kal;
        rssi_window_index = (rssi_window_index + 1) % kMaWindow;
        uint16_t sum = 0;
        for (int i = 0; i < kMaWindow; i++) sum += rssi_window[i];
        ma = (uint8_t)(sum / kMaWindow);

        // Stage 4: EMA
        if (isnan(v1Ema)) {
            v1Ema = (float)ma;
        } else {
            v1Ema = (kEmaAlpha * (float)ma) + ((1.0f - kEmaAlpha) * v1Ema);
        }
        uint8_t lp = (uint8_t)lroundf(v1Ema);

        // Stage 5: Step limiter
        preFiltered = lp;
        if (v1OutInit) {
            int delta = (int)lp - (int)v1OutPrev;
            if      (delta >  kMaxStepPerSample) preFiltered = (uint8_t)(v1OutPrev + kMaxStepPerSample);
            else if (delta < -kMaxStepPerSample) preFiltered = (uint8_t)(v1OutPrev - kMaxStepPerSample);
        } else {
            v1OutInit = true;
        }
        v1OutPrev = preFiltered;

        lastKalmanRssi = kalman_filtered;
        lastAvgRssi    = ma;
    }
    lastRawRssi = rawRssi;

    // ===== Bessel post-stage (independent slider, applies to either mode) =====
    const uint8_t besselLevel = conf->getBesselLevel();
    if (besselLevel == 0) {
        out = preFiltered;
    } else {
        const uint8_t lvl = (besselLevel > 10) ? 10 : besselLevel;
        const BesselCoef& c = kBesselTable[lvl];
        out = besselStep(v2Bv, c.b0, c.a1, c.a2, preFiltered);
    }

    // Store final value used by lap logic
    rssi[rssiCount] = out;

#if RSSI_LOGGING_ENABLED
    snapshot.timeMs           = currentTimeMs;
    snapshot.raw              = rawRssi;
    snapshot.kalman           = lastKalmanRssi;
    snapshot.ma               = lastAvgRssi;
    snapshot.out              = out;
    snapshot.enterThresh      = conf->getEnterRssi();
    snapshot.exitThresh       = conf->getExitRssi();
    snapshot.peak             = rssiPeak;
    snapshot.timerState       = (uint8_t)state;
    snapshot.enteredGate      = enteredGate;
    snapshot.gateExited       = gateExited;
    snapshot.enterHoldSamples = enterHoldSamples;
    snapshot.filterMode       = conf->getFilterMode();
    // snapshot.lapEvent is set in finishLap(); lapTimeMs and lapCount updated there
#endif

#if LAPTIMER_RACE_DEBUG
    if (state == RUNNING) {
        const uint8_t cur = rssi[rssiCount];
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();

        if (prevAvgRssi < enter && cur >= enter) {
            DEBUG("[RACE] ENTER crossed: b=%u raw=%u s=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, rawRssi, _threshSmoothOut,
                  (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }
        if (prevAvgRssi >= exitT && cur < exitT) {
            DEBUG("[RACE] EXIT  crossed: b=%u s=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, _threshSmoothOut,
                  (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }

        const uint32_t now = millis();
        if (lastRaceDebugPrintMs == 0 || (now - lastRaceDebugPrintMs) >= kRaceDebugPeriodMs) {
            lastRaceDebugPrintMs = now;
            const bool validPeak = (rssiPeak > 0) && (rssiPeak >= enter) && (rssiPeak > (exitT + 5));

            // Show whether we've *confirmed* exit (2 samples) below
            uint16_t prevIdx = (rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY;
            const bool belowExit2 = (rssi[rssiCount] < exitT) && (rssi[prevIdx] < exitT);
            /*
            DEBUG("[RACE] raw=%3u kal=%3u ma=%3u out=%3u | enter=%3u exit=%3u | peak=%3u validPeak=%d belowExit2=%d entered=%d hold=%u\n",
                  rawRssi, lastKalmanRssi, lastAvgRssi, out,
                  enter, exitT, rssiPeak,
                  (int)validPeak, (int)belowExit2, (int)enteredGate, (unsigned)enterHoldSamples);
            */
        }

        prevAvgRssi = cur;
    }
#endif

    // ── Threshold-smooth filter (asymmetric IIR) ────────────────────────────
    // Rises quickly (tracks drone approach) but falls very slowly (noise dips
    // within a single pass don't pull the signal below the exit threshold).
    // Used ONLY for enter/exit state decisions — peak value and timestamp still
    // come from the responsive Bessel output (out).
    {
        const float fOut = (float)out;
        const float alpha = (fOut > _threshSmooth) ? kThreshRiseAlpha : kThreshFallAlpha;
        _threshSmooth    = alpha * fOut + (1.0f - alpha) * _threshSmooth;
        _threshSmoothOut = (uint8_t)lroundf(_threshSmooth);
    }

    // ── Gate re-arm latch (uses smooth signal) ──────────────────────────────
    if (!gateExited) {
        if (_threshSmoothOut < conf->getExitRssi()) {
            if (gateCloseCount < kGateCloseRequired) ++gateCloseCount;
            if (gateCloseCount >= kGateCloseRequired) gateExited = true;
        } else {
            gateCloseCount = 0;
        }
    }

#if RSSI_STREAM_ENABLED
    // ── USB RSSI stream (toggle via /api/rssistream) ────────────────────────
    if (_rssiStream) {
        ++_streamCount;
        if (_streamCountMs == 0) _streamCountMs = currentTimeMs;
        if (currentTimeMs - _lastStreamMs >= 100) {
            _lastStreamMs = currentTimeMs;
            uint32_t elapsed = currentTimeMs - _streamCountMs;
            float hz = elapsed > 0 ? (_streamCount * 1000.0f / elapsed) : 0.0f;
            Serial.printf("RS r=%3u b=%3u s=%3u ent=%d gex=%d gcc=%2u hz=%.0f\n",
                rawRssi, out, _threshSmoothOut,
                (int)enteredGate, (int)gateExited, (unsigned)gateCloseCount, hz);
        }
    }
#endif

    switch (state) {
        case STOPPED:
            break;

        case WAITING:
            lapPeakCapture();
            if (lapPeakCaptured()) {
                state = RUNNING;
                startLap();
            }
            break;

        case RUNNING: {
            bool isGate1 = (lapCount == 0 && !lapCountWraparound);
            bool minLapElapsed = (currentTimeMs - startTimeMs) > conf->getMinLapMs();

            if (isGate1 || minLapElapsed) {
                lapPeakCapture();
                if (lapPeakCaptured()) {
                    DEBUG("Lap triggered! Time: %u ms (Gate 1: %s)\n",
                          currentTimeMs - startTimeMs, isGate1 ? "YES" : "NO");
                    finishLap();
                    startLap();
                }
            }
            break;
        }

        case CALIBRATION_WIZARD:
            if (calibrationRssiCount < LAPTIMER_CALIBRATION_HISTORY &&
                (currentTimeMs - lastCalibrationSampleMs) >= 20) {
                // Record the pre-Bessel value so the wizard sees the natural
                // peak shape regardless of where the user has the Bessel slider.
                // Otherwise the FWHM-based recommendation is circular: a high
                // Bessel setting widens visible peaks, recommending more Bessel.
                calibrationRssi[calibrationRssiCount] = preFiltered;
                calibrationTimestamps[calibrationRssiCount] = currentTimeMs;
                calibrationRssiCount++;
                lastCalibrationSampleMs = currentTimeMs;
            }
            break;

        default:
            break;
    }

    rssiCount = (rssiCount + 1) % LAPTIMER_RSSI_HISTORY;
}

void LapTimer::lapPeakCapture() {
    const uint8_t cur = rssi[rssiCount];
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();
    const uint32_t now = millis();
    const bool v2 = (conf->getFilterMode() == 1);

    if (cur >= enter) {
        if (!enteredGate) {
            if (!gateExited) {
                // Gate hasn't re-armed yet: RSSI hasn't sustained below exit long
                // enough since the last lap.  Ignore this rise above enter.
                return;
            }
            enteredGate = true;
            enterHoldSamples = 1;
            gateExited = false;
        } else {
            if (enterHoldSamples < 255) enterHoldSamples++;
        }

        // V1 requires kEnterHoldSamplesMin consecutive samples before tracking peak.
        // V2 trusts the Bessel filter and starts tracking immediately (hold = 1).
        const uint8_t holdMin = v2 ? 1 : conf->getEnterHoldSamples();

        if (enterHoldSamples >= holdMin) {
            if (cur > rssiPeak) {
                rssiPeak = cur;
                rssiPeakTimeMs = now;
                v2PeakDurationMs = 0;
                DEBUG("*** PEAK CAPTURED: %u (raw=%u kal=%u ma=%u) at %lu ms ***\n",
                      rssiPeak, lastRawRssi, lastKalmanRssi, lastAvgRssi,
                      (unsigned long)(rssiPeakTimeMs - startTimeMs));
            } else if (v2 && cur == rssiPeak) {
                // V2: extend peak duration so we can use the midpoint as the lap timestamp.
                v2PeakDurationMs = now - rssiPeakTimeMs;
            }
        }
    } else {
        if (enteredGate && _threshSmoothOut < exitT && rssiPeak == 0) {
            // The smooth signal dropped below exit without capturing any valid
            // peak — the Bessel rise was a noise blip never confirmed by smooth.
            enteredGate = false;
            gateExited = true;
            enterHoldSamples = 0;
            v2PeakDurationMs = 0;
        } else {
            enterHoldSamples = 0;
        }
    }
}

bool LapTimer::lapPeakCaptured() {
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();
    const bool v2 = (conf->getFilterMode() == 1);

    bool validPeak = (rssiPeak > 0) &&
                     (rssiPeak >= enter) &&
                     (rssiPeak > (exitT + 5));

    // Use the heavily smoothed RSSI for the exit decision so that brief noise
    // dips within a single gate pass don't count as a true exit.  The smooth
    // signal only crosses below exitT once the drone has genuinely departed.
    bool droppedBelowExit = (_threshSmoothOut < exitT);

    bool captured = enteredGate && validPeak && droppedBelowExit;

    if (captured) {
        if (v2 && v2PeakDurationMs > 0) {
            // V2: shift lap timestamp to the midpoint of the peak plateau.
            rssiPeakTimeMs += v2PeakDurationMs / 2;
            DEBUG("\n*** V2 LAP DETECTED! Peak=%u duration=%ums midpoint+%ums ***\n",
                  rssiPeak, v2PeakDurationMs, v2PeakDurationMs / 2);
        } else {
            DEBUG("\n*** LAP DETECTED! Peak=%u enter=%u exit=%u margin=%d ***\n",
                  rssiPeak, enter, exitT, rssiPeak - exitT);
        }
    }

    if (!captured && enteredGate && droppedBelowExit && !validPeak) {
        enteredGate = false;
        gateExited = true;
        enterHoldSamples = 0;
        rssiPeak = 0;
        rssiPeakTimeMs = 0;
        v2PeakDurationMs = 0;
    }

    return captured;
}

void LapTimer::startLap() {
    DEBUG("Lap started - Peak was %u, new lap begins\n", rssiPeak);
    startTimeMs = rssiPeakTimeMs;
    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    v2PeakDurationMs = 0;

    enteredGate = false;
    enterHoldSamples = 0;

    // lapCount has already been incremented by finishLap() for real laps.
    // If lapCount == 0 this is the WAITING→RUNNING pre-start transition —
    // allow gate 1 to open immediately.  For all other laps, require the
    // RSSI to sustain below exit (kGateCloseRequired samples) before the
    // next gate can open, preventing re-triggering within the same broad pass.
    if (lapCount > 0) {
        gateExited = false;
        gateCloseCount = 0;
    } else {
        gateExited = true;
        gateCloseCount = 0;
    }

    buz->beep(200);
    led->on(200);
}

void LapTimer::finishLap() {
    lapTimes[lapCount] = rssiPeakTimeMs - startTimeMs;
    if (lapCount == 0 && lapCountWraparound == false) {
        lapTimes[0] = rssiPeakTimeMs - raceStartTimeMs;
    } else {
        lapTimes[lapCount] = rssiPeakTimeMs - startTimeMs;
    }
    lastLapPeakRssi = rssiPeak;
    DEBUG("Lap finished, lap time = %u\n", lapTimes[lapCount]);

#if RSSI_LOGGING_ENABLED
    snapshot.lapEvent  = true;
    snapshot.lapTimeMs = lapTimes[lapCount];
    snapshot.lapCount  = lapCount;
#endif

    if (selectedTrack && selectedTrack->distance > 0) {
        totalDistanceTravelled += selectedTrack->distance;

        uint8_t maxLaps = conf->getMaxLaps();
        if (maxLaps > 0) {
            int lapsCompleted = lapCount + 1;
            if (lapCountWraparound) {
                lapsCompleted = LAPTIMER_LAP_HISTORY + (lapCount + 1);
            }
            int lapsRemaining = maxLaps - lapsCompleted;
            distanceRemaining = (lapsRemaining > 0) ? (lapsRemaining * selectedTrack->distance) : 0.0f;
        } else {
            distanceRemaining = 0.0f;
        }

        DEBUG("Distance: Travelled = %.2f m, Remaining = %.2f m\n",
              totalDistanceTravelled, distanceRemaining);
    }

    if ((lapCount + 1) % LAPTIMER_LAP_HISTORY == 0) {
        lapCountWraparound = true;
    }
    lapCount = (lapCount + 1) % LAPTIMER_LAP_HISTORY;
    lapAvailable = true;

#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashLap();
#endif

    if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookLap()) {
        webhooks->triggerLap();
    }
}

uint8_t LapTimer::getRssi() {
    // rssiCount was incremented at the end of handleLapTimerUpdate(), so it now
    // points to the *next* slot to write. The most recently written value is one
    // slot behind.
    return rssi[(rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY];
}

uint32_t LapTimer::getLapTime() {
    uint32_t lapTime = 0;
    lapAvailable = false;
    if (lapCount == 0) {
        lapTime = lapTimes[LAPTIMER_LAP_HISTORY - 1];
    } else {
        lapTime = lapTimes[lapCount - 1];
    }
    return lapTime;
}

uint8_t LapTimer::getLastLapPeakRssi() const {
    return lastLapPeakRssi;
}

void LapTimer::clearLapData() {
    lapCount = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));
}

void LapTimer::recordManualLap(uint32_t lapTimeMs) {
    lapTimes[lapCount] = lapTimeMs;
    if ((lapCount + 1) % LAPTIMER_LAP_HISTORY == 0) lapCountWraparound = true;
    lapCount = (lapCount + 1) % LAPTIMER_LAP_HISTORY;
}

bool LapTimer::isLapAvailable() {
    return lapAvailable;
}

void LapTimer::startCalibrationWizard() {
    DEBUG("Calibration wizard started\n");
    state = CALIBRATION_WIZARD;
    calibrationRssiCount = 0;
    lastCalibrationSampleMs = 0;  // Reset sample timing
    memset(calibrationRssi, 0, sizeof(calibrationRssi));
    memset(calibrationTimestamps, 0, sizeof(calibrationTimestamps));
    buz->beep(300);
    led->on(300);
#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashGreen();
#endif
}

void LapTimer::stopCalibrationWizard() {
    DEBUG("Calibration wizard stopped, recorded %u samples\n", calibrationRssiCount);
    state = STOPPED;
    buz->beep(300);
    led->on(300);
#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashReset();
#endif
}

uint16_t LapTimer::getCalibrationRssiCount() {
    return calibrationRssiCount;
}

uint8_t LapTimer::getCalibrationRssi(uint16_t index) {
    if (index < calibrationRssiCount) {
        return calibrationRssi[index];
    }
    return 0;
}

uint32_t LapTimer::getCalibrationTimestamp(uint16_t index) {
    if (index < calibrationRssiCount) {
        return calibrationTimestamps[index];
    }
    return 0;
}

void LapTimer::setTrack(Track* track) {
    selectedTrack = track;
    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;
    if (track) {
        DEBUG("Track selected: %s (%.2f m)\n", track->name.c_str(), track->distance);
    } else {
        DEBUG("Track deselected\n");
    }
}

float LapTimer::getTotalDistance() {
    return totalDistanceTravelled;
}

float LapTimer::getDistanceRemaining() {
    return distanceRemaining;
}

Track* LapTimer::getSelectedTrack() {
    return selectedTrack;
}
