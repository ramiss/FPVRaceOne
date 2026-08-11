#ifndef LAPTIMER_H
#define LAPTIMER_H

#include "RX5808.h"
#include "buzzer.h"
#include "config.h"
#include "lapsync.h"
#include "led.h"

// ── Small odd-window running median filter ──────────────────────────────────
// Matches the design intent of RotorHazard's FastRunningMedian (single-stage
// filter, spike-rejecting, peak-preserving) but sized for our sample rate.
// Their 255-sample window only works at ~1 kHz sampling on a dedicated MCU.
//
// NOTE ON SAMPLE RATE: the ESP32-C6 has a SINGLE HP RISC-V core.  There is no
// "Core 1" — this filter is fed from handleLapTimerUpdate() on the Arduino
// loop task, which shares that one core with parallelTask, WiFi and lwIP.
// One sample is taken per loop() iteration and loop() ends in vTaskDelay(1),
// so with CONFIG_FREERTOS_HZ=1000 the rate is hard-capped near 1 kHz and
// measures ~200-500 Hz in practice.  The cap is the loop structure, not the
// chip.  Enable TIMING_STATS_ENABLED to measure it on real hardware.
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

#if TIMING_STATS_ENABLED
// Sample-interval statistics for one reporting window.  Populated by
// handleLapTimerUpdate() and published whole (never partially filled) so the
// Diagnostics page and any reader always see a self-consistent snapshot.
//
// maxIntervalUs is the number that matters: it is the worst gap between two
// consecutive RSSI samples, which bounds both detection jitter and the risk
// of a narrow peak falling between samples.  Mean/rate are context only.
struct TimingStats {
    uint32_t minIntervalUs  = 0xFFFFFFFFUL;
    uint32_t maxIntervalUs  = 0;
    uint32_t meanIntervalUs = 0;   // computed at publish time
    uint64_t sumIntervalUs  = 0;
    uint32_t sampleCount    = 0;
    uint32_t lateCount      = 0;   // intervals over TIMING_STATS_LATE_THRESHOLD_MS
    uint32_t lastSampleUs   = 0;
    uint32_t windowStartMs  = 0;
    bool     valid          = false;
};
#endif

// Laps retained in RAM for page-reload restore.
//
// Was 10, which was far below a real race.  /api/laps/current rebuilds the
// browser's lap table from this ring after a refresh, and the browser numbers
// the restored laps by their position — so anything the ring has dropped
// silently renumbers every lap that survives.
//
// Now defined by the lap-sync protocol rather than locally: the client ring
// and the master's per-node ring MUST truncate at the same point, or the two
// sides advertise different windows and every digest compare after the first
// eviction is meaningless.  One number, one place.
#define LAPTIMER_LAP_HISTORY LAPSYNC_MAX_LAPS
#define LAPTIMER_RSSI_HISTORY 100
#define LAPTIMER_CALIBRATION_HISTORY 5000  // Increased buffer for longer recordings

class LapTimer {
   public:
    void init(Config *config, RX5808 *rx5808, Buzzer *buzzer, Led *l, WebhookManager *webhook = nullptr);
    void start();
    void stop();
    bool     isRunning()           const;
    uint32_t getElapsedMs()        const;  // ms since race start (0 if stopped)
    uint16_t getLapCount()         const;  // ring write position — NOT a lap total, see getLapTotal()
    uint32_t getLapTimeAt(uint16_t index) const;  // lap time at 0-based RING index

    // Monotonic count of laps recorded since the last start()/clearLapData().
    // Unlike getLapCount(), this does NOT wrap.
    //
    // getLapCount() returns the ring's write cursor, which wraps modulo
    // LAPTIMER_LAP_HISTORY.  /api/laps/current used it as a lap count, so after
    // exactly LAPTIMER_LAP_HISTORY laps it read 0 and a page reload restored an
    // EMPTY lap table; past that it restored the wrong laps under the wrong
    // numbers.  Anything that means "how many laps has this pilot done" must
    // use this instead.
    uint16_t getLapTotal() const;

    // Chronological access to the laps still held, oldest first.
    // getRetainedLapCount() is min(getLapTotal(), LAPTIMER_LAP_HISTORY).
    // getRetainedLap() writes the lap's TRUE lap number (which is not the index
    // once the ring has wrapped) and its time.  Returns false if out of range.
    uint16_t getRetainedLapCount() const;
    bool     getRetainedLap(uint16_t index, uint16_t* lapNumber, uint32_t* lapTimeMs) const;

    // ── Lap Sync Protocol (§4) ──────────────────────────────────────────
    // The client's own laps are the authoritative replica: this ring is the
    // source of truth for this pilot, and the master's copy is an aggregate
    // healed from it.  These accessors are what the sync layer reads; they
    // never recompute anything, because both values are folded at append.

    // Full record for a retained lap, oldest first — what a resync serves.
    // `seq` is the lap's TRUE sequence number, which is not the ring index
    // once the ring has wrapped.
    bool     getRetainedRecord(uint16_t index, LapSyncRecord* out) const;

    // Rolling digest over ALL laps this race, not just the retained window.
    // Window-independent by construction (§4) — and therefore NOT
    // recomputable by a device that restored a windowed copy, which is why
    // adoptSyncState() exists.
    uint32_t getLapCrc() const { return _lapCrc; }

    // Totals that survive ring eviction.  Never derived from lapTimes[].
    const RaceSummary& getSummary() const { return _summary; }

    // Oldest sequence number still retained.  Bounds what any peer may ask
    // for: below this the laps are RETIRED, not missing (§6.1).
    uint32_t getOldestSeq() const;

    // Adopt a digest + summary restored from a peer (§4, CRC adoption).
    // Used after a reboot, when this device holds laps it did not witness
    // and so cannot fold its way to the correct digest.
    void     adoptSyncState(uint32_t crc, const RaceSummary& summary);

    // Append a lap that arrived from a peer rather than from local detection
    // (a restore).  Folds digest and summary exactly as finishLap() would,
    // so a restored ring and a lived-through ring are indistinguishable.
    void     appendRestoredLap(const LapSyncRecord& lap);

    void handleLapTimerUpdate(uint32_t currentTimeMs);

#if RSSI_LOGGING_ENABLED
    // Updated every handleLapTimerUpdate() call; read by main.cpp for RSSI logging
    RssiSnapshot snapshot = {};
#endif
#if TIMING_STATS_ENABLED
    // Last completed sample-interval window.  `valid` is false until the
    // first window closes (TIMING_STATS_WINDOW_MS after the first sample).
    const TimingStats& getTimingStats() const { return _tsPublished; }
#endif
    uint8_t getRssi();
    uint32_t getLapTime();
    uint8_t  getLastLapPeakRssi() const;
    bool isLapAvailable();
    void recordManualLap(uint32_t lapTimeMs);
    void clearLapData();
    
    // Calibration wizard methods
    // Returns the number of samples actually granted, or 0 if even the smallest
    // tier could not be allocated.  Callers should surface the granted capacity
    // to the UI — a shorter recording is a normal outcome, not an error.
    uint16_t startCalibrationWizard();
    void     stopCalibrationWizard();
    uint16_t getCalibrationCapacity() const { return calibrationCapacity; }
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
    uint16_t lapCount;     // ring write cursor, wraps at LAPTIMER_LAP_HISTORY
    uint16_t lapTotal;     // monotonic laps this race; never wraps
    uint8_t rssiCount;
    uint32_t lapTimes[LAPTIMER_LAP_HISTORY];
    // Race-relative timestamp of each lap's peak, parallel to lapTimes[].
    //
    // Stored rather than derived because a lap that is later re-sent during a
    // resync must carry the timestamp it was MEASURED at, not the one it was
    // transmitted at.  Arrival time is not a timestamp (§8); without this the
    // master would be stamping restored laps with resync time and any merged
    // cross-pilot ordering would place them wrongly.
    uint32_t lapRaceElapsedMs[LAPTIMER_LAP_HISTORY];
    uint8_t rssi[LAPTIMER_RSSI_HISTORY];

    // ── Lap Sync derived state (§4) ─────────────────────────────────────
    // Both are folded once per lap in _foldLap() and never recomputed from
    // the ring, so eviction cannot change them.
    uint32_t    _lapCrc = 0;
    RaceSummary _summary = {};

    // Fold one appended lap into the digest and the summary.  Single point of
    // truth for both, so a locally-detected lap and a restored lap produce
    // identical derived state.
    void _foldLap(const LapSyncRecord& lap);

    // Single-stage running median.  Replaces the previous cascade of
    // Kalman → Median-3 → MA(7) → EMA → step limiter, which combined to
    // attenuate the peak of a fast pass by 30-50 % and made high-speed
    // gate crossings hard to detect.  Window size is driven by
    // conf->getV1Smoothing() (0-10) → medianNFromSlider(level).
    RunningMedian<15> medianFilter;

#if TIMING_STATS_ENABLED
    TimingStats _ts;           // accumulating window
    TimingStats _tsPublished;  // last completed window, safe for readers
#endif

#if TIMING_MARKER_ENABLED
    // True while a marker pulse is asserted; cleared on the next sample tick
    // so the pulse lasts one sample period without blocking the detect path.
    bool _markerHigh = false;
#endif

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
    
    // ── Calibration wizard data ──────────────────────────────────────────
    // HEAP, not static, and only while the wizard is actually running.
    //
    // These were fixed arrays of LAPTIMER_CALIBRATION_HISTORY (5000) entries:
    // 5,000 B + 20,000 B = 25,000 bytes of .bss, held for the entire life of
    // every boot, on every device, during every race — for a stationary bench
    // feature.  Measured 2026-08-08: the LapTimer instance was the single
    // largest static symbol in the whole firmware at 25,336 B, a third of all
    // static DRAM, against a steady-state free heap of ~152 KB.
    //
    // Allocated by startCalibrationWizard(), freed by stopCalibrationWizard().
    // Every access must tolerate nullptr — the wizard can be off, and the
    // allocation can be refused.
    // Concurrency, which the static arrays never needed:
    //   - the sampler WRITES from loopTask (priority 1)
    //   - the web handlers ALLOCATE and FREE from async_tcp (priority 10)
    // async_tcp can preempt loopTask mid-write, so freeing the buffer while the
    // sampler sits between its bounds check and its store would be a
    // use-after-free.  A spinlock around the store and around the pointer swap
    // closes that window.  Both critical sections are a handful of
    // instructions, and the sampler's only runs in wizard mode, so the 997 Hz
    // race path is untouched.
    //
    // The /calibration/data READER needs no guard: it runs on async_tcp, the
    // same task that does the freeing, and AsyncWebServer runs handlers
    // sequentially on that one task.
    portMUX_TYPE calibrationMux = portMUX_INITIALIZER_UNLOCKED;
    void _freeCalibrationBuffers();

    uint16_t calibrationRssiCount;
    uint16_t calibrationCapacity = 0;   // granted samples; 0 = not allocated
    uint8_t  *calibrationRssi       = nullptr;
    uint32_t *calibrationTimestamps = nullptr;
    uint32_t lastCalibrationSampleMs;  // Track when last sample was taken

    void lapPeakCapture();
    bool lapPeakCaptured();
    void lapPeakReset();

    void startLap();
    void finishLap();
};

#endif
