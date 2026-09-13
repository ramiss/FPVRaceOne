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
    // Measures the ambient RSSI noise floor at the CONFIGURED frequency and
    // reports how close it sits to the Enter/Exit thresholds lap detection
    // compares against.  Replaces testRX5808() and testRX5808SpiMode(), which
    // both swept the band and both drew conclusions the data could not support
    // — see the comment on the definition for the measurements that retired
    // them.  Deliberately makes no claim about SPI programming: without
    // register readback there is no honest way to test it.
    TestResult testRX5808Noise(RX5808* rx5808, Config* config, LapTimer* timer);
    TestResult testLapTimer(LapTimer* timer);
    TestResult testAudio(Buzzer* buzzer);
    TestResult testConfig(Config* config);
    TestResult testRaceHistory(RaceHistory* history);
    TestResult testWebServer();
    // Reports the worst gap between consecutive RSSI samples over the last
    // completed window.  This is the DIRECT evidence that the timer is not
    // late: it bounds detection jitter and bounds the risk of a narrow
    // fast-pass peak landing entirely between two samples.  Requires
    // TIMING_STATS_ENABLED; reports "not compiled in" otherwise.
    TestResult testTimingJitter(LapTimer* timer, RX5808* rx5808);
    // Reports per-FreeRTOS-task CPU load and idle headroom.  Proves the chip
    // is not saturated hosting web + AP + polling + race logic concurrently.
    // NOTE: this proves headroom, NOT punctuality — a task blocked on socket
    // I/O burns no cycles, so low load can coexist with a long stall.  Read
    // it alongside testTimingJitter(), never instead of it.
    TestResult testCpuLoad();
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
