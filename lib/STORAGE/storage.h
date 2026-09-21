#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <vector>

#ifdef ESP32S3
#include <SD.h>
#include <SPI.h>
#endif

#include <LittleFS.h>

class Storage {
   public:
    Storage();
    bool init();
    bool initSDDeferred();  // Initialize SD card after boot
    bool isSDAvailable() const { return sdAvailable; }
    
    // File operations - automatically use SD if available, fall back to LittleFS
    bool writeFile(const String& path, const String& data);
    bool readFile(const String& path, String& data);
    bool deleteFile(const String& path);
    bool exists(const String& path);
    bool mkdir(const String& path);
    bool listDir(const String& path, std::vector<String>& files);
    
    // Storage info
    uint64_t getTotalBytes();
    uint64_t getUsedBytes();
    uint64_t getFreeBytes();
    String getStorageType() const { return sdAvailable ? "SD" : "LittleFS"; }

    // Flash-write notification.  Every flash erase/program runs with the
    // instruction cache off: loopTask freezes, the ADC DMA ring fills in
    // ~6 ms and the controller halts (the ADC ISR is not IRAM-safe in the
    // Arduino core, so the driver never sees the descriptor error).  The RX5808
    // needs a re-arm after every such write.  Rather than teach CONFIG and
    // STORAGE about the receiver, they call notifyFlashWrite() and main.cpp
    // registers what that means.  Static so Config::write() can call it
    // without holding a Storage pointer.
    static void setFlashWriteNotify(void (*fn)()) { flashWriteNotify = fn; }
    static void notifyFlashWrite() { if (flashWriteNotify) flashWriteNotify(); }

    // Migration helpers
    bool migrateSoundsToSD();
    bool copyDirectory(const String& srcPath, const String& dstPath, bool deleteSource = false);
    
   private:
    bool sdAvailable;
    static void (*flashWriteNotify)();

#ifdef ESP32S3
    bool initSD();
    SPIClass* spi;
#endif
};

#endif
