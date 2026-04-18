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
    String   pilotCallsign;
    uint32_t pilotColor;
    String   clientIP;   // client's own AP IP (master uses this to push commands)
    String   staIP;      // client's STA IP assigned by master's DHCP
    String   macAddress; // client's WiFi MAC — primary unique key
    uint32_t lastSeen;
    bool     online;
    bool     running   = false;  // true while client's race timer is active
    bool     quitEarly = false;  // true if pilot stopped during a master-initiated race
    uint8_t  lapCount;
    std::vector<MultiNodeLap> laps;
};

class Config;

class MultiNodeManager {
public:
    void init(Config* config);
    void process(uint32_t currentTimeMs);  // Call from Core 0 (parallelTask)

    // Called from Core 1 (lap detection) — minimal work, thread-safe via volatile flag
    void queueLap(uint32_t lapTimeMs);

    // Queue race start/stop from the AsyncWebServer handler (non-blocking).
    // process() will execute the actual HTTP POSTs on the next Core 0 tick.
    void queueRaceStart();
    void queueRaceStop();

    // ── Master-side handlers (called from AsyncWebServer request threads) ──
    bool   handleRegister(const String& pilotName, const String& pilotCallsign,
                          uint32_t pilotColor,
                          const String& staIP, const String& clientIP,
                          const String& macAddress,
                          uint8_t& assignedNodeId);
    bool   handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber);
    bool   handleHeartbeat(uint8_t nodeId, bool running, bool& stateChanged);
    bool   handleQuit(uint8_t nodeId);
    bool   removeNode(uint8_t nodeId);     // master: manually remove a node slot
    void   clearAllLaps();                 // master: wipe all stored laps for all nodes

    // ── Client-side state setters (called from webserver handlers) ──
    void   setTimerRunning(bool running);
    void   setMasterRaceActive(bool active);
    void   setQuitPending();       // queue quit notification to master
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
    Config* _conf = nullptr;
    std::vector<NodeInfo> _nodes;  // master: list of registered clients

    // Client state
    bool     _masterConnected    = false;
    uint8_t  _myNodeId           = 0;
    uint32_t _lastHeartbeatMs    = 0;
    uint32_t _lastRegistrationMs = 0;
    uint8_t  _localLapCount      = 0;
    uint8_t  _heartbeatFailCount = 0;
    String   _myMacAddress;              // this device's WiFi MAC (set in init)

    // Client-side state
    bool     _masterRaceActive   = false;
    bool     _timerRunning       = false;

    // Thread-safe flags (set by async handler on Core 0, consumed by process() on Core 0)
    volatile bool     _lapPending              = false;
    volatile uint32_t _pendingLapTime          = 0;
    volatile bool     _raceStartPending        = false;
    volatile bool     _raceStopPending         = false;
    volatile bool     _quitPending             = false;
    volatile bool     _clientStateChangedFlag  = false;  // set in process() when _masterConnected changes

    void _sendRegistration();
    void _sendHeartbeat();
    void _processQueuedLap();
    void _sendQuitNotification();
    void _broadcastRaceStart();
    void _broadcastRaceStop();
    bool _postToMaster(const String& endpoint, const String& body);
    bool _postToMasterWithResponse(const String& endpoint, const String& body, String& response);
    void _checkNodeTimeouts(uint32_t currentTimeMs);
};
