#ifndef SELFTEST_H
#define SELFTEST_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// Forward declarations
class Config;
class Storage;
class RX5808;
class LapTimer;
class Buzzer;
class RaceHistory;

#ifdef ESP32S3
class RgbLed;
#endif

struct TestResult {
    String name;
    bool passed;
    String details;
    uint32_t duration_ms;
};

class SelfTest {
   public:
    SelfTest();
    void init(Storage* stor);
    
    // Run all tests
    bool runAllTests();
    
    // Individual tests
    TestResult testStorage();
    TestResult testSDCard();
    TestResult testLittleFS();
    TestResult testEEPROM();
    TestResult testWiFi();
    TestResult testBattery();
    TestResult testRX5808(RX5808* rx5808);
    // Confirms the RX5808 module is wired in SPI (register-programming) mode
    // rather than the legacy manual mode.  Writes two different frequencies
    // and reads them back via the module's register-read command — a match
    // proves the SPI programming path is live.  A failure here means either
    // the module's MODE pin is strapped to the wrong state, the SPI wiring
    // is broken, or the module itself is defective.
    TestResult testRX5808SpiMode(RX5808* rx5808);
    TestResult testLapTimer(LapTimer* timer);
    TestResult testAudio(Buzzer* buzzer);
    TestResult testConfig(Config* config);
    TestResult testRaceHistory(RaceHistory* history);
    TestResult testWebServer();
    TestResult testOTA();
    TestResult testWebhooks();
    TestResult testTransport();
    
#ifdef ESP32S3
    TestResult testRGBLED(RgbLed* rgbLed);
    TestResult testUSB();
#endif
    
    // Get results
    String getResultsJSON();
    bool allTestsPassed() const { return allPassed; }
    
   private:
    Storage* storage;
    std::vector<TestResult> results;
    bool allPassed;
};

#endif
