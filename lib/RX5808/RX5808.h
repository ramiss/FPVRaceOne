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

class RX5808 {
   public:
    RX5808(uint8_t _rssiInputPin, uint8_t _rx5808DataPin, uint8_t _rx5808SelPin, uint8_t _rx5808ClkPin);
    // mode selects the acquisition path and is latched for the object's life.
    // Falls back to polled automatically if DMA init fails for any reason.
    void init(uint8_t mode = ADC_MODE_POLLED);
    // Which path is actually running — may differ from what was requested if
    // DMA init failed.  Surfaced in Diagnostics so the A/B is unambiguous.
    uint8_t getAdcMode() const { return adcMode; }
    void setFrequency(uint16_t frequency);
    uint8_t readRssi();
    void handleFrequencyChange(uint32_t currentTimeMs, uint16_t potentiallyNewFreq);
    bool verifyFrequency();
    bool isSettingFrequency();
    // Whatever frequency setFrequency() last programmed.  Exposed for the
    // selftest's cleanup path: after the frequency-sweep loop it needs to
    // restore this value and re-verify so the UI's "Calibrating pilot
    // frequency" banner gets its expected "RX5808 frequency verified
    // properly" log line.
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
    // Running peak written by the DMA ISR, drained by readRssi().  uint32_t
    // rather than uint8_t so __atomic_exchange_n operates on a native word.
    volatile uint32_t dmaPeakRaw = 0;
    // Last non-zero DMA sample, held when a read finds the peak empty.  Keeps
    // the DMA path on its own scale rather than falling back to a oneshot
    // read, which reports ~1.9x lower counts for the same voltage.
    uint16_t lastDmaRaw = 0;

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
