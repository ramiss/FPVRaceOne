#ifndef RSSILOG_H
#define RSSILOG_H

// Dedicated RSSI logging SD card — completely independent from main storage.
// Uses its own SPI bus so it never conflicts with anything else.
// Only main.cpp knows this exists; nothing else is touched.
//
// Wiring (Generic ESP32):
//   SD Card Module    ESP32 GPIO
//   VCC           →   3.3V
//   GND           →   GND
//   CS            →   GPIO 15
//   SCK           →   GPIO 14
//   MOSI          →   GPIO 13
//   MISO          →   GPIO 25   ← use 25, NOT 12 (GPIO12 is a boot-strapping pin)

#include <SD.h>
#include <SPI.h>
#include <stdint.h>

#define LOG_SD_CS   15
#define LOG_SD_SCK  14
#define LOG_SD_MOSI 13
#define LOG_SD_MISO 25

// Snapshot of every internal value in one update cycle.
// Defined here so laptimer.h can include this header for the type.
struct RssiSnapshot {
    uint32_t timeMs;
    uint8_t  raw;            // straight off the ADC
    uint8_t  kalman;         // V1: post-Kalman/median; V2: same as out
    uint8_t  ma;             // V1: post-MA; V2: same as out
    uint8_t  out;            // final value used by lap logic
    uint8_t  enterThresh;
    uint8_t  exitThresh;
    uint8_t  peak;           // running peak inside current gate pass
    uint8_t  timerState;     // 0=STOPPED 1=WAITING 2=RUNNING 3=CALIBRATION
    bool     enteredGate;
    bool     gateExited;
    uint8_t  enterHoldSamples;
    uint8_t  filterMode;     // 0=V1 1=V2
    bool     lapEvent;       // true for exactly the one sample that triggered a lap
    uint32_t lapTimeMs;      // valid when lapEvent==true
    uint8_t  lapCount;
};

// Logs are throttled — call log() every loop iteration; it rate-limits internally.
#define RSSI_LOG_INTERVAL_MS 10   // 100 Hz

class RssiLogger {
public:
    bool init();
    void log(const RssiSnapshot& snap);
    bool isReady() const { return _ready; }

private:
    bool        _ready       = false;
    File        _file;
    SPIClass    _spi;
    char        _filename[24];
    uint32_t    _lastLogMs   = 0;
    uint32_t    _rowCount    = 0;
    uint32_t    _lastFlushMs = 0;

    bool openNextFile();
    void writeHeader();
};

#endif // RSSILOG_H
