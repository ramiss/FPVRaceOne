#pragma once
#include <Arduino.h>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
//  Lap Sync Protocol v1.1 — shared types and constants
//
//  Both LapTimer (the client's own laps) and MultiNodeManager (the master's
//  aggregate, and each client's peer cache) maintain the same two derived
//  values as laps are appended:
//
//    - a rolling CRC32 over the lap sequence, which IS the state digest
//    - a RaceSummary, which keeps totals exact after ring eviction
//
//  Both are O(1) per lap and neither ever walks the history.  Keeping the
//  fold in one place is what guarantees a client and the master compute the
//  same digest for the same laps — two implementations would drift the first
//  time either side changed a field.
// ─────────────────────────────────────────────────────────────────────────

// Retained laps per node, both sides.  Was 50 in two separate places that had
// to agree; now there is one number.
//
// Cost: 200 laps x 12 B = 2.4 KB per node on the master (16.8 KB for seven),
// 1.6 KB on a client for its own ring.  Paid for several times over by
// removing the lap arrays from the directorState payload, which scaled at
// 40 B per lap per node and was rebuilt four times a second.
#define LAPSYNC_MAX_LAPS 200

// §6.1 — the unacked window drains at this rate, oldest first.  An uncapped
// report would dump ~3.6 KB in one POST after a long disconnect, precisely
// when the master is also handling re-registration.
#define LAPSYNC_MAX_UNACKED_PER_REPORT 10

// §6.5 — lap deltas carried by one directorState broadcast.  Seven pilots on
// 3 s laps produce 4-5 per 2 s window, so this cap is headroom, not a normal
// path.  Overflow is detected by the recipient's digest compare and is NOT
// repaired: peer data is a cache (§3).
#define LAPSYNC_MAX_LAP_DELTAS 32

// §7 — resync governor.
#define LAPSYNC_RESYNC_COOLDOWN_MS 30000   // per node, since last completed resync
#define LAPSYNC_QUIET_GATE_MS        500   // no lap handled within this window
#define LAPSYNC_ESCAPE_VALVE_MS    60000   // force a grant after this long in NEEDED
#define LAPSYNC_CHUNK_RACING           5   // laps per chunk while a race is running
#define LAPSYNC_CHUNK_IDLE            25   // laps per chunk when idle
#define LAPSYNC_CHUNK_ESCAPE           1   // laps per chunk when the valve fired
#define LAPSYNC_VERIFY_ATTEMPTS        2   // failed verifies before DIVERGENT

// A lap as it travels and as it is stored.  12 bytes — the same size as the
// struct it replaces, because the 32-bit seq and the real timestamp are paid
// for out of padding that already existed.
struct LapSyncRecord {
    uint32_t lapTimeMs;       // client-measured duration, stored verbatim, never recomputed
    uint32_t raceElapsedMs;   // client's own race clock at the crossing
    uint32_t seq;             // monotonic within (raceId, nodeId)
};

// Totals that must stay exact after the ring evicts.  ~20 bytes, folded once
// per lap, never recomputed from the (windowed) array.
struct RaceSummary {
    uint16_t lapTotal;          // every crossing, including seq 0
    uint32_t fastestLapMs;      // 0 = none yet
    uint16_t fastestLapNumber;
    uint32_t slowestLapMs;
    uint32_t sumLapMs;          // seq >= 1 only, so mean stays exact
    uint16_t timedLapCount;     // laps folded into sum/fastest/slowest
};

// Bitwise CRC32 (IEEE, reflected) — no table, no ROM header.
//
// A table would cost 1 KB of RAM or flash to save time we do not need: this
// runs 12 bytes per lap, roughly once every three seconds, so ~96 iterations
// against a 3000 ms budget.  Staying table-free keeps the fold identical
// across every target without depending on esp_rom headers that move between
// IDF versions.
static inline uint32_t lapSyncCrc32Update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (uint8_t i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

// Fold one lap into the rolling digest.
//
// The fold covers ALL history and is never unfolded on eviction, which is
// what makes it window-independent: two devices that witnessed the same 250
// laps agree even though each stores only the last 200.
//
// The consequence, and it is load-bearing: a device that restores a windowed
// copy CANNOT recompute this value, because it never saw the evicted laps.
// A restored CRC must be ADOPTED verbatim from the resync response (§4).
static inline uint32_t lapSyncFoldLap(uint32_t crc, const LapSyncRecord &lap) {
    crc = lapSyncCrc32Update(crc, &lap.seq,           sizeof(lap.seq));
    crc = lapSyncCrc32Update(crc, &lap.lapTimeMs,     sizeof(lap.lapTimeMs));
    crc = lapSyncCrc32Update(crc, &lap.raceElapsedMs, sizeof(lap.raceElapsedMs));
    return crc;
}

static inline void lapSyncSummaryReset(RaceSummary &s) {
    s.lapTotal         = 0;
    s.fastestLapMs     = 0;
    s.fastestLapNumber = 0;
    s.slowestLapMs     = 0;
    s.sumLapMs         = 0;
    s.timedLapCount    = 0;
}

// Fold one lap into the summary.
//
// seq 0 is the gate-1 crossing, not a lap.  The browser already excludes it
// when highlighting the fastest lap ("Start from 1 to skip gate 1",
// script.js), and getRetainedLap documents lap numbering the same way.  A
// summary that folded it would report a different fastest lap than the table
// rendered directly beside it.
static inline void lapSyncSummaryFold(RaceSummary &s, const LapSyncRecord &lap) {
    if (s.lapTotal < 0xFFFF) s.lapTotal++;

    if (lap.seq == 0) return;          // gate-1 crossing — counted, not timed
    if (lap.lapTimeMs == 0) return;    // guard: a zero duration is not a measurement

    if (s.fastestLapMs == 0 || lap.lapTimeMs < s.fastestLapMs) {
        s.fastestLapMs     = lap.lapTimeMs;
        s.fastestLapNumber = (uint16_t)lap.seq;
    }
    if (lap.lapTimeMs > s.slowestLapMs) s.slowestLapMs = lap.lapTimeMs;

    s.sumLapMs += lap.lapTimeMs;
    if (s.timedLapCount < 0xFFFF) s.timedLapCount++;
}

// Mean of timed laps, or 0 when none. Integer division is intentional —
// callers display whole milliseconds.
static inline uint32_t lapSyncSummaryMeanMs(const RaceSummary &s) {
    return s.timedLapCount ? (s.sumLapMs / s.timedLapCount) : 0u;
}

// ─────────────────────────────────────────────────────────────────────────
//  Bounded append-only lap ring
//
//  Replaces the master's `push_back` + `erase(begin())` pattern, which
//  memmoved the whole array on every lap past the cap, for every node, and
//  never released the capacity it had grown.  A write cursor costs nothing
//  and does not move existing elements.
//
//  Storage is one heap block PER NODE rather than a fixed array inside
//  NodeInfo.  Seven fixed arrays would make the NodeInfo vector a single
//  ~17 KB contiguous allocation, and this device fails on fragmentation
//  (maxBlk), not on total usage — seven independent 2.4 KB blocks are far
//  easier to place than one 17 KB block.
//
//  Allocation is lazy: a master with no laps yet holds nothing.
// ─────────────────────────────────────────────────────────────────────────
struct LapRing {
    std::vector<LapSyncRecord> buf;   // empty until first append
    uint16_t    write   = 0;          // ring cursor
    // Records ACTUALLY written since the last reset, saturating at
    // LAPSYNC_MAX_LAPS.  This is deliberately NOT derived from `total`.
    //
    // `total` is set from the arriving lap's seq, which is authoritative but
    // says nothing about how many records this ring holds.  A ring whose first
    // append carries seq 61 — the normal shape after a master reboots mid-race
    // and the client resumes from its last ack — has total 62 and exactly one
    // record.  Deriving retained() from total then reported 62, and get()
    // served 61 zeroed slots as though they were laps: phantom 0 ms entries in
    // the UI, in resync chunks, and in the legacy arrays.
    uint16_t    stored  = 0;
    uint32_t    total   = 0;          // monotonic; also the seq of the NEXT lap
    uint32_t    crc     = 0;          // rolling digest over ALL laps (§4)
    RaceSummary summary = {};

    void reset() {
        write  = 0;
        stored = 0;
        total  = 0;
        crc    = 0;
        lapSyncSummaryReset(summary);
        // Keep the allocation: a race stop followed by a race start should not
        // hand 2.4 KB back to the allocator only to ask for it again seconds
        // later.  Same reasoning as _mnPayloadBuf and _directorStatePayload.
        for (size_t i = 0; i < buf.size(); i++) buf[i] = LapSyncRecord{0, 0, 0};
    }

    // Returns false if the ring could not be allocated — caller should drop
    // the lap rather than crash.  On a device this close to its heap floor,
    // an allocation failure is a real branch, not a theoretical one.
    bool ensure() {
        if (buf.size() == (size_t)LAPSYNC_MAX_LAPS) return true;
        buf.resize(LAPSYNC_MAX_LAPS);
        return buf.size() == (size_t)LAPSYNC_MAX_LAPS;
    }

    uint16_t retained() const { return stored; }

    // Laps below this are RETIRED, not missing.  A peer must never be asked
    // to resend them and must never interpret their absence as loss (§6.1).
    //
    // total - retained() is the base seq only because appends are contiguous:
    // handleLapBatch rejects any lap that does not continue the sequence, and
    // a restore is served in order from the window floor.  That contiguity is
    // the invariant this expression rests on — do not relax the gap check
    // without revisiting it.
    uint32_t oldestSeq() const {
        const uint16_t r = retained();
        return (total > r) ? (total - r) : 0u;
    }

    // Chronological read, oldest first.  Fills the record's true seq, which
    // is not the ring index once the ring has wrapped.
    bool get(uint16_t index, LapSyncRecord *out) const {
        const uint16_t r = retained();
        if (index >= r || !out || buf.empty()) return false;
        const uint16_t slot = (uint16_t)((write + LAPSYNC_MAX_LAPS - r + index) % LAPSYNC_MAX_LAPS);
        *out = buf[slot];
        return true;
    }

    // Append at the ring cursor and fold both derived values.  `lap.seq` is
    // authoritative — it comes from the owner of the data, never from a local
    // counter, so a replayed or out-of-order arrival keeps its identity.
    bool append(const LapSyncRecord &lap) {
        if (!ensure()) return false;
        buf[write] = lap;
        write = (uint16_t)((write + 1) % LAPSYNC_MAX_LAPS);
        if (stored < LAPSYNC_MAX_LAPS) stored++;
        if (lap.seq + 1 > total) total = lap.seq + 1;
        crc = lapSyncFoldLap(crc, lap);
        lapSyncSummaryFold(summary, lap);
        return true;
    }

    // Adopt digest + summary from the owner of the data (§4).  Required after
    // a restore: a device holding a windowed copy never saw the evicted laps
    // and so cannot fold its way to the correct digest.
    void adopt(uint32_t adoptedCrc, const RaceSummary &adoptedSummary) {
        crc     = adoptedCrc;
        summary = adoptedSummary;
    }
};
