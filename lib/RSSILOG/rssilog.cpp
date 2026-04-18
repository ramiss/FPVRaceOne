#include "rssilog.h"
#include <Arduino.h>

bool RssiLogger::init() {
    _spi.begin(LOG_SD_SCK, LOG_SD_MISO, LOG_SD_MOSI, LOG_SD_CS);
    pinMode(LOG_SD_CS, OUTPUT);
    digitalWrite(LOG_SD_CS, HIGH);

    if (!SD.begin(LOG_SD_CS, _spi, 4000000)) {
        Serial.println("[RSSILOG] No SD card detected — RSSI logging disabled");
        return false;
    }

    if (!openNextFile()) {
        Serial.println("[RSSILOG] Failed to open log file");
        return false;
    }

    _ready = true;
    Serial.printf("[RSSILOG] Logging RSSI to %s  (100 Hz)\n", _filename);
    return true;
}

bool RssiLogger::openNextFile() {
    // Find the next available LOGxxxx.CSV (never overwrites existing files)
    for (uint16_t n = 0; n < 9999; n++) {
        snprintf(_filename, sizeof(_filename), "/LOG%04u.CSV", n);
        if (!SD.exists(_filename)) {
            _file = SD.open(_filename, FILE_WRITE);
            if (_file) {
                writeHeader();
                return true;
            }
            return false;
        }
    }
    return false;
}

void RssiLogger::writeHeader() {
    _file.println(
        "ms,"
        "raw,kalman,ma,out,"
        "enter,exit,"
        "state,entered,gateExited,holdSamples,peak,"
        "filterMode,"
        "lapEvent,lapTimeMs,lapCount"
    );
    _file.flush();
}

void RssiLogger::log(const RssiSnapshot& s) {
    if (!_ready) return;

    // Lap events always get logged immediately so they are never skipped by the throttle.
    uint32_t now = millis();
    if (!s.lapEvent && (now - _lastLogMs) < RSSI_LOG_INTERVAL_MS) return;
    _lastLogMs = now;

    char buf[128];
    snprintf(buf, sizeof(buf),
        "%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%u",
        (unsigned long)s.timeMs,
        s.raw, s.kalman, s.ma, s.out,
        s.enterThresh, s.exitThresh,
        s.timerState,
        (uint8_t)s.enteredGate,
        (uint8_t)s.gateExited,
        s.enterHoldSamples,
        s.peak,
        s.filterMode,
        (uint8_t)s.lapEvent,
        (unsigned long)s.lapTimeMs,
        s.lapCount
    );
    _file.println(buf);
    _rowCount++;

    // Flush every 2 seconds — keeps data safe without hammering the bus
    if (now - _lastFlushMs >= 2000) {
        _file.flush();
        _lastFlushMs = now;
    }
}
