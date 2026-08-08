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
    // Claim the whole ring UP FRONT, at first use, while the heap is still
    // unfragmented.
    //
    // Without this the vector grows geometrically — 1, 2, 4 ... 64, 128 — and
    // LogEntry is 260 bytes, so the final step allocates 128*260 = 33,280
    // bytes WHILE still holding the 64-entry block.  That needs one contiguous
    // 33 KB chunk.
    //
    // Diagnosed 2026-08-08: the master crashed twice at ~40 s uptime with
    // 81-88 KB free heap but maxBlk of only 21,492 and 29,684 — both below
    // 33 KB.  operator new threw std::bad_alloc (the build enables
    // -fexceptions), nothing caught it, and __terminate called abort().  The
    // stack resolved to std::allocator<DebugLogger::LogEntry>::allocate.
    //
    // The 40 s timing is the ~65th log entry: boot emits ~60 lines (7 New
    // node, 8 AP STA connected, init), and the periodic CORE0/HEAP/MULTINODE/
    // TIMING set tips it over exactly as multi-node traffic has fragmented the
    // heap.  Reserving once removes the late large allocation entirely; steady
    // state never reallocates because erase(begin()) + push_back stays within
    // the existing capacity.
    DebugLogger() { buffer.reserve(DEBUG_BUFFER_SIZE); }
    std::vector<LogEntry> buffer;
};

// Redefine DEBUG macro to use logger
#undef DEBUG
#define DEBUG(...) DebugLogger::getInstance().log(__VA_ARGS__)
