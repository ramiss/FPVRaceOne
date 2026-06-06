#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <vector>

#define MULTINODE_MAX_NODES              7
#define MULTINODE_HEARTBEAT_INTERVAL_MS  2000   // send heartbeat every 2 s (less load on dual-mode WiFi)
#define MULTINODE_HEARTBEAT_FAIL_LIMIT      5   // consecutive failures before declaring disconnected
#define MULTINODE_REGISTER_INTERVAL_MS   5000   // re-register interval when already connected
#define MULTINODE_RECONNECT_INTERVAL_MS   800   // retry interval when disconnected
#define MULTINODE_NODE_TIMEOUT_MS        4000   // mark node offline after 4 s without heartbeat
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
                          const String& staIP, const String& clientIP,
                          const String& macAddress,
                          const String& apSuffix,
                          uint8_t& assignedNodeId);
    bool   handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber);
    bool   handleHeartbeat(uint8_t nodeId, bool running, bool independent, bool skipEnabled, bool& stateChanged);
    bool   handleQuit(uint8_t nodeId);
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
    volatile bool     _directorStateBroadcastPending = false;
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
