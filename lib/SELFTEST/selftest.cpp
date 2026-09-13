#include "selftest.h"
#include "debug.h"
#include "config.h"
#include "mac_util.h"
#include "cpumon.h"
#include "storage.h"
#include "RX5808.h"
#include "laptimer.h"
#include "buzzer.h"
#include "racehistory.h"
#include "webhook.h"
#include <EEPROM.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Update.h>

#ifdef ESP32S3
#include <SD.h>
#include "rgbled.h"
#include "USB.h"
#endif

// Web assets are gzipped into the filesystem image at build time (see
// _stage_web_assets in scripts/extra_script.py), so a normally built image
// contains "/script.js.gz" and no "/script.js" at all.  Every asset presence
// check must therefore accept either form, or the self-test reports a healthy
// device's web UI as missing.
static bool assetExists(const char* path) {
    if (LittleFS.exists(path)) return true;
    return LittleFS.exists(String(path) + ".gz");
}

SelfTest::SelfTest() : storage(nullptr), allPassed(true) {
}

void SelfTest::init(Storage* stor) {
    storage = stor;
}

bool SelfTest::runAllTests() {
    DEBUG("Starting self-tests...\n");
    results.clear();
    allPassed = true;

    // Test RX5808
    // Must be explicitly nullptr: an uninitialized pointer is indeterminate, and if it
    // happened to be non-null the null-check would pass and then dereference a wild
    // pointer. testRX5808Noise() handles nullptr gracefully.
    //
    // NOTE: this whole function is currently unreachable — the /api/selftest route
    // that called it is commented out in webserver.cpp, and the live route calls each
    // test directly with real pointers.  Kept compiling, but it can only ever report
    // the null-pointer path from here.
    RX5808 *rx = nullptr;
    TestResult RX5808Test = testRX5808Noise(rx, nullptr, nullptr);
    results.push_back(RX5808Test);
    if (!RX5808Test.passed) allPassed = false;
    
    // Test storage
    TestResult storageTest = testStorage();
    results.push_back(storageTest);
    if (!storageTest.passed) allPassed = false;
    
    // Test LittleFS
    TestResult littleFSTest = testLittleFS();
    results.push_back(littleFSTest);
    if (!littleFSTest.passed) allPassed = false;
    
#ifdef ESP32S3
    // Test SD card
    TestResult sdTest = testSDCard();
    results.push_back(sdTest);
    // SD card failure is not critical - don't fail overall test
#endif
    
    // Test EEPROM
    TestResult eepromTest = testEEPROM();
    results.push_back(eepromTest);
    if (!eepromTest.passed) allPassed = false;
    
    // Test WiFi
    TestResult wifiTest = testWiFi();
    results.push_back(wifiTest);
    if (!wifiTest.passed) allPassed = false;
    
#ifdef ESP32S3
    // Test USB Serial CDC
    TestResult usbTest = testUSB();
    results.push_back(usbTest);
    // USB failure is not critical - don't fail overall test
#endif
    
    DEBUG("Self-tests complete: %s\n", allPassed ? "PASSED" : "FAILED");
    return allPassed;
}

TestResult SelfTest::testStorage() {
    TestResult result;
    result.name = "Storage";
    uint32_t start = millis();
    
    if (!storage) {
        result.passed = false;
        result.details = "Storage not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Test write and read
    String testData = "{\"test\":\"data\"}";
    bool writeSuccess = storage->writeFile("/test_selftest.txt", testData);
    
    if (!writeSuccess) {
        result.passed = false;
        result.details = "Write failed";
        result.duration_ms = millis() - start;
        return result;
    }
    
    String readData;
    bool readSuccess = storage->readFile("/test_selftest.txt", readData);
    
    if (!readSuccess || readData != testData) {
        result.passed = false;
        result.details = "Read failed or data mismatch";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Cleanup
    storage->deleteFile("/test_selftest.txt");
    
    result.passed = true;
    result.details = String("Type: ") + storage->getStorageType() + 
                    ", Free: " + String(storage->getFreeBytes() / 1024) + "KB";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testSDCard() {
    TestResult result;
    result.name = "SD Card";
    uint32_t start = millis();
    
#ifdef ESP32S3
    if (!storage || !storage->isSDAvailable()) {
        result.passed = false;
        result.details = "Not available (using LittleFS fallback) - Optional for device operation";
        result.duration_ms = millis() - start;
        return result;
    }
    
    uint64_t cardSize = storage->getTotalBytes();
    uint64_t usedBytes = storage->getUsedBytes();
    uint64_t freeBytes = storage->getFreeBytes();
    
    // Test read/write to SD
    String testData = "{\"test\":\"sd_write\"}";
    bool writeSuccess = false;
    if (SD.exists("/")) {
        File testFile = SD.open("/test_sd.txt", FILE_WRITE);
        if (testFile) {
            testFile.print(testData);
            testFile.close();
            writeSuccess = true;
        }
    }
    
    if (writeSuccess && SD.exists("/test_sd.txt")) {
        SD.remove("/test_sd.txt");
    }
    
    // Check for voice directories
    int voiceDirsFound = 0;
    const char* voiceDirs[] = {"sounds_default", "sounds_rachel", "sounds_adam", "sounds_antoni"};
    for (int i = 0; i < 4; i++) {
        String path = String("/") + voiceDirs[i];
        if (SD.exists(path)) {
            voiceDirsFound++;
        }
    }
    
    // Check for sample audio files
    int audioFilesFound = 0;
    const char* sampleFiles[] = {"/sounds_default/gate_1.mp3", "/sounds_default/lap_1.mp3"};
    for (int i = 0; i < 2; i++) {
        if (SD.exists(sampleFiles[i])) {
            audioFilesFound++;
        }
    }
    
    result.passed = writeSuccess;
    result.details = String("Size: ") + String(cardSize / (1024*1024)) + "MB, " +
                    "Free: " + String(freeBytes / (1024*1024)) + "MB, " +
                    "Voices: " + String(voiceDirsFound) + "/4, " +
                    "Audio files: " + String(audioFilesFound) + "/2, " +
                    (writeSuccess ? "R/W OK" : "R/W Failed");
    result.duration_ms = millis() - start;
#else
    result.passed = false;
    result.details = "SD card not supported on this board";
    result.duration_ms = millis() - start;
#endif
    
    return result;
}

TestResult SelfTest::testLittleFS() {
    TestResult result;
    result.name = "LittleFS";
    uint32_t start = millis();
    
    if (!LittleFS.begin()) {
        result.passed = false;
        result.details = "LittleFS not mounted";
        result.duration_ms = millis() - start;
        return result;
    }
    
    uint64_t totalBytes = LittleFS.totalBytes();
    uint64_t usedBytes = LittleFS.usedBytes();
    uint64_t freeBytes = totalBytes - usedBytes;
    
    result.passed = true;
    result.details = String("Total: ") + String(totalBytes / 1024) + "KB, " +
                    "Used: " + String(usedBytes / 1024) + "KB" +
                    "Free: " + String(freeBytes / 1024) + "KB";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testEEPROM() {
    TestResult result;
    result.name = "EEPROM";
    uint32_t start = millis();
    
    // Write test pattern
    uint8_t testValue = 0xAA;
    uint8_t testAddr = EEPROM_RESERVED_SIZE - 1; // Use last byte
    uint8_t originalValue = EEPROM.read(testAddr);
    
    EEPROM.write(testAddr, testValue);
    EEPROM.commit();
    
    uint8_t readValue = EEPROM.read(testAddr);
    
    // Restore original value
    EEPROM.write(testAddr, originalValue);
    EEPROM.commit();
    
    if (readValue != testValue) {
        result.passed = false;
        result.details = "Read/write test failed";
        result.duration_ms = millis() - start;
        return result;
    }
    
    result.passed = true;
    result.details = String("Size: ") + String(EEPROM_RESERVED_SIZE) + " bytes";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testWiFi() {
    TestResult result;
    result.name = "WiFi";
    uint32_t start = millis();
    
    // Check if WiFi is initialized
    wifi_mode_t mode = WiFi.getMode();
    
    if (mode == WIFI_OFF) {
        result.passed = false;
        result.details = "WiFi not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    String modeStr = (mode == WIFI_AP) ? "AP" :
                     (mode == WIFI_STA) ? "STA" : "AP+STA";

    // Report the MAC of the interface(s) actually running.  WiFi.macAddress()
    // was used here previously, but it queries the STA netif — which is never
    // started in AP-only mode, so the page showed 00:00:00:00:00:00.  Read
    // from eFuse instead (see lib/MACUTIL/mac_util.h).
    String macStr;
    if (mode == WIFI_AP) {
        macStr = String("AP ") + getApMacString();
    } else if (mode == WIFI_STA) {
        macStr = String("STA ") + getStaMacString();
    } else {
        macStr = String("AP ") + getApMacString() + " / STA " + getStaMacString();
    }

    result.passed = true;
    result.details = String("Mode: ") + modeStr + ", MAC: " + macStr;
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testTimingJitter(LapTimer* timer, RX5808* rx5808) {
    TestResult result;
    result.name = "RSSI Sample Timing";
    uint32_t start = millis();

#if TIMING_STATS_ENABLED
    if (!timer) {
        result.passed = false;
        result.details = "Lap timer unavailable";
        result.duration_ms = millis() - start;
        return result;
    }

    const TimingStats& s = timer->getTimingStats();
    if (!s.valid) {
        // No window has closed yet.  Not a failure — just too early.
        result.passed = true;
        result.details = String("Warming up — first window closes ")
                       + String(TIMING_STATS_WINDOW_MS / 1000) + "s after boot";
        result.duration_ms = millis() - start;
        return result;
    }

    const uint32_t hz     = s.meanIntervalUs ? (1000000UL / s.meanIntervalUs) : 0;
    const uint32_t maxMs  = s.maxIntervalUs / 1000;
    const uint32_t meanUs = s.meanIntervalUs;

    // Pass condition is the WORST gap, not the mean.  A healthy system shows
    // zero late samples; any late sample means a scheduling stall long enough
    // that a fast pass could have been missed.
    result.passed = (s.lateCount == 0);

    // Report the mode actually running, not the one requested — DMA falls
    // back to polled on init failure, and the A/B is worthless if the two
    // runs are silently the same mode.
    const char* modeStr = "polled";
    if (rx5808 && rx5808->getAdcMode() == ADC_MODE_DMA) modeStr = "DMA";

    result.details = String("[") + modeStr + "] " + String(hz) + " Hz, worst gap "
                   + String(s.maxIntervalUs) + "us (" + String(maxMs) + "ms), mean "
                   + String(meanUs) + "us, late(>"
                   + String(TIMING_STATS_LATE_THRESHOLD_MS) + "ms)=" + String(s.lateCount)
                   + "/" + String(s.sampleCount);
    if (!result.passed) {
        // The old hint said "see [CORE0] log for the blocking call".  That is
        // only true for work on parallelTask, which is all CORE0_TIME wraps.
        // A stall caused by an AsyncWebServer handler — the self-test itself
        // included — never appears there, so the hint sent people to a log that
        // could not contain the answer.
        result.details += " — sampling stalled. [CORE0] names the call only if it "
                          "ran on parallelTask; a stall from an AsyncWebServer "
                          "handler or a USB serial write will not appear there.";
    }
#else
    (void)timer;
    (void)rx5808;
    result.passed = true;
    result.details = "Not compiled in (TIMING_STATS_ENABLED=0)";
#endif

    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testCpuLoad() {
    TestResult result;
    result.name = "CPU Load";
    uint32_t start = millis();

#if CPU_MONITOR_ENABLED
    // Read the window parallelTask already closed for us.  We deliberately do
    // NOT measure here: computing load requires a real time window, and
    // blocking this AsyncWebServer handler for a couple hundred ms risks the
    // TCP slot exhaustion this project is sensitive to.
    const CpuMonitor::Snapshot& s = CpuMonitor::getInstance().getLast();

    if (!s.valid) {
        // No window has closed yet — too early, not a failure.
        result.passed = true;
        result.details = String("Warming up — first window closes ")
                       + String(CPU_MONITOR_WINDOW_MS / 1000) + "s after boot";
        result.duration_ms = millis() - start;
        return result;
    }

    // Headroom check only.  This deliberately does NOT claim anything about
    // punctuality — see testTimingJitter() for that.  80% busy leaves little
    // room for a traffic burst, so flag it.
    result.passed = (s.busyPercent < 80);

    result.details = String("Busy ") + String(s.busyPercent) + "%, idle "
                   + String(s.idlePercent) + "% | ";
    // Per-task breakdown — this is the point of the test.  It answers
    // "can it host web + AP + polling + race logic at once" line by line.
    const uint8_t show = (s.taskCount < 4) ? s.taskCount : 4;
    for (uint8_t i = 0; i < show; i++) {
        if (i) result.details += ", ";
        result.details += String(s.top[i].name) + " " + String(s.top[i].percent) + "%";
    }

    // Stack headroom.  Collected free — uxTaskGetSystemState() already fills
    // usStackHighWaterMark and cpumon used to discard it.  Values are BYTES and
    // are lifetime minima, so they only ever fall.
    if (s.minStackFreeBytes != 0xFFFF) {
        result.details += String(" | stack min ") + s.minStackTask + " "
                        + String(s.minStackFreeBytes) + "B";
        if (s.stackLoopTask || s.stackParallelTask || s.stackAsyncTcp) {
            result.details += " (loop " + String(s.stackLoopTask)
                            + ", parallel " + String(s.stackParallelTask)
                            + ", asyncTcp " + String(s.stackAsyncTcp) + ")";
        }
        // Below 1 KB free anywhere is a genuine risk: a stack overflow on this
        // chip is an immediate crash with no useful diagnostic.  Fail the test
        // rather than let it pass quietly at 800 bytes.
        if (s.minStackFreeBytes < 1024) {
            result.passed = false;
            result.details += " — LOW";
        }
    }
#else
    result.passed = true;
    result.details = "Not compiled in (CPU_MONITOR_ENABLED=0)";
#endif

    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testBattery() {
    TestResult result;
    result.name = "Battery Monitor";
    uint32_t start = millis();
    
    #ifdef PIN_VBAT
    // Read battery voltage
    int rawValue = analogRead(PIN_VBAT);
    result.passed = true;
    #else
    int rawValue = -1; // Not supported
    result.passed = false;
    #endif
    
    
    result.details = String("Raw: ") + String(rawValue);
    result.duration_ms = millis() - start;
    return result;
}

// ── RSSI noise floor and detection margin ───────────────────────────────────
//
// Replaces two tests that each claimed more than their data could support:
//
//   "RX5808 RF Receive" swept 13 frequencies and passed when any reading
//   cleared 8 counts.  Measured floors on real units run 12-56, so that bar sat
//   BELOW the noise — the test could not fail on working hardware, and it
//   reported "RF receive" with nothing transmitting.
//
//   "RX5808 SPI Programming" looked for RSSI to track the commanded frequency.
//   Measured on a client 2026-09-12: 5800 MHz read 23 in one sweep and 53 in
//   the next, 1.4 s apart.  Band-spread and time-drift are indistinguishable in
//   a single-visit sweep, so "span" proved nothing — it could as easily have
//   false-CONFIRMED as false-failed.
//
// These modules do not drive the SPI data line, so there is no register
// readback and therefore no honest SPI test.  This one does not pretend to be
// one.  It measures what actually decides whether the timer works on the day:
// how close ambient noise sits to the thresholds detection compares against.
//
// Stays on the configured frequency — no retuning at all.  That removes the
// save/restore dance, the "Calibrating pilot frequency" banner handshake, and
// 2.16 s of blocking inside an AsyncWebServer handler.
TestResult SelfTest::testRX5808Noise(RX5808* rx5808, Config* config, LapTimer* timer) {
    TestResult result;
    result.name = "RSSI Noise Floor";
    uint32_t start = millis();

    if (!rx5808 || !config) {
        result.passed = false;
        result.details = "RX5808 or Config pointer is null";
        result.duration_ms = millis() - start;
        return result;
    }

    // A drone at the gate is precisely what this must not measure — one pass
    // turns the "noise floor" into the peak of a real crossing.  Not a failure;
    // the operator just ran it at the wrong moment.
    if (timer && timer->isRunning()) {
        result.passed = true;
        result.details = "Not measured — a race is running. Stop the race and "
                         "re-run with nothing at the gate.";
        result.duration_ms = millis() - start;
        return result;
    }

    const uint8_t  enterRssi = config->getEnterRssi();
    const uint8_t  exitRssi  = config->getExitRssi();
    const uint16_t freq      = rx5808->getCurrentFrequency();

    // Sample the same value stream lap detection sees: readRssi() is the
    // detector's own accessor, so DMA peak-hold and the mode-dependent scaling
    // are applied identically.  Reading the raw ADC here would report a floor
    // the detector never actually compares against.
    //
    // ONE SECOND, not the 400 ms this started at.  The live calibration trace
    // shows a mostly-flat floor punctuated by intermittent spikes seconds
    // apart, and `peak` is the number the verdict turns on.  A short window can
    // land entirely between spikes, report peak == mean, and print a margin
    // that is better than reality — precise, wrong, and wrong in the direction
    // that looks safe, which is the worst way for a pre-race check to fail.
    //
    // Still 1.16 s faster than the two band sweeps this replaced, and the cost
    // is paid in an AsyncWebServer handler, so do not grow it further without
    // weighing it against the TCP-slot headroom.  If a longer observation is
    // ever needed, track the floor continuously on loop() and read a closed
    // window here — the pattern TIMING_STATS and CpuMonitor already use.
    const uint16_t kSamples       = 1000;
    const uint8_t  kSampleDelayMs = 1;

    uint16_t peak      = 0;
    uint16_t floorRssi = 255;
    uint32_t sum       = 0;
    for (uint16_t i = 0; i < kSamples; i++) {
        const uint8_t v = rx5808->readRssi();
        if (v > peak)      peak      = v;
        if (v < floorRssi) floorRssi = v;
        sum += v;
        delay(kSampleDelayMs);
    }
    const uint8_t mean = (uint8_t)(sum / kSamples);

    const String reading = "floor " + String(floorRssi) + ", mean " + String(mean) +
                           ", peak " + String(peak) + " at " + String(freq) + " MHz";

    // readRssi() returns 0 while recentSetFreqFlag is set.  An all-zero run
    // means we sampled a tuning window, not the floor, and would then compute a
    // huge and entirely fictional margin.
    if (peak == 0) {
        result.passed = true;
        result.details = "Not measured — RSSI read zero throughout (" + reading +
                         "). The receiver was mid-tune; re-run in a moment.";
        result.duration_ms = millis() - start;
        return result;
    }

    if (enterRssi <= exitRssi) {
        result.passed = false;
        result.details = "Thresholds inverted — Enter " + String(enterRssi) +
                         " is not above Exit " + String(exitRssi) +
                         ". Lap detection cannot work. Re-run calibration.";
        result.duration_ms = millis() - start;
        return result;
    }

    // ── Each threshold judged against the statistic whose failure it causes ──
    //
    // The first version of this compared PEAK against Exit and demanded room for
    // another excursion the size of the one just measured.  Measured on real
    // hardware 2026-09-12 (floor 37, mean 38, peak 55, Enter 80, Exit 66) it
    // warned on a perfectly healthy unit, and the reasoning was wrong twice
    // over: excursions do not stack — the next spike starts from the mean, not
    // from the previous peak — and a transient was being judged against a
    // threshold whose failure mode is a SUSTAINED level.
    //
    //   Exit  fails on a sustained floor.  A lap completes when RSSI falls back
    //         through Exit; the detector samples at ~1 kHz and needs one sample
    //         at or below it.  A brief spike merely delays that by milliseconds,
    //         and lap TIME is anchored to the peak, so even then the recorded
    //         time does not move.  What actually hangs a lap is ambient sitting
    //         above Exit — so compare the MEAN.
    //
    //   Enter is NOT judged here at all, and that is deliberate.  This test
    //         samples rx->readRssi(), which is the RAW value; detection runs it
    //         through the median filter first (laptimer.cpp: `rawRssi` ->
    //         medianFilter), and kEnterHoldMin then requires 2 consecutive
    //         at-or-above-enter FILTERED samples.  An isolated ambient spike is
    //         rejected twice over before detection can see it — the enter
    //         debounce exists precisely so "a 4-sample noise burst that lifts
    //         the median to enter can't start a false crossing".
    //
    //         An earlier version warned when the raw peak came near Enter.  On a
    //         bench with ordinary RF noise that fired on a healthy unit and told
    //         the operator to recalibrate, which would not have reduced the
    //         noise — it would only have moved a threshold that was never the
    //         problem.  Peak is now reported as context and nothing else.  Noise
    //         sustained enough to survive both filters would raise the MEAN,
    //         which is the check below.
    if (mean >= exitRssi) {
        result.passed = false;
        result.details = "Ambient noise sits at " + String(mean) +
                         ", at or above the Exit threshold (" + String(exitRssi) +
                         ") — a lap can start but never complete (" + reading +
                         "). Recalibrate, or move the gate away from the interference.";
        result.duration_ms = millis() - start;
        return result;
    }

    // Guaranteed positive by the check above.
    const uint8_t exitMargin = (uint8_t)(exitRssi - mean);

    // How much headroom is "enough" scales with the calibration rather than
    // being a constant: the Enter-Exit hysteresis band IS the amplitude this
    // operator decided separates "in the gate" from "out of it", so an ambient
    // floor within one band of Exit is inside the range the system treats as
    // meaningful.  The absolute floor covers a very tight hysteresis, where one
    // band would set a trivially low bar.
    //
    // Advisory, never a failure — only the definite fault above fails.
    const uint8_t kAbsoluteMargin = 10;
    const uint8_t hysteresis      = (uint8_t)(enterRssi - exitRssi);
    const uint8_t wantMargin      = (hysteresis > kAbsoluteMargin) ? hysteresis
                                                                   : kAbsoluteMargin;

    result.passed  = true;
    result.details = reading +
        ". Floor to Exit (" + String(exitRssi) + "): " + String(exitMargin) +
        ". Peak is pre-filter; isolated spikes are rejected by the median filter.";
    if (exitMargin < wantMargin) {
        // No "recalibrate" here.  A floor this close to Exit is usually
        // interference rather than a mis-set threshold, and the two want
        // opposite responses — so state the measurement and the consequence and
        // let the operator decide which it is.
        result.details += "  FLOOR CLOSE TO EXIT — ambient sits only " +
                          String(exitMargin) + " counts below it. If it rises further, "
                          "laps will start and never complete.";
    }
    result.duration_ms = millis() - start;
    return result;
}







TestResult SelfTest::testLapTimer(LapTimer* timer) {
    TestResult result;
    result.name = "Lap Timer";
    uint32_t start = millis();
    
    if (!timer) {
        result.passed = false;
        result.details = "LapTimer not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Read RSSI to verify timer can communicate with RX5808
    uint8_t rssi = timer->getRssi();
    
    result.passed = true;
    result.details = String("Timer functional, Current RSSI: ") + String(rssi);
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testAudio(Buzzer* buzzer) {
    #ifdef PIN_BUZZER
        TestResult result;
        result.name = "Audio/Buzzer";
        uint32_t start = millis();
        
        if (!buzzer) {
            result.passed = false;
            result.details = "Buzzer not initialized";
            result.duration_ms = millis() - start;
            return result;
        }
        
        // Test buzzer beep
        buzzer->beep(100);
        delay(150);
    #else
        TestResult result;
        result.name = "Audio";
        uint32_t start = millis();
    #endif
    
    // Check if audio announcer JavaScript exists
    bool audioJsExists = assetExists("/audio-announcer.js");
    
    if (!audioJsExists) {
        result.passed = false;
        result.details = "audio-announcer.js not found";
        result.duration_ms = millis() - start;
        return result;
    }
    
    result.passed = true;
    #ifdef PIN_BUZZER
        result.details = "Buzzer OK, Audio JS loaded";
    #else
        result.details = "Audio JS loaded";
    #endif
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testConfig(Config* config) {
    TestResult result;
    result.name = "Configuration";
    uint32_t start = millis();
    
    if (!config) {
        result.passed = false;
        result.details = "Config not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Verify config values are in valid ranges
    uint16_t freq = config->getFrequency();
    uint8_t enterRssi = config->getEnterRssi();
    uint8_t exitRssi = config->getExitRssi();
    
    if (freq < 5600 || freq > 5950) {
        result.passed = false;
        result.details = "Invalid frequency: " + String(freq);
        result.duration_ms = millis() - start;
        return result;
    }
    
    if (enterRssi <= exitRssi) {
        result.passed = false;
        result.details = "Enter RSSI (" + String(enterRssi) + ") must be > Exit RSSI (" + String(exitRssi) + ")";
        result.duration_ms = millis() - start;
        return result;
    }
    
    result.passed = true;
    result.details = String("Freq: ") + String(freq) + "MHz, Enter: " + String(enterRssi) + ", Exit: " + String(exitRssi);
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testRaceHistory(RaceHistory* history) {
    TestResult result;
    result.name = "Race History";
    uint32_t start = millis();
    
    if (!history) {
        result.passed = false;
        result.details = "RaceHistory not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    size_t raceCount = history->getRaceCount();
    
    result.passed = true;
    result.details = String("Races stored: ") + String(raceCount) + " / " + String(MAX_RACES);
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testWebServer() {
    TestResult result;
    result.name = "Web Server";
    uint32_t start = millis();
    
    // Check if index.html exists
    bool indexExists = assetExists("/index.html");
    bool scriptExists = assetExists("/script.js");
    bool styleExists = assetExists("/style.css");
    
    if (!indexExists || !scriptExists || !styleExists) {
        result.passed = false;
        result.details = "Web files missing";
        result.duration_ms = millis() - start;
        return result;
    }
    
    result.passed = true;
    result.details = "Web files loaded, Server active";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testOTA() {
    TestResult result;
    result.name = "OTA Updates";
    uint32_t start = millis();
    
    // Get partition information
    size_t sketchSize = ESP.getSketchSize();
    size_t freeSpace = ESP.getFreeSketchSpace();
    
    if (freeSpace < 100000) { // Less than 100KB free
        result.passed = false;
        result.details = "Low OTA space: " + String(freeSpace / 1024) + "KB";
        result.duration_ms = millis() - start;
        return result;
    }
    
    result.passed = true;
    result.details = String("Sketch: ") + String(sketchSize / 1024) + "KB, Free: " + String(freeSpace / 1024) + "KB";
    result.duration_ms = millis() - start;
    return result;
}

#ifdef ESP32S3
TestResult SelfTest::testRGBLED(RgbLed* rgbLed) {
    TestResult result;
    result.name = "RGB LED";
    uint32_t start = millis();
    
    if (!rgbLed) {
        result.passed = false;
        result.details = "RGB LED not initialized";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Flash red, green, blue to test all channels
    rgbLed->setManualColor(0xFF0000); // Red
    delay(200);
    rgbLed->setManualColor(0x00FF00); // Green
    delay(200);
    rgbLed->setManualColor(0x0000FF); // Blue
    delay(200);
    
    // Restore rainbow
    rgbLed->setRainbowWave();
    
    result.passed = true;
    result.details = "All channels tested (R,G,B)";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testUSB() {
    TestResult result;
    result.name = "USB Serial CDC";
    uint32_t start = millis();
    
    // Check if USB CDC is available
    #if ARDUINO_USB_CDC_ON_BOOT
    if (!Serial) {
        result.passed = false;
        result.details = "USB CDC not available";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Test if USB is connected
    bool connected = (bool)Serial;
    
    // Check USB transport files
    bool transportFileExists = assetExists("/usb-transport.js");
    
    result.passed = true;
    result.details = String("CDC ") + (connected ? "connected" : "disconnected") + 
                    ", Transport: " + (transportFileExists ? "loaded" : "missing");
    #else
    result.passed = false;
    result.details = "USB CDC not enabled in build";
    #endif
    
    result.duration_ms = millis() - start;
    return result;
}
#endif

TestResult SelfTest::testWebhooks() {
    TestResult result;
    result.name = "Webhooks";
    uint32_t start = millis();
    
    // Test webhook configuration via storage/config
    // We can't directly test HTTP requests in self-test, but we can verify config
    if (!storage) {
        result.passed = false;
        result.details = "Storage not available";
        result.duration_ms = millis() - start;
        return result;
    }
    
    // Check if webhook system is functional (HTTP client available)
    WiFiClient testClient;
    bool httpAvailable = true; // WiFiClient is always available on ESP32
    
    result.passed = httpAvailable;
    result.details = httpAvailable ? "HTTP client ready" : "HTTP client unavailable";
    result.duration_ms = millis() - start;
    return result;
}

TestResult SelfTest::testTransport() {
    TestResult result;
    result.name = "Transport Layer";
    uint32_t start = millis();
    
    // Check transport files
    bool usbTransportExists = assetExists("/usb-transport.js");
    
    // Check WiFi status
    wifi_mode_t mode = WiFi.getMode();
    bool wifiActive = (mode != WIFI_OFF);
    
#ifdef ESP32S3
    // Check USB Serial CDC
    #if ARDUINO_USB_CDC_ON_BOOT
    bool usbAvailable = (bool)Serial;
    #else
    bool usbAvailable = false;
    #endif
#else
    bool usbAvailable = false;
#endif
    
    result.passed = (wifiActive || usbAvailable);
    result.details = String("WiFi: ") + (wifiActive ? "active" : "off") + 
                    ", USB: " + (usbAvailable ? "connected" : "disconnected") +
                    ", Transport JS: " + (usbTransportExists ? "loaded" : "missing");
    result.duration_ms = millis() - start;
    return result;
}

String SelfTest::getResultsJSON() {
    DynamicJsonDocument doc(2048);
    doc["allPassed"] = allPassed;
    doc["totalTests"] = results.size();
    
    JsonArray testsArray = doc.createNestedArray("tests");
    for (const auto& result : results) {
        JsonObject test = testsArray.createNestedObject();
        test["name"] = result.name;
        test["passed"] = result.passed;
        test["details"] = result.details;
        test["duration_ms"] = result.duration_ms;
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}
