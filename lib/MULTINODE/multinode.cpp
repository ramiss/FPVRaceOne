#include "multinode.h"
#include "config.h"
#include "debug.h"
#include "led.h"
#include "fpv_webserver.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_mac.h>

// Defined in webserver.cpp.  Format: "FPVRaceOne_<6-hex>".
extern String wifi_ap_ssid;

// Map nodeId (1..7 for clients, 0 for the master itself, max 26) to the same
// slot letter the browser UI uses (A..G in normal operation).  Matches
// _slotLetter() in data/script.js so trace lines line up with what the
// director sees on screen.  Returns '?' for out-of-range ids (including 0
// for master, which we don't expect to appear in per-client log lines).
static inline char slotLetter(uint8_t nodeId) {
    return (nodeId >= 1 && nodeId <= 26) ? (char)('A' + nodeId - 1) : '?';
}

// Read the device's STA MAC directly from eFuse, bypassing WiFi.macAddress().
// init() runs before any WiFi.mode(...) call, so WiFi is in WIFI_MODE_NULL —
// in that state Arduino-ESP32's WiFi.macAddress() takes a code path that has
// returned the SAME string on distinct ESP32-C6 chips (same family of bug as
// the SSID issue we fixed with esp_read_mac).  When two clients report the
// same MAC, handleRegister's macMatch path overwrites slot 1 for the second
// client instead of opening slot 2.
// Reading eFuse directly is what we already do for the SoftAP MAC and is
// guaranteed to return this chip's actual unique STA MAC.
static String _readStaMacString() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void MultiNodeManager::init(Config* config, Led* led, Webserver* webserver) {
    _conf                = config;
    _led                 = led;
    _webserver           = webserver;
    _masterConnected     = false;
    // Restore the last slot a master assigned to us so registration can request
    // it.  0 means "no preference yet — let the master pick first available".
    _myNodeId            = config ? config->getMnPreferredSlot() : 0;
    _lapPending          = false;
    _localLapCount       = 0;
    _lastHeartbeatMs     = 0;
    _lastRegistrationMs  = 0;
    _heartbeatFailCount  = 0;
    _masterRaceActive         = false;
    _timerRunning             = false;
    _reconnectPausedUntilMs   = 0;
    _racePreArmPending   = false;
    _raceStartPending    = false;
    _raceStopPending     = false;
    _quitPending         = false;
    _myMacAddress        = _readStaMacString();
    DEBUG("[MULTINODE] My STA MAC: %s\n", _myMacAddress.c_str());
    _nodes.clear();
}

void MultiNodeManager::process(uint32_t currentTimeMs) {
    if (!_conf) return;

    // While paused for OTA we skip all multinode networking.  Safety auto-
    // resume covers the case where the caller (frontend / OTA) forgot to
    // resume — e.g. the user closed the browser tab mid-flow.
    if (_pausedForOta) {
        if (_pauseExpiresAtMs > 0 && currentTimeMs > _pauseExpiresAtMs) {
            DEBUG("[MULTINODE] OTA pause timed out — auto-resuming\n");
            resumeFromOta();
        } else {
            return;
        }
    }

    if (isClientMode()) {
        bool prevConnected = _masterConnected;
        bool pauseActive   = (_reconnectPausedUntilMs > 0 && currentTimeMs < _reconnectPausedUntilMs);

        if (!pauseActive && WiFi.status() != WL_CONNECTED) {
            if (_masterConnected) {
                _masterConnected = false;
                DEBUG("[MULTINODE] STA disconnected from master\n");
            }
        } else if (!pauseActive) {
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

        // Periodic "who's actually connected right now" summary.  Every 10 s
        // log a single line listing slot=name for every online node — replaces
        // the firehose of per-heartbeat re-registration lines that used to
        // confirm fleet health.  Stack-allocated buffer (no heap pressure).
        static uint32_t lastNodeSummaryMs = 0;
        if (currentTimeMs - lastNodeSummaryMs > 10000) {
            lastNodeSummaryMs = currentTimeMs;
            char buf[256];
            size_t pos = (size_t)snprintf(buf, sizeof(buf), "[MULTINODE] connected:");
            bool any = false;
            for (const auto& n : _nodes) {
                if (!n.online) continue;
                any = true;
                int w = snprintf(buf + pos, sizeof(buf) - pos, " %c=%s",
                                 slotLetter(n.nodeId), n.pilotName.c_str());
                if (w < 0) break;
                pos += (size_t)w;
                if (pos >= sizeof(buf) - 8) break;   // leave room for trailing " (none)" / newline
            }
            if (!any) snprintf(buf + pos, sizeof(buf) - pos, " (none)");
            DEBUG("%s\n", buf);
        }

        // Periodic director-state resync — every 10 s, force a rebroadcast
        // even if no state change has fired.  Fixes: a client's Multi Race
        // browser opens after the last state-driven push and would otherwise
        // never see the current node list until something happened.  Uses
        // the existing pushMultiNodeState -> queueDirectorStateBroadcast
        // path so the fanout still goes through the same throttle
        // (MIN_DIRECTOR_BROADCAST_INTERVAL_MS) and the same sequential
        // one-client-at-a-time HTTPClient loop as event-driven pushes.
        static uint32_t lastDirectorHeartbeatMs = 0;
        if (currentTimeMs - lastDirectorHeartbeatMs > 10000 && _webserver) {
            lastDirectorHeartbeatMs = currentTimeMs;
            _webserver->pushMultiNodeState();
        }

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
        // Throttled director-state broadcast.  When the pending flag is set
        // but it's been less than MIN_DIRECTOR_BROADCAST_INTERVAL_MS since
        // the previous fanout, leave it pending — it'll be picked up on a
        // later tick once the interval has elapsed.  The latest payload from
        // _directorStatePayload is what actually ships, so coalesced bursts
        // still publish the final state without burning Core 0 on every
        // one of them.
        if (_directorStateBroadcastPending &&
            (currentTimeMs - _lastDirectorBroadcastMs) >= MIN_DIRECTOR_BROADCAST_INTERVAL_MS) {
            _directorStateBroadcastPending = false;
            _lastDirectorBroadcastMs       = currentTimeMs;
            _broadcastDirectorState();
        }
        if (_recruitPending) {
            _recruitPending = false;
            bool force = _recruitForce;
            _runRecruitJob(force);
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

void MultiNodeManager::pauseReconnect(uint32_t durationMs) {
    _reconnectPausedUntilMs = millis() + durationMs;
    if (_masterConnected) {
        _masterConnected = false;
        _clientStateChangedFlag = true;  // notify browser immediately
    }
}

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
//  OTA pause / resume
// ──────────────────────────────────────────────────────────────────────

void MultiNodeManager::pauseForOta() {
    if (_pausedForOta) return;
    _savedNodeModeForOta = _conf ? _conf->getNodeMode() : 0;
    if (_savedNodeModeForOta == 0) return;   // already single — nothing to pause

    _pausedForOta     = true;
    _pauseExpiresAtMs = millis() + OTA_PAUSE_TIMEOUT_MS;
    DEBUG("[MULTINODE] Paused for OTA (was mode %u)\n", _savedNodeModeForOta);

    if (_savedNodeModeForOta == 2) {
        // Client mode: free the STA radio so OTA can associate with home WiFi.
        // The ESP32 has one STA — without this disconnect, WiFi.begin(homeSsid)
        // would have to wait for the existing association to drop on its own.
        WiFi.disconnect(false);   // keep stored creds; just drop the association
        _masterConnected     = false;
        _heartbeatFailCount  = 0;
    }
    // Master mode: leave the AP up so the director's browser stays connected at
    // 192.168.5.1.  Active clients will hit master-side handlers that early-
    // return false (see handleRegister/handleHeartbeat/handleLap/handleQuit),
    // their TCP connections close fast, and the freed slots are available for
    // the outbound HTTPS call to GitHub.
}

void MultiNodeManager::resumeFromOta() {
    if (!_pausedForOta) return;
    DEBUG("[MULTINODE] Resumed from OTA (restoring mode %u)\n", _savedNodeModeForOta);
    _pausedForOta     = false;
    _pauseExpiresAtMs = 0;
    // process() picks back up naturally on the next tick: client re-associates
    // to master, master starts heartbeating its clients again.
}

bool MultiNodeManager::wouldOtaDisruptMultinode() const {
    if (!_conf) return false;
    if (isClientMode()) return true;
    if (isMasterMode()) {
        for (const auto& n : _nodes) {
            if (n.online) return true;
        }
    }
    return false;
}

String MultiNodeManager::getOtaDisruptionMessage() const {
    if (!_conf) return String();
    if (isClientMode()) {
        return String(F(
                 "This device is currently a client of a master race director. "
                 "Checking for updates will disconnect this device from the master "
                 "for the duration of the check (and the update, if you apply one). "
                 "The master will mark you offline. You will reconnect automatically "
                 "when the check or update finishes - or if you cancel."));
    }
    if (isMasterMode()) {
        int onlineCount = 0;
        for (const auto& n : _nodes) if (n.online) onlineCount++;
        if (onlineCount == 0) return String();
        String s = onlineCount == 1
            ? String(F("There is 1 pilot currently connected to this master. "))
            : String(onlineCount) + F(" pilots are currently connected to this master. ");
        s += F("Checking for updates will pause the mesh: connected pilots will be "
               "temporarily disconnected and will reconnect automatically when the "
               "check or update finishes - or if you cancel.");
        return s;
    }
    return String();
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
    doc["enterRssi"]     = _conf->getEnterRssi();
    doc["exitRssi"]      = _conf->getExitRssi();
    // Last 6 hex chars of our own AP SSID (FPVRaceOne_xxxxxx).  Master shows
    // this in the Edit Pilot modal so the director can correlate a slot to a
    // physical unit by its broadcast SSID.
    doc["apSuffix"]      = (wifi_ap_ssid.length() >= 6)
                              ? wifi_ap_ssid.substring(wifi_ap_ssid.length() - 6)
                              : String();

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
                DEBUG("[MULTINODE] Registered as node %c\n", slotLetter(_myNodeId));
                // Persist the assigned slot so we can request it again on the
                // next boot.  Only writes when the slot actually changed —
                // setMnPreferredSlot is a no-op if the stored value matches.
                if (_conf && _conf->getMnPreferredSlot() != id) {
                    _conf->setMnPreferredSlot(id);
                    _conf->write();
                    DEBUG("[MULTINODE] Persisted preferred slot %u\n", id);
                }
            }
        }
    } else {
        _masterConnected = false;
        DEBUG("[MULTINODE] Registration failed\n");
    }
}

void MultiNodeManager::_sendHeartbeat() {
    DynamicJsonDocument doc(192);
    doc["nodeId"]      = _myNodeId;
    doc["mac"]         = _myMacAddress;   // master verifies this matches its stored MAC for nodeId
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
    // Read the body for any HTTP code (positive — meaning the server actually
    // responded).  Heartbeat callers MUST see the "NOT_FOUND" body on a 404
    // to trigger immediate re-registration with nodeId=0; without it the
    // recovery path is dead code and the client spends 10+ seconds in
    // _heartbeatFailCount limbo before noticing.
    if (code > 0) {
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
                                       uint8_t enterRssi, uint8_t exitRssi,
                                       const String& staIP, const String& clientIP,
                                       const String& macAddress,
                                       const String& apSuffix,
                                       uint8_t& assignedNodeId,
                                       bool& stateChanged) {
    if (_pausedForOta) { stateChanged = false; return false; }   // master is OTA-busy
    stateChanged = false;  // default — set true only when there's real new info
    // Re-registration matching.  Only MAC and nodeId are reliable identifiers —
    // STA IPs can be reassigned by DHCP to a different physical client, so a
    // bare IP match would silently merge two distinct units into the same slot
    // (observed bug: second client overwrote slot 1 because both ended up at
    // the same DHCP-assigned address).  We also block any match when the
    // incoming MAC explicitly disagrees with the stored MAC, so a stale nodeId
    // never lets a different unit hijack an existing slot.
    uint8_t incomingNodeId = assignedNodeId;
    for (auto& n : _nodes) {
        bool macKnown    = macAddress.length() > 0 && n.macAddress.length() > 0;
        bool macMatch    = macKnown && macAddress == n.macAddress;
        bool macMismatch = macKnown && macAddress != n.macAddress;
        bool idMatch     = incomingNodeId > 0 && n.nodeId == incomingNodeId;
        if (macMatch || (idMatch && !macMismatch)) {
            // Detect whether this is a true re-registration (something the
            // director would care about changed) or just the periodic 5 s
            // keep-alive from a steady client.  Keep-alives outnumber real
            // re-registrations ~100:1 — logging both at the same volume
            // turned the serial monitor into noise that buried real events
            // (timeouts, crashes, the [HEAP] watermark).
            bool wasOffline = !n.online;
            bool changed =
                (n.pilotName    != pilotName)    ||
                (n.pilotColor   != pilotColor)   ||
                (n.bandIndex    != bandIndex)    ||
                (n.channelIndex != channelIndex) ||
                (n.frequency    != frequency)    ||
                (n.enterRssi    != enterRssi)    ||
                (n.exitRssi     != exitRssi)     ||
                (n.clientIP     != clientIP)     ||
                (n.staIP        != staIP)        ||
                (apSuffix.length() > 0 && n.apSuffix != apSuffix);

            n.pilotName     = pilotName;
            n.pilotColor    = pilotColor;
            n.bandIndex     = bandIndex;
            n.channelIndex  = channelIndex;
            n.frequency     = frequency;
            n.enterRssi     = enterRssi;
            n.exitRssi      = exitRssi;
            n.clientIP      = clientIP;
            n.staIP         = staIP;
            if (macAddress.length() > 0) n.macAddress = macAddress;
            if (apSuffix.length() > 0)   n.apSuffix   = apSuffix;
            n.lastSeen      = millis();
            n.online        = true;
            assignedNodeId  = n.nodeId;
            // Loud message for the events that warrant operator attention:
            // a node that was offline coming back, OR any identity field
            // change.  Silent steady-state keep-alives.
            if (wasOffline || changed) {
                DEBUG("[MULTINODE] %s node %c (mac=%s ip=%s suffix=%s): %s\n",
                      wasOffline ? "Re-registered" : "Updated",
                      slotLetter(n.nodeId), n.macAddress.c_str(), staIP.c_str(), n.apSuffix.c_str(), pilotName.c_str());
                // Same gate flags the broadcast.  THIS is the fix for the
                // Core-0 stall storm: with 7 clients each re-registering
                // every 5 s, the caller used to fire pushMultiNodeState()
                // 1.4 times/sec.  Each broadcast iterates 7 clients with an
                // 800 ms HTTPClient timeout per POST, so one slow client
                // alone burns 600-800 ms of Core 0 — observable in the
                // [CORE0] sub-call log as "multinode blocked for ~750 ms"
                // back-to-back forever.  Skipping the broadcast when nothing
                // actually changed drops the trigger rate to ~zero in
                // steady state.
                stateChanged = true;
            }
            return true;
        }
    }

    if (_nodes.size() >= MULTINODE_MAX_NODES) {
        DEBUG("[MULTINODE] Max nodes reached — rejecting %s\n", staIP.c_str());
        return false;
    }

    // Slot assignment.  Honour the client's requested slot first if it's valid
    // and currently free — clients persist their last-assigned slot in EEPROM
    // and send it back here as nodeId so they can reclaim it across reboots.
    // If the requested slot is taken (or no preference was sent), fall back to
    // the first-available slot 1..7.
    uint8_t newId = 0;
    if (incomingNodeId >= 1 && incomingNodeId <= MULTINODE_MAX_NODES) {
        bool used = false;
        for (const auto& n : _nodes) {
            if (n.nodeId == incomingNodeId) { used = true; break; }
        }
        if (!used) newId = incomingNodeId;
    }
    if (newId == 0) {
        for (uint8_t id = 1; id <= MULTINODE_MAX_NODES; id++) {
            bool used = false;
            for (const auto& n : _nodes) {
                if (n.nodeId == id) { used = true; break; }
            }
            if (!used) { newId = id; break; }
        }
    }
    if (newId == 0) return false;

    NodeInfo n;
    n.nodeId        = newId;
    n.pilotName     = pilotName;
    n.pilotColor    = pilotColor;
    n.bandIndex     = bandIndex;
    n.channelIndex  = channelIndex;
    n.frequency     = frequency;
    n.enterRssi     = enterRssi;
    n.exitRssi      = exitRssi;
    n.staIP         = staIP;
    n.clientIP      = clientIP;
    n.macAddress    = macAddress;
    n.apSuffix      = apSuffix;
    n.lastSeen      = millis();
    n.online        = true;
    n.lapCount      = 0;
    _nodes.push_back(n);

    assignedNodeId = newId;
    DEBUG("[MULTINODE] New node %c (mac=%s ip=%s suffix=%s): %s\n",
          slotLetter(newId), macAddress.c_str(), staIP.c_str(), apSuffix.c_str(), pilotName.c_str());
    stateChanged = true;   // brand-new slot occupant — broadcast so all clients learn about it
    return true;
}

bool MultiNodeManager::handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber) {
    if (_pausedForOta) return false;   // master is OTA-busy
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

bool MultiNodeManager::handleHeartbeat(uint8_t nodeId, const String& macAddress,
                                        bool running, bool independent, bool skipEnabled,
                                        bool& stateChanged) {
    if (_pausedForOta) { stateChanged = false; return false; }   // master is OTA-busy
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            // MAC verification: if the incoming heartbeat carries a MAC AND it
            // doesn't match the stored MAC for this slot, this is a stale
            // client whose nodeId collides with the current slot occupant.
            // Reject so the client falls into the NOT_FOUND fast-recovery
            // path and re-registers with nodeId=0.  Empty incoming MAC is
            // tolerated (legacy clients pre-this-fix) so a mixed fleet doesn't
            // break — but the stored MAC for a "live" slot has been non-empty
            // since the slot was registered, so legacy heartbeats from the
            // _correct_ device still match the nodeId-only path.
            if (macAddress.length() > 0 && n.macAddress.length() > 0 &&
                macAddress != n.macAddress) {
                DEBUG("[MULTINODE] Heartbeat MAC mismatch on slot %c (stored=%s, incoming=%s) — rejecting\n",
                      slotLetter(n.nodeId), n.macAddress.c_str(), macAddress.c_str());
                stateChanged = false;
                return false;
            }
            // Capture the offline-to-online edge before the write so we can
            // log the recovery.  Without this, a node that timed out and
            // then resumed via a heartbeat (the common case after a brief
            // RF blip) would silently flip back to online and the serial
            // trace looked like the node had never recovered — which made
            // every transient timeout look permanent in the log.
            bool wasOffline = !n.online;
            stateChanged  = (n.running != running || n.independent != independent || n.skipEnabled != skipEnabled);
            n.running     = running;
            n.independent = independent;
            n.skipEnabled = skipEnabled;
            n.lastSeen    = millis();
            n.online      = true;
            if (wasOffline) {
                DEBUG("[MULTINODE] Node %c (%s) reconnected via heartbeat\n",
                      slotLetter(n.nodeId), n.pilotName.c_str());
            }
            return true;
        }
    }
    stateChanged = false;
    return false;
}

bool MultiNodeManager::handleQuit(uint8_t nodeId) {
    if (_pausedForOta) return false;   // master is OTA-busy
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            n.quitEarly = true;
            n.running   = false;
            DEBUG("[MULTINODE] Node %c (%s) quit early\n", slotLetter(n.nodeId), n.pilotName.c_str());
            return true;
        }
    }
    return false;
}

void MultiNodeManager::clearAllLaps() {
    for (auto& n : _nodes) {
        n.laps.clear();
        n.lapCount               = 0;
        n.running                = false;
        n.quitEarly              = false;
        n.excludedFromCurrentRace = false;
    }
    _excludeNodes.clear();
    DEBUG("[MULTINODE] All laps cleared\n");
}

void MultiNodeManager::_checkNodeTimeouts(uint32_t /*currentTimeMs*/) {
    // The `currentTimeMs` parameter is a TRAP and we deliberately ignore it.
    // It was captured at the start of parallelTask's iteration in main.cpp,
    // BEFORE eight other sub-calls (buzzer, led, ota, webUpdate, usb,
    // eeprom, rxFreq, webhooks) ran in front of us.  During those tens of
    // milliseconds, AsyncTCP can — and does, at every heartbeat tick —
    // preempt parallelTask and run handleHeartbeat / handleRegister, both
    // of which set n.lastSeen = millis() with the actual current millis,
    // which is by then ahead of the snapshot we were passed.  Computing
    // (snapshot - lastSeen) with uint32_t then underflows to ~4 billion
    // and we falsely declare the node timed out — milliseconds after it
    // checked in.  This was the cause of the "Node X reconnected via
    // heartbeat / Node X timed out within 2 ms" pairs in serial traces.
    //
    // Fix: refresh `now` HERE so it can't be staler than any concurrent
    // lastSeen write, AND defensively skip any node whose lastSeen still
    // somehow lands ahead of `now` (impossible after this refresh, but
    // belt-and-braces — single read, no division, basically free).
    uint32_t now = millis();
    bool anyWentOffline = false;
    for (auto& n : _nodes) {
        if (!n.online) continue;
        uint32_t lastSeen = n.lastSeen;
        if (lastSeen > now) continue;                       // race: lastSeen newer than `now`
        if ((now - lastSeen) <= MULTINODE_NODE_TIMEOUT_MS) continue;
        n.online  = false;
        n.running = false;
        anyWentOffline = true;
        DEBUG("[MULTINODE] Node %c (%s) timed out\n", slotLetter(n.nodeId), n.pilotName.c_str());
    }
    // Notify Race View browsers that a peer just went offline — otherwise
    // the other clients keep rendering a disconnected pilot as still
    // online until the next state-change or 10 s heartbeat push.
    if (anyWentOffline && _webserver) {
        _webserver->pushMultiNodeState();
    }
    // Nodes are never auto-removed — use removeNode() to manually free a slot.
}

bool MultiNodeManager::moveNode(uint8_t fromNodeId, uint8_t toSlot) {
    if (!isMasterMode()) return false;
    if (fromNodeId == 0 || toSlot == 0) return false;  // master can't be moved or be a target
    if (fromNodeId > MULTINODE_MAX_NODES || toSlot > MULTINODE_MAX_NODES) return false;
    if (fromNodeId == toSlot) return true;  // no-op

    NodeInfo* source = nullptr;
    NodeInfo* target = nullptr;
    for (auto& n : _nodes) {
        if (n.nodeId == fromNodeId) source = &n;
        if (n.nodeId == toSlot)     target = &n;
    }
    if (!source) return false;

    // POST /multinode/setSlot?slot=N to the given client IP.  Best-effort:
    // a failed POST still updates the master's _nodes list so the UI is
    // consistent; the client will re-sync to the new slot on its next
    // registration (it uses macMatch in handleRegister to find its NodeInfo
    // by MAC, regardless of slot id mismatches).
    auto sendSetSlot = [](const String& staIP, uint8_t newSlot) -> bool {
        if (staIP.isEmpty()) return false;
        HTTPClient http;
        String url = "http://" + staIP + "/multinode/setSlot?slot=" + String(newSlot);
        if (!http.begin(url)) return false;
        http.setTimeout(800);
        int code = http.POST("");
        http.end();
        return code == 200;
    };

    sendSetSlot(source->staIP, toSlot);
    if (target) {
        sendSetSlot(target->staIP, fromNodeId);
        target->nodeId = fromNodeId;
    }
    source->nodeId = toSlot;
    return true;
}

void MultiNodeManager::touchNode(uint8_t nodeId) {
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            n.lastSeen = millis();
            return;
        }
    }
}

bool MultiNodeManager::removeNode(uint8_t nodeId) {
    for (auto it = _nodes.begin(); it != _nodes.end(); ++it) {
        if (it->nodeId == nodeId) {
            DEBUG("[MULTINODE] Node %c (%s) manually removed\n", slotLetter(it->nodeId), it->pilotName.c_str());
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
            DEBUG("[MULTINODE] Node %c pilot updated locally: %s\n", slotLetter(nodeId), name.c_str());
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

void MultiNodeManager::_broadcastRacePreArm() {
    for (const auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        bool excluded = false;
        for (uint8_t id : _excludeNodes) { if (id == n.nodeId) { excluded = true; break; } }
        if (excluded) {
            DEBUG("[MULTINODE] Race pre-arm → node %c (%s): SKIPPED (excluded)\n", slotLetter(n.nodeId), n.staIP.c_str());
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterPreArm";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race pre-arm → node %c (%s): HTTP %d\n", slotLetter(n.nodeId), n.staIP.c_str(), code);
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
            DEBUG("[MULTINODE] Race start → node %c (%s): SKIPPED (excluded)\n", slotLetter(n.nodeId), n.staIP.c_str());
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStart";
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race start → node %c (%s): HTTP %d\n", slotLetter(n.nodeId), n.staIP.c_str(), code);
        }
        vTaskDelay(1);  // yield between nodes so async_tcp stays fed
    }
    _excludeNodes.clear();  // consumed — reset for next race
}

void MultiNodeManager::setPrearmPhase(bool active) {
    _prearmPhase        = active;
    _prearmPhaseSetAtMs = millis();
}

bool MultiNodeManager::getPrearmPhase() const {
    if (!_prearmPhase) return false;
    if (millis() - _prearmPhaseSetAtMs > PREARM_PHASE_TIMEOUT_MS) return false;
    return true;
}

void MultiNodeManager::queueDirectorStateBroadcast(const String& payload) {
    if (!isMasterMode()) return;
    _directorStatePayload          = payload;     // overwrites any pending payload — newest wins
    _directorStateBroadcastPending = true;
}

void MultiNodeManager::queueRecruit(bool force) {
    if (!isMasterMode()) return;
    _recruitForce          = force;
    _recruitSummary        = RecruitSummary{};
    _recruitSummary.inProgress = true;
    _recruitPending        = true;
}

void MultiNodeManager::_runRecruitJob(bool force) {
    DEBUG("[RECRUIT] Starting (force=%d)\n", force ? 1 : 0);

    // Hold the LED solid-on for the entire procedure so the director sees the
    // master is busy and knows the AP is temporarily down.  on(0) means no
    // auto-off — we explicitly clear it at the end.
    if (_led) _led->on(0);

    // Snapshot our own SSID so we can both tell targets which master to join,
    // and skip our own AP during the scan.
    String ownSsid = wifi_ap_ssid;

    // Drop the AP entirely.  Any connected director phone or pilot client will
    // lose its connection — by design, the user was warned.
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    WiFi.mode(WIFI_STA);
    vTaskDelay(50 / portTICK_PERIOD_MS);

    int n = WiFi.scanNetworks(false, false, false, 300);
    DEBUG("[RECRUIT] Scan found %d networks total\n", n);

    uint8_t found = 0, recruited = 0, skipped = 0, failed = 0;

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.startsWith("FPVRaceOne_")) continue;
        if (ssid == ownSsid) continue;
        found++;

        DEBUG("[RECRUIT] Target: %s (RSSI %d, ch %d)\n", ssid.c_str(), WiFi.RSSI(i), WiFi.channel(i));

        WiFi.begin(ssid.c_str(), "fpvraceone");
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        if (WiFi.status() != WL_CONNECTED) {
            DEBUG("[RECRUIT] Could not associate with %s\n", ssid.c_str());
            failed++;
            WiFi.disconnect(true);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // Target may be running as single (192.168.4.1) or master (192.168.5.1).
        // Try single first — that's the common case.  If /api/mode returns mode 1
        // we know it's a master, switch to that IP.
        String targetIp = "192.168.4.1";
        int    targetMode = -1;
        {
            HTTPClient http;
            String url = "http://" + targetIp + "/api/mode";
            if (http.begin(url)) {
                http.setTimeout(2000);
                int code = http.GET();
                if (code == 200) {
                    String resp = http.getString();
                    DynamicJsonDocument d(256);
                    if (!deserializeJson(d, resp)) targetMode = d["nodeMode"] | -1;
                }
                http.end();
            }
            if (targetMode == -1) {
                targetIp = "192.168.5.1";
                HTTPClient http2;
                if (http2.begin("http://" + targetIp + "/api/mode")) {
                    http2.setTimeout(2000);
                    int code = http2.GET();
                    if (code == 200) {
                        String resp = http2.getString();
                        DynamicJsonDocument d(256);
                        if (!deserializeJson(d, resp)) targetMode = d["nodeMode"] | -1;
                    }
                    http2.end();
                }
            }
        }

        if (targetMode == -1) {
            DEBUG("[RECRUIT] %s: /api/mode unreachable\n", ssid.c_str());
            failed++;
            WiFi.disconnect(true);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // Filter: by default only recruit single-mode units (mode 0).  When force
        // is true, recruit regardless of current mode — including other masters
        // and clients already bound to a different master.
        if (!force && targetMode != 0) {
            DEBUG("[RECRUIT] %s: mode=%d, skipping (force=false)\n", ssid.c_str(), targetMode);
            skipped++;
            WiFi.disconnect(true);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // POST /config with the patch — nodeMode=2 + masterSSID=ours.  /config
        // already supports partial updates and persists immediately.
        DynamicJsonDocument patch(192);
        patch["nodeMode"]   = 2;
        patch["masterSSID"] = ownSsid;
        String body;
        serializeJson(patch, body);

        bool configOk = false;
        {
            HTTPClient http;
            if (http.begin("http://" + targetIp + "/config")) {
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(3000);
                int code = http.POST(body);
                http.end();
                configOk = (code == 200);
            }
        }
        if (!configOk) {
            DEBUG("[RECRUIT] %s: /config POST failed\n", ssid.c_str());
            failed++;
            WiFi.disconnect(true);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // Tell the target to reboot so the new nodeMode takes effect.  We don't
        // care about the response — the unit will tear down WiFi mid-request.
        {
            HTTPClient http;
            if (http.begin("http://" + targetIp + "/reboot")) {
                http.setTimeout(1000);
                http.POST("");
                http.end();
            }
        }

        DEBUG("[RECRUIT] %s recruited\n", ssid.c_str());
        recruited++;
        WiFi.disconnect(true);
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    WiFi.scanDelete();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Bring our own AP back up via the webserver helper.  startServices() is
    // idempotent so this restores the listening sockets cleanly.
    if (_webserver) _webserver->startAP();

    // Restore the normal LED state — handleLed() runs on Core 0 and turns the
    // LED off after this on() pulse expires, leaving the device in its
    // idle/awaiting-clients state.
    if (_led) _led->on(500);

    _recruitSummary.valid      = true;
    _recruitSummary.inProgress = false;
    _recruitSummary.found      = found;
    _recruitSummary.recruited  = recruited;
    _recruitSummary.skipped    = skipped;
    _recruitSummary.failed     = failed;

    DEBUG("[RECRUIT] Done — found=%u recruited=%u skipped=%u failed=%u\n",
          found, recruited, skipped, failed);
}

void MultiNodeManager::_broadcastDirectorState() {
    if (_directorStatePayload.isEmpty()) return;
    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        HTTPClient http;
        String url = "http://" + n.staIP + "/api/multinode/directorState";
        if (http.begin(url)) {
            http.setTimeout(300);
            http.addHeader("Content-Type", "application/json");
            http.POST(_directorStatePayload);
            http.end();
        }
        vTaskDelay(1);
    }
    _directorStatePayload = String();
}

void MultiNodeManager::_broadcastRaceStop() {
    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) { n.excludedFromCurrentRace = false; continue; }
        if (n.excludedFromCurrentRace) {
            DEBUG("[MULTINODE] Race stop → node %c (%s): SKIPPED (was excluded)\n", slotLetter(n.nodeId), n.staIP.c_str());
            n.excludedFromCurrentRace = false;
            continue;
        }
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStop";
        bool acked = false;
        if (http.begin(url)) {
            http.setTimeout(500);
            int code = http.POST("");
            http.end();
            acked = (code == 200);
            DEBUG("[MULTINODE] Race stop → node %c (%s): HTTP %d\n", slotLetter(n.nodeId), n.staIP.c_str(), code);
        }
        // Optimistically mark the client stopped on successful ack so the
        // master's view doesn't flag the last few clients as "Solo race in
        // progress" during the lag between this broadcast and their next
        // heartbeat (~2 s).  If the client somehow doesn't actually stop,
        // its next heartbeat will reassert running=true and we'll see it.
        if (acked) {
            n.running     = false;
            n.independent = false;
        }
        vTaskDelay(1);
    }
    // Republish so the master UI + connected client Race Views reflect the
    // post-stop state without waiting for the 2 s poll / next heartbeat.
    if (_webserver) _webserver->pushMultiNodeState();
}

// ── Master-discovery promiscuous sniffer ─────────────────────────────────────
// Active only during the scanForNodesJson() window (~300 ms).  Captures beacon
// and probe-response frames and records BSSIDs that carry the FPV master vendor
// IE (OUI 'F','P','V' = 0x46,0x50,0x56, type 0x01).
// Storage is a fixed-size array — no heap allocation inside the callback.

static char _masterBssids[8][18]; // "AA:BB:CC:DD:EE:FF\0" × 8 slots
static int  _masterBssidCount = 0;

static void _scanPromiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    if (!pkt) return;
    int len = (int)pkt->rx_ctrl.sig_len - 4; // exclude FCS

    // Validate length BEFORE dereferencing the payload. sig_len is attacker-controllable
    // RF input; a truncated/malformed frame could otherwise over-read p[0] (and below,
    // the fixed 36-byte mgmt header + BSSID at bytes 16-21). 37 = header(36) + 1 IE byte.
    if (len < 37) return;
    const uint8_t* p = pkt->payload;

    // Beacon = 0x80, Probe Response = 0x50 (frame-control byte masked to type+subtype)
    uint8_t fc = p[0] & 0xFC;
    if (fc != 0x80 && fc != 0x50) return;

    // 802.11 mgmt header (24 B) + timestamp(8) + beacon-interval(2) + capability(2) = 36 B
    // BSSID = Address 3, bytes 16-21
    int off = 36;
    while (off + 2 <= len) {
        uint8_t id  = p[off];
        uint8_t iel = p[off + 1];
        if (off + 2 + iel > len) break;
        if (id == 0xDD && iel >= 4 &&
            p[off+2] == 0x46 && p[off+3] == 0x50 &&
            p[off+4] == 0x56 && p[off+5] == 0x01) {
            if (_masterBssidCount < 8) {
                snprintf(_masterBssids[_masterBssidCount++], 18,
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         p[16], p[17], p[18], p[19], p[20], p[21]);
            }
            return;
        }
        off += 2 + iel;
    }
}

static bool _isMasterBssid(const String& bssid) {
    for (int i = 0; i < _masterBssidCount; i++) {
        if (bssid.equalsIgnoreCase(_masterBssids[i])) return true;
    }
    return false;
}
// ─────────────────────────────────────────────────────────────────────────────

String MultiNodeManager::scanForNodesJson() {
    // Note: Scanning temporarily interrupts the STA connection; use with care.
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.createNestedArray("networks");

    // Enable promiscuous mode to sniff vendor IEs during the scan window.
    _masterBssidCount = 0;
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(_scanPromiscuousCb);

    int n = WiFi.scanNetworks(false, false, false, 300);

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.startsWith("FPVRaceOne_")) continue;
        JsonObject o = arr.createNestedObject();
        o["ssid"]     = ssid;
        o["rssi"]     = WiFi.RSSI(i);
        o["channel"]  = WiFi.channel(i);
        o["isMaster"] = _isMasterBssid(WiFi.BSSIDstr(i));
    }
    WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    return out;
}
