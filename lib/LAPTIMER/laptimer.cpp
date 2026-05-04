#include "laptimer.h"
#include "trackmanager.h"
#include "webhook.h"

#include "debug.h"

#ifdef ESP32S3
#include "rgbled.h"
extern RgbLed* g_rgbLed;
#endif

// ── Kalman filter tuning ──────────────────────────────────────────────────
//
// Two distinct gain pairs:
//   _Default   — used by the FPVRaceOne (Path B) pre-filter.  Standard
//                Kalman naming convention: Q is process noise, R is
//                measurement noise.  Higher R = more smoothing.
//   _Upstream  — used by the verbatim upstream FPVGate path.  Upstream uses
//                an *inverted* naming convention (Q maps to
//                setMeasurementNoise, R maps to setProcessNoise).  We apply
//                the gains the same way upstream does, so this path produces
//                the exact same Kalman behaviour as the upstream RX5808 path.
static const float kKalmanQ_Default  = 3.0f;
static const float kKalmanR_Default  = 16.0f;
static const float kKalmanQ_Upstream = 9.0f;
static const float kKalmanR_Upstream = 0.002f;

static void applyKalmanGains(KalmanFilter& f, uint8_t filterMode) {
    if (filterMode == 0) {
        // V1 (verbatim upstream FPVGate) — upstream's inverted call ordering
        f.setMeasurementNoise(kKalmanQ_Upstream);
        f.setProcessNoise(kKalmanR_Upstream);
    } else {
        // V2 (FPVRaceOne) / V3 (RotorHazard, Kalman irrelevant — raw passthrough)
        f.setProcessNoise(kKalmanQ_Default);
        f.setMeasurementNoise(kKalmanR_Default);
    }
}

// Race debug output (Serial) — throttled so it doesn't overwhelm.
// Set to 0 to compile out the periodic race debug print.
#ifndef LAPTIMER_RACE_DEBUG
#define LAPTIMER_RACE_DEBUG 1
#endif

static const uint32_t kRaceDebugPeriodMs = 100;  // 10 Hz

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

// ── V1 / V2 enter/exit debounce constants (from upstream FPVGate) ──────────
// V2 (FPVRaceOne / Path B) uses kEnterHoldSamplesMin + kExitConfirmSamples
// for its debounce.  V1 (verbatim upstream) uses the full upstream set
// including the Gate-1 relaxations.
static const uint8_t kEnterHoldSamplesMin   = 4;   // consecutive at-or-above-enter samples before peak tracking starts
static const uint8_t kExitConfirmSamples    = 2;   // consecutive below-exit samples to confirm exit (raw, not smoothed)
static const uint8_t kPeakMinAboveExit      = 5;   // peak must exceed exit by at least this much to be valid
// V1-only Gate-1 relaxations (used only when gate1Bootstrap is enabled):
static const uint8_t kGate1RelaxMargin      = 4;   // effective Gate-1 enter ~= exit + 4 (if enter is much higher)
static const uint8_t kGate1HoldSamplesMin   = 2;   // lower debounce just for Gate 1
static const uint8_t kGate1PeakMinAboveExit = 3;   // lower peak margin just for Gate 1
// V1-only ceiling-drift watchdog: if the gate has been "entered" longer than
// this without exit, force-reset state (signal must have drifted up to enter).
static const uint32_t kCeilingDriftTimeoutMs = 3000;

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

    // Apply Kalman gains based on the *currently configured* filterMode so that
    // a device booting into V1 immediately uses upstream's heavy smoothing.
    lastFilterMode = conf ? conf->getFilterMode() : 0;
    applyKalmanGains(filter, lastFilterMode);

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

    // FPVRaceOne (V2) pipeline state
    memset(v1KHist, 0, sizeof(v1KHist));
    v1KHistIdx  = 0;
    v1Ema       = NAN;
    v1OutInit   = false;
    v1OutPrev   = 0;

    // Bessel post-stage IIR state (used by V2 + V3) and RotorHazard plateau timer
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
    enterHoldSamples = 0;
    enterHoldStartMs = 0;
    gate1Armed = false;
    gateCloseCount = 0;

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

    // Filter state reset depends on mode:
    //   V1 (upstream FPVGate) — keep Kalman running (verbatim upstream:
    //                          "resetting it causes a transient ramp that can
    //                          false-trigger the first lap").
    //   V2 / V3               — reset everything, seed Bessel/_threshSmooth
    //                           from current sample.
    const uint8_t fm = conf ? conf->getFilterMode() : 0;
    lastFilterMode = fm;

    if (fm != 0) {
        filter = KalmanFilter();
    }
    applyKalmanGains(filter, fm);

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
    enterHoldStartMs = 0;
    gate1Armed = false;  // V1 Gate-1 bootstrap re-arms each race

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
    enterHoldStartMs = 0;
    gate1Armed = false;

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

    // Detect runtime filter-mode changes and re-apply Kalman gains accordingly.
    // V1 uses upstream's heavier Kalman gains; switching V1 ↔ V2 mid-session
    // must re-tune the filter or it'll behave wrong until the next race start.
    const uint8_t filterMode = conf->getFilterMode();
    if (filterMode != lastFilterMode) {
        applyKalmanGains(filter, filterMode);
        lastFilterMode = filterMode;
    }

    // --- Stage 1: raw RSSI ---
    uint8_t rawRssi = rx->readRssi();

    uint8_t out;
    uint8_t kalman_filtered = 0;
    uint8_t ma = 0;

    // ===== Pre-filter stage (Filter Mode) =====
    // V1 (upstream FPVGate) and V2 (FPVRaceOne) share the same
    // Kalman+Median+MA+EMA+step-limiter pipeline — only the Kalman tuning
    // differs (handled by applyKalmanGains).  V3 (RotorHazard) is a raw
    // passthrough.
    uint8_t preFiltered;
    if (filterMode == 2) {
        // V3 — RotorHazard: raw passthrough (Bessel is the independent post-stage)
        preFiltered = rawRssi;
        lastKalmanRssi = 0;
        lastAvgRssi    = 0;
    } else {
        // V1 / V2 — 5-stage pipeline (Kalman → Median → MA → EMA → step limiter)

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

    // ===== Bessel post-stage (V2 + V3 only — V1 is verbatim upstream, no Bessel) =====
    const uint8_t besselLevel = (filterMode == 0) ? 0 : conf->getBesselLevel();
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
    snapshot.enterHoldSamples = enterHoldSamples;   // V1 + V2: live debounce counter
    snapshot.filterMode       = filterMode;
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

    // ── Gate re-arm latch (V3 only — uses _threshSmooth signal) ─────────────
    // V1 (upstream) and V2 (FPVRaceOne) re-arm immediately in startLap().
    // V3 (RotorHazard) keeps the threshold-smooth-driven latch because its
    // raw passthrough signal can dip below exit briefly during a single pass.
    if (filterMode == 2 && !gateExited) {
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
            const bool isGate1 = (lapCount == 0 && !lapCountWraparound);
            const bool minLapElapsed = (currentTimeMs - startTimeMs) > conf->getMinLapMs();
            bool canCapture = false;

            // V1 + gate1Bootstrap=on: run the upstream first-lap bootstrap.
            // Other modes (and V1 with bootstrap off) just gate on minLapElapsed.
            const bool useGate1Bootstrap = (filterMode == 0) && conf->getGate1Bootstrap();
            if (useGate1Bootstrap && isGate1) {
                if (!gate1Armed) {
                    const uint8_t cur = rssi[rssiCount];
                    const uint8_t enter = conf->getEnterRssi();
                    const uint32_t now = millis();

                    gate1Armed = true;

                    // Bootstrap based on where we are at race start:
                    //   - If already in gate (>= enter), treat the first confirmed
                    //     exit as Gate 1 and seed the peak so validPeak passes.
                    //   - Otherwise, fall through to normal enter→peak→exit.
                    if (cur >= enter) {
                        const uint8_t exitT = conf->getExitRssi();
                        uint8_t seedPeak = cur;
                        const uint8_t minValidPeak = (exitT >= (255 - kGate1PeakMinAboveExit))
                                                     ? 255 : (uint8_t)(exitT + kGate1PeakMinAboveExit);
                        if (seedPeak < minValidPeak) seedPeak = minValidPeak;
                        enteredGate = true;
                        gateExited = false;
                        enterHoldSamples = kEnterHoldSamplesMin;
                        enterHoldStartMs = now;
                        rssiPeak = seedPeak;
                        rssiPeakTimeMs = now;
                        DEBUG("[Gate1] Armed in-gate bootstrap (cur=%u enter=%u seedPeak=%u)\n",
                              cur, enter, seedPeak);
                    } else {
                        enteredGate = false;
                        gateExited = true;
                        enterHoldSamples = 0;
                        enterHoldStartMs = 0;
                        rssiPeak = 0;
                        rssiPeakTimeMs = 0;
                        DEBUG("[Gate1] Armed out-of-gate bootstrap (cur=%u enter=%u)\n",
                              cur, enter);
                    }
                }
                canCapture = gate1Armed;
            } else if (isGate1) {
                canCapture = true;            // V2/V3 (and V1-no-bootstrap): Gate 1 may fire immediately
            } else {
                canCapture = minLapElapsed;
            }

            if (canCapture) {
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

// ===========================================================================
//  Detection state machines — one per filter mode
// ===========================================================================
//
// The public lapPeakCapture / lapPeakCaptured are thin dispatchers; each
// filter mode owns its own internal pair so logic for one mode can change
// without risk of regressing another.
//
// User-facing → behaviour mapping:
//   V1 (filterMode == 0, default): _FpvGate    — verbatim upstream FPVGate
//                                  (RX5808 pipeline + Gate-1 bootstrap +
//                                   ceiling-drift watchdog).  Smoothest peaks.
//   V2 (filterMode == 1):          _FpvRaceOne — Path B simplified pipeline
//                                  (4-sample enter / 2-sample raw exit confirm,
//                                   no _threshSmooth, no Gate-1 bootstrap).
//   V3 (filterMode == 2):          _RotorHazard — raw + Bessel + _threshSmooth
//                                  (uses the smoothed signal for the exit
//                                   decision and supports peak-plateau midpoint).
// ---------------------------------------------------------------------------

void LapTimer::lapPeakCapture() {
    switch (conf->getFilterMode()) {
        case 1:  lapPeakCapture_FpvRaceOne();  break;  // V2
        case 2:  lapPeakCapture_RotorHazard(); break;  // V3
        default: lapPeakCapture_FpvGate();     break;  // V1 (default)
    }
}

bool LapTimer::lapPeakCaptured() {
    switch (conf->getFilterMode()) {
        case 1:  return lapPeakCaptured_FpvRaceOne();   // V2
        case 2:  return lapPeakCaptured_RotorHazard();  // V3
        default: return lapPeakCaptured_FpvGate();      // V1 (default)
    }
}

// ── FPVRaceOne (Path B) — user-facing V2 ────────────────────────────────────
void LapTimer::lapPeakCapture_FpvRaceOne() {
    const uint8_t cur   = rssi[rssiCount];
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();
    const uint32_t now  = millis();

    if (cur >= enter) {
        if (!enteredGate) {
            enteredGate = true;
            enterHoldSamples = 1;
            gateExited = false;
        } else if (enterHoldSamples < 255) {
            enterHoldSamples++;
        }

        // 4-sample debounce — same as upstream FPVGate.  Keeps brief spikes
        // out of peak-tracking even if they crossed enter momentarily.
        if (enterHoldSamples >= kEnterHoldSamplesMin) {
            if (cur > rssiPeak) {
                rssiPeak = cur;
                rssiPeakTimeMs = now;
                DEBUG("*** PEAK CAPTURED (FPVRaceOne): %u (raw=%u kal=%u ma=%u) at %lu ms ***\n",
                      rssiPeak, lastRawRssi, lastKalmanRssi, lastAvgRssi,
                      (unsigned long)(rssiPeakTimeMs - startTimeMs));
            }
        }
    } else {
        if (enteredGate && cur < exitT && rssiPeak == 0) {
            // Noise blip — rose above enter but dropped below exit without
            // ever reaching a real peak.  Reset before it confuses captured().
            enteredGate = false;
            gateExited = true;
            enterHoldSamples = 0;
        } else {
            enterHoldSamples = 0;
        }
    }
}

bool LapTimer::lapPeakCaptured_FpvRaceOne() {
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();

    const bool validPeak =
        (rssiPeak > 0) && (rssiPeak >= enter) && (rssiPeak > (exitT + kPeakMinAboveExit));

    // Two-sample raw exit confirm — direct comparison against `rssi[]`,
    // not _threshSmoothOut.  Slow-falling smooth signal was the FPVRaceOne regression.
    const uint16_t prevIdx = (rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY;
    const bool droppedBelowExit =
        (rssi[rssiCount] < exitT) && (rssi[prevIdx] < exitT);

    const bool captured = enteredGate && validPeak && droppedBelowExit;

    if (captured) {
        DEBUG("\n*** LAP DETECTED (FPVRaceOne)! Peak=%u enter=%u exit=%u margin=%d ***\n",
              rssiPeak, enter, exitT, rssiPeak - exitT);
    }

    if (!captured && enteredGate && droppedBelowExit && !validPeak) {
        // Entered then exited without a real peak — bogus crossing, reset.
        enteredGate = false;
        gateExited = true;
        enterHoldSamples = 0;
        rssiPeak = 0;
        rssiPeakTimeMs = 0;
    }

    return captured;
}

// ── RotorHazard — user-facing V3 ────────────────────────────────────────────
void LapTimer::lapPeakCapture_RotorHazard() {
    const uint8_t cur   = rssi[rssiCount];
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();
    const uint32_t now  = millis();

    if (cur >= enter) {
        if (!enteredGate) {
            if (!gateExited) {
                // Gate hasn't re-armed yet (kGateCloseRequired latch).
                return;
            }
            enteredGate = true;
            gateExited = false;
        }

        if (cur > rssiPeak) {
            rssiPeak = cur;
            rssiPeakTimeMs = now;
            v2PeakDurationMs = 0;
            DEBUG("*** PEAK CAPTURED (RotorHazard): %u (raw=%u) at %lu ms ***\n",
                  rssiPeak, lastRawRssi,
                  (unsigned long)(rssiPeakTimeMs - startTimeMs));
        } else if (cur == rssiPeak) {
            // Plateau — extend duration so we can use the midpoint.
            v2PeakDurationMs = now - rssiPeakTimeMs;
        }
    } else if (enteredGate && _threshSmoothOut < exitT && rssiPeak == 0) {
        enteredGate = false;
        gateExited = true;
        v2PeakDurationMs = 0;
    }
}

bool LapTimer::lapPeakCaptured_RotorHazard() {
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();

    const bool validPeak =
        (rssiPeak > 0) && (rssiPeak >= enter) && (rssiPeak > (exitT + kPeakMinAboveExit));

    // V3 trusts the threshold-smooth IIR for exit detection so a single
    // brief dip mid-pass doesn't end the lap prematurely.
    const bool droppedBelowExit = (_threshSmoothOut < exitT);

    const bool captured = enteredGate && validPeak && droppedBelowExit;

    if (captured) {
        if (v2PeakDurationMs > 0) {
            rssiPeakTimeMs += v2PeakDurationMs / 2;
            DEBUG("\n*** LAP DETECTED (RotorHazard)! Peak=%u duration=%ums midpoint+%ums ***\n",
                  rssiPeak, v2PeakDurationMs, v2PeakDurationMs / 2);
        } else {
            DEBUG("\n*** LAP DETECTED (RotorHazard)! Peak=%u enter=%u exit=%u margin=%d ***\n",
                  rssiPeak, enter, exitT, rssiPeak - exitT);
        }
    }

    if (!captured && enteredGate && droppedBelowExit && !validPeak) {
        enteredGate = false;
        gateExited = true;
        rssiPeak = 0;
        rssiPeakTimeMs = 0;
        v2PeakDurationMs = 0;
    }

    return captured;
}

// ── FPVGate (verbatim upstream RX5808 path) — user-facing V1 (default) ──────
void LapTimer::lapPeakCapture_FpvGate() {
    const uint8_t cur = rssi[rssiCount];
    const uint32_t now = millis();
    const bool isGate1 = (lapCount == 0 && !lapCountWraparound && gate1Armed);

    bool entryCondition;
    bool noiseBlipExit;
    uint8_t holdSamplesRequired;

    {
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();
        uint8_t effectiveEnter = enter;
        // Gate 1 only: relax effective enter slightly so the first crossing
        // is not lost when enter is set very high relative to exit.
        const uint8_t gate1RelaxedEnter =
            (exitT >= (255 - kGate1RelaxMargin)) ? 255 : (uint8_t)(exitT + kGate1RelaxMargin);
        if (isGate1 && gate1RelaxedEnter < enter) {
            effectiveEnter = gate1RelaxedEnter;
            holdSamplesRequired = kGate1HoldSamplesMin;
        } else {
            holdSamplesRequired = kEnterHoldSamplesMin;
        }
        entryCondition = (cur >= effectiveEnter);
        noiseBlipExit  = (cur < exitT && rssiPeak == 0);
    }

    if (entryCondition) {
        if (!enteredGate) {
            enteredGate = true;
            enterHoldSamples = 1;
            enterHoldStartMs = now;
            gateExited = false;
        } else {
            if (enterHoldSamples < 255) enterHoldSamples++;

            // Ceiling-drift watchdog: if we've been "in the gate" for too long
            // without an exit, the antenna RSSI must have drifted up to enter.
            // Force-reset rather than emit a phantom lap.
            if ((now - enterHoldStartMs) > kCeilingDriftTimeoutMs) {
                DEBUG("[FPVGate] Ceiling-drift timeout (>%lu ms in gate) — resetting state\n",
                      (unsigned long)kCeilingDriftTimeoutMs);
                enteredGate = false;
                gateExited = true;
                enterHoldSamples = 0;
                enterHoldStartMs = 0;
                rssiPeak = 0;
                rssiPeakTimeMs = 0;
                return;
            }
        }

        if (enterHoldSamples >= holdSamplesRequired) {
            if (cur > rssiPeak) {
                rssiPeak = cur;
                rssiPeakTimeMs = now;
                DEBUG("*** PEAK CAPTURED (FPVGate): %u (raw=%u kal=%u ma=%u) at %lu ms ***\n",
                      rssiPeak, lastRawRssi, lastKalmanRssi, lastAvgRssi,
                      (unsigned long)(rssiPeakTimeMs - startTimeMs));
            }
        }
    } else {
        if (enteredGate && noiseBlipExit) {
            enteredGate = false;
            gateExited = true;
            enterHoldSamples = 0;
            enterHoldStartMs = 0;
        } else {
            enterHoldSamples = 0;
        }
    }
}

bool LapTimer::lapPeakCaptured_FpvGate() {
    const bool isGate1 = (lapCount == 0 && !lapCountWraparound && gate1Armed);
    bool validPeak;
    bool droppedBelowExit;

    {
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();
        uint8_t peakThreshold = enter;
        const uint8_t gate1RelaxedPeak =
            (exitT >= (255 - kGate1RelaxMargin)) ? 255 : (uint8_t)(exitT + kGate1RelaxMargin);
        if (isGate1 && gate1RelaxedPeak < enter) {
            peakThreshold = gate1RelaxedPeak;
        }
        const uint8_t minPeakMargin    = isGate1 ? kGate1PeakMinAboveExit : kPeakMinAboveExit;
        const uint8_t minPeakAboveExit =
            (exitT >= (255 - minPeakMargin)) ? 255 : (uint8_t)(exitT + minPeakMargin);

        validPeak =
            (rssiPeak > 0) && (rssiPeak >= peakThreshold) && (rssiPeak > minPeakAboveExit);

        // 2-sample raw exit confirm — same as upstream.
        const uint16_t prevIdx = (rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY;
        droppedBelowExit = (rssi[rssiCount] < exitT) && (rssi[prevIdx] < exitT);
    }

    const bool captured = enteredGate && validPeak && droppedBelowExit;

    if (captured) {
        DEBUG("\n*** LAP DETECTED (FPVGate)! ***\n");
        DEBUG("  Current RSSI: %u\n", rssi[rssiCount]);
        DEBUG("  Peak was: %u\n", rssiPeak);
        DEBUG("  Enter: %u  Exit: %u\n", conf->getEnterRssi(), conf->getExitRssi());
        DEBUG("******************\n\n");
    }

    if (!captured && enteredGate && droppedBelowExit && !validPeak) {
        enteredGate = false;
        gateExited = true;
        enterHoldSamples = 0;
        enterHoldStartMs = 0;
        rssiPeak = 0;
        rssiPeakTimeMs = 0;
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
    enterHoldStartMs = 0;

    // V3 (RotorHazard) re-arms via the kGateCloseRequired sample-count latch
    // driven from _threshSmoothOut in handleLapTimerUpdate.  V1 (upstream
    // FPVGate) and V2 (FPVRaceOne) re-arm immediately and trust the per-mode
    // debounce (4-sample enter hold + 2-sample raw exit confirm).
    const uint8_t fm = conf ? conf->getFilterMode() : 0;
    if (fm == 2 && lapCount > 0) {
        // V3 mid-race re-arm uses the latch.
        gateExited = false;
        gateCloseCount = 0;
    } else {
        // V1, V2, or pre-Gate-1 (lapCount == 0): re-arm immediately.
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
