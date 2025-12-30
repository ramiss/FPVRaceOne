#include "laptimer.h"
#include "trackmanager.h"
#include "webhook.h"

#include "debug.h"

#ifdef ESP32S3
#include "rgbled.h"
extern RgbLed* g_rgbLed;
#endif

// Kalman filtering tuning
// Higher Q = trust measurements less (more smoothing)
// Lower R = assume system changes slower (more smoothing)
const uint16_t rssi_filter_q = 900;
const uint16_t rssi_filter_r = 20;

// Race debug output (Serial) — throttled so it doesn't overwhelm.
// Set to 0 to compile out the periodic race debug print.
#ifndef LAPTIMER_RACE_DEBUG
#define LAPTIMER_RACE_DEBUG 1
#endif

static const uint32_t kRaceDebugPeriodMs = 100;  // 10 Hz

// Debounce: require consecutive samples at/above enter before peak tracking
static const uint8_t kEnterHoldSamplesMin = 4;

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

// NEW: require N consecutive samples below exit to confirm "exit"
static const uint8_t kExitConfirmSamples = 2;

void LapTimer::init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook) {
    conf = config;
    rx = rx5808;
    buz = buzzer;
    led = l;
    webhooks = webhook;

    filter.setMeasurementNoise(rssi_filter_q * 0.01f);
    filter.setProcessNoise(rssi_filter_r * 0.0001f);

    selectedTrack = nullptr;
    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;

    stop();
    memset(rssi, 0, sizeof(rssi));
    memset(rssi_window, 0, sizeof(rssi_window));
    rssi_window_index = 0;

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
}

void LapTimer::start() {
    DEBUG("\n=== RACE STARTED ===\n");
    DEBUG("Current Thresholds:\n");
    DEBUG("  Enter RSSI: %u\n", conf->getEnterRssi());
    DEBUG("  Exit RSSI: %u\n", conf->getExitRssi());
    DEBUG("  Min Lap Time: %u ms\n", conf->getMinLapMs());
    DEBUG("\nCurrent RSSI: %u\n", rssi[rssiCount]);
    DEBUG("====================\n\n");

    // Reset filter state at race start so we don't carry stale estimates.
    filter = KalmanFilter();
    filter.setMeasurementNoise(rssi_filter_q * 0.01f);
    filter.setProcessNoise(rssi_filter_r * 0.0001f);

    raceStartTimeMs = millis();
    startTimeMs = raceStartTimeMs;
    state = RUNNING;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;

    gateExited = true;
    enteredGate = false;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;
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

void LapTimer::stop() {
    DEBUG("LapTimer stopped\n");
    state = STOPPED;
    lapCountWraparound = false;
    lapCount = 0;
    rssiCount = 0;

    rssiPeak = 0;
    rssiPeakTimeMs = 0;
    startTimeMs = 0;

    gateExited = true;
    enteredGate = false;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;

    totalDistanceTravelled = 0.0f;
    distanceRemaining = 0.0f;

    memset(lapTimes, 0, sizeof(lapTimes));
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
    // --- Stage 1: raw RSSI ---
    uint8_t rawRssi = rx->readRssi();

    // --- Stage 2: Kalman filter ---
    uint8_t kalman_filtered = (uint8_t)round(filter.filter(rawRssi, 0));

    // --- Stage 2.5: Median-of-3 on Kalman (kills single-sample glitches) ---
    static uint8_t kHist[3] = {0, 0, 0};
    static uint8_t kHistIdx = 0;
    kHist[kHistIdx] = kalman_filtered;
    kHistIdx = (kHistIdx + 1) % 3;

    uint8_t a = kHist[0], b = kHist[1], c = kHist[2];
    uint8_t median_kal =
        (a > b) ? ((b > c) ? b : ((a > c) ? c : a))
                : ((a > c) ? a : ((b > c) ? c : b));

    // --- Stage 3: Moving average (kills short spikes) ---
    rssi_window[rssi_window_index] = median_kal;
    rssi_window_index = (rssi_window_index + 1) % kMaWindow;

    uint16_t sum = 0;
    for (int i = 0; i < kMaWindow; i++) sum += rssi_window[i];
    uint8_t ma = (uint8_t)(sum / kMaWindow);

    // --- Stage 4: EMA low-pass ---
    static float ema = NAN;
    if (isnan(ema)) {
        ema = (float)ma;
    } else {
        ema = (kEmaAlpha * (float)ma) + ((1.0f - kEmaAlpha) * ema);
    }
    uint8_t lp = (uint8_t)lroundf(ema);

    // --- Stage 5: Step limiter (prevents one-sample cliff drops that cause false exits) ---
    static bool outInit = false;
    static uint8_t outPrev = 0;
    uint8_t out = lp;

    if (outInit) {
        int delta = (int)lp - (int)outPrev;
        if (delta > kMaxStepPerSample) out = (uint8_t)(outPrev + kMaxStepPerSample);
        else if (delta < -kMaxStepPerSample) out = (uint8_t)(outPrev - kMaxStepPerSample);
    } else {
        outInit = true;
    }
    outPrev = out;

    // Store final value used by lap logic
    rssi[rssiCount] = out;

    // Debug/state tracking (raw/kalman/ma)
    lastRawRssi = rawRssi;
    lastKalmanRssi = kalman_filtered;
    lastAvgRssi = ma;

#if LAPTIMER_RACE_DEBUG
    if (state == RUNNING) {
        const uint8_t cur = rssi[rssiCount];
        const uint8_t enter = conf->getEnterRssi();
        const uint8_t exitT = conf->getExitRssi();

        if (prevAvgRssi < enter && cur >= enter) {
            DEBUG("[RACE] ENTER crossed: cur=%u raw=%u kal=%u med=%u ma=%u lp=%u out=%u t=%lu(ms since lap start)\n",
                  cur, rawRssi, kalman_filtered, median_kal, ma, lp, out,
                  (unsigned long)(millis() - startTimeMs));
        }
        if (prevAvgRssi >= exitT && cur < exitT) {
            DEBUG("[RACE] EXIT crossed: cur=%u peak=%u t=%lu(ms since lap start)\n",
                  cur, rssiPeak, (unsigned long)(millis() - startTimeMs));
        }

        const uint32_t now = millis();
        if (lastRaceDebugPrintMs == 0 || (now - lastRaceDebugPrintMs) >= kRaceDebugPeriodMs) {
            lastRaceDebugPrintMs = now;
            const bool validPeak = (rssiPeak > 0) && (rssiPeak >= enter) && (rssiPeak > (exitT + 5));

            // Show whether we've *confirmed* exit (2 samples) below
            uint16_t prevIdx = (rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY;
            const bool belowExit2 = (rssi[rssiCount] < exitT) && (rssi[prevIdx] < exitT);

            DEBUG("[RACE] raw=%3u kal=%3u med=%3u ma7=%3u lp=%3u out=%3u | enter=%3u exit=%3u | peak=%3u validPeak=%d belowExit2=%d entered=%d hold=%u\n",
                  rawRssi, kalman_filtered, median_kal, ma, lp, out,
                  enter, exitT, rssiPeak,
                  (int)validPeak, (int)belowExit2, (int)enteredGate, (unsigned)enterHoldSamples);
        }

        prevAvgRssi = cur;
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
                calibrationRssi[calibrationRssiCount] = rssi[rssiCount];
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

    // Debounce: require consecutive samples at/above enter before peak tracking
    if (cur >= enter) {
        if (!enteredGate) {
            enteredGate = true;
            enterHoldSamples = 1;
            enterHoldStartMs = now;
            gateExited = false;
        } else {
            if (enterHoldSamples < 255) enterHoldSamples++;
        }

        if (enterHoldSamples >= kEnterHoldSamplesMin) {
            if (cur > rssiPeak) {
                rssiPeak = cur;
                rssiPeakTimeMs = now;
                DEBUG("*** PEAK CAPTURED: %u (raw=%u kal=%u ma=%u) at %lu ms (since lap start: %lu ms) ***\n",
                      rssiPeak, lastRawRssi, lastKalmanRssi, lastAvgRssi,
                      (unsigned long)rssiPeakTimeMs, (unsigned long)(rssiPeakTimeMs - startTimeMs));
            }
        }
    } else {
        if (enteredGate && cur < exitT && rssiPeak == 0) {
            // noise blip: dipped below exit without any peak
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
    const uint8_t enter = conf->getEnterRssi();
    const uint8_t exitT = conf->getExitRssi();

    bool validPeak = (rssiPeak > 0) &&
                     (rssiPeak >= enter) &&
                     (rssiPeak > (exitT + 5));

    // NEW: confirm "below exit" for 2 consecutive samples (rejects one-sample cliff drops)
    bool droppedBelowExit = false;
    if (kExitConfirmSamples <= 1) {
        droppedBelowExit = (rssi[rssiCount] < exitT);
    } else {
        uint16_t prevIdx = (rssiCount + LAPTIMER_RSSI_HISTORY - 1) % LAPTIMER_RSSI_HISTORY;
        droppedBelowExit = (rssi[rssiCount] < exitT) && (rssi[prevIdx] < exitT);
    }

    bool captured = enteredGate && validPeak && droppedBelowExit;

    if (captured) {
        DEBUG("\n*** LAP DETECTED! ***\n");
        DEBUG("  Current RSSI: %u\n", rssi[rssiCount]);
        DEBUG("  Peak was: %u\n", rssiPeak);
        DEBUG("  Enter threshold: %u\n", enter);
        DEBUG("  Exit threshold: %u\n", exitT);
        DEBUG("  Peak margin above exit: %d\n", rssiPeak - exitT);
        DEBUG("******************\n\n");
    }

    if (!captured && enteredGate && droppedBelowExit && !validPeak) {
        // enteredGate without a real peak, then fell below exit → reset
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
    gateExited = true;
    enterHoldSamples = 0;
    enterHoldStartMs = 0;

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
    DEBUG("Lap finished, lap time = %u\n", lapTimes[lapCount]);

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
    return rssi[rssiCount];
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
