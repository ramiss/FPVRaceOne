#pragma once
#include <Arduino.h>
#include <WiFi.h>
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
    // orderMs = anchorMs + lap.raceElapsedMs.  Absorbs both the clock offset
    // and the Start All spread in one value.
    uint32_t anchorMs    = 0;
    uint32_t anchorRttMs = 0xFFFFFFFF;  // best (lowest) RTT seen; lower is less asymmetric
    bool     anchored    = false;       // false for solo/skip nodes — excluded from ordering

    // True once this node has advertised that it consumes lapDeltas (§6.5).
    // Until EVERY registered node has, the master keeps emitting the legacy
    // lap arrays as well — see _allNodesLapSyncCapable().
    bool     lapSyncCapable   = false;

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
        uint32_t orderMs;      // anchorMs + raceElapsedMs; 0 when unanchored
        bool     ordered;      // false for solo/legacy nodes (§8)
    };
    const std::vector<LapDelta>& getLapDeltas() const { return _lapDeltas; }
    void clearLapDeltas() { _lapDeltas.clear(); }

    // Stage-4 gate (§10).  The lap arrays may only leave the directorState
    // payload once EVERY registered node consumes lapDeltas instead — a
    // client that still renders peers from the arrays would show a blank
    // multi-race tab the moment they disappeared.
    //
    // Deliberately all-or-nothing rather than per-recipient: seven tailored
    // payloads would mean seven distinct strings instead of one shared
    // buffer, which trades the heap win for a heap loss (Rule 3).  One old
    // node on the field therefore costs performance, never correctness.
    bool allNodesLapSyncCapable() const {
        if (_nodes.empty()) return false;
        for (const auto& n : _nodes) {
            if (n.online && !n.lapSyncCapable) return false;
        }
        return true;
    }

    // Master-side: record this node's clock anchor from a start acknowledgement
    // (§8).  rttMs is the round trip of the ack; the lowest-RTT sample wins
    // because least queueing means least path asymmetry.
    void   recordAnchor(uint8_t nodeId, uint32_t masterAckMs, uint32_t rttMs);

    // Master-side: mint a new race epoch.  Returns the new raceId.
    uint32_t beginRaceEpoch();

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
    static constexpr uint32_t PREARM_PHASE_TIMEOUT_MS = 15000;

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
    void _runRecruitJob(bool force);
};
