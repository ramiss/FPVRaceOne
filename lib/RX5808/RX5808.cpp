#include "RX5808.h"
#include <Arduino.h>
#include "debug.h"
#include "config.h"
// Included here only, never in RX5808.h — keeps the ESP-IDF ADC driver off
// the include path of every translation unit that touches RX5808.
#include "esp_adc/adc_continuous.h"

RX5808::RX5808(uint8_t _rssiInputPin, uint8_t _rx5808DataPin, uint8_t _rx5808SelPin, uint8_t _rx5808ClkPin) {
    rssiInputPin = _rssiInputPin;
    rx5808DataPin = _rx5808DataPin;
    rx5808SelPin = _rx5808SelPin;
    rx5808ClkPin = _rx5808ClkPin;
    lastSetFreqTimeMs = millis();
}

void RX5808::init(uint8_t mode) {
    // INPUT (no pull-up): the RX5808 RSSI pin is an analog voltage output (~0–1V).
    // INPUT_PULLUP connects a ~45kΩ resistor to 3.3V which fights the signal and
    // artificially raises the ADC reading when the module has weak/no signal.
    pinMode(rssiInputPin, INPUT);
    pinMode(rx5808DataPin, OUTPUT);
    pinMode(rx5808SelPin, OUTPUT);
    pinMode(rx5808ClkPin, OUTPUT);
    analogReadResolution(12); // 12-bit: 0..4095

    // The RX5808 RSSI output is 0–1V. The default ESP32-C6 ADC attenuation
    // (ADC_11db) covers 0–3.1V, meaning the RSSI signal uses only ~32% of the
    // ADC scale, which crushes dynamic range and worsens the built-in non-linearity.
    // ADC_6db covers 0–1.75V on the C6: the 0–1V RSSI signal now uses ~57% of
    // the scale, giving much better peak definition with no change to filtering.
    analogSetPinAttenuation(rssiInputPin, ADC_6db);
    
    digitalWrite(rx5808SelPin, HIGH);
    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);

    resetRxModule();
    // Don't power down on init - leave module powered up and ready
    // Set currentFrequency to 0 to force initial frequency programming
    currentFrequency = 0;
    recentSetFreqFlag = false;
    // Delay to ensure module is ready before first frequency change
    delay(50);

    // Bring up DMA acquisition if requested.  Any failure falls back to the
    // polled path rather than leaving the timer without RSSI — a degraded
    // sample rate is recoverable, no signal at all is not.
    adcMode = ADC_MODE_POLLED;
    if (mode == ADC_MODE_DMA) {
        if (startDmaSampling()) {
            adcMode = ADC_MODE_DMA;
            DEBUG("[ADC] DMA continuous mode active: %u Hz, %u-byte frames\n",
                  (unsigned)ADC_DMA_SAMPLE_HZ, (unsigned)ADC_DMA_FRAME_BYTES);
        } else {
            DEBUG("[ADC] DMA init FAILED — falling back to polled analogRead\n");
        }
    } else {
        DEBUG("[ADC] Polled analogRead mode active\n");
    }
}

// ── DMA conversion-done ISR ─────────────────────────────────────────────────
//
// Runs in ISR context on every completed conversion frame (~625/sec at 20 kHz
// with 128-byte frames).  Reduces the frame to its MAXIMUM and merges that
// into a running peak.
//
// Peak — not mean, not last — is the entire reason DMA is worth doing here.
// It means each readRssi() returns the highest value the hardware saw since
// the previous read, so a narrow gate-pass peak cannot be lost just because
// loop() was late. Mean would average the peak away; last would sample it
// with the same aliasing problem the polled path has.
//
// Kept minimal and IRAM-resident: no logging, no allocation, no floating
// point. `user_data` is the RX5808 instance's dmaPeakRaw.
static bool IRAM_ATTR rx5808AdcConvDone(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data) {
    volatile uint32_t *peak = (volatile uint32_t *)user_data;
    if (!peak || !edata || !edata->conv_frame_buffer) return false;

    uint32_t frameMax = 0;
    const uint32_t n = edata->size / SOC_ADC_DIGI_RESULT_BYTES;
    for (uint32_t i = 0; i < n; i++) {
        const adc_digi_output_data_t *p =
            (const adc_digi_output_data_t *)&edata->conv_frame_buffer[i * SOC_ADC_DIGI_RESULT_BYTES];
        const uint32_t v = p->type2.data;
        if (v > frameMax) frameMax = v;
    }

    // Merge into the running peak.  Plain compare-and-store is safe here:
    // this ISR is the only writer, and readRssi()'s atomic exchange either
    // sees the old value or the new one — never a torn word.
    if (frameMax > *peak) *peak = frameMax;

    return false;  // no higher-priority task woken
}

bool RX5808::startDmaSampling() {
    adc_unit_t    unit;
    adc_channel_t channel;
    if (adc_continuous_io_to_channel(rssiInputPin, &unit, &channel) != ESP_OK) {
        DEBUG("[ADC] pin %u is not an ADC-capable input\n", (unsigned)rssiInputPin);
        return false;
    }
    adcChannel = (uint8_t)channel;

    adc_continuous_handle_cfg_t handleCfg = {};
    handleCfg.max_store_buf_size = ADC_DMA_POOL_BYTES;
    handleCfg.conv_frame_size    = ADC_DMA_FRAME_BYTES;
    // Drop oldest rather than stall if we ever fall behind draining — we only
    // care about the running peak, not a complete sample record.
    handleCfg.flags.flush_pool   = 1;

    adc_continuous_handle_t handle = nullptr;
    if (adc_continuous_new_handle(&handleCfg, &handle) != ESP_OK) {
        DEBUG("[ADC] adc_continuous_new_handle failed\n");
        return false;
    }

    // Single channel, same attenuation as the polled path so both modes see
    // an identical voltage-to-count mapping and the A/B stays apples-to-apples.
    adc_digi_pattern_config_t pattern = {};
    pattern.atten     = ADC_ATTEN_DB_6;
    pattern.channel   = channel & 0x7;
    pattern.unit      = unit;
    pattern.bit_width = ADC_BITWIDTH_12;

    adc_continuous_config_t contCfg = {};
    contCfg.pattern_num    = 1;
    contCfg.adc_pattern    = &pattern;
    contCfg.sample_freq_hz = ADC_DMA_SAMPLE_HZ;
    contCfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
    contCfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

    if (adc_continuous_config(handle, &contCfg) != ESP_OK) {
        DEBUG("[ADC] adc_continuous_config failed (freq %u Hz out of range?)\n",
              (unsigned)ADC_DMA_SAMPLE_HZ);
        adc_continuous_deinit(handle);
        return false;
    }

    adc_continuous_evt_cbs_t cbs = {};
    cbs.on_conv_done = rx5808AdcConvDone;
    if (adc_continuous_register_event_callbacks(handle, &cbs, (void *)&dmaPeakRaw) != ESP_OK) {
        DEBUG("[ADC] callback registration failed\n");
        adc_continuous_deinit(handle);
        return false;
    }

    if (adc_continuous_start(handle) != ESP_OK) {
        DEBUG("[ADC] adc_continuous_start failed\n");
        adc_continuous_deinit(handle);
        return false;
    }

    adcHandle = (void *)handle;
    return true;
}

void RX5808::stopDmaSampling() {
    if (!adcHandle) return;
    adc_continuous_handle_t handle = (adc_continuous_handle_t)adcHandle;
    adc_continuous_stop(handle);
    adc_continuous_deinit(handle);
    adcHandle = nullptr;
}

void RX5808::handleFrequencyChange(uint32_t currentTimeMs, uint16_t potentiallyNewFreq) {
    // If a frequency change is requested and bus is free, program it
    if ((currentFrequency != potentiallyNewFreq) &&
        ((currentTimeMs - lastSetFreqTimeMs) > RX5808_MIN_BUSTIME)) {
        settingFrequency = true;
        // Start timing window from the moment we issued the write
        lastSetFreqTimeMs = currentTimeMs;

        setFrequency(potentiallyNewFreq);
        // setFrequency() sets recentSetFreqFlag = true
        return; // avoid falling through and "tune done" on the same tick
    }

    // If we recently set frequency, wait for tune time then verify once
    if (recentSetFreqFlag) {
        const uint32_t dt = currentTimeMs - lastSetFreqTimeMs;
        if (dt > RX5808_MIN_TUNETIME + 100) {
            DEBUG("RX5808 Tune done: %u\n", currentFrequency);
            verifyFrequency();     // NOTE: consider making this debug-only if flaky

            settingFrequency = false;
            recentSetFreqFlag = false;  // don't need to check again until next freq change
            // Do NOT update lastSetFreqTimeMs here; it is used as the write timestamp
        }
    }
}


bool RX5808::verifyFrequency() {
    // Start of Read Reg code :
    // Verify read HEX value in RX5808 module Frequency Register 0x01
    uint16_t vtxRegisterHex = 0;
    //  Modified copy of packet code in setRxModuleToFreq(), to read Register 0x01
    //  20 bytes of register data are read, but the
    //  MSB 4 bits are zeros
    //  Data Packet is: register address (4-bits) = 0x1, read/write bit = 1 for read, data D0-D15 stored in vtxHexVerify, data15-19=0x0

    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0x1
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();

    rx5808SerialSendBit0();  // Read register r/w

    // receive data D0-D15, and ignore D16-D19
    pinMode(rx5808DataPin, INPUT_PULLUP);
    for (uint8_t i = 0; i < 20; i++) {
        delayMicroseconds(10);
        // only use D0-D15, ignore D16-D19
        if (i < 16) {
            if (digitalRead(rx5808DataPin)) {
                bitWrite(vtxRegisterHex, i, 1);
            } else {
                bitWrite(vtxRegisterHex, i, 0);
            }
        }
        if (i >= 16) {
            digitalRead(rx5808DataPin);
        }
        digitalWrite(rx5808ClkPin, HIGH);
        delayMicroseconds(10);
        digitalWrite(rx5808ClkPin, LOW);
        delayMicroseconds(10);
    }

    pinMode(rx5808DataPin, OUTPUT);  // return status of Data pin after INPUT_PULLUP above
    rx5808SerialEnableHigh();        // Finished clocking data in
    delay(2);

    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);

    if (vtxRegisterHex != freqMhzToRegVal(currentFrequency)) {
        DEBUG("RX5808 frequency not matching, register = %u, currentFreq = %u\n", vtxRegisterHex, currentFrequency);
        return false;
    }
    DEBUG("RX5808 frequency verified properly %u\n", currentFrequency);
    return true;
}

// Set frequency on RX5808 module to given value
void RX5808::setFrequency(uint16_t vtxFreq) {
    DEBUG("RX5808 Setting frequency to %u\n", vtxFreq);

    currentFrequency = vtxFreq;

    if (vtxFreq == POWER_DOWN_FREQ_MHZ)  // frequency value to power down rx module
    {
        powerDownRxModule();
        rxPoweredDown = true;
        return;
    }
    if (rxPoweredDown) {
        resetRxModule();
        rxPoweredDown = false;
    }

    // Get the hex value to send to the rx module
    uint16_t vtxHex = freqMhzToRegVal(vtxFreq);

    // Channel data from the lookup table, 20 bytes of register data are sent, but the
    // MSB 4 bits are zeros register address = 0x1, write, data0-15=vtxHex data15-19=0x0
    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0x1
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();
    rx5808SerialSendBit0();

    rx5808SerialSendBit1();  // Write to register

    // D0-D15, note: loop runs backwards as more efficent on AVR
    uint8_t i;
    for (i = 16; i > 0; i--) {
        if (vtxHex & 0x1) {  // Is bit high or low?
            rx5808SerialSendBit1();
        } else {
            rx5808SerialSendBit0();
        }
        vtxHex >>= 1;  // Shift bits along to check the next one
    }

    for (i = 4; i > 0; i--)  // Remaining D16-D19
        rx5808SerialSendBit0();

    rx5808SerialEnableHigh();  // Finished clocking data in
    delay(2);

    digitalWrite(rx5808ClkPin, LOW);
    digitalWrite(rx5808DataPin, LOW);

    recentSetFreqFlag = true;  // indicate need to wait RX5808_MIN_TUNETIME before reading RSSI
}

bool RX5808::isSettingFrequency() {
    return settingFrequency;
}

// ADC is 12-bit (0..4095), ADC_6db attenuation → ~0–1750mV input range.
// RX5808 RSSI output is ~0–1V, so 1V ≈ 4095*(1000/1750) ≈ 2340 counts.
// 2400 adds a small headroom margin above the expected peak.
#define RSSI_SCALE_MAX 2400UL

uint8_t RX5808::readRssi() {
    if (recentSetFreqFlag) return 0; // RSSI unstable immediately after tune

    uint16_t raw;

    if (adcMode == ADC_MODE_DMA) {
        // Read-and-clear the running peak in one atomic operation.  A plain
        // read-then-zero would race the ISR: a frame completing between the
        // two would have its peak discarded.  __atomic_exchange_n is a single
        // instruction on RISC-V and needs no critical section.
        raw = (uint16_t)__atomic_exchange_n(&dmaPeakRaw, 0, __ATOMIC_RELAXED);
        // A zero here means no frame completed since the last read — possible
        // if loop() runs faster than the ~625 Hz frame rate.  Reporting 0
        // would inject a false trough into the median, so fall back to a
        // direct read for this tick instead.
        if (raw == 0) raw = (uint16_t)analogRead(rssiInputPin);
    } else {
        raw = (uint16_t)analogRead(rssiInputPin);
    }

    return (raw >= RSSI_SCALE_MAX)
           ? 255
           : (uint8_t)((raw * 255UL) / RSSI_SCALE_MAX);
}


void RX5808::rx5808SerialSendBit1() {
    digitalWrite(rx5808DataPin, HIGH);
    delayMicroseconds(300);
    digitalWrite(rx5808ClkPin, HIGH);
    delayMicroseconds(300);
    digitalWrite(rx5808ClkPin, LOW);
    delayMicroseconds(300);
}

void RX5808::rx5808SerialSendBit0() {
    digitalWrite(rx5808DataPin, LOW);
    delayMicroseconds(300);
    digitalWrite(rx5808ClkPin, HIGH);
    delayMicroseconds(300);
    digitalWrite(rx5808ClkPin, LOW);
    delayMicroseconds(300);
}

void RX5808::rx5808SerialEnableLow() {
    digitalWrite(rx5808SelPin, LOW);
    delayMicroseconds(200);
}

void RX5808::rx5808SerialEnableHigh() {
    digitalWrite(rx5808SelPin, HIGH);
    delayMicroseconds(200);
}

// Reset rx5808 module to wake up from power down
void RX5808::resetRxModule() {
    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit1();  // Register 0xF
    rx5808SerialSendBit1();
    rx5808SerialSendBit1();
    rx5808SerialSendBit1();

    rx5808SerialSendBit1();  // Write to register

    for (uint8_t i = 20; i > 0; i--)
        rx5808SerialSendBit0();

    rx5808SerialEnableHigh();  // Finished clocking data in

    setupRxModule();
}

// Set power options on the rx5808 module
void RX5808::setRxModulePower(uint32_t options) {
    rx5808SerialEnableHigh();
    rx5808SerialEnableLow();

    rx5808SerialSendBit0();  // Register 0xA
    rx5808SerialSendBit1();
    rx5808SerialSendBit0();
    rx5808SerialSendBit1();

    rx5808SerialSendBit1();  // Write to register

    for (uint8_t i = 20; i > 0; i--) {
        if (options & 0x1) {  // Is bit high or low?
            rx5808SerialSendBit1();
        } else {
            rx5808SerialSendBit0();
        }
        options >>= 1;  // Shift bits along to check the next one
    }

    rx5808SerialEnableHigh();  // Finished clocking data in

    digitalWrite(rx5808DataPin, LOW);
}

// Power down rx5808 module
void RX5808::powerDownRxModule() {
    setRxModulePower(0b11111111111111111111);
}

// Set up rx5808 module (disabling unused features to save some power)
void RX5808::setupRxModule() {
    setRxModulePower(0b11010000110111110011);
}

// Calculate rx5808 register hex value for given frequency in MHz
uint16_t RX5808::freqMhzToRegVal(uint16_t freqInMhz) {
    uint16_t tf, N, A;
    tf = (freqInMhz - 479) / 2;
    N = tf / 32;
    A = tf % 32;
    return (N << (uint16_t)7) + A;
}
