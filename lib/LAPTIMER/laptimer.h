#ifndef LAPTIMER_H
#define LAPTIMER_H

#include "RX5808.h"
#include "buzzer.h"
#include "config.h"
#include "kalman.h"
#include "led.h"
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
    // Peak RSSI seen since the last call to this method, then resets the
    // accumulator to the current sample.  Used by the Edit Pilot modal's
    // live RSSI feed so a fast drone's brief peak gets captured even if the
    // 200 ms HTTP poll happens to land at a non-peak moment.  Reading is
    // O(1); the accumulator updates inside handleLapTimerUpdate() so peaks
    // are tracked at the full sample rate (hundreds of Hz), not the poll
    // rate (5 Hz).
    uint8_t getRssiPeakSinceLast();
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
    // Peak RSSI seen since the last getRssiPeakSinceLast() call.  Volatile +
    // single-byte means reads / writes are atomic on the ESP32, so the
    // sampling side (high-rate, Core 1 main loop) and the HTTP read side
    // (AsyncWebServer task on Core 0) don't need a mutex.  The worst race
    // is a single dropped peak update — acceptable.
    volatile uint8_t rssiPeakSinceLast = 0;
    laptimer_state_e state = STOPPED;
    RX5808 *rx;
    Config *conf;
    Buzzer *buz;
    Led *led;
    WebhookManager *webhooks;
    KalmanFilter filter;
    boolean lapCountWraparound;
    uint32_t raceStartTimeMs;
    uint32_t startTimeMs;
    uint8_t lapCount;
    uint8_t rssiCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    uint8_t rssi[LAPTIMER_RSSI_HISTORY];
    uint8_t rssi_window[7];
    uint8_t rssi_window_index;

    // Pre-filter pipeline state (Kalman → Median-3 → MA → EMA → step limiter)
    uint8_t v1KHist[3];     // Median-of-3 history buffer
    uint8_t v1KHistIdx;
    float   v1Ema;          // EMA accumulator (NAN = uninitialised)
    bool    v1OutInit;      // Step-limiter has a valid previous sample
    uint8_t v1OutPrev;      // Step-limiter previous output

    uint8_t rssiPeak;
    uint32_t rssiPeakTimeMs;
    uint8_t lastLapPeakRssi = 0;  // peak RSSI of the most recently completed lap

    // Gate state tracking / debounce helpers
    bool gateExited;          // True when gate has re-armed (sustained below exit after last lap)
    bool enteredGate;         // True once we have crossed the enter threshold
    uint8_t enterHoldSamples; // Consecutive samples at/above enter (debounce counter)
    uint32_t enterHoldStartMs;// Timestamp of first at-enter sample (used for ceiling-drift watchdog)
    bool gate1Armed;          // Gate-1 bootstrap fired for current race

#if RSSI_STREAM_ENABLED
    // USB RSSI stream (toggle via /api/rssistream)
    bool     _rssiStream     = false;
    uint32_t _lastStreamMs   = 0;
    uint32_t _streamCount    = 0;
    uint32_t _streamCountMs  = 0;
#endif

    // Debug helpers (last processed RSSI chain)
    uint8_t lastRawRssi;
    uint8_t lastKalmanRssi;
    uint8_t lastAvgRssi;
    uint8_t prevAvgRssi;
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
