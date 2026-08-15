#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncUDP.h>
#include <vector>
#include "lapsync.h"

#define MULTINODE_MAX_NODES              7
#define MULTINODE_HEARTBEAT_INTERVAL_MS  2000   // send heartbeat every 2 s (less load on dual-mode WiFi)
#define MULTINODE_HEARTBEAT_FAIL_LIMIT      5   // consecutive failures before declaring disconnected
#define MULTINODE_REGISTER_INTERVAL_MS   5000   // re-register interval when already connected
#define MULTINODE_RECONNECT_INTERVAL_MS   800   // retry interval when disconnected
#define MULTINODE_NODE_TIMEOUT_MS        6000   // mark node offline after 6 s without heartbeat
                                                 // (was 4 s — too tight on top of a 2 s heartbeat;
                                                 //  one missed heartbeat + scheduler jitter would
                                                 //  fire a false timeout right after a legitimate
                                                 //  re-register, then the node would immediately
                                                 //  recover.  6 s gives breathing room for two
                                                 //  missed heartbeats and pushes the cross-core
                                                 //  race window in _checkNodeTimeouts out of reach.)
#define MULTINODE_MASTER_IP              "192.168.5.1"
#define MULTINODE_CLIENT_AP_IP           "192.168.4.1"

// MultiNodeLap is now LapSyncRecord (lapsync.h) — same 12 bytes, but the
// arrival timestamp is replaced by the client's own raceElapsedMs and the
// 8-bit lapNumber by a 32-bit seq.
//
// The old struct stamped `timestamp = millis()` at RECEIPT, so every stored
// lap carried the moment its packet landed — after WiFi queueing and after a
// fanout stall measured at 2.9 s.  Any ordering built on that was ordering by
// network luck.  See §8.
typedef LapSyncRecord MultiNodeLap;

struct NodeInfo {
    uint8_t  nodeId;
    String   pilotName;
    uint32_t pilotColor;
    uint8_t  bandIndex    = 0;
    uint8_t  channelIndex = 0;
    uint16_t frequency    = 0;
    uint8_t  enterRssi    = 0;   // sent by client in registration so the master's Edit Pilot modal can show + edit it
    uint8_t  exitRssi     = 0;
    String   clientIP;   // client's own AP IP (master uses this to push commands)
    String   staIP;      // client's STA IP assigned by master's DHCP
    String   macAddress; // client's WiFi MAC — primary unique key
    String   apSuffix;   // last 6 hex chars of this client's AP SSID — shown in master's Edit Pilot modal
    uint32_t lastSeen;
    bool     online;
    bool     running                = false;  // true while client's race timer is active
    bool     quitEarly              = false;  // true if pilot stopped during a master-initiated race
    // Set by removeNode() on async_tcp; the slot is actually erased by
    // _reapRemovedNodes() on parallelTask.  See removeNode() for why the
    // erase cannot happen where the request arrives.
    bool     pendingRemoval         = false;
    bool     independent            = false;  // true when pilot is solo-racing with skip flag enabled
    bool     skipEnabled            = false;  // true when pilot has "ignore race director" config enabled
    bool     excludedFromCurrentRace = false; // true when master chose "ignore solo racers" for this node
    // ── Lap Sync state (§4, §5, §6) ─────────────────────────────────────
    // NOTE: there is no separate lapCount member.  There used to be, fed from
    // summary.lapTotal — which counts from seq 1 because seq 0 is the gate-1
    // crossing — while every reader had moved to laps.total, which counts from
    // seq 0.  Two counters that disagree by one and only one of them read.
    // laps.total is the count.
    LapRing  laps;                     // bounded ring, lazily allocated

    // Highest seq for which this master holds EVERY lap in
    // [laps.oldestSeq() .. ackSeq].  Contiguity is measured from oldestSeq,
    // never from zero: once the ring evicts, a lap that is gone because it
    // aged out is indistinguishable from one that was lost unless the ack
    // floor moves with the window (§6.1).
    uint32_t ackSeq   = 0;
    bool     hasAck   = false;         // false until the first lap is stored

    // Epoch identity (§5).  A change in either invalidates every digest
    // comparison for this node.
    uint32_t raceId   = 0;
    uint32_t bootId   = 0;

    // Last digest this node reported, so the master can detect divergence
    // without asking for anything.
    uint32_t reportedCrc   = 0;
    uint32_t reportedCount = 0;
    uint32_t reportedOldest = 0;

    // ── Clock anchor (§8) ───────────────────────────────────────────────
    // Master-time instant at which THIS node's race clock read zero.
    // orderUs = anchorUs + elapsed (drift-corrected).  Absorbs both the clock
    // offset and the Start All spread in one value.
    //
    // Held in µs because the offset behind it is measured in µs; rounding to
    // ms happens once, at the point orderMs is published.
    int64_t  anchorUs    = 0;
    uint32_t anchorRttMs = 0xFFFFFFFF;  // fallback path only: best RTT seen
    bool     anchored    = false;       // false for solo/skip nodes — excluded from ordering

    // ── Clock sync (§8) ─────────────────────────────────────────────────
    // Offset of this node's µs clock relative to the master's, such that
    //     master_us = client_us - clockOffsetUs
    // Measured by a four-timestamp exchange against /timer/clockProbe, best
    // sample (lowest round-trip delay) kept — least queueing means least path
    // asymmetry, which is the assumption the NTP formula rests on.
    int64_t  clockOffsetUs   = 0;
    uint32_t clockDelayUs    = 0xFFFFFFFF;  // delay of the sample we kept

    // The MOST RECENT probe, unfiltered — every sample, whether or not it beat
    // the stored best.
    //
    // clockOffsetUs above is min-delay sticky: it only moves when a better
    // sample arrives, so as a time series it is a step function and fitting a
    // slope to it measures the filter rather than the crystals.  These two are
    // what an external logger needs to compute drift independently and check
    // the firmware's own fit against something.
    int64_t  rawOffsetUs     = 0;
    uint32_t rawDelayUs      = 0;
    // Master time at which that sample was TAKEN, not when it was read back.
    //
    // Steady-state probing is round-robin at one node per 25 s, so a given
    // node produces a fresh sample only every ~150 s on a six-client fleet.
    // A logger polling faster than that would otherwise both re-count the same
    // sample (understating its own error bar) and mis-place it in time by up
    // to a poll period.  Publishing the instant makes the series exact and
    // makes duplicates trivially detectable.
    int64_t  rawAtUs         = 0;
    uint16_t clockSamples    = 0;           // probes landed since the last resync burst
    bool     clockValid      = false;

    // Set when this node acknowledges its GO datagram (§8).  Cleared at each
    // race start, so it answers "did this pilot get THIS race's start?".
    bool     startAcked      = false;
    // How late this pilot's race clock actually began, in ms.  0 = on time.
    // Reported by the client after a backdated start so the director can see
    // that a pilot may have missed their first gate even though the rest of
    // the race is perfectly aligned.
    uint32_t startLateMs     = 0;

    // esp_reset_reason() as this node last reported it.  0 == not yet known.
    uint8_t  resetReason     = 0;

    // Drift history: (master timestamp, offset) pairs for the slope fit.
    // A sliding window rather than the whole race, because a unit warming up
    // in the sun drifts NON-linearly — a rate fitted across the full race
    // would be a stale average that is wrong at both ends.
    static constexpr uint8_t CLOCK_DRIFT_SAMPLES = 8;
    int64_t  driftAtUs[CLOCK_DRIFT_SAMPLES]  = {0};  // master µs of each sample
    int64_t  driftOffUs[CLOCK_DRIFT_SAMPLES] = {0};  // offset µs of each sample
    uint8_t  driftCount      = 0;   // valid entries (saturates at window size)
    uint8_t  driftHead       = 0;   // next write position (ring)
    int32_t  driftPpm        = 0;   // fitted rate; >0 means this node runs fast
    bool     driftValid      = false;
    bool     driftClamped    = false; // fit hit ±CLOCK_MAX_DRIFT_PPM — not data

    // ── §10 capability, as a TRI-STATE ──────────────────────────────────
    // lapSyncHeard distinguishes the two cases a single bool conflated:
    //   heard=false                 -> nothing known yet (just registered)
    //   heard=true,  capable=true   -> consumes lapDeltas
    //   heard=true,  capable=false  -> pre-protocol firmware, needs the arrays
    //
    // Only the third state requires legacy lap arrays.  Treating the FIRST
    // state as "incapable" is what let a node that had merely not heartbeated
    // yet hold the fleet-wide gate shut — see anyNodeNeedsLegacyLaps().
    bool     lapSyncHeard     = false;  // a heartbeat has been processed
    bool     lapSyncCapable   = false;  // ...and it advertised lapSync

    // ── Resync governor (§7) ────────────────────────────────────────────
    uint8_t  resyncState      = 0;      // LapSyncState
    uint32_t resyncNeededAtMs = 0;      // when NEEDED was entered (escape valve)
    uint32_t resyncDoneAtMs   = 0;      // last completed resync (cooldown)
    uint8_t  resyncAttempts   = 0;      // failed verifies before DIVERGENT
};

// §7 resync governor states.  Kept as a plain enum stored in a uint8_t so
// NodeInfo stays cheap to copy inside the node vector.
enum LapSyncState : uint8_t {
    LAPSYNC_IDLE = 0,
    LAPSYNC_NEEDED,
    LAPSYNC_GRANTED,
    // The client is pulling chunks.  There is no separate VERIFY state: the
    // master learns a restore succeeded from the next heartbeat digest, so
    // STREAMING is what the node sits in until that compare clears it.
    LAPSYNC_STREAMING,
    LAPSYNC_DIVERGENT
};

class Config;
class Led;
class Webserver;
class LapTimer;

struct RecruitSummary {
    bool     valid     = false;
    bool     inProgress= false;
    uint8_t  found     = 0;
    uint8_t  recruited = 0;
    uint8_t  skipped   = 0;
    uint8_t  failed    = 0;
};

class MultiNodeManager {
public:
    // led + webserver are needed by the "Recruit nearby units" operation:
    // LED stays solid-on for the duration so the director knows the master
    // is offline; webserver provides the AP-restart helper after we drop
    // back from STA.  Both may be nullptr on builds that don't recruit.
    void init(Config* config, Led* led = nullptr, Webserver* webserver = nullptr);
    void process(uint32_t currentTimeMs);  // Call from Core 0 (parallelTask)

    // The client's own laps live in LapTimer, which is their authoritative
    // replica (§3).  The sync layer reads that ring rather than keeping a
    // second copy — see the notes on _timer below for why that alone fixes
    // three of the four uplink defects.
    void setLapTimer(LapTimer* timer) { _timer = timer; }

    // Called from the lap-detection path — minimal work.  The lap itself is
    // already stored in LapTimer by the time this runs; this only nudges the
    // reporter so it ships on the next Core 0 tick instead of waiting for the
    // heartbeat.
    void queueLap(uint32_t lapTimeMs);

    // ── Lap Sync accessors (§6) ─────────────────────────────────────────
    // Master-side digest for one node, used to build the directorState
    // per-node digest and to answer resync pulls.
    const NodeInfo* findNode(uint8_t nodeId) const;

    // Client-side: adopt a raceId minted by the master (§5).  Clears the
    // local ack state, because acks are only meaningful within one epoch.
    void   setRaceId(uint32_t raceId);

    // The epoch this device is operating in.  A master mints its own; a client
    // echoes the one it was given.  One accessor so callers never have to know
    // which side they are on — asking the wrong field would publish 0 from a
    // master and make every client think the epoch had been cleared.
    uint32_t getRaceId() const { return isMasterMode() ? _masterRaceId : _raceId; }
    uint32_t getBootId() const { return _bootId; }

    // ── Peer cache feed (§6.5) ──────────────────────────────────────────
    // Laps appended since the previous directorState broadcast, already
    // normalized to master time for ordering.  The webserver serializes these
    // into the payload and then calls clearLapDeltas().
    //
    // This exists because stripping the lap arrays from directorState removes
    // the ONLY path by which a client learns about its peers: clients render
    // the Race View purely from that payload, and the multiNodeLap SSE event
    // goes to the master's own browser, not to client nodes.  Without this
    // feed the multi-race tab would simply go blank on every client.
    struct LapDelta {
        uint8_t  nodeId;
        uint32_t seq;
        uint32_t lapTimeMs;
        uint32_t raceElapsedMs;
        uint32_t orderMs;      // master-time instant of this lap; 0 when unanchored
        bool     ordered;      // false for solo/legacy nodes (§8)
    };
    const std::vector<LapDelta>& getLapDeltas() const { return _lapDeltas; }
    void clearLapDeltas() { _lapDeltas.clear(); }

    // Stage-4 gate (§10), stated as the question that actually matters: does
    // anyone on this fleet still NEED the legacy lap arrays?
    //
    // This replaced allNodesLapSyncCapable(), which asked the inverse and was
    // fail-CLOSED: every node started lapSyncCapable=false, so the arrays kept
    // shipping until all of them had been positively confirmed.  A node that
    // had merely registered without heartbeating yet was indistinguishable
    // from pre-protocol firmware, and held the payload win shut for the whole
    // fleet — with nothing exposed to say which node was responsible.
    //
    // Fail-OPEN instead.  Only a node that has actually been heard from AND
    // did not advertise lapSync forces the arrays back on.  Absence of the
    // field in a heartbeat is itself the positive signal for old firmware, so
    // a genuine legacy client still gets its arrays — from its very first
    // heartbeat, i.e. within one 2 s interval of joining.
    //
    // Worst case is therefore one interval during which a legacy client's
    // multi-race tab renders empty, self-correcting on its next heartbeat.
    // The old behaviour's worst case was the heap win never landing at all.
    //
    // Still all-or-nothing rather than per-recipient: seven tailored payloads
    // would mean seven distinct strings instead of one shared buffer, trading
    // the heap win for a heap loss (Rule 3).
    bool anyNodeNeedsLegacyLaps() const {
        for (const auto& n : _nodes) {
            if (n.online && n.lapSyncHeard && !n.lapSyncCapable) return true;
        }
        return false;
    }

    // Master-side: anchor a node from the race start it REPORTED, converted
    // with the clock offset measured during pre-arm (§8).  Exact — no
    // inference.  Returns false when this node has no valid offset, in which
    // case the caller falls back to recordAnchorFromRtt().
    bool   recordAnchorFromReport(uint8_t nodeId, int64_t clientRaceStartUs);

    // Fallback anchor for a node that missed the pre-arm sync (registered
    // during the countdown, or its probes all failed).  This is the old
    // half-RTT estimate, kept ONLY as a degraded path: it measures the reply
    // of a handler that clears laps and starts a timer before answering, so
    // the client's own work lands inside what we call network latency.
    void   recordAnchorFromRtt(uint8_t nodeId, uint32_t masterAckMs, uint32_t rttMs);

    // Master-side diagnostic: per-node clock sync state as JSON.  Deliberately
    // its own endpoint rather than fields in directorState — this is
    // instrumentation, and directorState is on the 2 s fanout path whose size
    // we just spent a release bounding.
    String buildClockReport() const;

    // Record how late a client's race clock actually began, from its heartbeat.
    void   setNodeStartLate(uint8_t nodeId, uint32_t lateMs);

    // Why this node last restarted (esp_reset_reason(), as reported by the
    // client on its heartbeat).  A panic, a task watchdog, the heap guard and a
    // brownout all look identical from the master -- the node simply comes back
    // with a new bootId -- and each needs a completely different fix.  Two
    // clients restarting during an idle hour went undiagnosable for exactly
    // this reason, so the cause now travels with the reboot.
    void   setNodeResetReason(uint8_t nodeId, uint8_t reason);
    static const char* resetReasonName(uint8_t reason);

    // Master-side: mint a new race epoch.  Returns the new raceId.
    //
    // Split in two deliberately.  beginRaceEpoch() only mints and distributes
    // an id — it is called at PRE-ARM so the GO datagram can be authenticated
    // against an epoch the clients already hold.  resetEpochLapState() is the
    // DESTRUCTIVE half and runs at the actual start.
    //
    // They used to be one call, which meant pre-arming and then cancelling
    // destroyed the previous race's stored laps without a race having run.
    uint32_t beginRaceEpoch();
    void     resetEpochLapState();

    // Queue race start/stop from the AsyncWebServer handler (non-blocking).
    // process() will execute the actual HTTP POSTs on the next Core 0 tick.
    void queueRaceStart();
    void queueRaceStop();
    void queueRacePreArm();

    // ── Master-side handlers (called from AsyncWebServer request threads) ──
    bool   handleRegister(const String& pilotName,
                          uint32_t pilotColor,
                          uint8_t bandIndex, uint8_t channelIndex, uint16_t frequency,
                          uint8_t enterRssi, uint8_t exitRssi,
                          const String& staIP, const String& clientIP,
                          const String& macAddress,
                          const String& apSuffix,
                          uint8_t& assignedNodeId,
                          bool& stateChanged);   // out: true on new-node insert,
                                                 //   offline→online recovery,
                                                 //   or any user-visible field change.
                                                 //   false for steady-state keep-alives
                                                 //   so the caller can skip broadcasts.
    // Store a batch of laps from one client and return the ack (§6.1).
    //
    // Idempotent by construction: any lap whose seq is at or below the stored
    // ack is dropped, so a lost RESPONSE (the client retries a lap the master
    // already has) cannot double-store.  outAckSeq/outOldestSeq are what the
    // client needs to trim its unacked window and to learn which laps are
    // retired rather than missing.
    bool   handleLapBatch(uint8_t nodeId, uint32_t raceId, uint32_t bootId,
                          const LapSyncRecord* laps, size_t count,
                          uint32_t& outAckSeq, uint32_t& outOldestSeq);

    // Legacy single-lap path, kept indefinitely (§10) so an un-updated client
    // keeps working exactly as it does today — just without self-healing.
    bool   handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber);
    // macAddress is verified against the stored MAC for nodeId.  An empty
    // incoming MAC (legacy clients) skips the check for backwards compat,
    // but a non-empty mismatch returns false → master replies 404 NOT_FOUND,
    // which triggers the client's fast-recovery re-register-with-nodeId-0
    // path.  Without this, two devices whose nodeIds collide can both think
    // they're connected while only one is actually known to the master.
    bool   handleHeartbeat(uint8_t nodeId, const String& macAddress,
                           bool running, bool independent, bool skipEnabled,
                           bool& stateChanged);
    // ── §6.2 heartbeat digest ───────────────────────────────────────────
    struct HeartbeatSyncReply {
        uint32_t ackSeq      = 0;
        uint32_t oldestSeq   = 0;
        int32_t  wantFrom    = -1;    // >=0: master is missing from here
        bool     resyncGrant = false; // §7 — at most one node fleet-wide
        uint32_t chunkLimit  = LAPSYNC_CHUNK_RACING;
        uint32_t raceId      = 0;
    };

    // Fold a client's reported digest into master state and produce its reply.
    // Detection is cheap and unconditional; repair is what the governor rations.
    bool   handleHeartbeatSync(uint8_t nodeId, uint32_t raceId, uint32_t bootId,
                               bool lapSyncCapable,
                               uint32_t count, uint32_t crc, uint32_t oldestSeq,
                               HeartbeatSyncReply& out);

    // Serve a bounded chunk of one node's laps (§6.3).  Symmetric: the same
    // shape a client serves when the master is the one restoring.
    // `since` below oldestSeq is answered from oldestSeq rather than refused —
    // those laps are retired, and refusing would stall a legitimate restore.
    bool   buildLapChunk(uint8_t nodeId, uint32_t since, uint32_t limit, String& out) const;

    bool   handleQuit(uint8_t nodeId);
    // Master-side: bump a node's lastSeen without a full heartbeat.  Used by
    // request handlers that already proved the client is reachable (e.g. the
    // RSSI proxy successfully completed a round-trip) — keeps the heartbeat
    // watchdog from marking the client offline when the link is in heavy use
    // and incoming heartbeat packets are sometimes lost in the WiFi traffic.
    void   touchNode(uint8_t nodeId);

    // Master-side: move a node to a different slot.  If the target slot is
    // already occupied the two nodes swap places.  Both affected clients
    // receive a setSlot command so they persist the new preferred slot to
    // EEPROM and update their _myNodeId immediately; the master's own _nodes
    // list is updated in-place so subsequent heartbeats arrive labelled with
    // the new slot ids and match cleanly.  Returns true on success.
    bool   moveNode(uint8_t fromNodeId, uint8_t toSlot);

    // Client-side: set the local node id at runtime (after master sends a
    // setSlot command following a move).  The caller is expected to also
    // persist the new value via Config::setMnPreferredSlot.
    void   setMyNodeId(uint8_t newId) { _myNodeId = newId; }

    bool   removeNode(uint8_t nodeId);     // master: manually remove a node slot
    bool   updateNodePilot(uint8_t nodeId, const String& name, uint32_t color);
    bool   updateNodeChannel(uint8_t nodeId, uint8_t bandIndex, uint8_t channelIndex, uint16_t frequency);
    void   clearAllLaps();                 // master: wipe all stored laps for all nodes
    void   setExcludeNodes(const std::vector<uint8_t>& ids);  // master: exclude specific nodes from next broadcast

    // Queue a director-state payload to be broadcast to all online clients.
    // Called from Webserver request threads; actual HTTP POSTs run in process()
    // on Core 0 so the AsyncWebServer threads stay free.
    void   queueDirectorStateBroadcast(const String& payload);

    // True when the HTTP fanout is actually due.  Callers use this to avoid
    // handing over a payload that will only be overwritten before it ships.
    //
    // The SSE build runs every MN_STATE_BUILD_INTERVAL_MS (250 ms) but the
    // fanout is throttled to MIN_DIRECTOR_BROADCAST_INTERVAL_MS (2000 ms), so
    // seven of every eight payloads handed to queueDirectorStateBroadcast() were
    // deep-copied — up to ~17 KB each — and then discarded unread.  Four
    // multi-kilobyte malloc/copy/free cycles per second, forever, is a
    // first-class heap fragmenter, and fragmentation (not total usage) is what
    // actually crashes this device.
    bool   directorBroadcastDue(uint32_t nowMs);

    // Pre-arm phase tracking — exposed to clients via the director-state payload
    // so the Race View banner can prompt "Arm your quad" during the countdown.
    void   setPrearmPhase(bool active);
    bool   getPrearmPhase() const;

    // Queue a "Recruit nearby units" job — master only.  When force is true the
    // master configures ALL FPVRaceOne units in range regardless of their
    // current mode; when false only units currently in single mode are touched.
    // The job runs on Core 0 from process() and drops the AP for the duration.
    void   queueRecruit(bool force);
    RecruitSummary getRecruitSummary() const { return _recruitSummary; }
    void   clearRecruitSummary() { _recruitSummary.valid = false; }

    // ── Client-side state setters (called from webserver handlers) ──
    void   setTimerRunning(bool running);
    void   setMasterRaceActive(bool active);
    void   setQuitPending();       // queue quit notification to master
    void   pauseReconnect(uint32_t durationMs);  // pause registration for durationMs (client, called on kick)
    bool   isMasterRaceActive() const { return _masterRaceActive; }
    String scanForNodesJson();   // WiFi scan — call from Core 0 only

    // ── Status getters ──
    bool    isClientMode()      const;
    bool    isMasterMode()      const;
    bool    isMasterConnected()        const { return _masterConnected; }
    bool    consumeClientStateChanged()      { bool v = _clientStateChangedFlag; _clientStateChangedFlag = false; return v; }
    uint8_t getMyNodeId()       const { return _myNodeId; }

    const std::vector<NodeInfo>& getNodes() const { return _nodes; }

    // ── OTA pause/resume ──────────────────────────────────────────────
    // OTA needs the STA radio (to reach home WiFi for GitHub) and a clean
    // TCP slot pool (the ESP32-C6 LwIP cap is 16).  In client mode the STA
    // is busy talking to the master; in master mode active clients churn
    // through TCP slots and the AP-retune during STA association can stall
    // the outbound TLS handshake.  pauseForOta() temporarily halts all
    // multinode networking — process() becomes a no-op, master-side handlers
    // reject, and (in client mode) the STA is disconnected from the master.
    // The persisted Config mode is unchanged, so a reboot or resumeFromOta()
    // returns the device to its previous master/client role with all state
    // intact.  Includes a 5-minute safety auto-resume in case the caller
    // forgets (page closed, OTA aborted).
    void   pauseForOta();
    void   resumeFromOta();
    bool   isPausedForOta() const { return _pausedForOta; }

    // True when the *current* multinode state would interfere with OTA:
    // - client mode, OR
    // - master mode with at least one online client.
    // Used by the UI to gate the Check for Updates dialog.
    bool   wouldOtaDisruptMultinode() const;

    // UI-ready disruption explanation, or empty string if no disruption.
    String getOtaDisruptionMessage() const;

private:
    Config*    _conf      = nullptr;
    Led*       _led       = nullptr;
    Webserver* _webserver = nullptr;
    std::vector<NodeInfo> _nodes;  // master: list of registered clients
    std::vector<uint8_t>  _excludeNodes;  // node IDs to skip in next _broadcastRaceStart()

    // §6.5 delta window — drained by the webserver into each directorState.
    // Bounded at LAPSYNC_MAX_LAP_DELTAS: overflow is detected by the
    // recipient's digest compare and deliberately NOT repaired, because peer
    // data is a cache (§3), not a replica.
    std::vector<LapDelta> _lapDeltas;
    void _queueLapDelta(const NodeInfo& n, const LapSyncRecord& lap);

    // ── §7 resync governor (master side) ────────────────────────────────
    // Exactly one grant exists fleet-wide.  Without that, seven sick nodes
    // would each be granted concurrently and the repair traffic would be the
    // very burst this protocol exists to remove.
    uint8_t  _resyncGrantNodeId = 0;      // 0 = no grant outstanding
    uint32_t _resyncGrantAtMs   = 0;
    uint32_t _lastLapHandledMs  = 0;      // quiet gate reference
    void     _runResyncGovernor(uint32_t nowMs);
    bool     _anyNodeRunning() const;

    // Master's own race epoch.  Clients echo it; a mismatch invalidates every
    // digest comparison rather than silently merging two races (§5).
    uint32_t _masterRaceId = 0;
    // True from pre-arm until the start that consumes the epoch.  Without it,
    // a start arriving with no pre-arm re-used the previous race's id — so its
    // laps were never reset and run 2 inherited run 1's, which is the exact
    // regression the epoch system exists to prevent.
    bool     _epochArmed   = false;

    // Client state
    bool     _masterConnected    = false;
    uint8_t  _myNodeId           = 0;
    uint32_t _lastHeartbeatMs    = 0;
    uint32_t _lastRegistrationMs = 0;
    uint8_t  _heartbeatFailCount = 0;
    String   _myMacAddress;              // this device's WiFi MAC (set in init)

    // ── Client-side lap sync (§6.1) ─────────────────────────────────────
    // There is deliberately NO local copy of the laps here.  LapTimer already
    // holds this pilot's authoritative ring, complete with seq, raceElapsedMs
    // and the rolling digest, and it holds them whether or not a master is
    // reachable.  Reading the unacked window straight out of that ring is
    // what fixes three of the four uplink defects at once:
    //
    //   - a lap detected while disconnected is already stored, so nothing is
    //     discarded at a connection guard
    //   - two laps in one tick are two ring entries, not one overwritten slot
    //   - a failed POST leaves _ackedSeq where it was, so the lap is simply
    //     still unacked and rides the next report
    //
    // The fourth (a counter that advanced before the POST) disappears because
    // there is no separate counter left to advance.
    LapTimer *_timer        = nullptr;
    uint32_t  _ackedSeq     = 0;      // highest seq the master has confirmed
    bool      _hasAckedSeq  = false;  // false until the first ack this race
    uint32_t  _masterOldest = 0;      // master's window floor; never resend below it

    // Epoch identity (§5)
    uint32_t  _raceId       = 0;      // minted by master, echoed by us
    uint32_t  _bootId       = 0;      // random at boot; a change means "I rebooted"
    uint8_t   _resetReason  = 0;      // esp_reset_reason() at boot; rides the heartbeat

    // Set when the master asks for laps we have (wantFrom >= 0), cleared when
    // the gap closes.  Distinct from a resync: this is the fast path catching
    // up, not the governor running.
    bool      _masterWantsFrom    = false;
    uint32_t  _masterWantFromSeq  = 0;

    // §7 — set from the heartbeat response.  Exactly one node fleet-wide holds
    // a grant at a time, so a client may only pull while this is true.
    bool      _resyncGranted      = false;
    uint32_t  _resyncChunk        = LAPSYNC_CHUNK_RACING;
    uint32_t  _lastResyncPullMs   = 0;

    void _consumeHeartbeatResponse(const String& resp);
    // Pull our own laps back from the master after a reboot (§6.3).  Runs only
    // while granted, one bounded chunk per tick.
    void _processResyncPull();

    // Client-side state
    bool     _masterRaceActive        = false;
    bool     _timerRunning            = false;
    uint32_t _reconnectPausedUntilMs  = 0;  // millis() deadline set when master kicks this node

    // Thread-safe flags (set by async handler on Core 0, consumed by process() on Core 0)
    volatile bool     _heartbeatForcePending    = false;  // set by setTimerRunning(); consumed by process()
    volatile bool     _lapPending              = false;
    volatile uint32_t _pendingLapTime          = 0;
    volatile bool     _racePreArmPending       = false;
    volatile bool     _raceStartPending        = false;
    volatile bool     _raceStopPending         = false;
    volatile bool     _quitPending             = false;
    volatile bool     _clientStateChangedFlag  = false;  // set in process() when _masterConnected changes

    // Director-state broadcast queue (master-side push to clients).
    // Setting _directorStateBroadcastPending coalesces multiple bursts into one push.
    // _lastDirectorBroadcastMs throttles the actual HTTP fanout to once every
    // MIN_DIRECTOR_BROADCAST_INTERVAL_MS — during a multi-node recovery storm
    // a dozen pushMultiNodeState() calls can stack up in <100 ms, and running
    // them serially with a 300 ms-per-client HTTPClient timeout stalls Core 0
    // long enough that the recovered nodes' next heartbeats expire and we get
    // a cascade.  With throttling, the latest payload is broadcast at most
    // every interval; Race View UIs see at most that much lag on host-side
    // changes.  Race-critical broadcasts (pre-arm/start/stop) are NOT
    // throttled because their timing matters.
    volatile bool     _directorStateBroadcastPending = false;
    uint32_t          _lastDirectorBroadcastMs       = 0;
    static constexpr uint32_t MIN_DIRECTOR_BROADCAST_INTERVAL_MS = 2000;
    // Emptied with `= ""` after each fanout, never with `= String()`.  The
    // former keeps the allocated capacity (Arduino's String::copy calls
    // reserve(0), which returns early when a buffer already exists); the latter
    // hands ~17 KB back to the allocator so the next assignment has to find a
    // contiguous 17 KB block all over again.  Same pattern, and same reasoning,
    // as _mnPayloadBuf in fpv_webserver.h.
    //
    // Cost of keeping it: one payload's worth of capacity resident on the
    // master for the life of the boot.  Worth it — this device fails on
    // fragmentation, not on total usage.
    String            _directorStatePayload;
    // isEmpty() can no longer serve as the "have I got something to send?" test,
    // because an emptied-but-retained buffer is also empty.
    bool              _directorStatePayloadValid = false;

    // Pre-arm phase: master entered the countdown but the race hasn't actually started yet.
    // Auto-clears after a timeout so a missed race/start (e.g. director cancelled) doesn't
    // pin clients in the "Arm your quad" state forever.
    bool              _prearmPhase           = false;
    uint32_t          _prearmPhaseSetAtMs    = 0;
    // LAST-RESORT safety net, not an estimate of countdown length.
    //
    // Both real exits from pre-arm are now explicit: race/start clears it, and
    // race/stop clears it (the cancel path, which used to be missing).  This
    // only fires when the browser driving the countdown disappears without
    // sending either — tab closed, device slept, WiFi dropped mid-countdown.
    //
    // It was 15000, which was implicitly a guess at how long a countdown runs.
    // Measured, a countdown can exceed 16 s: _raceCountdown() waits on the Web
    // Speech API between utterances, and that wait has its own 15 s bail-out
    // for engines that never report completion.  A timeout shorter than the
    // thing it is timing meant the banner vanished just before the start it
    // was announcing.  Duration is not predictable from any setting, so this
    // deliberately does not try to track it — it just has to outlast it.
    static constexpr uint32_t PREARM_PHASE_TIMEOUT_MS = 60000;

    // "Recruit nearby units" job — flag set by web handler, consumed on Core 0.
    volatile bool     _recruitPending        = false;
    volatile bool     _recruitForce          = false;
    RecruitSummary    _recruitSummary;

    // OTA pause state.  _savedNodeModeForOta is the Config mode at pause time —
    // unused for restoration (Config is the source of truth) but kept in DEBUG
    // output to help diagnose stuck-paused situations.  _pauseExpiresAtMs is a
    // safety deadline so we resume even if the caller forgets.
    bool              _pausedForOta          = false;
    uint8_t           _savedNodeModeForOta   = 0;
    uint32_t          _pauseExpiresAtMs      = 0;
    static constexpr uint32_t OTA_PAUSE_TIMEOUT_MS = 300000;  // 5 minutes

    // ── Clock sync scheduling (§8, master side) ─────────────────────────
    // Pre-arm opens a sync window: the countdown that follows it is 6-9 s of
    // idle network with nobody racing, which is the only quiet, uncontended
    // moment this system ever gets.  Probing there rather than inside the
    // Start All fanout is the whole design.
    uint32_t _clockSyncUntilMs   = 0;   // burst active while millis() < this
    uint32_t _lastClockProbeMs   = 0;   // spacing between individual probes
    uint8_t  _clockProbeCursor   = 0;   // round-robin over _nodes

    // Sample COUNT is the lever that matters.  Min-delay filtering is a
    // tournament — the best sample is only as good as the number of draws.
    //
    // This is a SAFETY CAP, not the intended window length.  The burst opens
    // at pre-arm and closes when _broadcastRaceStart() runs, so it covers the
    // whole countdown however long that takes; the cap only matters if the
    // director cancels and the start never comes.
    //
    // It was 7000 ms, sized on an assumption that the countdown ran 6-9 s.
    // Measured, it runs ~18 s — _raceCountdown() waits for TTS to finish
    // between every utterance.  So the burst stopped 11 s before the anchor
    // was taken, and those final seconds are the most valuable ones: an offset
    // measured 11 s before the start is 11 s stale.
    static constexpr uint32_t CLOCK_SYNC_MAX_MS       = 30000;
    static constexpr uint32_t CLOCK_PROBE_SPACING_MS  = 60;
    // Steady state: one node every 25 s, feeding the drift fit.  Deliberately
    // NOT per-lap — at ±20 ppm two crystals diverge ~60 µs over a 3 s lap,
    // while a single probe carries 1-3 ms of error, so per-lap resync would
    // overwrite a good number with a worse one dozens of times a minute.
    static constexpr uint32_t CLOCK_REPROBE_INTERVAL_MS = 25000;
    // Sized against the MEASURED round-trip distribution, not an assumption
    // about what a LAN ought to do.
    //
    // A previous revision used 60 ms on the reasoning that a later reply is
    // lost rather than late.  The data already said otherwise: best-of-ten
    // delays on this fleet included 19.7, 49.3 and 63.1 ms, so most of four
    // nodes' distributions sat above the cutoff and the burst landed 1-2
    // samples per node instead of ~19.  Truncating the distribution does not
    // bias the minimum — it just starves the tournament that finds it.
    //
    // Costs little at 200 ms: replies mostly arrive in 20-40 ms and spacing
    // gates throughput anyway, so the full timeout is only ever spent on a
    // datagram that genuinely went missing.
    static constexpr uint16_t CLOCK_PROBE_TIMEOUT_MS  = 200;

    // UDP, not HTTP — the reason NTP uses UDP.  The first implementation of
    // this probe bracketed HTTPClient::GET(), which performs a TCP connect
    // handshake, then the request, then the response.  Measured floor was
    // 18-22 ms across five independent nodes: systematic, so min-delay
    // filtering could not touch it.  Worse, the handshake happens BEFORE the
    // client's handler exists, so none of it falls between t2 and t3 and all
    // of it lands on the outbound leg — a bias in the offset, not just noise.
    //
    // A datagram has no handshake, no retransmit, no Nagle, no delayed ACK and
    // no socket lifecycle, and both ends stamp inside the LwIP task rather
    // than an async HTTP handler.
    static constexpr uint16_t CLOCK_PROBE_UDP_PORT    = 5808;
    static constexpr uint32_t CLOCK_PROBE_MAGIC_REQ   = 0x43503151;  // "CP1Q"
    static constexpr uint32_t CLOCK_PROBE_MAGIC_RSP   = 0x43503152;  // "CP1R"
    // The GO message: raceId + the instant to start, in the recipient's own
    // clock.  Unacknowledged by design — it is sent CLOCK_START_REPEATS times
    // because repeating a 16-byte datagram is cheaper than waiting for an ack,
    // and losing a start is the one failure a pilot cannot recover from.
    static constexpr uint32_t CLOCK_START_MAGIC       = 0x43503153;  // "CP1S"
    static constexpr uint8_t  CLOCK_START_REPEATS     = 3;
    static constexpr uint32_t CLOCK_START_REPEAT_GAP_US = 3000;      // 3 ms
    // GO acknowledgement.  Nothing BLOCKS on it — it is a datagram arriving
    // asynchronously — but it turns "did everyone start?" from a 3-4 s wait on
    // the heartbeat into a ~50 ms check, which is the difference between a
    // repaired pilot missing one gate and missing several.
    static constexpr uint32_t CLOCK_START_ACK_MAGIC   = 0x43503141;  // "CP1A"
    static constexpr uint32_t START_ACK_WAIT_MS       = 60;

    // ── Start margin: MEASURED, not assumed (§8) ────────────────────────
    //
    // How far ahead the common start instant is placed.  It must exceed the
    // time it takes to distribute the start, or a late client receives a
    // target that has already passed and falls back to starting on arrival —
    // reintroducing the spread this exists to remove.
    //
    // That duration is a property of the venue, not of the code: RF noise,
    // distance, how many pilots, what else is on the channel.  A constant
    // tuned on one bench is wrong everywhere else, so the margin is derived
    // each race from what this fleet actually did last time:
    //
    //   1.5x the measured fanout duration   (how long distribution took)
    // + 3x the worst measured one-way delay (flight of the last datagram)
    // + slack                               (scheduling granularity)
    // + a healing boost                     (raised on a miss, decayed on success)
    //
    // The bounds below are limits on that calculation, not estimates of it.
    static constexpr int64_t START_MARGIN_MIN_US   =   80000LL;   // 80 ms
    static constexpr int64_t START_MARGIN_MAX_US   = 2500000LL;   // 2.5 s
    static constexpr int64_t START_MARGIN_SLACK_US =   25000LL;   // 25 ms
    // First race after boot has no fanout measurement yet.  Per-node, so a
    // seven-pilot fleet starts more conservatively than a two-pilot one.
    static constexpr int64_t START_MARGIN_PER_NODE_US = 120000LL; // 120 ms
    // Healing: a client that missed its target costs this much extra next
    // race; a clean race gives a little back.  Asymmetric on purpose — being
    // slightly late is invisible, being early breaks the start.
    static constexpr int64_t START_BOOST_STEP_US   =  150000LL;   // +150 ms on a miss
    static constexpr int64_t START_BOOST_DECAY_US  =   25000LL;   // -25 ms on success
    static constexpr int64_t START_BOOST_MAX_US    = 1000000LL;   // 1 s

    // ── Start verification (§8) ─────────────────────────────────────────
    // The GO datagram is unacknowledged, so a node that never received it
    // would simply sit out the race with nothing to notice.  After the target
    // has passed — plus enough grace for a heartbeat to report the truth — any
    // node that should be racing and is not gets an HTTP start as repair, and
    // is counted as a miss so the margin heals upward.
    uint32_t _startVerifyAtMs = 0;   // 0 = nothing to verify
    int64_t  _lastStartTargetUs = 0; // master-time instant the fleet was told to start
    static constexpr uint32_t START_VERIFY_GRACE_MS = 3000;  // > heartbeat interval
    void _verifyRaceStarts();

    int64_t  _lastFanoutUs   = 0;   // measured duration of the last start fanout
    int64_t  _startBoostUs   = 0;   // healing term, grown on misses
    int64_t  _lastStartMarginUs = 0;// what we used last race, for the report
    int64_t _computeStartMarginUs() const;

    // Drift is MEASURED and REPORTED but not yet APPLIED.
    //
    // Your own data is what gates this.  Across 600 s the raw offsets moved by
    // at most ~5 ms; a real ±100 ppm would have moved them 60 ms, which would
    // have been unmissable even through the probe noise.  So true drift is
    // bounded below ~10 ppm, and the fit reporting -100/-35/+25 is measuring
    // its own jitter.  Applying that would inject up to 30 ms of error across
    // a 5-minute race to correct a few hundred µs of reality.
    //
    // Flip to true once the UDP probe brings sigma down and _fitClockDrift()
    // gates on residual rather than sample count.
    static constexpr bool     CLOCK_APPLY_DRIFT       = false;
    // Anything past this is measurement noise, not a real crystal.  Without
    // the clamp one bad probe under contention can inject a rate that
    // corrupts every subsequent lap in the race.
    static constexpr int32_t  CLOCK_MAX_DRIFT_PPM     = 100;
    // A slope fitted over a short span is dominated by per-sample noise.
    static constexpr uint8_t  CLOCK_DRIFT_MIN_SAMPLES = 4;
    static constexpr int64_t  CLOCK_DRIFT_MIN_SPAN_US = 20000000LL;  // 20 s
    // Minimum spacing between drift-window entries.  Keeps the pre-arm burst
    // (a probe every 60 ms) from flushing the window and collapsing its
    // baseline right when a race is about to start.  See _recordClockSample().
    static constexpr int64_t  CLOCK_DRIFT_SPACING_US  = 5000000LL;   // 5 s

    // Quality gate on what may enter the drift window.  The window was
    // accepting EVERY sample, including 49-63 ms ones — which is precisely the
    // sigma the slope fit is trying to see through.  A sample is admitted only
    // if its round trip is within 3x this node's best and under an absolute
    // ceiling, so one node with a poor link cannot quietly widen its own bar.
    // 25 ms rather than 15: at the delays this fleet currently produces, a
    // 15 ms ceiling rejected nearly everything and driftSamples fell to 0-2,
    // so the window learned nothing.  Drift is measured but not applied
    // (CLOCK_APPLY_DRIFT), so letting the window fill costs nothing and
    // starving it costs the only data that would justify turning it on.
    static constexpr uint32_t CLOCK_DRIFT_DELAY_MULT  = 3;
    static constexpr uint32_t CLOCK_DRIFT_MAX_DELAY_US = 25000;      // 25 ms

    // ── Clock probe transport (UDP) ─────────────────────────────────────
    // One socket, both roles.  A master sends REQ and consumes RSP; a client
    // receives REQ and answers RSP.  Symmetric, so a unit that changes role
    // needs no re-plumbing.
    AsyncUDP          _clockUdp;
    bool              _clockUdpReady   = false;
    // Written by the AsyncUDP callback (LwIP task), read by parallelTask.
    // The callback stamps t4 the instant the datagram lands — that is the
    // whole reason for using the async socket rather than polling one, since
    // poll granularity would otherwise quantize t4.
    // GO acknowledgements, one bit per slot (nodeId 1..7).  Written by the
    // UDP callback on the LwIP task, folded into NodeInfo::startAcked by
    // parallelTask — see the note in the ack handler for why this may not be
    // a walk over _nodes.
    volatile uint8_t  _startAckMask    = 0;
    void _foldStartAcks();

    volatile uint32_t _probeSeq        = 0;   // sequence we are waiting on
    volatile bool     _probeReady      = false;
    volatile int64_t  _probeT2         = 0;
    volatile int64_t  _probeT3         = 0;
    volatile int64_t  _probeT4         = 0;
    void _initClockUdp();

    // One four-timestamp exchange against a node.  Returns true if a sample
    // landed (whether or not it beat the stored best).  parallelTask only.
    bool _probeNodeClock(NodeInfo& n);
    // Advance the probe schedule; called once per process() tick.
    void _runClockSync(uint32_t nowMs);
    // Fold a fresh sample into a node's best-offset and drift window.
    void _recordClockSample(NodeInfo& n, int64_t offsetUs, int64_t delayUs);
    // Least-squares slope over the sliding window, clamped and gated.
    void _fitClockDrift(NodeInfo& n);
    // Master-time µs of a lap, drift-corrected.  The single place the whole
    // chain converges.
    int64_t _lapOrderUs(const NodeInfo& n, uint32_t raceElapsedMs) const;

    void _sendRegistration();
    void _sendHeartbeat();
    void _processQueuedLap();
    void _sendQuitNotification();
    void _broadcastRacePreArm();
    void _broadcastRaceStart();
    void _broadcastRaceStop();
    void _broadcastDirectorState();
    bool _postToMaster(const String& endpoint, const String& body);
    bool _postToMasterWithResponse(const String& endpoint, const String& body, String& response);
    bool _getFromMaster(const String& endpoint, String& response);
    void _checkNodeTimeouts(uint32_t currentTimeMs);
    // parallelTask ONLY.  Erases slots that removeNode() marked from the
    // async_tcp task; running it anywhere else reintroduces the iterator
    // invalidation it exists to prevent.
    void _reapRemovedNodes();
    void _runRecruitJob(bool force);
};
