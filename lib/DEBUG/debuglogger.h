#pragma once

#include <Arduino.h>
#include <vector>

#define DEBUG_BUFFER_SIZE 100

class DebugLogger {
public:
    struct LogEntry {
        unsigned long timestamp;
        char message[256];
    };
    
    static DebugLogger& getInstance() {
        static DebugLogger instance;
        return instance;
    }
    
    void log(const char* format, ...) {
        va_list args;
        va_start(args, format);
        
        LogEntry entry;
        entry.timestamp = millis();
        vsnprintf(entry.message, sizeof(entry.message), format, args);
        
        va_end(args);
        
        // Add to ring buffer
        if (buffer.size() >= DEBUG_BUFFER_SIZE) {
            buffer.erase(buffer.begin());
        }
        buffer.push_back(entry);
        
        // Also print to serial
        Serial.printf("[%lu] %s", entry.timestamp, entry.message);
    }
    
    const std::vector<LogEntry>& getBuffer() const {
        return buffer;
    }
    
    void clear() {
        buffer.clear();
    }

    // Actually release the std::vector's backing storage back to the heap.
    // clear() only shrinks size, not capacity — the 100-entry × 260-byte
    // backing allocation stays put.  releaseBuffer() swaps with an empty
    // temporary, so the old buffer is destroyed and its memory returned to
    // the allocator.  Used by OTA before TLS handshake: mbedTLS needs ~16 KB
    // contiguous, and on a fragmented master-mode heap the freed log-buffer
    // block is often exactly what unblocks the handshake.  The buffer
    // regrows naturally as future DEBUG() calls push entries.
    void releaseBuffer() {
        std::vector<LogEntry> empty;
        buffer.swap(empty);   // empty goes out of scope at end and frees the old storage
    }
    
private:
    DebugLogger() {}
    std::vector<LogEntry> buffer;
};

// Redefine DEBUG macro to use logger
#undef DEBUG
#define DEBUG(...) DebugLogger::getInstance().log(__VA_ARGS__)
