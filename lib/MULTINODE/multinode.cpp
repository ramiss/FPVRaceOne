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
    _heartbeatFailCount  = 0;
    _masterRaceActive    = false;
    _timerRunning        = false;
    _racePreArmPending   = false;
    _raceStartPending    = false;
    _raceStopPending     = false;
    _quitPending         = false;
    _myMacAddress        = WiFi.macAddress();
    _nodes.clear();
}

void MultiNodeManager::process(uint32_t currentTimeMs) {
    if (!_conf) return;

    if (isClientMode()) {
        bool prevConnected = _masterConnected;

        if (WiFi.status() != WL_CONNECTED) {
            if (_masterConnected) {
                _masterConnected = false;
                DEBUG("[MULTINODE] STA disconnected from master\n");
            }
        } else {
            // Periodic registration — fast retry when disconnected, slow keep-alive when connected
            uint32_t regInterval = _masterConnected ? MULTINODE_REGISTER_INTERVAL_MS
                                                    : MULTINODE_RECONNECT_INTERVAL_MS;
            if (_lastRegistrationMs == 0 ||
                (currentTimeMs - _lastRegistrationMs) > regInterval) {
                _sendRegistration();
                _lastRegistrationMs = currentTimeMs;
            }

            // Heartbeat — on the normal interval, or immediately when forced by setTimerRunning()
            if (_masterConnected) {
                bool force = _heartbeatForcePending;
                if (force) _heartbeatForcePending = false;
                if (force || (currentTimeMs - _lastHeartbeatMs) > MULTINODE_HEARTBEAT_INTERVAL_MS) {
                    _sendHeartbeat();
                    _lastHeartbeatMs = currentTimeMs;
                }
            }
        }

        // Notify browser via SSE when connection state changes
        if (prevConnected != _masterConnected) {
            _clientStateChangedFlag = true;
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
        if (_racePreArmPending) {
            _racePreArmPending = false;
            _broadcastRacePreArm();
        }
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

void MultiNodeManager::queueRacePreArm() { _racePreArmPending = true; }
void MultiNodeManager::queueRaceStart()  { _raceStartPending  = true; }
void MultiNodeManager::queueRaceStop()   { _raceStopPending   = true; }

void MultiNodeManager::setTimerRunning(bool running) {
    _timerRunning = running;
    _heartbeatForcePending = true;  // volatile — visible to Core 0 on its next process() tick
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
    doc["pilotColor"]    = _conf->getPilotColor();
    doc["band"]          = _conf->getBandIndex();
    doc["chan"]          = _conf->getChannelIndex();
    doc["freq"]          = _conf->getFrequency();
    doc["clientIP"]      = MULTINODE_CLIENT_AP_IP;
    doc["nodeId"]        = _myNodeId;  // 0 on first registration
    doc["mac"]           = _myMacAddress;

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
    DynamicJsonDocument doc(128);
    doc["nodeId"]      = _myNodeId;
    doc["running"]     = _timerRunning;
    doc["independent"]  = _conf->getMnSkipMasterStart() && _timerRunning && !_masterRaceActive;
    doc["skipEnabled"]  = _conf->getMnSkipMasterStart();
    String body;
    serializeJson(doc, body);

    String resp;
    bool ok = _postToMasterWithResponse("/api/multinode/heartbeat", body, resp);
    if (ok) {
        _heartbeatFailCount = 0;
    } else if (resp.indexOf("NOT_FOUND") >= 0) {
        // Master doesn't know this nodeId (e.g. master rebooted) — re-register immediately
        DEBUG("[MULTINODE] Heartbeat 404 — master lost our node, re-registering\n");
        _masterConnected = false;
        _myNodeId = 0;
        _heartbeatFailCount = 0;
        _lastRegistrationMs = 0;  // fire registration on next process() tick
    } else {
        _heartbeatFailCount++;
        DEBUG("[MULTINODE] Heartbeat failed (%u/%u)\n", _heartbeatFailCount, MULTINODE_HEARTBEAT_FAIL_LIMIT);
        if (_heartbeatFailCount >= MULTINODE_HEARTBEAT_FAIL_LIMIT) {
            _masterConnected = false;
            _heartbeatFailCount = 0;
            DEBUG("[MULTINODE] Master unreachable — marking disconnected\n");
        }
    }
}

void MultiNodeManager::_processQueuedLap() {
    if (!_masterConnected || _myNodeId == 0) return;

    DynamicJsonDocument doc(128);
    doc["nodeId"]    = _myNodeId;
    doc["lapTimeMs"] = (uint32_t)_pendingLapTime;
    doc["lapNumber"] = _localLapCount;
    _localLapCount++;
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
    http.setTimeout(800);
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

bool MultiNodeManager::handleRegister(const String& pilotName,
                                       uint32_t pilotColor,
                                       uint8_t bandIndex, uint8_t channelIndex, uint16_t frequency,
                                       const String& staIP, const String& clientIP,
                                       const String& macAddress,
                                       uint8_t& assignedNodeId) {
    // Re-registration: match by MAC first (most stable), then nodeId, then STA IP
    uint8_t incomingNodeId = assignedNodeId;
    for (auto& n : _nodes) {
        bool macMatch  = macAddress.length() > 0 && macAddress == n.macAddress;
        bool idMatch   = incomingNodeId > 0 && n.nodeId == incomingNodeId;
        bool ipMatch   = n.staIP.length() > 0 && n.staIP == staIP;
        if (macMatch || idMatch || ipMatch) {
            n.pilotName     = pilotName;
            n.pilotColor    = pilotColor;
            n.bandIndex     = bandIndex;
            n.channelIndex  = channelIndex;
            n.frequency     = frequency;
            n.clientIP      = clientIP;
            n.staIP         = staIP;
            if (macAddress.length() > 0) n.macAddress = macAddress;
            n.lastSeen      = millis();
            n.online        = true;
            assignedNodeId  = n.nodeId;
            DEBUG("[MULTINODE] Re-registered node %u (%s): %s\n", n.nodeId, macAddress.c_str(), pilotName.c_str());
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
    n.pilotColor    = pilotColor;
    n.bandIndex     = bandIndex;
    n.channelIndex  = channelIndex;
    n.frequency     = frequency;
    n.staIP         = staIP;
    n.clientIP      = clientIP;
    n.macAddress    = macAddress;
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

bool MultiNodeManager::handleHeartbeat(uint8_t nodeId, bool running, bool independent, bool skipEnabled, bool& stateChanged) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            stateChanged  = (n.running != running || n.independent != independent || n.skipEnabled != skipEnabled);
            n.running     = running;
            n.independent = independent;
            n.skipEnabled = skipEnabled;
            n.lastSeen    = millis();
            n.online      = true;
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
            n.online  = false;
            n.running = false;
            DEBUG("[MULTINODE] Node %u (%s) timed out\n", n.nodeId, n.pilotName.c_str());
        }
    }
    // Nodes are never auto-removed — use removeNode() to manually free a slot.
}

bool MultiNodeManager::removeNode(uint8_t nodeId) {
    for (auto it = _nodes.begin(); it != _nodes.end(); ++it) {
        if (it->nodeId == nodeId) {
            DEBUG("[MULTINODE] Node %u (%s) manually removed\n", it->nodeId, it->pilotName.c_str());
            _nodes.erase(it);
            return true;
        }
    }
    return false;
}

bool MultiNodeManager::updateNodePilot(uint8_t nodeId, const String& name, uint32_t color) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            n.pilotName  = name;
            n.pilotColor = color;
            DEBUG("[MULTINODE] Node %u pilot updated locally: %s\n", nodeId, name.c_str());
            return true;
        }
    }
    return false;
}

bool MultiNodeManager::updateNodeChannel(uint8_t nodeId, uint8_t bandIndex, uint8_t channelIndex, uint16_t frequency) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            n.bandIndex    = bandIndex;
            n.channelIndex = channelIndex;
            n.frequency    = frequency;
            return true;
        }
    }
    return false;
}

String MultiNodeManager::getNodesToJson() const {
    DynamicJsonDocument doc(8192);  // enlarged for full lap history
    JsonArray arr = doc.createNestedArray("nodes");
    for (const auto& n : _nodes) {
        JsonObject o        = arr.createNestedObject();
        o["nodeId"]         = n.nodeId;
        o["pilotName"]      = n.pilotName;
        o["pilotColor"]     = n.pilotColor;
        o["bandIndex"]      = n.bandIndex;
        o["channelIndex"]   = n.channelIndex;
        o["frequency"]      = n.frequency;
        o["online"]         = n.online;
        o["running"]                 = n.running;
        o["quitEarly"]               = n.quitEarly;
        o["independent"]             = n.independent;
        o["skipEnabled"]             = n.skipEnabled;
        o["excludedFromCurrentRace"] = n.excludedFromCurrentRace;
        o["lapCount"]       = n.lapCount;
        o["clientIP"]       = n.clientIP;
        o["mac"]            = n.macAddress;
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

void MultiNodeManager::_broadcastRacePreArm() {
    for (const auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        bool excluded = false;
        for (uint8_t id : _excludeNodes) { if (id == n.nodeId) { excluded = true; break; } }
        if (excluded) {
            DEBUG("[MULTINODE] Race pre-arm → node %u (%s): SKIPPED (excluded)\n", n.nodeId, n.staIP.c_str());
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterPreArm";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race pre-arm → node %u (%s): HTTP %d\n", n.nodeId, n.staIP.c_str(), code);
        }
        vTaskDelay(1);
    }
}

void MultiNodeManager::setExcludeNodes(const std::vector<uint8_t>& ids) {
    _excludeNodes = ids;
}

void MultiNodeManager::_broadcastRaceStart() {
    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        // Skip excluded nodes (e.g., solo racers the director chose to leave running)
        bool excluded = false;
        for (uint8_t id : _excludeNodes) { if (id == n.nodeId) { excluded = true; break; } }
        n.excludedFromCurrentRace = excluded;
        if (excluded) {
            DEBUG("[MULTINODE] Race start → node %u (%s): SKIPPED (excluded)\n", n.nodeId, n.staIP.c_str());
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStart";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race start → node %u (%s): HTTP %d\n", n.nodeId, n.staIP.c_str(), code);
        }
        vTaskDelay(1);  // yield between nodes so async_tcp stays fed
    }
    _excludeNodes.clear();  // consumed — reset for next race
}

void MultiNodeManager::_broadcastRaceStop() {
    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) { n.excludedFromCurrentRace = false; continue; }
        if (n.excludedFromCurrentRace) {
            DEBUG("[MULTINODE] Race stop → node %u (%s): SKIPPED (was excluded)\n", n.nodeId, n.staIP.c_str());
            n.excludedFromCurrentRace = false;
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStop";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race stop → node %u (%s): HTTP %d\n", n.nodeId, n.staIP.c_str(), code);
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
