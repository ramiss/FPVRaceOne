#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <vector>

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

struct MultiNodeLap {
    uint32_t lapTimeMs;
    uint32_t timestamp;
    uint8_t  lapNumber;
};

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
    uint8_t  lapCount;
    std::vector<MultiNodeLap> laps;
};

class Config;
class Led;
class Webserver;

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

    // Called from Core 1 (lap detection) — minimal work, thread-safe via volatile flag
    void queueLap(uint32_t lapTimeMs);

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
    bool   handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber);
    bool   handleHeartbeat(uint8_t nodeId, bool running, bool independent, bool skipEnabled, bool& stateChanged);
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
    String getNodesToJson() const;
    String scanForNodesJson();   // WiFi scan — call from Core 0 only

    // ── Status getters ──
    bool    isClientMode()      const;
    bool    isMasterMode()      const;
    bool    isMasterConnected()        const { return _masterConnected; }
    bool    consumeClientStateChanged()      { bool v = _clientStateChangedFlag; _clientStateChangedFlag = false; return v; }
    uint8_t getMyNodeId()       const { return _myNodeId; }
    String  getMasterStatusJson() const;

    const std::vector<NodeInfo>& getNodes() const { return _nodes; }

private:
    Config*    _conf      = nullptr;
    Led*       _led       = nullptr;
    Webserver* _webserver = nullptr;
    std::vector<NodeInfo> _nodes;  // master: list of registered clients
    std::vector<uint8_t>  _excludeNodes;  // node IDs to skip in next _broadcastRaceStart()

    // Client state
    bool     _masterConnected    = false;
    uint8_t  _myNodeId           = 0;
    uint32_t _lastHeartbeatMs    = 0;
    uint32_t _lastRegistrationMs = 0;
    uint8_t  _localLapCount      = 0;
    uint8_t  _heartbeatFailCount = 0;
    String   _myMacAddress;              // this device's WiFi MAC (set in init)

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
    String            _directorStatePayload;

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
    void _checkNodeTimeouts(uint32_t currentTimeMs);
    void _runRecruitJob(bool force);
};
