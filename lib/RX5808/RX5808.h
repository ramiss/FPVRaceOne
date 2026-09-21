#ifndef RX5808_H
#define RX5808_H

#include <stdint.h>

#define RX5808_MIN_TUNETIME 35    // after set freq need to wait this long before read RSSI
#define RX5808_MIN_BUSTIME 30     // after set freq need to wait this long before setting again
#define POWER_DOWN_FREQ_MHZ 1111  // signal to power down the module

// ── RSSI acquisition modes ──────────────────────────────────────────────────
//
// ADC_MODE_POLLED — one analogRead() per handleLapTimerUpdate() call, i.e. one
//   sample per loop() iteration.  loop() ends in vTaskDelay(1) and
//   CONFIG_FREERTOS_HZ is 1000, so this is hard-capped near 1 kHz and measures
//   ~200-500 Hz in practice.  A narrow peak can fall BETWEEN two samples.
//
// ADC_MODE_DMA — the ADC peripheral samples continuously into a DMA ring at
//   ADC_DMA_SAMPLE_HZ, independent of CPU scheduling.  An ISR reduces each
//   completed frame to its MAXIMUM, and readRssi() returns (and clears) that
//   running peak.  Every read therefore carries the peak of all hardware-timed
//   samples since the previous read, so a peak can no longer be missed because
//   the loop was late — which is the whole point of doing this.
//
// Selected at boot from Config::getAdcMode(); changing it requires a reboot.
enum RssiAdcMode : uint8_t {
    ADC_MODE_POLLED = 0,
    ADC_MODE_DMA    = 1,
};

// Start at 20 kHz.  The C6 ceiling is SOC_ADC_SAMPLE_FREQ_THRES_HIGH (83333),
// so there is headroom to raise this once the path is proven on hardware.
#define ADC_DMA_SAMPLE_HZ    20000
// Bytes per DMA conversion frame.  Must be a multiple of
// SOC_ADC_DIGI_DATA_BYTES_PER_CONV (4).  128 bytes = 32 conversions, giving
// ~625 ISR callbacks/sec at 20 kHz — frequent enough that the peak-hold window
// stays short, infrequent enough that ISR overhead stays negligible.
#define ADC_DMA_FRAME_BYTES  128
#define ADC_DMA_POOL_BYTES   (ADC_DMA_FRAME_BYTES * 4)
// How long the one-shot boot variance check observes RSSI before deciding
// whether the input is alive.  A few seconds: long enough that even a very
// quiet gate must show some jitter, short enough to be over before anyone has
// opened the web UI.  Costs nothing after it closes.
#define RSSI_BOOT_CHECK_MS   3000
// DMA stall watchdog — see RX5808::serviceRearm().  A healthy stream delivers
// a frame every ~1.6 ms; if none arrives for this long the DMA has halted and
// the stream is re-armed.  100 ms is >10x any legitimate runtime loop() stall,
// so a live stream is never torn down by mistake, and short enough that a
// gate pass (150-400 ms above threshold) cannot fall entirely inside the gap.
#define ADC_DMA_STALL_MS         100
// Consecutive stall re-arms with no frame in between before giving up.  A
// stream that three restarts cannot revive is a hardware fault; retrying
// forever would also keep resetting the boot variance window and hide it.
#define ADC_DMA_STALL_MAX_REARMS 3

// Shared between the DMA ISR and readRssi()/serviceRearm().  The ISR gets a
// pointer to this and nothing else — no access to the object.
struct RX5808DmaShared {
    // Running peak written by the ISR, drained by readRssi().  uint32_t
    // rather than uint8_t so __atomic_exchange_n operates on a native word.
    volatile uint32_t peak   = 0;
    // Frames delivered since start.  Only ever compared for "has it moved",
    // so wrap-around is harmless.  The stall watchdog's sole input.
    volatile uint32_t frames = 0;
};

class RX5808 {
   public:
    RX5808(uint8_t _rssiInputPin, uint8_t _rx5808DataPin, uint8_t _rx5808SelPin, uint8_t _rx5808ClkPin);
    // mode selects the acquisition path and is latched for the object's life.
    // Falls back to polled automatically if DMA init fails for any reason.
    void init(uint8_t mode = ADC_MODE_POLLED);
    // Which path is actually running — may differ from what was requested if
    // DMA init failed.  Surfaced in Diagnostics so the A/B is unambiguous.
    uint8_t getAdcMode() const { return adcMode; }
    // Did RSSI hold one constant value for the whole boot check window?
    //
    // A disconnected or failing RX5808 has ONE signature: a flat signal.  The
    // level it sticks at is arbitrary — high, low, or anywhere between,
    // depending on how the module died — so the level tells you nothing and
    // the VARIANCE tells you everything.  A live receiver always jitters; a
    // value that never moves is not a quiet gate, it is a disconnected one.
    //
    // This also covers a stopped ADC feed, where readRssi() repeats a held
    // sample forever: same flat signal, same detector, no second mechanism.
    //
    // Answers false until the window closes, so a caller during the first few
    // seconds sees "no fault" rather than a premature verdict.
    bool rssiInputFlat()  const { return bootFlat; }
    bool rssiCheckDone()  const { return bootCheckDone; }
    uint16_t rssiFlatLevel() const { return bootCheckMax; }   // raw ADC counts

    // The RAW ADC value behind the most recent readRssi(), before scaling to
    // 0-255.  Exposed so a caller measuring VARIANCE can work at full ADC
    // resolution: the scale divides by ~18 (RSSI_SCALE_MAX_DMA / 255), which is
    // plenty for reporting a level but throws away exactly the detail a
    // liveness check needs in a quiet RF environment.
    uint16_t lastRawSample() const { return lastRawUsed; }

    // Re-arm DMA acquisition — a two-step handshake, and the split is NOT
    // optional.
    //
    //   requestRearm()  — safe from ANY task.  Sets a flag.  Called from the
    //                     WiFi AP_START / STA_START events (arduino_events
    //                     task) and after every flash write (parallelTask or
    //                     the webserver task, via Storage::notifyFlashWrite).
    //   serviceRearm()  — MUST run on the task that called init(), i.e. the
    //                     Arduino loopTask.  main.cpp calls it from loop()
    //                     every iteration.  Also runs the DMA stall watchdog:
    //                     if no frame has arrived for ADC_DMA_STALL_MS it
    //                     requests a re-arm itself, up to
    //                     ADC_DMA_STALL_MAX_REARMS times in a row.
    //
    // Why: IDF's adc_continuous_start() takes the ADC1 unit lock — a FreeRTOS
    // mutex — and HOLDS it for the life of the stream; adc_continuous_stop()
    // releases it.  A mutex may only be released by the task that owns it, and
    // that owner is whoever started the stream: loopTask, via init() from
    // setup().  Doing the stop from the WiFi event task released a mutex it
    // did not hold, and FreeRTOS asserted in xTaskPriorityDisinherit — a boot
    // loop on every unit, observed 2026-09-20.  See the .cpp for the full
    // account of why the re-arm exists at all.
    void requestRearm(const char* reason);
    void serviceRearm();
    // Lifetime count of stall-watchdog recoveries.  Reported in /timer/rssi
    // so a unit that keeps re-arming is visible without a serial port.
    uint32_t dmaRecoveries() const { return dmaStallRecoveries; }
    void setFrequency(uint16_t frequency);
    uint8_t readRssi();
    void handleFrequencyChange(uint32_t currentTimeMs, uint16_t potentiallyNewFreq);
    bool isSettingFrequency();
    // Whatever frequency setFrequency() last programmed.  Read by Diagnostics
    // to report the frequency alongside the RSSI noise floor.
    //
    // There is deliberately no verifyFrequency() counterpart.  SPI readback
    // does not work on these modules — see handleFrequencyChange() — and the
    // receiver's health is established from the analog line instead, by the
    // RSSI variance check.  One check, one thing to troubleshoot.
    uint16_t getCurrentFrequency() const { return currentFrequency; }
    bool recentSetFreqFlag = false;

   private:
    uint8_t rx5808DataPin = 0;  // DATA (CH1) output line to RX5808 module
    uint8_t rx5808ClkPin = 0;   // CLK (CH3) output line to RX5808 module
    uint8_t rx5808SelPin = 0;   // SEL (CH2) output line to RX5808 module
    uint8_t rssiInputPin = 0;   // RSSI input from RX5808

    uint16_t currentFrequency = 0;
    bool settingFrequency = false;

    bool rxPoweredDown = false;
    uint32_t lastSetFreqTimeMs = 0;

    // ── DMA acquisition state ───────────────────────────────────────────
    // adcHandle is an opaque adc_continuous_handle_t.  Kept as void* so
    // esp_adc/adc_continuous.h stays out of this header and off the include
    // path of everything that pulls in RX5808.h.
    uint8_t  adcMode      = ADC_MODE_POLLED;
    void*    adcHandle    = nullptr;
    uint8_t  adcChannel   = 0;
    // Peak + frame counter shared with the ISR — see RX5808DmaShared.
    RX5808DmaShared dma;
    // ── DMA stall watchdog state (loopTask only) ────────────────────────
    uint32_t dmaLastFrames       = 0;   // dma.frames as of the last check
    uint32_t dmaLastFrameMs      = 0;   // millis() when it last advanced
    uint8_t  dmaStallRearms      = 0;   // consecutive re-arms with no frame since
    uint32_t dmaStallRecoveries  = 0;   // lifetime, see dmaRecoveries()
    // Last non-zero DMA sample, held when a read finds the peak empty.  Keeps
    // the DMA path on its own scale rather than falling back to a oneshot
    // read, which reports ~1.9x lower counts for the same voltage.
    uint16_t lastDmaRaw = 0;
    // Raw value behind the last readRssi() on EITHER path — see lastRawSample().
    uint16_t lastRawUsed = 0;
    // ── Boot variance check ─────────────────────────────────────────────
    // One-shot: min/max of the first RSSI_BOOT_CHECK_MS of readings, then a
    // single verdict and the tracking stops.  Re-opened by serviceRearm()
    // after each WiFi bring-up, so the verdict describes the feed that is
    // actually running, not the one WiFi may have just disturbed.  See
    // rssiInputFlat().
    // RAW ADC counts, not the scaled 0-255 value — see updateBootVarianceCheck().
    bool     bootCheckDone  = false;
    uint32_t bootCheckEndMs = 0;
    uint16_t bootCheckMin   = 0xFFFF;
    uint16_t bootCheckMax   = 0;
    bool     bootFlat       = false;
    void     updateBootVarianceCheck(uint16_t raw);
    void     resetBootCheck();

    // Cross-task handshake for the re-arm — see requestRearm()/serviceRearm().
    // volatile: written on the WiFi event task or parallelTask, read and
    // cleared on loopTask.  The reason is only ever a string literal, so a
    // bare pointer is safe.
    volatile bool rearmPending = false;
    const char*   rearmReason  = "";

    bool startDmaSampling();
    void stopDmaSampling();

    void rx5808SerialSendBit1();
    void rx5808SerialSendBit0();
    void rx5808SerialEnableLow();
    void rx5808SerialEnableHigh();

    void setRxModulePower(uint32_t options);
    void resetRxModule();
    void setupRxModule();
    void powerDownRxModule();

    static uint16_t freqMhzToRegVal(uint16_t freqInMhz);
};

#endif
