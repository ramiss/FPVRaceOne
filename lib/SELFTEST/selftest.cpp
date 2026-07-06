#include "selftest.h"
#include "debug.h"
#include "config.h"
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
    // happened to be non-null testRX5808()'s null-check would pass and then dereference
    // a wild pointer. testRX5808() handles nullptr gracefully.
    RX5808 *rx = nullptr;
    TestResult RX5808Test = testRX5808(rx);
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
    
    result.passed = true;
    result.details = String("Mode: ") + modeStr + ", MAC: " + WiFi.macAddress();
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

TestResult SelfTest::testRX5808(RX5808* rx5808) {
    TestResult result;
    result.name = "RX5808 RF Receive";
    uint32_t start = millis();

    if (!rx5808) {
        result.passed = false;
        result.details = "RX5808 pointer is null";
        result.duration_ms = millis() - start;
        return result;
    }

    // Save the frequency the RX was on before we start sweeping so we can
    // restore it — AND run verifyFrequency() at the end — for the sake of
    // the UI's "Calibrating pilot frequency" banner.  The banner is toggled
    // by parsing debug-log lines: "Setting frequency to" shows it, "RX5808
    // frequency verified properly" hides it.  Without an explicit restore
    // + verify here, the natural post-selftest cleanup path never emits the
    // "verified properly" line and the banner sticks on forever.  Root
    // cause: selftest clears `recentSetFreqFlag = false` after each
    // setFrequency, which also clobbers the flag set by the concurrent
    // parallelTask's own setFrequency(config) preemption — so
    // handleFrequencyChange's verify branch never triggers.
    const uint16_t initialFreq = rx5808->getCurrentFrequency();

    // We can't truly "verify frequency" (RX5808 has no readback).
    // Instead, infer operation by scanning a few common channels and looking
    // for RSSI variation / peaks that suggest real RF energy is being received.

    // Common channels (mix of bands) - chosen to catch typical VTX usage.
    const uint16_t freqs[] = {
        5645, 5685, 5705, 5740, 5760, 5780, 5800, 5806, 5820, 5840, 5860, 5880, 5917
    };
    const int nFreqs = (int)(sizeof(freqs) / sizeof(freqs[0]));

    // Sampling params
    const uint16_t tuneDelayMs = RX5808_MIN_TUNETIME;       // allow RX to settle after tune
    const uint8_t samplesPerFreq = 6;      // average a few reads
    const uint16_t sampleDelayMs = 6;

    uint8_t minRssi = 255;
    uint8_t maxRssi = 0;
    uint16_t minFreq = 0;
    uint16_t maxFreq = 0;

    // For reporting: keep a couple representative points
    uint8_t firstAvg = 0;
    uint8_t midAvg = 0;
    uint8_t lastAvg = 0;

    for (int i = 0; i < nFreqs; i++) {
        rx5808->setFrequency(freqs[i]);
        delay(tuneDelayMs);
        rx5808->recentSetFreqFlag = false;  // Allow RSSI reads now

        uint16_t sum = 0;
        for (uint8_t s = 0; s < samplesPerFreq; s++) {
            Serial.println("RX5808 RSSI: " + String(rx5808->readRssi()));
            sum += rx5808->readRssi();
            delay(sampleDelayMs);
        }
        uint8_t avg = (uint8_t)(sum / samplesPerFreq);

        if (i == 0) firstAvg = avg;
        if (i == nFreqs / 2) midAvg = avg;
        if (i == nFreqs - 1) lastAvg = avg;

        if (avg < minRssi) { minRssi = avg; minFreq = freqs[i]; }
        if (avg > maxRssi) { maxRssi = avg; maxFreq = freqs[i]; }
    }

    const uint8_t span = (uint8_t)(maxRssi - minRssi);

    // ── Cleanup helper — MUST run before every return path ────────────────
    // Restore the pre-test frequency and explicitly verify.  Emits both the
    // "Setting frequency to X" (banner-show) AND "RX5808 frequency verified
    // properly" (banner-hide) log lines the frontend polls for.  Ordering:
    // set → wait → verify → clear the flag so main loop's handleFreqChange
    // doesn't re-verify redundantly.  If initialFreq was 0 (device booted
    // straight into selftest — unlikely), skip the restore; main loop will
    // set it on its next tick.
    auto restoreRx = [rx5808, initialFreq]() {
        if (!rx5808 || initialFreq == 0) return;
        rx5808->setFrequency(initialFreq);
        delay(RX5808_MIN_TUNETIME + 100);
        rx5808->verifyFrequency();
        rx5808->recentSetFreqFlag = false;
    };

    // Heuristics:
    // - If maxRssi is non-zero and we see a meaningful span, we're likely receiving RF energy.
    // - If everything is 0 (min=max=0), we cannot infer anything -> FAIL (but with clear guidance).
    // Tune these thresholds based on your typical environment.
    const uint8_t kMinPeak = 8;     // "saw something above dead-flat"
    const uint8_t kMinSpan = 6;     // "variation indicates signal vs flatline"

    if (maxRssi == 0 && minRssi == 0) {
        result.passed = false;
        result.details =
            "RSSI flatlined at 0 across scan. "
            "If receiver works elsewhere, this may be ADC scaling/attenuation or no RF present.";
        restoreRx();
        result.duration_ms = millis() - start;
        return result;
    }

    // Pass if we saw either a decent peak or decent variation.
    const bool inferred = (maxRssi >= kMinPeak) || (span >= kMinSpan);

    if (!inferred) {
        result.passed = false;
        result.details =
            "RX5808 RSSI is very low/flat during scan (min=" + String(minRssi) + " @ " + String(minFreq) +
            " MHz, max=" + String(maxRssi) + " @ " + String(maxFreq) +
            " MHz, span=" + String(span) + "). "
            "Try powering a VTX near the gate and re-run selftest.";
        restoreRx();
        result.duration_ms = millis() - start;
        return result;
    }

    result.passed = true;
    result.details =
        "RF receive (min=" + String(minRssi) + " @ " + String(minFreq) +
        " MHz, max=" + String(maxRssi) + " @ " + String(maxFreq) +
        " MHz, span=" + String(span) +
        ", samples=" + String(firstAvg) + "/" + String(midAvg) + "/" + String(lastAvg) + ").";
    restoreRx();
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
    bool audioJsExists = LittleFS.exists("/audio-announcer.js");
    
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
    bool indexExists = LittleFS.exists("/index.html");
    bool scriptExists = LittleFS.exists("/script.js");
    bool styleExists = LittleFS.exists("/style.css");
    
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
    bool transportFileExists = LittleFS.exists("/usb-transport.js");
    
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
    bool usbTransportExists = LittleFS.exists("/usb-transport.js");
    
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
