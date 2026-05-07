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
// Verbatim upstream FPVGate gains.  Note that upstream uses an *inverted*
// naming convention (their `Q` maps to setMeasurementNoise, their `R` maps
// to setProcessNoise) — we apply the gains the same way upstream does so
// the Kalman behaviour matches the original RX5808 path byte-for-byte.
static const float kKalmanMeasurementNoise = 9.0f;
static const float kKalmanProcessNoise     = 0.002f;

// Race debug output (Serial) — throttled so it doesn't overwhelm.
// Set to 0 to compile out the periodic race debug print.
#ifndef LAPTIMER_RACE_DEBUG
#define LAPTIMER_RACE_DEBUG 1
#endif

static const uint32_t kRaceDebugPeriodMs = 100;  // 10 Hz

// Moving average window (must match rssi_window[] size in laptimer.h)
static const uint8_t kMaWindow = 7;

// EMA low-pass tuning.
// alpha closer to 0 = stronger smoothing (slower response, more lag)
// alpha closer to 1 = weaker smoothing (faster response, more noise)
//
// Exposed to the user as a 0..10 slider (`v1Smoothing` in config).  Level 5
// is exactly the upstream FPVGate alpha (0.15) so the saved default produces
// behaviour identical to the original pre-slider firmware.  Lower numbers
// reduce lag at the cost of letting more noise through; higher numbers add
// extra smoothing on top of upstream's design.
static const float kEmaAlphaTable[11] = {
    0.50f,  // 0  — almost no EMA; minimum lag, most noise
    0.40f,  // 1
    0.30f,  // 2
    0.25f,  // 3
    0.20f,  // 4
    0.15f,  // 5  ── upstream default ──
    0.12f,  // 6
    0.10f,  // 7
    0.07f,  // 8
    0.05f,  // 9
    0.03f,  // 10 — heaviest smoothing; noticeable lag, minimum noise
};
static inline float getEmaAlpha(uint8_t level) {
    if (level > 10) level = 10;
    return kEmaAlphaTable[level];
}

// NEW: reject one-sample "teleport" drops/rises with a step limiter.
// This is NOT a low-pass; it only clamps absurd per-sample jumps.
// Lower = stricter (less likely to false-trigger), Higher = more responsive.
static const int kMaxStepPerSample = 12;

// ── Enter/exit debounce constants (from upstream FPVGate) ─────────────────
static const uint8_t kEnterHoldSamplesMin   = 4;   // consecutive at-or-above-enter samples before peak tracking starts
static const uint8_t kExitConfirmSamples    = 2;   // consecutive below-exit samples to confirm exit (raw, not smoothed)
static const uint8_t kPeakMinAboveExit      = 5;   // peak must exceed exit by at least this much to be valid
// Gate-1 relaxations (used only when gate1Bootstrap is enabled):
static const uint8_t kGate1RelaxMargin      = 4;   // effective Gate-1 enter ~= exit + 4 (if enter is much higher)
static const uint8_t kGate1HoldSamplesMin   = 2;   // lower debounce just for Gate 1
static const uint8_t kGate1PeakMinAboveExit = 3;   // lower peak margin just for Gate 1
// Ceiling-drift watchdog: if the gate has been "entered" longer than
// this without exit, force-reset state (signal must have drifted up to enter).
static const uint32_t kCeilingDriftTimeoutMs = 3000;

void LapTimer::init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook) {
    conf = config;
    rx = rx5808;
    buz = buzzer;
    led = l;
    webhooks = webhook;

    selectedTrack = nullptr;
    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;

    // Apply the upstream Kalman gains once at construction.  These never
    // change at runtime since there's only one filter mode now.
    filter.setMeasurementNoise(kKalmanMeasurementNoise);
    filter.setProcessNoise(kKalmanProcessNoise);

    stop();
    lapCount = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));
    memset(rssi, 0, sizeof(rssi));
    memset(rssi_window, 0, sizeof(rssi_window));
    rssi_window_index = 0;

    // Pre-filter pipeline state
    memset(v1KHist, 0, sizeof(v1KHist));
    v1KHistIdx  = 0;
    v1Ema       = NAN;
    v1OutInit   = false;
    v1OutPrev   = 0;

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

    // Verbatim upstream FPVGate behaviour: keep the Kalman filter running
    // across races.  Upstream's comment: "resetting it causes a transient
    // ramp that can false-trigger the first lap; the step limiter rate-
    // limits output changes anyway, so a reset provides no benefit."

    memset(v1KHist, 0, sizeof(v1KHist));
    v1KHistIdx = 0;
    v1Ema      = NAN;
    v1OutInit  = false;
    v1OutPrev  = 0;

    lapCount = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));

    raceStartTimeMs = millis();
    startTimeMs = raceStartTimeMs;
    state = RUNNING;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;

    gateExited = true;   // Gate 1 may open immediately at race start
    enteredGate = false;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;
    gate1Armed = false;  // Gate-1 bootstrap re-arms each race

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

void LapTimer::handleLapTimerUpdate(uint32_t currentTimeMs) {
#if RSSI_LOGGING_ENABLED
    snapshot.lapEvent = false;
#endif

    // --- Stage 1: raw RSSI ---
    const uint8_t rawRssi = rx->readRssi();

    // --- Stage 2: Kalman ---
    const uint8_t kalman_filtered = (uint8_t)round(filter.filter(rawRssi, 0));

    // --- Stage 2.5: Median-of-3 ---
    v1KHist[v1KHistIdx] = kalman_filtered;
    v1KHistIdx = (v1KHistIdx + 1) % 3;
    const uint8_t a = v1KHist[0], b = v1KHist[1], c = v1KHist[2];
    const uint8_t median_kal =
        (a > b) ? ((b > c) ? b : ((a > c) ? c : a))
                : ((a > c) ? a : ((b > c) ? c : b));

    // --- Stage 3: Moving average ---
    rssi_window[rssi_window_index] = median_kal;
    rssi_window_index = (rssi_window_index + 1) % kMaWindow;
    uint16_t sum = 0;
    for (int i = 0; i < kMaWindow; i++) sum += rssi_window[i];
    const uint8_t ma = (uint8_t)(sum / kMaWindow);

    // --- Stage 4: EMA — alpha is user-tunable via the v1Smoothing slider.
    // Level 5 (default) = 0.15 = upstream FPVGate behaviour.
    const float emaAlpha = getEmaAlpha(conf->getV1Smoothing());
    if (isnan(v1Ema)) {
        v1Ema = (float)ma;
    } else {
        v1Ema = (emaAlpha * (float)ma) + ((1.0f - emaAlpha) * v1Ema);
    }
    const uint8_t lp = (uint8_t)lroundf(v1Ema);

    // --- Stage 5: Step limiter ---
    uint8_t out = lp;
    if (v1OutInit) {
        const int delta = (int)lp - (int)v1OutPrev;
        if      (delta >  kMaxStepPerSample) out = (uint8_t)(v1OutPrev + kMaxStepPerSample);
        else if (delta < -kMaxStepPerSample) out = (uint8_t)(v1OutPrev - kMaxStepPerSample);
    } else {
        v1OutInit = true;
    }
    v1OutPrev = out;

    lastRawRssi    = rawRssi;
    lastKalmanRssi = kalman_filtered;
    lastAvgRssi    = ma;

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
    snapshot.filterMode       = 0;   // log-format compat (single-mode firmware)
    // snapshot.lapEvent is set in finishLap(); lapTimeMs and lapCount updated there
#endif

#if LAPTIMER_RACE_DEBUG
    if (state == RUNNING) {
        const uint8_t cur = rssi[rssiCount];
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();

        if (prevAvgRssi < enter && cur >= enter) {
            DEBUG("[RACE] ENTER crossed: out=%u raw=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, rawRssi, (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }
        if (prevAvgRssi >= exitT && cur < exitT) {
            DEBUG("[RACE] EXIT  crossed: out=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }

        const uint32_t now = millis();
        if (lastRaceDebugPrintMs == 0 || (now - lastRaceDebugPrintMs) >= kRaceDebugPeriodMs) {
            lastRaceDebugPrintMs = now;
        }

        prevAvgRssi = cur;
    }
#endif

#if RSSI_STREAM_ENABLED
    // ── USB RSSI stream (toggle via /api/rssistream) ────────────────────────
    if (_rssiStream) {
        ++_streamCount;
        if (_streamCountMs == 0) _streamCountMs = currentTimeMs;
        if (currentTimeMs - _lastStreamMs >= 100) {
            _lastStreamMs = currentTimeMs;
            const uint32_t elapsed = currentTimeMs - _streamCountMs;
            const float hz = elapsed > 0 ? (_streamCount * 1000.0f / elapsed) : 0.0f;
            Serial.printf("RS r=%3u out=%3u ent=%d gex=%d hz=%.0f\n",
                rawRssi, out,
                (int)enteredGate, (int)gateExited, hz);
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

            // gate1Bootstrap=on: run the upstream first-lap bootstrap.
            // Off: Gate 1 may fire immediately (no bootstrap, no minLap gate).
            const bool useGate1Bootstrap = conf->getGate1Bootstrap();
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
                canCapture = true;            // bootstrap off: Gate 1 may fire immediately
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
                calibrationRssi[calibrationRssiCount] = out;
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
//  Lap detection — verbatim upstream FPVGate state machine
// ===========================================================================
//   - 4-sample enter debounce (kEnterHoldSamplesMin), relaxed to 2 on Gate 1
//   - Peak must exceed exit by kPeakMinAboveExit (3 on Gate 1)
//   - 2-sample raw exit confirm (uses rssi[], not a smoothed copy)
//   - Ceiling-drift watchdog forces reset if "in gate" longer than 3 s
// ---------------------------------------------------------------------------

void LapTimer::lapPeakCapture() {
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
                DEBUG("*** PEAK CAPTURED: %u (raw=%u kal=%u ma=%u) at %lu ms ***\n",
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

bool LapTimer::lapPeakCaptured() {
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
        DEBUG("\n*** LAP DETECTED! ***\n");
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

    enteredGate = false;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;
    gateExited = true;

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
