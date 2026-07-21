#ifndef LAPTIMER_H
#define LAPTIMER_H

#include "RX5808.h"
#include "buzzer.h"
#include "config.h"
#include "led.h"

// ── Small odd-window running median filter ──────────────────────────────────
// Matches the design intent of RotorHazard's FastRunningMedian (single-stage
// filter, spike-rejecting, peak-preserving) but sized for our shared-Core
// sample rate.  Their 255-sample window only works at ~1 kHz sampling; ours
// runs on Core 1 alongside the Arduino loop() so we get ~200-500 Hz.
//
// MaxN = 15 caps the runtime-selected window at ~50 ms even in the worst
// under-sampled case, which is the widest window that still preserves the
// peak of a 50 ms fast-pass (competitive racing drone at ~150 mph).
//
// Cost per addAndGet(): one 15-element insertion sort — well under 1 μs on
// the ESP32.  Kept in-header so it's trivially inline-able.
template <uint8_t MaxN>
class RunningMedian {
public:
    void reset() {
        _writeIdx = 0;
        _count = 0;
    }
    // Change window size — clears the history buffer if the size actually
    // changed so we never mix samples from two configurations.
    void setWindow(uint8_t n) {
        if (n < 3) n = 3;
        if ((n & 1) == 0) n++;      // enforce odd so median is well-defined
        if (n > MaxN) n = MaxN;
        if (n != _n) {
            _n        = n;
            _writeIdx = 0;
            _count    = 0;
        }
    }
    uint8_t window() const { return _n; }
    // Push one sample, return the median of what's currently buffered.
    // Before the window fills we return the median of the samples so far,
    // matching RotorHazard's "isFilled()" behaviour — early samples don't
    // gate lap detection but they also aren't representative until the
    // buffer stabilises.
    uint8_t addAndGet(uint8_t v) {
        _buf[_writeIdx] = v;
        _writeIdx = (uint8_t)((_writeIdx + 1) % _n);
        if (_count < _n) _count++;
        uint8_t scratch[MaxN];
        for (uint8_t i = 0; i < _count; i++) scratch[i] = _buf[i];
        for (uint8_t i = 1; i < _count; i++) {
            uint8_t x = scratch[i];
            int8_t  j = (int8_t)i - 1;
            while (j >= 0 && scratch[j] > x) {
                scratch[(uint8_t)(j + 1)] = scratch[j];
                j--;
            }
            scratch[(uint8_t)(j + 1)] = x;
        }
        return scratch[_count / 2];
    }
    bool isFilled() const { return _count >= _n; }

private:
    uint8_t _buf[MaxN] = {};
    uint8_t _writeIdx  = 0;
    uint8_t _count     = 0;
    uint8_t _n         = 3;
};
#if RSSI_LOGGING_ENABLED
#include "rssilog.h"   // for RssiSnapshot — logger is wired in main.cpp only
#endif

// Forward declarations to avoid circular dependency
class WebhookManager;

typedef enum {
    STOPPED,
    WAITING,
    RUNNING,
    CALIBRATION_WIZARD
} laptimer_state_e;

#define LAPTIMER_LAP_HISTORY 10
#define LAPTIMER_RSSI_HISTORY 100
#define LAPTIMER_CALIBRATION_HISTORY 5000  // Increased buffer for longer recordings

class LapTimer {
   public:
    void init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook = nullptr);
    void start();
    void stop();
    bool     isRunning()           const;
    uint32_t getElapsedMs()        const;  // ms since race start (0 if stopped)
    uint8_t  getLapCount()         const;  // laps completed so far
    uint32_t getLapTimeAt(uint8_t index) const;  // lap time at 0-based index
    void handleLapTimerUpdate(uint32_t currentTimeMs);

#if RSSI_LOGGING_ENABLED
    // Updated every handleLapTimerUpdate() call; read by main.cpp for RSSI logging
    RssiSnapshot snapshot = {};
#endif
    uint8_t getRssi();
    uint32_t getLapTime();
    uint8_t  getLastLapPeakRssi() const;
    bool isLapAvailable();
    void recordManualLap(uint32_t lapTimeMs);
    void clearLapData();
    
    // Calibration wizard methods
    void startCalibrationWizard();
    void stopCalibrationWizard();
    uint16_t getCalibrationRssiCount();
    uint8_t getCalibrationRssi(uint16_t index);
    uint32_t getCalibrationTimestamp(uint16_t index);
    
   private:
    laptimer_state_e state = STOPPED;
    RX5808 *rx;
    Config *conf;
    Buzzer *buz;
    Led *led;
    WebhookManager *webhooks;
    boolean lapCountWraparound;
    uint32_t raceStartTimeMs;
    uint32_t startTimeMs;
    uint8_t lapCount;
    uint8_t rssiCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    uint8_t rssi[LAPTIMER_RSSI_HISTORY];

    // Single-stage running median.  Replaces the previous cascade of
    // Kalman → Median-3 → MA(7) → EMA → step limiter, which combined to
    // attenuate the peak of a fast pass by 30-50 % and made high-speed
    // gate crossings hard to detect.  Window size is driven by
    // conf->getV1Smoothing() (0-10) → medianNFromSlider(level).
    RunningMedian<15> medianFilter;

    uint8_t rssiPeak;
    uint32_t rssiPeakTimeMs;
    uint8_t lastLapPeakRssi = 0;  // peak RSSI of the most recently completed lap

    // Gate state tracking.  The 2-sample enter-hold debounce and the
    // ceiling-drift watchdog are both compile-time in laptimer.cpp
    // (kEnableEnterDebounce / kEnableCeilingWatchdog) — flip either to
    // false ONLY for bench characterisation of pipeline behaviour.
    bool     gateExited;        // True when gate has re-armed (crossed back below exit)
    bool     enteredGate;       // True once filtered RSSI crossed enterAt for enough samples
    bool     gate1Armed;        // Gate-1 bootstrap fired for current race
    uint8_t  enterHoldSamples;  // Consecutive at-or-above-enter samples (debounce counter)
    uint32_t enterHoldStartMs;  // millis() when we first saw at-or-above-enter (0 = reset)

#if RSSI_STREAM_ENABLED
    // USB RSSI stream (toggle via /api/rssistream)
    bool     _rssiStream     = false;
    uint32_t _lastStreamMs   = 0;
    uint32_t _streamCount    = 0;
    uint32_t _streamCountMs  = 0;
#endif

    // Debug helpers — last raw ADC read and last median-filtered output,
    // used for the periodic race debug print and for the /status page.
    uint8_t lastRawRssi;
    uint8_t lastFilteredRssi;
    uint8_t prevFilteredRssi;   // one-sample delay for edge-detect debug prints
    uint32_t lastRaceDebugPrintMs;

    bool lapAvailable = false;

#if RSSI_STREAM_ENABLED
public:
    void setRssiStream(bool e)       { _rssiStream = e; }
    bool isRssiStreamEnabled() const { return _rssiStream; }
private:
#endif
    
    // Calibration wizard data
    uint16_t calibrationRssiCount;
    uint8_t calibrationRssi[LAPTIMER_CALIBRATION_HISTORY];
    uint32_t calibrationTimestamps[LAPTIMER_CALIBRATION_HISTORY];
    uint32_t lastCalibrationSampleMs;  // Track when last sample was taken

    void lapPeakCapture();
    bool lapPeakCaptured();
    void lapPeakReset();

    void startLap();
    void finishLap();
};

#endif
