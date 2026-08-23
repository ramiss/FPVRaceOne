#pragma once

#include <Arduino.h>
#include <vector>

// ── USB serial log output ───────────────────────────────────────────────────
//
// Gates ONLY the Serial.printf in log() below.  The in-memory ring is filled
// either way, so /api/debuglog and the web Serial Monitor keep the complete
// record — turning this off costs no diagnostic the UI can reach, it only stops
// the bytes leaving the USB port.
//
// DEFINED HERE, not in config.h, for two reasons.  log() is an inline function
// in this header, so every translation unit must see the same value or they
// compile different bodies for one function — which means the flag has to live
// in a header everyone including this one already gets.  And config.h cannot be
// that header: it pulls in ArduinoJson/AsyncJson, which the WEBHOOK, STORAGE
// and RACEHISTORY libraries do not carry, so including it here fails to build.
//
// NOT tied to TIMING_MARKER_ENABLED: that flag means "the rig's marker wire is
// on GPIO21".  Bench work without the marker wired is normal, and coupling the
// two would silently kill logging for anyone doing it.
//
// DEFAULTS ON because serial is the only view into a failure that happens
// BEFORE the webserver starts — /api/debuglog cannot report a boot that never
// reaches it. Set to 0 for a silent shipping build.
#ifndef DEBUG_SERIAL_ENABLED
#define DEBUG_SERIAL_ENABLED 1
#endif

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

        // Also print to serial.
        //
        // The ring above is filled FIRST and unconditionally, so /api/debuglog
        // and the web Serial Monitor keep the complete record whether or not
        // this write happens — gating it costs no diagnostic the UI can reach.
        //
        // On this target Serial is HWCDC (ARDUINO_USB_CDC_ON_BOOT=1).  When USB
        // is plugged but nothing is draining the port, its 256-byte TX ring
        // fills and this call blocks; setup() calls Serial.setTxTimeoutMs(1) to
        // bound that to ~2 ms.  Without that bound it is 100 ms, which is what
        // made this very logger stall the sampler it reports on.
#if DEBUG_SERIAL_ENABLED
        Serial.printf("[%lu] %s", entry.timestamp, entry.message);
#endif
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
