#include "laptimer.h"
#include "webhook.h"

#include "debug.h"

#ifdef ESP32S3
#include "rgbled.h"
extern RgbLed* g_rgbLed;
#endif

// Race debug output (Serial) — throttled so it doesn't overwhelm.
// Set to 0 to compile out the periodic race debug print.
#ifndef LAPTIMER_RACE_DEBUG
#define LAPTIMER_RACE_DEBUG 1
#endif

static const uint32_t kRaceDebugPeriodMs = 100;  // 10 Hz

// ── Median filter window size, mapped from the v1Smoothing slider ────────
//
// The slider is 0..10 in the UI; each level maps to an odd running-median
// window size.  Sizes chosen for a shared-Core sample rate of ~200-500 Hz
// so the effective window stays under 50 ms even at the low end — a peak
// of a fast racing pass (50-100 ms) is preserved without noticeable
// attenuation.
//
// Design intent matches RotorHazard's FastRunningMedian (single stage,
// spike-rejecting, peak-preserving) — the numeric window differs because
// their 1 kHz sample rate on a dedicated ATmega gave them headroom to run
// N=255.
//
// Level 5 (default) → N=7 → ~14-35 ms window at 500-200 Hz respectively.
// Lower = more peak fidelity + more noise; higher = more smoothing + more lag.
static const uint8_t kMedianWindowTable[11] = {
    3,   // 0  — bare metal, near-raw, most peak fidelity
    3,   // 1
    5,   // 2  — high-speed racing (100+ mph), clean RF
    5,   // 3
    7,   // 4
    7,   // 5  ── default ── race speeds, mixed RF
    9,   // 6
    11,  // 7  — slower race pace / noisier RF
    13,  // 8
    13,  // 9
    15,  // 10 — cruise / freestyle / very noisy environment
};
static inline uint8_t medianNFromSlider(uint8_t level) {
    if (level > 10) level = 10;
    return kMedianWindowTable[level];
}

// ── Gate-1 relaxations (used only when gate1Bootstrap is enabled) ────────
static const uint8_t kGate1RelaxMargin = 4;   // effective Gate-1 enter ~= exit + 4 (if enter is much higher)

// ── Detection safety knobs (compile-time) ────────────────────────────────
//
// Both defaults ON — safe for every drone we've seen in the field.  Flip
// either to false ONLY for bench characterisation of pipeline behaviour;
// production builds should keep both enabled.
//
// kEnableEnterDebounce=true: require kEnterHoldMin consecutive at-or-
//   above-enter samples before the crossing starts.  Backs up the median
//   filter's spike rejection so a 4-sample noise burst that lifts the
//   median to enter can't start a false crossing.  Detection latency
//   added: (kEnterHoldMin - 1) samples ≈ 3-7 ms depending on loop rate.
//   Disable ONLY if characterising pipeline behaviour on the bench — a
//   drone would need to be flying faster than any current racing drone
//   for this to become the failure mode.
//
// kEnableCeilingWatchdog=true: if we've been "in gate" longer than
//   kCeilingDriftTimeoutMs without an exit, force-reset the crossing
//   state so a slow RSSI drift up to the enter threshold can't lock
//   detection.
static constexpr bool     kEnableEnterDebounce   = true;
static constexpr uint8_t  kEnterHoldMin          = 2;
static constexpr bool     kEnableCeilingWatchdog = true;
static constexpr uint32_t kCeilingDriftTimeoutMs = 3000;

void LapTimer::init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook) {
    conf = config;
    rx = rx5808;
    buz = buzzer;
    led = l;
    webhooks = webhook;

    stop();
    lapCount = 0;
    lapTotal = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));
    memset(rssi, 0, sizeof(rssi));

    // Median filter starts empty; window is set from the config slider
    // on the first sample in handleLapTimerUpdate.
    medianFilter.reset();
    medianFilter.setWindow(medianNFromSlider(conf ? conf->getV1Smoothing() : 5));

#if TIMING_MARKER_ENABLED && defined(PIN_TIMING_MARKER)
    // Bench timing marker — idle low so the emulator sees a clean rising edge
    // on the first confirmed lap.
    pinMode(PIN_TIMING_MARKER, OUTPUT);
    digitalWrite(PIN_TIMING_MARKER, LOW);
    _markerHigh = false;
#endif

    // Debug/state init
    lastRawRssi          = 0;
    lastFilteredRssi     = 0;
    prevFilteredRssi     = 0;
    lastRaceDebugPrintMs = 0;
    enteredGate          = false;
    gateExited           = true;
    gate1Armed           = false;
    enterHoldSamples     = 0;
    enterHoldStartMs     = 0;

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

    // Fresh median every race (Fix #6) — clears any samples accumulated
    // during the idle/countdown window so the first race sample doesn't
    // mix with stale pre-race noise.  Combined with the isFilled() gate
    // in the RUNNING branch, this gives a clean ~N-sample warmup before
    // detection begins.
    medianFilter.reset();
    medianFilter.setWindow(medianNFromSlider(conf ? conf->getV1Smoothing() : 5));

    lapCount = 0;
    lapTotal = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));

    raceStartTimeMs = millis();
    startTimeMs = raceStartTimeMs;
    state = RUNNING;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;

    gateExited       = true;   // Gate 1 may open immediately at race start
    enteredGate      = false;
    gate1Armed       = false;  // Gate-1 bootstrap re-arms each race
    enterHoldSamples = 0;
    enterHoldStartMs = 0;

#if RSSI_STREAM_ENABLED
    _lastStreamMs  = 0;
    _streamCount   = 0;
    _streamCountMs = 0;
#endif
    prevFilteredRssi     = 0;
    lastRaceDebugPrintMs = 0;

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

uint16_t LapTimer::getLapTotal() const { return lapTotal; }

uint16_t LapTimer::getRetainedLapCount() const {
    return (lapTotal < LAPTIMER_LAP_HISTORY) ? lapTotal : LAPTIMER_LAP_HISTORY;
}

// Oldest-first walk of the ring.
//
// lapCount is the NEXT slot to write, so the most recent lap sits one slot
// behind it and the oldest retained lap sits `retained` slots behind that.
// Once the ring has wrapped, a lap's position no longer equals its lap number,
// which is exactly what broke the reload restore: the browser numbers restored
// laps by position, so it renumbered every surviving lap.  Reporting the true
// number here lets the caller keep them straight.
bool LapTimer::getRetainedLap(uint16_t index, uint16_t* lapNumber, uint32_t* lapTimeMs) const {
    const uint16_t retained = getRetainedLapCount();
    if (index >= retained) return false;

    const uint16_t slot = (uint16_t)((lapCount + LAPTIMER_LAP_HISTORY - retained + index)
                                     % LAPTIMER_LAP_HISTORY);
    if (lapTimeMs) *lapTimeMs = lapTimes[slot];
    // Lap numbering is 0-based and lap 0 is the initial gate crossing (shown as
    // "-" in the UI), so the oldest retained lap is numbered lapTotal-retained.
    if (lapNumber) *lapNumber = (uint16_t)(lapTotal - retained + index);
    return true;
}

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

    gateExited       = true;
    enteredGate      = false;
    gate1Armed       = false;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;

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

    // While the RX5808 is mid-tune its RSSI line is invalid (readRssi() returns 0).
    // Pushing those zeros through the Kalman/MA/EMA filters drags the whole pipeline
    // toward 0 and can suppress a real peak for many samples right after a channel
    // change. Skip the tick entirely instead. handleFrequencyChange() on the main
    // loop clears this flag ~RX5808_MIN_TUNETIME+100 ms after the write, so this can
    // never wedge detection.
    if (rx->recentSetFreqFlag) {
        return;
    }

    // ── Single-stage pipeline (RotorHazard-style) ──────────────────────────
    // 1. Raw RSSI from the RX5808 ADC (12-bit → 8-bit in RX5808::readRssi).
    // 2. Running median with a small odd window — replaces the previous
    //    Kalman + Median-3 + MA(7) + EMA + step-limiter cascade.  The
    //    median rejects single-sample spikes while preserving the true peak
    //    amplitude of a fast pass, which the old cascade was smearing to
    //    the point that high-speed gate crossings sometimes missed enter.
    //
    // Refresh window size every sample — cheap, and lets the user tune the
    // filter mid-race via the settings slider without a restart.
    medianFilter.setWindow(medianNFromSlider(conf->getV1Smoothing()));
    const uint8_t rawRssi = rx->readRssi();
    const uint8_t out     = medianFilter.addAndGet(rawRssi);

    lastRawRssi      = rawRssi;
    lastFilteredRssi = out;

    // Store final value used by lap logic
    rssi[rssiCount] = out;

#if TIMING_MARKER_ENABLED && defined(PIN_TIMING_MARKER)
    // Retire a marker pulse raised on the previous tick.  Doing it here rather
    // than with a delayMicroseconds() at the raise site keeps all blocking out
    // of the detection path; the resulting pulse is one sample period wide.
    if (_markerHigh) {
        digitalWrite(PIN_TIMING_MARKER, LOW);
        _markerHigh = false;
    }
#endif

#if TIMING_STATS_ENABLED
    // ── Continuous sample-interval statistics ────────────────────────────
    //
    // This replaced a one-shot mean-rate log that only ran for the first 2 s
    // of uptime.  A mean cannot answer the question that actually matters —
    // "are we ever LATE" — because a run that averages 400 Hz can still
    // contain a single 300 ms gap, and it is the gap that loses a lap.
    //
    // WORST GAP IS THE KEY NUMBER HERE.  It bounds detection jitter and
    // bounds the risk of a narrow fast-pass peak landing entirely between
    // two samples.  Mean rate is reported alongside only for context.
    //
    // micros() rather than millis(): the interval being measured is 2-5 ms,
    // which millis() cannot resolve to any useful precision.
    {
        const uint32_t nowUs = micros();
        if (_ts.lastSampleUs != 0) {
            // Unsigned subtraction is wrap-safe (micros() wraps ~71 min).
            const uint32_t dtUs = nowUs - _ts.lastSampleUs;
            if (dtUs > _ts.maxIntervalUs) _ts.maxIntervalUs = dtUs;
            if (dtUs < _ts.minIntervalUs) _ts.minIntervalUs = dtUs;
            _ts.sumIntervalUs += dtUs;
            _ts.sampleCount++;
            if (dtUs > (TIMING_STATS_LATE_THRESHOLD_MS * 1000UL)) _ts.lateCount++;
        }
        _ts.lastSampleUs = nowUs;

        if (_ts.windowStartMs == 0) _ts.windowStartMs = currentTimeMs;
        if (currentTimeMs - _ts.windowStartMs >= TIMING_STATS_WINDOW_MS &&
            _ts.sampleCount > 0) {
            const uint32_t meanUs = (uint32_t)(_ts.sumIntervalUs / _ts.sampleCount);
            const uint32_t hz     = meanUs ? (1000000UL / meanUs) : 0;
            DEBUG("[TIMING] %lu Hz | interval min/mean/max = %lu/%lu/%lu us | late(>%ums)=%lu of %lu | median N=%u (~%lu ms)\n",
                  (unsigned long)hz,
                  (unsigned long)_ts.minIntervalUs,
                  (unsigned long)meanUs,
                  (unsigned long)_ts.maxIntervalUs,
                  (unsigned)TIMING_STATS_LATE_THRESHOLD_MS,
                  (unsigned long)_ts.lateCount,
                  (unsigned long)_ts.sampleCount,
                  (unsigned)medianFilter.window(),
                  (unsigned long)((medianFilter.window() * (uint32_t)meanUs) / 1000UL));

            // Publish the completed window for the Diagnostics page, then
            // reset the accumulators.  Readers always see a whole window,
            // never a partially-filled one.
            _tsPublished = _ts;
            _tsPublished.meanIntervalUs = meanUs;
            _tsPublished.valid          = true;

            _ts.minIntervalUs  = 0xFFFFFFFFUL;
            _ts.maxIntervalUs  = 0;
            _ts.sumIntervalUs  = 0;
            _ts.sampleCount    = 0;
            _ts.lateCount      = 0;
            _ts.windowStartMs  = currentTimeMs;
        }
    }
#endif

#if RSSI_LOGGING_ENABLED
    // The RSSI CSV log format retains "kalman" and "ma" columns for tooling
    // compat, but the corresponding pipeline stages are gone.  Duplicate
    // `out` into both slots so historical parsers still line up.
    snapshot.timeMs      = currentTimeMs;
    snapshot.raw         = rawRssi;
    snapshot.kalman      = out;
    snapshot.ma          = out;
    snapshot.out         = out;
    snapshot.enterThresh = conf->getEnterRssi();
    snapshot.exitThresh  = conf->getExitRssi();
    snapshot.peak        = rssiPeak;
    snapshot.timerState  = (uint8_t)state;
    snapshot.enteredGate = enteredGate;
    snapshot.gateExited  = gateExited;
    snapshot.enterHoldSamples = enterHoldSamples;   // debounce is back (Fix #1)
    snapshot.filterMode  = 0;
    // snapshot.lapEvent is set in finishLap(); lapTimeMs and lapCount updated there
#endif

#if LAPTIMER_RACE_DEBUG
    if (state == RUNNING) {
        const uint8_t cur = rssi[rssiCount];
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();

        if (prevFilteredRssi < enter && cur >= enter) {
            DEBUG("[RACE] ENTER crossed: out=%u raw=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, rawRssi, (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }
        if (prevFilteredRssi >= exitT && cur < exitT) {
            DEBUG("[RACE] EXIT  crossed: out=%u ent=%d gex=%d peak=%u t=%lu ms\n",
                  cur, (int)enteredGate, (int)gateExited, rssiPeak,
                  (unsigned long)(millis() - startTimeMs));
        }

        const uint32_t now = millis();
        if (lastRaceDebugPrintMs == 0 || (now - lastRaceDebugPrintMs) >= kRaceDebugPeriodMs) {
            lastRaceDebugPrintMs = now;
        }

        prevFilteredRssi = cur;
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
            // Fix #5: skip detection until the median filter has enough
            // samples to give a representative output.  Prevents cold-
            // start bias (a partially-filled median is skewed upward)
            // from firing spurious enter crossings.
            if (!medianFilter.isFilled()) break;
            lapPeakCapture();
            if (lapPeakCaptured()) {
                state = RUNNING;
                startLap();
            }
            break;

        case RUNNING: {
            // Fix #5: same warm-up gate as above.  The median filter is
            // reset in start() (Fix #6), so we need ~N samples of new
            // race data before we trust it for detection.
            if (!medianFilter.isFilled()) break;
            const bool isGate1 = (lapCount == 0 && !lapCountWraparound);
            // Fix #4: use >= so the exact-boundary tick counts as elapsed
            // (previously off by one sample).
            const bool minLapElapsed = (currentTimeMs - startTimeMs) >= conf->getMinLapMs();
            bool canCapture = false;

            // gate1Bootstrap=on: run the first-lap bootstrap.  If the drone
            // is already inside the gate at race start, seed the peak so
            // the first exit fires Gate 1 rather than being ignored.
            // Off: Gate 1 may fire immediately (no bootstrap, no minLap gate).
            const bool useGate1Bootstrap = conf->getGate1Bootstrap();
            if (useGate1Bootstrap && isGate1) {
                if (!gate1Armed) {
                    const uint8_t cur   = rssi[rssiCount];
                    const uint8_t enter = conf->getEnterRssi();
                    const uint32_t now  = millis();

                    gate1Armed = true;

                    if (cur >= enter) {
                        // In-gate at start — seed the peak AND the debounce
                        // state (as if debounce had already been satisfied)
                        // so the first filtered drop below exit fires Gate 1.
                        enteredGate      = true;
                        gateExited       = false;
                        rssiPeak         = cur;
                        rssiPeakTimeMs   = now;
                        enterHoldSamples = kEnterHoldMin;
                        enterHoldStartMs = now;
                        DEBUG("[Gate1] Armed in-gate bootstrap (cur=%u enter=%u)\n", cur, enter);
                    } else {
                        // Out-of-gate at start — normal enter → peak → exit.
                        enteredGate      = false;
                        gateExited       = true;
                        rssiPeak         = 0;
                        rssiPeakTimeMs   = 0;
                        enterHoldSamples = 0;
                        enterHoldStartMs = 0;
                        DEBUG("[Gate1] Armed out-of-gate bootstrap (cur=%u enter=%u)\n", cur, enter);
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
#if TIMING_MARKER_ENABLED && defined(PIN_TIMING_MARKER)
                    // FIRST statement in this block, deliberately.  Everything
                    // below — the DEBUG here, and more inside finishLap() and
                    // startLap() — writes to USB CDC and can block for
                    // milliseconds if the host is not draining the port.  Any
                    // of that between the decision and the marker would be
                    // measured as detection latency that does not exist.
                    //
                    // digitalWrite (~1-2 us) rather than a raw register write:
                    // this file builds for C3/C6/S3/ESP32 whose GPIO register
                    // structs differ, and 2 us is 500x finer than the
                    // millisecond effects being measured.
                    //
                    // Cleared on the next sample tick, giving a pulse one
                    // sample period wide (~2-5 ms) — far more than the
                    // emulator's edge ISR needs, and no blocking delay here.
                    digitalWrite(PIN_TIMING_MARKER, HIGH);
                    _markerHigh = true;
#endif
                    DEBUG("Lap triggered! Time: %u ms (Gate 1: %s)\n",
                          currentTimeMs - startTimeMs, isGate1 ? "YES" : "NO");
                    finishLap();
                    startLap();
                }
            }
            break;
        }

        case CALIBRATION_WIZARD:
            // Spinlock: the buffers are heap-allocated now and the web handler
            // that frees them runs at a HIGHER priority than this task, so it
            // can preempt between the bounds check and the store.  See the note
            // on calibrationMux in laptimer.h.  Capacity is per-session and may
            // be below LAPTIMER_CALIBRATION_HISTORY if the heap was tight.
            portENTER_CRITICAL(&calibrationMux);
            if (calibrationRssi && calibrationTimestamps &&
                calibrationRssiCount < calibrationCapacity &&
                (currentTimeMs - lastCalibrationSampleMs) >= 20) {
                calibrationRssi[calibrationRssiCount] = out;
                calibrationTimestamps[calibrationRssiCount] = currentTimeMs;
                calibrationRssiCount++;
                lastCalibrationSampleMs = currentTimeMs;
            }
            portEXIT_CRITICAL(&calibrationMux);
            break;

        default:
            break;
    }

    rssiCount = (rssiCount + 1) % LAPTIMER_RSSI_HISTORY;
}

// ===========================================================================
//  Lap detection — RotorHazard-style state machine
// ===========================================================================
//   - Enter when filtered RSSI ≥ enterAt
//   - Track running max of filtered RSSI while inside the crossing
//   - Exit when filtered RSSI < exitAt (single sample — the median filter
//     is what rejects spikes; a second-sample confirm on top of a median
//     is redundant)
//   - Fire lap if peak reached enterAt during the crossing
//   - No enter-hold debounce, no peak-above-exit margin, no ceiling-drift
//     watchdog: the median filter's inherent spike rejection replaces all
//     of that machinery.
// ---------------------------------------------------------------------------

void LapTimer::lapPeakCapture() {
    const uint8_t cur = rssi[rssiCount];
    const uint32_t now = millis();
    const bool isGate1 = (lapCount == 0 && !lapCountWraparound && gate1Armed);

    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();
    uint8_t effectiveEnter = enter;
    // Gate 1 only: relax effective enter slightly so the first crossing
    // isn't lost when enter is set high relative to exit.
    const uint8_t gate1RelaxedEnter =
        (exitT >= (255 - kGate1RelaxMargin)) ? 255 : (uint8_t)(exitT + kGate1RelaxMargin);
    if (isGate1 && gate1RelaxedEnter < enter) {
        effectiveEnter = gate1RelaxedEnter;
    }

    if (cur >= effectiveEnter) {
        // Count consecutive at-or-above-enter samples for debounce.
        if (enterHoldSamples < 255) enterHoldSamples++;
        if (enterHoldStartMs == 0) enterHoldStartMs = now;

        // Enter debounce: require kEnterHoldMin consecutive samples above
        // enter before starting the crossing.  Compile-time constant —
        // flip kEnableEnterDebounce to false in this file for bench
        // characterisation.
        const uint8_t needed = kEnableEnterDebounce ? kEnterHoldMin : 1;

        if (enterHoldSamples >= needed) {
            if (!enteredGate) {
                enteredGate = true;
                gateExited  = false;
            }
            if (cur > rssiPeak) {
                rssiPeak       = cur;
                rssiPeakTimeMs = now;
                DEBUG("*** PEAK CAPTURED: %u (raw=%u) at %lu ms ***\n",
                      rssiPeak, lastRawRssi,
                      (unsigned long)(rssiPeakTimeMs - startTimeMs));
            }
        }

        // Ceiling-drift watchdog (Fix #3): if we've been "in gate" too long
        // without an exit, the antenna baseline must have drifted up to
        // enter.  Force-reset so detection can recover.  Flip
        // kEnableCeilingWatchdog to false at compile time to observe the
        // stuck-state behaviour directly.
        if (kEnableCeilingWatchdog && enteredGate &&
            (now - enterHoldStartMs) > kCeilingDriftTimeoutMs) {
            DEBUG("[LAP] Ceiling-drift timeout (>%lu ms in gate) — resetting state\n",
                  (unsigned long)kCeilingDriftTimeoutMs);
            enteredGate      = false;
            gateExited       = true;
            rssiPeak         = 0;
            rssiPeakTimeMs   = 0;
            enterHoldSamples = 0;
            enterHoldStartMs = 0;
        }
    } else {
        // Below enter: reset the debounce counter so a future above-enter
        // burst starts a fresh count.  Do NOT clear enteredGate here — if
        // the crossing already started (debounce satisfied earlier in this
        // pass), we're descending from the peak and lapPeakCaptured() will
        // fire the lap when we drop below exit.
        enterHoldSamples = 0;
        if (!enteredGate) enterHoldStartMs = 0;
    }
}

bool LapTimer::lapPeakCaptured() {
    const uint8_t cur   = rssi[rssiCount];
    const uint8_t exitT = conf->getExitRssi();

    // Not currently inside a crossing — nothing to fire.
    if (!enteredGate) return false;

    // Filtered value has dropped below exit — end of crossing.
    if (cur >= exitT) return false;

    // Peak was captured during the crossing (any sample that reached enter
    // has been recorded).  No margin check: if we entered, the median-
    // filtered signal already reached enterAt, which is by definition a
    // valid pass.
    if (rssiPeak == 0) {
        // Should not happen if enteredGate == true, but guard anyway —
        // reset state and skip.
        enteredGate = false;
        gateExited  = true;
        return false;
    }

    DEBUG("\n*** LAP DETECTED! ***\n");
    DEBUG("  Current RSSI: %u\n", cur);
    DEBUG("  Peak was: %u\n", rssiPeak);
    DEBUG("  Enter: %u  Exit: %u\n", conf->getEnterRssi(), exitT);
    DEBUG("******************\n\n");
    return true;
}

void LapTimer::startLap() {
    DEBUG("Lap started - Peak was %u, new lap begins\n", rssiPeak);
    startTimeMs = rssiPeakTimeMs;
    rssiPeak = 0;
    rssiPeakTimeMs = 0;

    enteredGate      = false;
    gateExited       = true;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;

    // No-op on XIAO C6: PIN_BUZZER is undefined there, so Buzzer::beep()
    // returns immediately.  The timing marker uses its own pin and does not
    // interact with this.
    buz->beep(200);
    led->on(200);
}

void LapTimer::finishLap() {
    // Gate 1 measures from the race start; all other laps from the previous lap's
    // peak time. Compute signed and clamp at 0: in the Gate-1 in-gate bootstrap path
    // rssiPeakTimeMs can be seeded at/just before the start reference, and an unsigned
    // subtraction there would underflow into a ~49-day bogus lap time.
    const uint32_t startRef = (lapCount == 0 && lapCountWraparound == false)
                              ? raceStartTimeMs : startTimeMs;
    const int32_t lapDelta = (int32_t)(rssiPeakTimeMs - startRef);
    lapTimes[lapCount] = (lapDelta > 0) ? (uint32_t)lapDelta : 0;
    lastLapPeakRssi = rssiPeak;
    DEBUG("Lap finished, lap time = %u\n", lapTimes[lapCount]);

#if RSSI_LOGGING_ENABLED
    snapshot.lapEvent  = true;
    snapshot.lapTimeMs = lapTimes[lapCount];
    snapshot.lapCount  = lapCount;
#endif

    if ((lapCount + 1) % LAPTIMER_LAP_HISTORY == 0) {
        lapCountWraparound = true;
    }
    lapCount = (lapCount + 1) % LAPTIMER_LAP_HISTORY;
    if (lapTotal < 0xFFFF) lapTotal++;   // saturate rather than wrap
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
    lapTotal = 0;
    lapCountWraparound = false;
    memset(lapTimes, 0, sizeof(lapTimes));
}

void LapTimer::recordManualLap(uint32_t lapTimeMs) {
    lapTimes[lapCount] = lapTimeMs;
    if ((lapCount + 1) % LAPTIMER_LAP_HISTORY == 0) lapCountWraparound = true;
    lapCount = (lapCount + 1) % LAPTIMER_LAP_HISTORY;
    if (lapTotal < 0xFFFF) lapTotal++;
}

bool LapTimer::isLapAvailable() {
    return lapAvailable;
}

// Recording capacity tiers, largest first.  At the 20 ms sample gate these are
// 100 / 60 / 30 / 15 / 7.5 seconds of recording.
//
// The wizard asks for what the heap can actually give rather than demanding one
// fixed size and failing.  A user who came to calibrate is far better served by
// 60 seconds of recording than by an error message, and the difference is
// invisible to them unless we say so — which /calibration/start does, by
// returning the granted capacity for the UI to display.
static const uint16_t kCalibrationTiers[] = { 5000, 3000, 1500, 750, 375 };

uint16_t LapTimer::startCalibrationWizard() {
    // Idempotent: a second start without a stop would leak the first pair.
    _freeCalibrationBuffers();

    for (uint8_t t = 0; t < sizeof(kCalibrationTiers) / sizeof(kCalibrationTiers[0]); t++) {
        const uint16_t n = kCalibrationTiers[t];

        // Timestamps first — it is 4x the size of the RSSI array and therefore
        // the allocation that actually fails on a fragmented heap.  Trying the
        // small one first would succeed, then fail, then have to be undone.
        uint32_t *ts = (uint32_t *)calloc(n, sizeof(uint32_t));
        if (!ts) continue;
        uint8_t *rssi = (uint8_t *)calloc(n, sizeof(uint8_t));
        if (!rssi) { free(ts); continue; }

        calibrationTimestamps = ts;
        calibrationRssi       = rssi;
        calibrationCapacity   = n;
        break;
    }

    if (!calibrationCapacity) {
        // Every tier refused. The caller turns this into a 503 rather than
        // entering a state whose sample path would write through a null pointer.
        DEBUG("Calibration wizard: allocation failed at every tier (free=%u maxBlk=%u)\n",
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        return 0;
    }

    DEBUG("Calibration wizard started: %u samples (%u s) using %u bytes, free=%u\n",
          (unsigned)calibrationCapacity, (unsigned)(calibrationCapacity / 50),
          (unsigned)(calibrationCapacity * 5), (unsigned)ESP.getFreeHeap());

    state = CALIBRATION_WIZARD;
    calibrationRssiCount = 0;
    lastCalibrationSampleMs = 0;  // Reset sample timing
    // calloc already zeroed both buffers.
    buz->beep(300);
    led->on(300);
#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashGreen();
#endif
    return calibrationCapacity;
}

void LapTimer::stopCalibrationWizard() {
    DEBUG("Calibration wizard stopped, recorded %u samples\n", calibrationRssiCount);
    // Leave state first, so the sampling path in loop() cannot enter the
    // CALIBRATION_WIZARD case between here and the free() below.
    state = STOPPED;
    _freeCalibrationBuffers();
    buz->beep(300);
    led->on(300);
#ifdef ESP32S3
    if (g_rgbLed) g_rgbLed->flashReset();
#endif
}

// Returns the 25 KB to the allocator.  Deliberately clears the count and
// capacity too: the accessors gate on calibrationRssiCount, and leaving a
// non-zero count beside freed pointers would be a use-after-free waiting for
// the next /calibration/data poll to arrive late.
void LapTimer::_freeCalibrationBuffers() {
    // Detach under the spinlock, release outside it.  free() must never be
    // called with interrupts disabled — it can take the allocator's own lock —
    // so the critical section covers only the pointer swap, which is all that
    // is needed to make the sampler's guarded write safe.
    portENTER_CRITICAL(&calibrationMux);
    uint8_t  *rssi = calibrationRssi;
    uint32_t *ts   = calibrationTimestamps;
    calibrationRssi       = nullptr;
    calibrationTimestamps = nullptr;
    calibrationCapacity   = 0;
    calibrationRssiCount  = 0;
    portEXIT_CRITICAL(&calibrationMux);

    if (rssi) free(rssi);
    if (ts)   free(ts);
}

uint16_t LapTimer::getCalibrationRssiCount() {
    return calibrationRssiCount;
}

// The buffers are freed when the wizard stops, so both accessors must tolerate
// nullptr — a /calibration/data poll can land after a /calibration/stop.
uint8_t LapTimer::getCalibrationRssi(uint16_t index) {
    if (calibrationRssi && index < calibrationRssiCount) {
        return calibrationRssi[index];
    }
    return 0;
}

uint32_t LapTimer::getCalibrationTimestamp(uint16_t index) {
    if (calibrationTimestamps && index < calibrationRssiCount) {
        return calibrationTimestamps[index];
    }
    return 0;
}
