#include "storage.h"
#include "debug.h"
#include <FS.h>

Storage::Storage() : sdAvailable(false) {
#ifdef ESP32S3
    spi = nullptr;
#endif
}

bool Storage::init() {
    DEBUG("Initializing storage...\n");
    
    // SD card init deferred to after boot to prevent watchdog timeout
    sdAvailable = false;
    DEBUG("Storage: Using LittleFS (SD card will be initialized after boot)\n");
    return true;
}

bool Storage::initSDDeferred() {
    DEBUG("Attempting deferred SD card initialization...\n");
    
#ifdef ESP32S3
    if (sdAvailable) {
        DEBUG("SD card already initialized\n");
        return true;
    }
    
    uint32_t startTime = millis();
    bool success = initSD();
    uint32_t duration = millis() - startTime;
    
    if (success) {
        sdAvailable = true;
        DEBUG("SD card initialized successfully (took %dms)\n", duration);
        return true;
    } else {
        DEBUG("SD card init failed after %dms\n", duration);
        return false;
    }
#else
    DEBUG("SD card not supported on this platform\n");
    return false;
#endif
}

#ifdef ESP32S3
bool Storage::initSD() {
    DEBUG("\n=== SD Card Initialization ===\n");
    DEBUG("Pin Configuration:\n");
    DEBUG("  CS   = GPIO %d\n", PIN_SD_CS);
    DEBUG("  SCK  = GPIO %d\n", PIN_SD_SCK);
    DEBUG("  MOSI = GPIO %d\n", PIN_SD_MOSI);
    DEBUG("  MISO = GPIO %d\n", PIN_SD_MISO);
    
    // Initialize CS pin
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    
    // Create custom SPI bus for SD card
    spi = new SPIClass(HSPI);
    DEBUG("SPI bus object created\n");
    
    // Begin SPI bus with correct pin order: SCK, MISO, MOSI, SS
    spi->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    DEBUG("SPI bus initialized\n");
    
    // Try to initialize SD card at 10MHz (safe for short wires)
    DEBUG("Attempting SD.begin() at 10MHz...\n");
    if (!SD.begin(PIN_SD_CS, *spi, 10000000)) {
        DEBUG("ERROR: SD.begin() failed!\n");
        DEBUG("Possible causes:\n");
        DEBUG("  1. Card not inserted\n");
        DEBUG("  2. Card not FAT32 formatted\n");
        DEBUG("  3. Loose wiring\n");
        DEBUG("  4. Incompatible card\n");
        DEBUG("  5. Insufficient power\n");
        spi->end();
        delete spi;
        spi = nullptr;
        return false;
    }
    
    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        DEBUG("No SD card attached\n");
        SD.end();
        spi->end();
        delete spi;
        spi = nullptr;
        return false;
    }
    
    DEBUG("✅ SD card mounted successfully\n");
    DEBUG("SD Card Type: ");
    if (cardType == CARD_MMC) {
        DEBUG("MMC\n");
    } else if (cardType == CARD_SD) {
        DEBUG("SDSC\n");
    } else if (cardType == CARD_SDHC) {
        DEBUG("SDHC/SDXC\n");
    } else {
        DEBUG("UNKNOWN\n");
    }
    
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    DEBUG("SD Card Size: %lluMB\n", cardSize);
    
    uint64_t usedBytes = SD.usedBytes() / (1024 * 1024);
    DEBUG("SD Card Used: %lluMB\n", usedBytes);
    
    return true;
}
#endif

bool Storage::writeFile(const String& path, const String& data) {
    DEBUG("Storage: Writing to %s (%d bytes)\n", path.c_str(), data.length());
    
#ifdef ESP32S3
    if (sdAvailable) {
        File file = SD.open(path, FILE_WRITE);
        if (!file) {
            DEBUG("Failed to open file on SD: %s\n", path.c_str());
            return false;
        }
        size_t written = file.print(data);
        file.close();
        return written > 0;
    }
#endif
    
    // Fall back to LittleFS
    File file = LittleFS.open(path, "w");
    if (!file) {
        DEBUG("Failed to open file on LittleFS: %s\n", path.c_str());
        return false;
    }
    size_t written = file.print(data);
    file.close();
    return written > 0;
}

bool Storage::readFile(const String& path, String& data) {
#ifdef ESP32S3
    if (sdAvailable) {
        if (!SD.exists(path)) {
            return false;
        }
        File file = SD.open(path, FILE_READ);
        if (!file) {
            DEBUG("Failed to open file on SD: %s\n", path.c_str());
            return false;
        }
        data = file.readString();
        file.close();
        return true;
    }
#endif
    
    // Fall back to LittleFS
    if (!LittleFS.exists(path)) {
        return false;
    }
    File file = LittleFS.open(path, "r");
    if (!file) {
        DEBUG("Failed to open file on LittleFS: %s\n", path.c_str());
        return false;
    }
    data = file.readString();
    file.close();
    return true;
}

bool Storage::deleteFile(const String& path) {
#ifdef ESP32S3
    if (sdAvailable) {
        return SD.remove(path);
    }
#endif
    return LittleFS.remove(path);
}

bool Storage::exists(const String& path) {
#ifdef ESP32S3
    if (sdAvailable) {
        return SD.exists(path);
    }
#endif
    return LittleFS.exists(path);
}

bool Storage::mkdir(const String& path) {
#ifdef ESP32S3
    if (sdAvailable) {
        return SD.mkdir(path);
    }
#endif
    return LittleFS.mkdir(path);
}

bool Storage::listDir(const String& path, std::vector<String>& files) {
    files.clear();
    
#ifdef ESP32S3
    if (sdAvailable) {
        File root = SD.open(path);
        if (!root || !root.isDirectory()) {
            return false;
        }
        
        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                files.push_back(String(file.name()));
            }
            file = root.openNextFile();
        }
        return true;
    }
#endif
    
    // LittleFS
    File root = LittleFS.open(path);
    if (!root || !root.isDirectory()) {
        return false;
    }
    
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            files.push_back(String(file.name()));
        }
        file = root.openNextFile();
    }
    return true;
}

uint64_t Storage::getTotalBytes() {
#ifdef ESP32S3
    if (sdAvailable) {
        return SD.cardSize();
    }
#endif
    return LittleFS.totalBytes();
}

uint64_t Storage::getUsedBytes() {
#ifdef ESP32S3
    if (sdAvailable) {
        return SD.usedBytes();
    }
#endif
    return LittleFS.usedBytes();
}

uint64_t Storage::getFreeBytes() {
    return getTotalBytes() - getUsedBytes();
}

bool Storage::migrateSoundsToSD() {
#ifdef ESP32S3
    if (!sdAvailable) {
        DEBUG("SD card not available, cannot migrate sounds\n");
        return false;
    }
    
    // Check if sounds already exist on SD
    if (SD.exists("/sounds")) {
        DEBUG("Sounds directory already exists on SD card\n");
        return true;
    }
    
    // Check if sounds exist in LittleFS
    if (!LittleFS.exists("/sounds")) {
        DEBUG("No sounds directory in LittleFS to migrate\n");
        return false;
    }
    
    DEBUG("\n=== Migrating sounds to SD card ===\n");
    
    // Create sounds directory on SD
    if (!SD.mkdir("/sounds")) {
        DEBUG("Failed to create /sounds directory on SD\n");
        return false;
    }
    
    // Copy all files from LittleFS /sounds to SD /sounds
    bool success = copyDirectory("/sounds", "/sounds", false);
    
    if (success) {
        DEBUG("✅ Migration complete!\n");
        DEBUG("You can now delete /sounds from LittleFS to free space\n");
    } else {
        DEBUG("❌ Migration failed\n");
    }
    
    return success;
#else
    return false;
#endif
}

bool Storage::copyDirectory(const String& srcPath, const String& dstPath, bool deleteSource) {
    DEBUG("Copying %s to SD card...\n", srcPath.c_str());
    
    File srcDir = LittleFS.open(srcPath);
    if (!srcDir || !srcDir.isDirectory()) {
        DEBUG("Failed to open source directory: %s\n", srcPath.c_str());
        return false;
    }
    
    int fileCount = 0;
    int errorCount = 0;
    uint32_t totalBytes = 0;
    
    File file = srcDir.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());
            String dstFilePath = dstPath + "/" + fileName.substring(fileName.lastIndexOf('/') + 1);
            
            DEBUG("  Copying: %s -> %s", fileName.c_str(), dstFilePath.c_str());
            
            // Open destination file on SD
#ifdef ESP32S3
            File dstFile = SD.open(dstFilePath, FILE_WRITE);
            if (!dstFile) {
                DEBUG(" [FAIL - can't open dest]\n");
                errorCount++;
                file = srcDir.openNextFile();
                continue;
            }
            
            // Copy data in chunks
            uint8_t buffer[512];
            size_t fileSize = file.size();
            size_t bytesRead;
            
            while ((bytesRead = file.read(buffer, sizeof(buffer))) > 0) {
                dstFile.write(buffer, bytesRead);
            }
            
            dstFile.close();
            totalBytes += fileSize;
            fileCount++;
            
            DEBUG(" [OK - %d bytes]\n", fileSize);
            
            // Delete source file if requested
            if (deleteSource) {
                LittleFS.remove(fileName);
                DEBUG("  Deleted source file\n");
            }
#endif
        }
        file = srcDir.openNextFile();
    }
    
    srcDir.close();
    
    DEBUG("\nCopied %d files (%d KB) with %d errors\n", 
          fileCount, totalBytes / 1024, errorCount);
    
    return errorCount == 0;
}
