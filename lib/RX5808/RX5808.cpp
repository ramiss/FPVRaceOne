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
// point. `user_data` is the RX5808 instance's RX5808DmaShared.
//
// The frame counter is the stall watchdog's only input: a stream that has
// halted stops incrementing it.  One add per frame, nothing else.
static bool IRAM_ATTR rx5808AdcConvDone(adc_continuous_handle_t handle,
                                        const adc_continuous_evt_data_t *edata,
                                        void *user_data) {
    RX5808DmaShared *shared = (RX5808DmaShared *)user_data;
    if (!shared || !edata || !edata->conv_frame_buffer) return false;
    volatile uint32_t *peak = &shared->peak;
    shared->frames++;

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
    if (adc_continuous_register_event_callbacks(handle, &cbs, (void *)&dma) != ESP_OK) {
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
    // Watchdog baseline: "now" counts as the last frame so the first check
    // measures from the start, not from time zero.
    dmaLastFrames  = dma.frames;
    dmaLastFrameMs = millis();
    return true;
}

void RX5808::stopDmaSampling() {
    if (!adcHandle) return;
    adc_continuous_handle_t handle = (adc_continuous_handle_t)adcHandle;
    adc_continuous_stop(handle);
    adc_continuous_deinit(handle);
    adcHandle = nullptr;
}

// ── Re-arm after WiFi bring-up ──────────────────────────────────────────────
//
// THE INTERMITTENT FLATLINE.  Observed 2026-09-18..20 on several units, in
// single and master mode, on good power: RSSI froze at one arbitrary non-zero
// level from boot, the calibration trace drew a flat line, and a reboot
// cleared it.  Compared against v1.0.1 the only firmware change was a logging
// flag with no side effects, and DMA mode was the default in both — so this
// is not a regression, it is a boot race that heavier rebooting finally hit.
//
// The race, as read from the IDF 5.5 sources the framework ships with:
//
//   * The ESP32-C6 has a single SAR ADC and NO hardware arbiter for it —
//     hal/esp32c6/include/hal/adc_ll.h: "Only ADC2 have arbiter function",
//     and the C6 has no ADC2.
//   * The WiFi PHY uses that same SAR ADC as its power detector.  esp_phy/
//     phy_override.c acquires it (set_xpd_sar / phy_set_pwdet_power) and
//     writes PWDET_CONF_REG while the RF calibration in esp_phy_enable() runs.
//   * setup() starts this DMA stream (rx.init) BEFORE WiFi, and the first
//     parallelTask tick then starts the AP.  If the PHY takes the SAR while a
//     digital-controller conversion is in flight, that conversion never
//     completes, the DMA descriptor never sees its EOF, and the conv-done
//     ISR never fires again.  readRssi() then repeats lastDmaRaw forever —
//     a real reading, taken before WiFi started, now frozen.
//
// Intermittent because it depends on where in a conversion the PHY lands;
// reboot-fixable because the next boot rolls the dice again.  The sequence
// power-refcounting in sar_periph_ctrl.c does NOT prevent it: that only keeps
// the SAR powered, it does not sequence two controllers' conversions.
//
// The mechanism is inferred from source, not observed on a scope.  The fix
// does not depend on it being exactly right: re-arming the stream after any
// WiFi bring-up is safe and cheap (~1 ms) whatever disturbed it.
//
// Why a stop/start on the existing handle and not deinit/reinit:
// adc_continuous_stop() releases the digital controller and adc_continuous_
// start() fully re-initialises it (adc_hal_set_controller, adc_hal_digi_init)
// and re-arms the DMA — the API is designed for exactly this.  Deinit would
// also free and re-allocate the GDMA channel and the ring, for nothing.
//
// Why deferring the initial start until after WiFi was NOT chosen: the timer
// would have to sample via analogRead() until then, and the oneshot and DMA
// paths return counts on different scales (see RSSI_DMA_GAIN_NUM).  The
// median filter would see a 1.9x step at the switch-over.
//
// lastDmaRaw is deliberately NOT cleared here.  During the ~1 ms gap readRssi()
// finds an empty peak and holds the previous sample, which is the designed
// behaviour; zeroing it would inject a false trough into the median filter.
//
// TASK AFFINITY — the part that caused a boot loop the first time round.
//
// adc_continuous_start() acquires the ADC1 unit lock, a FreeRTOS mutex, and
// holds it until adc_continuous_stop() releases it.  FreeRTOS asserts if a
// mutex is released by any task other than its holder (xTaskPriorityDisinherit,
// "pxTCB == pxCurrentTCBs[0]").  The holder is whichever task started the
// stream — loopTask, because init() runs from setup().  So the stop/start
// below MUST execute on loopTask, and the WiFi event (arduino_events task)
// may only ASK for it.  requestRearm() sets a flag; serviceRearm() does the
// work from loop().  Observed 2026-09-20: the first version did the restart
// directly in the event handler and every unit rebooted on WiFi start.
//
// Concurrency of the work itself: serviceRearm() runs on the same task as
// readRssi(), so the two never overlap.  The only other party is the ISR,
// which adc_dma_stop() disables for the duration.
//
// ── Flash writes and the stall watchdog ─────────────────────────────────────
//
// THE SECOND FLATLINE.  Observed 2026-09-20: four or five config saves in a
// row and the RSSI trace froze, on a unit that had booted clean.  Cause, from
// the IDF sources: every flash erase/program runs with the instruction cache
// off, so loopTask stops and every non-IRAM ISR is masked — and the Arduino
// core does not set CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE, so that includes the
// ADC driver's ISR.  A sector erase takes ~50-100 ms; the DMA ring holds
// 6.4 ms.  When it fills, the GDMA owner check halts the channel with
// RX_DESC_ERROR.  adc_continuous registers only on_recv_eof, never
// on_descr_err, so nothing restarts it: readRssi() repeats lastDmaRaw
// until reboot.  Not new — as old as DMA mode itself — just never provoked.
//
// Two layers, sharing this one re-arm path:
//
//   1. Every flash writer asks for a re-arm when it finishes
//      (Storage::notifyFlashWrite → requestRearm).  Deterministic, no
//      detection wait, covers the writes we can name.
//   2. The watchdog in serviceRearm() covers the ones we cannot: if the
//      ISR's frame counter has not moved for ADC_DMA_STALL_MS, request a
//      re-arm.  After a flash write the counter's age already includes the
//      write, so it fires on the first loop() back.  The value, not the
//      counter, is what a disconnected receiver freezes — so a flat RX5808
//      never trips this.
//
// The gap is the write itself plus ~1 ms: nothing can sample while the
// cache is off.  A gate pass sits above threshold for 150-400 ms, so the
// worst case is a peak timestamp shifted by up to the gap, not a lost lap.
void RX5808::requestRearm(const char* reason) {
    if (adcMode != ADC_MODE_DMA || !adcHandle) return;
    rearmReason  = reason ? reason : "";
    rearmPending = true;
}

void RX5808::serviceRearm() {
    if (adcMode != ADC_MODE_DMA || !adcHandle) return;
    const uint32_t nowMs = millis();

    // Stall watchdog.  Reads the ISR's counter once; a plain 32-bit load is
    // atomic on RISC-V so no critical section is needed.
    const uint32_t frames = dma.frames;
    if (frames != dmaLastFrames) {
        dmaLastFrames  = frames;
        dmaLastFrameMs = nowMs;
        dmaStallRearms = 0;                 // stream is alive — reset the strike count
    } else if (!rearmPending && !rxPoweredDown &&
               (nowMs - dmaLastFrameMs) >= ADC_DMA_STALL_MS) {
        if (dmaStallRearms < ADC_DMA_STALL_MAX_REARMS) {
            dmaStallRearms++;
            dmaStallRecoveries++;
            // Uptime stamp, same format as the MULTINODE log — a timer has no
            // RTC, so this is the only stamp comparable across the log.
            const uint32_t upSec = nowMs / 1000;
            DEBUG("[ADC] %02u:%02u:%02u DMA stalled (no frame for %u ms) — re-arming (#%u of %u)\n",
                  (unsigned)(upSec / 3600), (unsigned)((upSec % 3600) / 60), (unsigned)(upSec % 60),
                  (unsigned)(nowMs - dmaLastFrameMs),
                  (unsigned)dmaStallRearms, (unsigned)ADC_DMA_STALL_MAX_REARMS);
            requestRearm("DMA stall");
        }
        // At the cap: stop retrying.  The last re-arm re-opened the variance
        // window; the held value reads flat and rssiInputFlat() reports it.
        // A stream that keeps failing is a hardware fault, and retrying would
        // only keep resetting that window and hide the verdict.
    }

    if (!rearmPending) return;
    rearmPending = false;
    adc_continuous_handle_t handle = (adc_continuous_handle_t)adcHandle;

    // Start is attempted even if stop reports "already stopped": that is the
    // state a wedged controller may well be in, and start is what revives it.
    const esp_err_t s = adc_continuous_stop(handle);
    const esp_err_t r = adc_continuous_start(handle);
    if (r != ESP_OK) {
        DEBUG("[ADC] DMA restart (%s) FAILED — stop=%d start=%d; RSSI feed may be dead\n",
              rearmReason, (int)s, (int)r);
    } else {
        DEBUG("[ADC] DMA re-armed after %s\n", rearmReason);
    }

    // Give the restarted stream a fresh ADC_DMA_STALL_MS before the watchdog
    // may judge it; the first frame lands ~1.6 ms from now.
    dmaLastFrameMs = nowMs;

    // The liveness verdict has to describe the feed AFTER the event, not
    // before: a window that closed earlier would say "alive" about a stream
    // this very event may just have killed, and then stay silent about it.
    resetBootCheck();
}

void RX5808::resetBootCheck() {
    bootCheckDone  = false;
    bootCheckEndMs = 0;      // 0 = the window re-opens on the next readRssi()
    bootCheckMin   = 0xFFFF; // raw counts
    bootCheckMax   = 0;
    bootFlat       = false;
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

    // If we recently set a frequency, wait out the tune time and release the bus.
    //
    // NOTHING IS VERIFIED HERE, deliberately.  A verifyFrequency() call used to
    // sit on this line, reading register 0x01 back over SPI.  These modules do
    // not drive the SPI data line, so it read a floating pin: 7680 on every
    // boot of every unit, which decodes to 4319 MHz — not a frequency at all,
    // let alone the one we set.  It could not pass, and a check that always
    // fails is worse than none, because it would mask a real fault.
    //
    // The receiver is confirmed alive by the RSSI variance check instead (see
    // updateBootVarianceCheck): a live module's analog output always jitters,
    // a dead or disconnected one is flat.  That measures the signal path the
    // timer actually depends on, and it is the ONLY such check by design —
    // two of them means two things to troubleshoot when one goes wrong.
    if (recentSetFreqFlag) {
        const uint32_t dt = currentTimeMs - lastSetFreqTimeMs;
        if (dt > RX5808_MIN_TUNETIME + 100) {
            DEBUG("RX5808 Tune done: %u\n", currentFrequency);
            settingFrequency = false;
            recentSetFreqFlag = false;  // don't need to check again until next freq change
            // Do NOT update lastSetFreqTimeMs here; it is used as the write timestamp
        }
    }
}


// verifyFrequency() lived here.  Removed 2026-09-20 — it read register 0x01
// back over SPI, but these modules do not drive the data line, so it was
// sampling a floating pin: 7680 on every boot of every unit, which decodes to
// 4319 MHz.  Never passed, could never pass.  See handleFrequencyChange() for
// why the RSSI variance check replaces it rather than anything being added
// here.  freqMhzToRegVal() stays — setFrequency() still needs it to WRITE.

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

// ── DMA path scale correction ───────────────────────────────────────────────
//
// The oneshot driver (analogRead) and the continuous/DMA driver do NOT return
// the same raw counts for the same input voltage on ESP32-C6, despite both
// being configured for 12-bit at ADC_ATTEN_DB_6.  Measured with a DC sweep
// through a fixed divider on real hardware:
//
//     polled : rssi = 0.888 * dac + 4.6      (RSSI_SCALE_MAX = 2400)
//     DMA    : rssi = 1.706 * dac + 10.4     (same scale)   → 1.92x higher
//
// Left uncorrected this has two consequences, both bad:
//
//   1. readRssi() saturates at 255 once raw passes 2400 — which the DMA path
//      reaches at only ~60% of the input range, while the ADC itself still has
//      most of its 12-bit span left.  That is the "clipping" seen on the bench.
//   2. More seriously, enterRssi/exitRssi calibrated in one mode are wrong by
//      ~2x in the other, so toggling adcMode silently invalidates a user's
//      calibration.
//
// Scaling the DMA path by the measured ratio makes both modes report the same
// RSSI for the same voltage, so thresholds transfer between them.
//
// TO RE-DERIVE (if a future chip or IDF version shifts this): run
// tools/timing-harness "Check Analog Path" in each mode and take the ratio of
// the reported slopes; that is exactly what this constant is.
#define RSSI_DMA_GAIN_NUM  1921UL   // measured DMA/polled slope ratio x1000
#define RSSI_DMA_GAIN_DEN  1000UL
#define RSSI_SCALE_MAX_DMA (RSSI_SCALE_MAX * RSSI_DMA_GAIN_NUM / RSSI_DMA_GAIN_DEN)

// One-shot liveness verdict: did RSSI move at all in the first few seconds?
//
// The only failure signature a disconnected or failing RX5808 has is a FLAT
// signal.  Which level it sticks at depends on how it died — pinned to a rail,
// parked mid-scale, or a frozen sample repeated by a stopped ADC feed — so the
// level is not diagnostic and the variance is.  One check, all causes.
//
// Runs on both ADC paths: a pinned input looks the same sampled by DMA or by
// analogRead.  Ends permanently once the window closes, so the steady-state
// cost is the single `bootCheckDone` test in readRssi().
//
// MEASURED ON THE RAW ADC VALUE, NOT THE SCALED 0-255 ONE, and that matters.
// The scale maps 0..RSSI_SCALE_MAX_DMA (4610) onto 0..255, so one display count
// is ~18 raw counts: sub-18-count movement is invisible after scaling.  In a
// NOISY environment that is harmless — measured spreads there ran 14-77 display
// counts — but a race site is far quieter, and the whole point of this check is
// that it must not cry wolf on a healthy receiver in a quiet field.  At raw
// resolution the ADC's own noise on a steady input (tens of counts) guarantees
// movement regardless of the RF environment, while the fault this exists to
// catch — a frozen feed repeating lastDmaRaw — is still bit-identical and
// therefore still caught exactly.
void RX5808::updateBootVarianceCheck(uint16_t raw) {
    const uint32_t now = millis();

    // First call opens the window.  Started here rather than in init() because
    // the module needs its first tune to settle, and readRssi() returns 0
    // throughout that — a window opened earlier would spend its first samples
    // on the tuning gap and could call a healthy receiver flat.
    if (bootCheckEndMs == 0) {
        bootCheckEndMs = now + RSSI_BOOT_CHECK_MS;
        bootCheckMin   = raw;
        bootCheckMax   = raw;
        return;
    }

    if (raw < bootCheckMin) bootCheckMin = raw;
    if (raw > bootCheckMax) bootCheckMax = raw;

    if (now < bootCheckEndMs) return;

    bootCheckDone = true;
    bootFlat      = (bootCheckMin == bootCheckMax);
    if (bootFlat) {
        DEBUG("[ADC] RSSI FLAT at raw %u for %u ms — receiver disconnected or "
              "failed; lap detection cannot work\n",
              (unsigned)bootCheckMax, (unsigned)RSSI_BOOT_CHECK_MS);
    } else {
        // Spread is printed so the margin against the fail threshold (zero) is
        // visible at a glance in any environment, not just inferred.
        DEBUG("[ADC] RSSI input alive (raw %u-%u, spread %u, over %u ms)\n",
              (unsigned)bootCheckMin, (unsigned)bootCheckMax,
              (unsigned)(bootCheckMax - bootCheckMin),
              (unsigned)RSSI_BOOT_CHECK_MS);
    }
}

uint8_t RX5808::readRssi() {
    if (recentSetFreqFlag) return 0; // RSSI unstable immediately after tune

    uint16_t raw;
    uint32_t scaleMax;

    if (adcMode == ADC_MODE_DMA) {
        // Read-and-clear the running peak in one atomic operation.  A plain
        // read-then-zero would race the ISR: a frame completing between the
        // two would have its peak discarded.  __atomic_exchange_n is a single
        // instruction on RISC-V and needs no critical section.
        raw = (uint16_t)__atomic_exchange_n(&dma.peak, 0, __ATOMIC_RELAXED);

        // Zero means no frame completed since the last read — possible when
        // loop() runs faster than the ~625 Hz frame rate.
        //
        // This must NOT fall back to analogRead(): the oneshot driver returns
        // counts on a different scale (see RSSI_SCALE_MAX_DMA above), so
        // mixing one in would inject a value ~1.9x too low — a false trough,
        // arriving precisely when the median filter is least able to reject
        // it.  Hold the previous DMA sample instead; at worst that repeats a
        // value for one tick, which the median absorbs harmlessly.
        //
        // A feed that stops ALTOGETHER freezes this value until the stall
        // watchdog in serviceRearm() restarts the stream (~100 ms).  If three
        // restarts cannot revive it, the held value is a flat signal and the
        // variance check below reports it — the same check that catches a
        // receiver whose output is pinned while the ADC keeps sampling
        // happily.  Both faults look identical on the wire, so they share one
        // detector.
        if (raw == 0) raw = lastDmaRaw;
        else          lastDmaRaw = raw;

        scaleMax = RSSI_SCALE_MAX_DMA;
    } else {
        raw = (uint16_t)analogRead(rssiInputPin);
        scaleMax = RSSI_SCALE_MAX;
    }

    // Published for callers measuring variance at full ADC resolution.
    lastRawUsed = raw;

    const uint8_t scaled = (raw >= scaleMax)
                           ? 255
                           : (uint8_t)((raw * 255UL) / scaleMax);

    // RAW, not scaled — see updateBootVarianceCheck().  One-shot, and only
    // while the module is actually powered: a deliberate power-down parks RSSI
    // at a constant by design and is not a fault.
    if (!bootCheckDone && !rxPoweredDown) updateBootVarianceCheck(raw);

    return scaled;
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
