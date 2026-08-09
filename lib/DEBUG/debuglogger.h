#pragma once

#include <Arduino.h>
#include <vector>

// Ring depth and per-line length.
//
// Was 100 x 256 = 26,000 bytes, claimed in one contiguous heap block and held
// for the life of the boot — the largest single allocation in the firmware
// after the HTML cache.  Now 40 x 192 = 7,840, returning ~18 KB permanently.
//
// Why 192 and not 160: the longest line the firmware actually emits is the
// 10-second multi-node summary in multinode.cpp — "[MULTINODE] connected:"
// followed by up to seven " X=<pilot name>" pairs.  pilotName is char[21]
// (config.h), so the worst case is 22 + 7*23 = 183 characters.  160 would have
// silently truncated the last two pilots off a line whose entire purpose is
// confirming fleet health.  The extra 32 bytes per entry costs 1,280 bytes
// total, which is a cheap price for not lying in the log.
#define DEBUG_BUFFER_SIZE 40
#define DEBUG_MESSAGE_LEN 192

class DebugLogger {
public:
    struct LogEntry {
        unsigned long timestamp;
        char message[DEBUG_MESSAGE_LEN];
    };

    static DebugLogger& getInstance() {
        static DebugLogger instance;
        return instance;
    }

    // A true ring: fixed capacity, head index, no element ever moves.
    //
    // The previous implementation did buffer.erase(buffer.begin()) once the
    // vector was full, which memmoves the ENTIRE remaining buffer down by one
    // slot — 25,740 bytes shifted on every single log line, on whichever task
    // happened to call DEBUG().  It also built the entry on the stack first, so
    // every logging call carried a 260-byte frame plus vsnprintf's own, raising
    // the floor on all three task stacks at once.
    //
    // Writing straight into the ring slot removes both costs.
    void log(const char* format, ...) {
        // Claim the whole ring UP FRONT, at first use, while the heap is still
        // unfragmented — and re-claim it if releaseBuffer() handed it back.
        //
        // Without this the vector grew geometrically (1, 2, 4 ... 64, 128) and
        // the final step had to allocate a single contiguous block while still
        // holding the previous one.  Diagnosed 2026-08-08: the master crashed
        // twice at ~40 s uptime with 81-88 KB free heap but a largest free
        // block of only 21,492 and 29,684.  operator new threw std::bad_alloc
        // (this build enables -fexceptions), nothing caught it, and
        // __terminate called abort().  One up-front sizing removes the late
        // large allocation entirely.
        if (buffer.size() != DEBUG_BUFFER_SIZE) {
            buffer.resize(DEBUG_BUFFER_SIZE);
            head  = 0;
            count = 0;
        }

        LogEntry& entry = buffer[head];
        entry.timestamp = millis();

        va_list args;
        va_start(args, format);
        vsnprintf(entry.message, sizeof(entry.message), format, args);
        va_end(args);

        head = (head + 1) % DEBUG_BUFFER_SIZE;
        if (count < DEBUG_BUFFER_SIZE) count++;

        // Also print to serial
        Serial.printf("[%lu] %s", entry.timestamp, entry.message);
    }

    // Number of entries currently held (0..DEBUG_BUFFER_SIZE).
    uint16_t entryCount() const { return count; }

    // Oldest-first access.  index 0 is the oldest retained line, so callers
    // iterate 0..entryCount()-1 to get chronological order — the ring's
    // physical order is not chronological once it has wrapped.
    const LogEntry& entryAt(uint16_t index) const {
        const uint16_t start = (count == DEBUG_BUFFER_SIZE) ? head : 0;
        return buffer[(start + index) % DEBUG_BUFFER_SIZE];
    }

    void clear() {
        head  = 0;
        count = 0;
    }

    // Actually release the backing storage back to the heap.  clear() only
    // resets the indices — the DEBUG_BUFFER_SIZE-entry allocation stays put.
    // releaseBuffer() swaps with an empty temporary, so the old buffer is
    // destroyed and its memory returned to the allocator.  Used by OTA before
    // the TLS handshake: mbedTLS needs ~16 KB contiguous, and on a fragmented
    // master-mode heap the freed log ring is often exactly what unblocks it.
    // The ring re-forms on the next DEBUG() call via the resize above.
    void releaseBuffer() {
        std::vector<LogEntry> empty;
        buffer.swap(empty);   // empty goes out of scope here and frees the storage
        head  = 0;
        count = 0;
    }

private:
    DebugLogger() { buffer.reserve(DEBUG_BUFFER_SIZE); }
    std::vector<LogEntry> buffer;
    uint16_t head  = 0;   // next slot to write
    uint16_t count = 0;   // entries currently valid
};

// Redefine DEBUG macro to use logger
#undef DEBUG
#define DEBUG(...) DebugLogger::getInstance().log(__VA_ARGS__)
