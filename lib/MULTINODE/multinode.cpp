#include "multinode.h"
#include "config.h"
#include "debug.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>

void MultiNodeManager::init(Config* config) {
    _conf                = config;
    _masterConnected     = false;
    _myNodeId            = 0;
    _lapPending          = false;
    _localLapCount       = 0;
    _lastHeartbeatMs     = 0;
    _lastRegistrationMs  = 0;
    _masterRaceActive    = false;
    _timerRunning        = false;
    _raceStartPending    = false;
    _raceStopPending     = false;
    _quitPending         = false;
    _nodes.clear();
}

void MultiNodeManager::process(uint32_t currentTimeMs) {
    if (!_conf) return;

    if (isClientMode()) {
        if (WiFi.status() != WL_CONNECTED) {
            if (_masterConnected) {
                _masterConnected = false;
                DEBUG("[MULTINODE] STA disconnected from master\n");
            }
            return;
        }

        // Periodic registration (initial and keep-alive)
        if (_lastRegistrationMs == 0 ||
            (currentTimeMs - _lastRegistrationMs) > MULTINODE_REGISTER_INTERVAL_MS) {
            _sendRegistration();
            _lastRegistrationMs = currentTimeMs;
        }

        // 1-second heartbeat
        if (_masterConnected &&
            (currentTimeMs - _lastHeartbeatMs) > MULTINODE_HEARTBEAT_INTERVAL_MS) {
            _sendHeartbeat();
            _lastHeartbeatMs = currentTimeMs;
        }

        // Drain queued lap (written by Core 1)
        if (_lapPending) {
            _lapPending = false;  // clear before sending to avoid double-send
            _processQueuedLap();
        }

        // Drain quit notification (set when pilot stops during a master race)
        if (_quitPending) {
            _quitPending = false;
            _sendQuitNotification();
        }

    } else if (isMasterMode()) {
        _checkNodeTimeouts(currentTimeMs);

        // Deferred broadcasts — queued by the async handler, executed here on Core 0
        if (_raceStartPending) {
            _raceStartPending = false;
            _broadcastRaceStart();
        }
        if (_raceStopPending) {
            _raceStopPending = false;
            _broadcastRaceStop();
        }
    }
}

void MultiNodeManager::queueRaceStart() { _raceStartPending = true; }
void MultiNodeManager::queueRaceStop()  { _raceStopPending  = true; }

void MultiNodeManager::setTimerRunning(bool running) {
    _timerRunning = running;
    _lastHeartbeatMs = 0;  // force immediate heartbeat so master learns the new state within one process() tick
}
void MultiNodeManager::setMasterRaceActive(bool active)  { _masterRaceActive = active; }
void MultiNodeManager::setQuitPending()                  { _quitPending = true; }

void MultiNodeManager::queueLap(uint32_t lapTimeMs) {
    // Called from Core 1 — keep this fast
    if (!isClientMode()) return;
    _pendingLapTime = lapTimeMs;
    _lapPending     = true;
}

bool MultiNodeManager::isClientMode() const {
    return _conf && _conf->getNodeMode() == 2;
}

bool MultiNodeManager::isMasterMode() const {
    return _conf && _conf->getNodeMode() == 1;
}

// ──────────────────────────────────────────────────────────────────────
//  Client-side helpers
// ──────────────────────────────────────────────────────────────────────

void MultiNodeManager::_sendRegistration() {
    DynamicJsonDocument doc(256);
    doc["pilotName"]     = _conf->getPilotName()     ? _conf->getPilotName()     : "";
    doc["pilotCallsign"] = _conf->getPilotCallsign() ? _conf->getPilotCallsign() : "";
    doc["pilotColor"]    = _conf->getPilotColor();
    doc["clientIP"]      = MULTINODE_CLIENT_AP_IP;
    doc["nodeId"]        = _myNodeId;  // 0 on first registration

    String body;
    serializeJson(doc, body);

    String resp;
    if (_postToMasterWithResponse("/api/multinode/register", body, resp)) {
        DynamicJsonDocument r(128);
        if (!deserializeJson(r, resp) && r.containsKey("nodeId")) {
            uint8_t id = r["nodeId"].as<uint8_t>();
            if (id > 0) {
                _myNodeId        = id;
                _masterConnected = true;
                DEBUG("[MULTINODE] Registered as node %u\n", _myNodeId);
            }
        }
    } else {
        _masterConnected = false;
        DEBUG("[MULTINODE] Registration failed\n");
    }
}

void MultiNodeManager::_sendHeartbeat() {
    DynamicJsonDocument doc(96);
    doc["nodeId"]  = _myNodeId;
    doc["running"] = _timerRunning;
    String body;
    serializeJson(doc, body);

    if (!_postToMaster("/api/multinode/heartbeat", body)) {
        _masterConnected = false;
        DEBUG("[MULTINODE] Heartbeat failed — master unreachable\n");
    }
}

void MultiNodeManager::_processQueuedLap() {
    if (!_masterConnected || _myNodeId == 0) return;

    _localLapCount++;

    DynamicJsonDocument doc(128);
    doc["nodeId"]    = _myNodeId;
    doc["lapTimeMs"] = (uint32_t)_pendingLapTime;
    doc["lapNumber"] = _localLapCount;
    String body;
    serializeJson(doc, body);

    if (!_postToMaster("/api/multinode/lap", body)) {
        DEBUG("[MULTINODE] Lap POST to master failed\n");
    }
}

void MultiNodeManager::_sendQuitNotification() {
    if (!_masterConnected || _myNodeId == 0) return;
    DynamicJsonDocument doc(64);
    doc["nodeId"] = _myNodeId;
    String body;
    serializeJson(doc, body);
    if (_postToMaster("/api/multinode/quit", body)) {
        DEBUG("[MULTINODE] Quit notification sent to master\n");
    } else {
        DEBUG("[MULTINODE] Quit notification to master failed\n");
    }
}

bool MultiNodeManager::_postToMaster(const String& endpoint, const String& body) {
    String resp;
    return _postToMasterWithResponse(endpoint, body, resp);
}

bool MultiNodeManager::_postToMasterWithResponse(const String& endpoint, const String& body, String& response) {
    HTTPClient http;
    String url = String("http://") + MULTINODE_MASTER_IP + endpoint;
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(2000);
    int code = http.POST(body);
    if (code == 200) {
        response = http.getString();
    }
    http.end();
    return code == 200;
}

// ──────────────────────────────────────────────────────────────────────
//  Master-side handlers (called from AsyncWebServer request threads)
// ──────────────────────────────────────────────────────────────────────

bool MultiNodeManager::handleRegister(const String& pilotName, const String& pilotCallsign,
                                       uint32_t pilotColor,
                                       const String& staIP, const String& clientIP,
                                       uint8_t& assignedNodeId) {
    // Re-registration: update existing node by STA IP
    for (auto& n : _nodes) {
        if (n.staIP == staIP) {
            n.pilotName     = pilotName;
            n.pilotCallsign = pilotCallsign;
            n.pilotColor    = pilotColor;
            n.clientIP      = clientIP;
            n.lastSeen      = millis();
            n.online        = true;
            n.quitEarly     = false;  // clear DNF on re-registration
            assignedNodeId  = n.nodeId;
            DEBUG("[MULTINODE] Re-registered node %u: %s\n", n.nodeId, pilotName.c_str());
            return true;
        }
    }

    if (_nodes.size() >= MULTINODE_MAX_NODES) {
        DEBUG("[MULTINODE] Max nodes reached — rejecting %s\n", staIP.c_str());
        return false;
    }

    // Assign first available node ID (1–7)
    uint8_t newId = 0;
    for (uint8_t id = 1; id <= MULTINODE_MAX_NODES; id++) {
        bool used = false;
        for (const auto& n : _nodes) {
            if (n.nodeId == id) { used = true; break; }
        }
        if (!used) { newId = id; break; }
    }
    if (newId == 0) return false;

    NodeInfo n;
    n.nodeId        = newId;
    n.pilotName     = pilotName;
    n.pilotCallsign = pilotCallsign;
    n.pilotColor    = pilotColor;
    n.staIP         = staIP;
    n.clientIP      = clientIP;
    n.lastSeen      = millis();
    n.online        = true;
    n.lapCount      = 0;
    _nodes.push_back(n);

    assignedNodeId = newId;
    DEBUG("[MULTINODE] New node %u: %s @ %s\n", newId, pilotName.c_str(), staIP.c_str());
    return true;
}

bool MultiNodeManager::handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            MultiNodeLap lap;
            lap.lapTimeMs = lapTimeMs;
            lap.timestamp = millis();
            lap.lapNumber = lapNumber;
            n.laps.push_back(lap);
            if (n.laps.size() > 50) n.laps.erase(n.laps.begin());
            n.lapCount = lapNumber;
            n.lastSeen = millis();
            return true;
        }
    }
    return false;
}

bool MultiNodeManager::handleHeartbeat(uint8_t nodeId, bool running, bool& stateChanged) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            stateChanged = (n.running != running);
            n.running  = running;
            n.lastSeen = millis();
            n.online   = true;
            return true;
        }
    }
    stateChanged = false;
    return false;
}

bool MultiNodeManager::handleQuit(uint8_t nodeId) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            n.quitEarly = true;
            n.running   = false;
            DEBUG("[MULTINODE] Node %u (%s) quit early\n", n.nodeId, n.pilotName.c_str());
            return true;
        }
    }
    return false;
}

void MultiNodeManager::clearAllLaps() {
    for (auto& n : _nodes) {
        n.laps.clear();
        n.lapCount  = 0;
        n.running   = false;
        n.quitEarly = false;
    }
    DEBUG("[MULTINODE] All laps cleared\n");
}

void MultiNodeManager::_checkNodeTimeouts(uint32_t currentTimeMs) {
    for (auto& n : _nodes) {
        if (n.online && (currentTimeMs - n.lastSeen) > MULTINODE_NODE_TIMEOUT_MS) {
            n.online = false;
            DEBUG("[MULTINODE] Node %u (%s) timed out\n", n.nodeId, n.pilotName.c_str());
        }
    }
}

String MultiNodeManager::getNodesToJson() const {
    DynamicJsonDocument doc(8192);  // enlarged for full lap history
    JsonArray arr = doc.createNestedArray("nodes");
    for (const auto& n : _nodes) {
        JsonObject o        = arr.createNestedObject();
        o["nodeId"]         = n.nodeId;
        o["pilotName"]      = n.pilotName;
        o["pilotCallsign"]  = n.pilotCallsign;
        o["pilotColor"]     = n.pilotColor;
        o["online"]         = n.online;
        o["running"]        = n.running;
        o["quitEarly"]      = n.quitEarly;
        o["lapCount"]       = n.lapCount;
        o["clientIP"]       = n.clientIP;
        JsonArray laps      = o.createNestedArray("laps");
        // Send all stored laps so the race view can compute full stats
        for (size_t i = 0; i < n.laps.size(); i++) {
            JsonObject l    = laps.createNestedObject();
            l["lapNumber"]  = n.laps[i].lapNumber;
            l["lapTimeMs"]  = n.laps[i].lapTimeMs;
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void MultiNodeManager::_broadcastRaceStart() {
    for (const auto& n : _nodes) {
        if (!n.online || n.clientIP.isEmpty()) continue;
        HTTPClient http;
        String url = "http://" + n.clientIP + "/timer/masterStart";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race start → node %u: HTTP %d\n", n.nodeId, code);
        }
        vTaskDelay(1);  // yield between nodes so async_tcp stays fed
    }
}

void MultiNodeManager::_broadcastRaceStop() {
    for (const auto& n : _nodes) {
        if (!n.online || n.clientIP.isEmpty()) continue;
        HTTPClient http;
        String url = "http://" + n.clientIP + "/timer/masterStop";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race stop → node %u: HTTP %d\n", n.nodeId, code);
        }
        vTaskDelay(1);
    }
}

String MultiNodeManager::getMasterStatusJson() const {
    DynamicJsonDocument doc(128);
    doc["connected"] = _masterConnected;
    doc["nodeId"]    = _myNodeId;
    doc["masterIP"]  = MULTINODE_MASTER_IP;
    String out;
    serializeJson(doc, out);
    return out;
}

String MultiNodeManager::scanForNodesJson() {
    // Note: Scanning temporarily interrupts the STA connection; use with care.
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("networks");

    int n = WiFi.scanNetworks(false, false, false, 300);
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.startsWith("FPVRaceOne_")) {
            JsonObject o = arr.createNestedObject();
            o["ssid"]    = ssid;
            o["rssi"]    = WiFi.RSSI(i);
            o["channel"] = WiFi.channel(i);
        }
    }
    WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    return out;
}
