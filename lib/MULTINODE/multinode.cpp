#include "multinode.h"
#include "config.h"
#include "debug.h"
#include "laptimer.h"
#include "led.h"
#include "fpv_webserver.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include "esp_timer.h"   // µs clock domain for the sync probes (§8)
#include <math.h>        // llround() for the drift slope fit
#include "mac_util.h"

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
//
// The implementation now lives in lib/MACUTIL/mac_util.h so the webserver,
// selftest and USB transport share it — they were all still calling
// WiFi.macAddress() and reporting 00:00:00:00:00:00 in AP-only mode.
static inline String _readStaMacString() {
    return getStaMacString();
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
    // Random per boot, non-zero.  A changed bootId is how a peer distinguishes
    // "this device restarted mid-race" from "the race just started" — the two
    // are otherwise identical (count 0, crc 0) and one of them must not
    // trigger a resync (§5).
    do { _bootId = esp_random(); } while (_bootId == 0);
    _ackedSeq            = 0;
    _hasAckedSeq         = false;
    _masterOldest        = 0;
    _raceId              = 0;
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
    // Claim the full capacity ONCE, here, before any client can register.
    //
    // This is a concurrency fix, not a heap optimisation.  handleRegister()
    // runs on the async_tcp task and does _nodes.push_back(); parallelTask
    // iterates _nodes continuously (_checkNodeTimeouts, the fanout builders,
    // the resync governor, which even holds a raw NodeInfo*).  async_tcp is
    // priority 10 and preempts parallelTask at priority 2, so a push_back
    // that REALLOCATED would free the buffer parallelTask was walking —
    // a use-after-free, every time an eighth registration arrived at the
    // wrong moment.
    //
    // With capacity reserved up front and the size ceiling enforced in
    // handleRegister(), the buffer address never changes for the life of the
    // boot, so no reader can be left holding a dangling pointer.
    _nodes.reserve(MULTINODE_MAX_NODES);

    // Same reasoning, second reason: _queueLapDelta() already refuses past
    // LAPSYNC_MAX_LAP_DELTAS, so reserving that bound up front means push_back
    // never allocates on the lap path.  With -fno-exceptions an allocation
    // failure is std::terminate, not a catchable error, so the fix is to never
    // allocate late rather than to handle the failure.
    _lapDeltas.reserve(LAPSYNC_MAX_LAP_DELTAS);

    // NOTE: the clock-probe UDP socket is deliberately NOT opened here.
    // init() runs before ws.init() brings WiFi up, and AsyncUDP::listen()
    // reaches into LwIP through the tcpip task — which does not exist yet at
    // this point in boot.  Opening it here posts to an uncreated mailbox and
    // asserts inside xQueueGenericSend, boot-looping the device.
    // process() opens it lazily once the stack is actually up.
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

    // Clock-probe socket (§8), opened lazily and for BOTH roles: a master
    // consumes replies, a client answers requests.  Gated on the WiFi stack
    // existing — see the note in init() for the boot loop this avoids.
    if (!_clockUdpReady && WiFi.getMode() != WIFI_MODE_NULL) _initClockUdp();

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

        // Unacked laps can also exist without _lapPending — a lap detected
        // while the master was unreachable is already in the ring and simply
        // outstanding.  Retry them on the heartbeat cadence so a dropout heals
        // without needing a fresh crossing to trigger it.
        if (_masterConnected && _timer && !_lapPending) {
            const uint32_t lapTotal = (uint32_t)_timer->getLapTotal();
            const uint32_t nextWanted = _hasAckedSeq ? (_ackedSeq + 1) : _timer->getOldestSeq();
            if (lapTotal > nextWanted &&
                (uint32_t)(currentTimeMs - _lastHeartbeatMs) < MULTINODE_HEARTBEAT_INTERVAL_MS) {
                _processQueuedLap();
            }
        }

        // Pull our own laps back from the master, one bounded chunk per tick,
        // and only while holding the fleet-wide grant (§7).
        _processResyncPull();

        // Drain quit notification (set when pilot stops during a master race)
        if (_quitPending) {
            _quitPending = false;
            _sendQuitNotification();
        }

    } else if (isMasterMode()) {
        // FIRST, before anything iterates _nodes this tick.  Slots marked by
        // removeNode() on the async_tcp task are erased here, on the one task
        // that walks the vector, so the erase can never invalidate an
        // iterator underneath a reader.
        _reapRemovedNodes();

        _checkNodeTimeouts(currentTimeMs);

        // §7 — decide who, if anyone, may repair right now.  Detection already
        // happened in handleHeartbeatSync; this only rations the repair.
        _runResyncGovernor(currentTimeMs);

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
        //
        // ...and held off entirely while the clock-sync window is open (§8).
        // The fanout blocks this task for 400-900 ms per cycle and fires ~3
        // times across a 7 s countdown, so up to half of the "quiet" window
        // was not quiet — and probes issued alongside it were contended by
        // the very traffic that made the old measurements unusable.  A
        // countdown has nothing new to broadcast anyway.
        //
        // _directorStateBroadcastPending is deliberately LEFT SET: the payload
        // is not dropped, just deferred, and the first tick after the window
        // closes ships it.  Race-critical broadcasts (pre-arm, start, stop) do
        // not come through here at all, so none of them is delayed by this.
        const bool clockSyncOpen = ((int32_t)(_clockSyncUntilMs - currentTimeMs) > 0);
        if (!clockSyncOpen &&
            _directorStateBroadcastPending &&
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
        // Did everyone actually start?  (§8 — the GO datagram has no ack.)
        if (_startVerifyAtMs != 0 &&
            (int32_t)(currentTimeMs - _startVerifyAtMs) >= 0) {
            _verifyRaceStarts();
        }
        // Clock sync (§8).  Last in the tick, and at most one probe per tick,
        // so it can never be the thing that delays a race command.
        _runClockSync(currentTimeMs);
    }
}

// Probe scheduling.  Two cadences, one mechanism:
//
//   burst    — during the pre-arm countdown, every CLOCK_PROBE_SPACING_MS.
//              ~8 samples per node across 7 s of idle network, which is the
//              only uncontended window this system gets.
//   steady   — otherwise, one node every CLOCK_REPROBE_INTERVAL_MS.  Keeps
//              offsets warm between races and, more importantly, accumulates
//              the drift window so a slope is already fitted by the time a
//              race starts.
//
// One probe per tick either way.  Seven nodes × one small GET is still seven
// sequential HTTP round trips if you let them stack up, and sequential HTTP on
// this task is exactly what starved SSE the last time.
void MultiNodeManager::_runClockSync(uint32_t nowMs) {
    if (_nodes.empty()) return;

    const bool burst = ((int32_t)(_clockSyncUntilMs - nowMs) > 0);
    const uint32_t spacing = burst ? CLOCK_PROBE_SPACING_MS
                                   : CLOCK_REPROBE_INTERVAL_MS;
    if ((uint32_t)(nowMs - _lastClockProbeMs) < spacing) return;
    _lastClockProbeMs = nowMs;

    // Round-robin so no node is starved when one is unreachable.  Safe to hold
    // a reference across the probe: _nodes is reserved to MULTINODE_MAX_NODES
    // and only ever erased by _reapRemovedNodes(), which runs on this task.
    for (size_t tries = 0; tries < _nodes.size(); tries++) {
        if (_clockProbeCursor >= _nodes.size()) _clockProbeCursor = 0;
        NodeInfo& n = _nodes[_clockProbeCursor];
        _clockProbeCursor++;
        if (n.pendingRemoval || !n.online || n.staIP.isEmpty()) continue;
        _probeNodeClock(n);
        return;
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
    DynamicJsonDocument doc(320);
    doc["nodeId"]      = _myNodeId;
    doc["mac"]         = _myMacAddress;   // master verifies this matches its stored MAC for nodeId
    doc["running"]     = _timerRunning;
    doc["independent"]  = _conf->getMnSkipMasterStart() && _timerRunning && !_masterRaceActive;
    doc["skipEnabled"]  = _conf->getMnSkipMasterStart();

    // §6.2 — the digest rides the heartbeat that already runs every 2 s.  Cost
    // is roughly twenty bytes per node per interval: no new endpoint, no new
    // timer, and the repair channel exists whether or not anything is wrong.
    doc["raceId"]      = _raceId;
    doc["bootId"]      = _bootId;
    // A real BOOLEAN, not 1.  The master reads this with ArduinoJson, whose
    // operator| and is<bool>() are strict about JSON types: an integer 1 is
    // not a boolean, so `obj["lapSync"] | false` silently yielded false and
    // the §10 gate never opened for any node.  The master now also accepts
    // the integer spelling, but sending the correct type is the actual fix.
    doc["lapSync"]     = true;          // capability advertisement (§10 gate)
    if (_timer) {
        doc["count"]     = (uint32_t)_timer->getLapTotal();
        doc["crc"]       = (uint32_t)_timer->getLapCrc();
        doc["oldestSeq"] = (uint32_t)_timer->getOldestSeq();
        doc["nextSeq"]   = (uint32_t)_timer->getLapTotal();
        // How late this pilot's clock actually began (§8).  Only sent when it
        // is non-zero, so a healthy fleet pays nothing for it.
        const uint32_t lateMs = _timer->getStartLateMs();
        if (lateMs > 0) doc["startLateMs"] = lateMs;
    }

    String body;
    serializeJson(doc, body);

    String resp;
    bool ok = _postToMasterWithResponse("/api/multinode/heartbeat", body, resp);
    if (ok) {
        _heartbeatFailCount = 0;
        _consumeHeartbeatResponse(resp);
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

void MultiNodeManager::_processResyncPull() {
    if (!_resyncGranted || !_masterConnected || _myNodeId == 0 || !_timer) return;

    // One chunk per tick, and never faster than the heartbeat cadence — the
    // grant authorises repair, it does not authorise a burst.
    const uint32_t now = millis();
    if (_lastResyncPullMs != 0 &&
        (uint32_t)(now - _lastResyncPullMs) < MULTINODE_HEARTBEAT_INTERVAL_MS) return;
    _lastResyncPullMs = now;

    // Restore from where our own ring ends.  If we hold nothing, that is 0 and
    // the master answers from its own window floor.
    const uint32_t since = (uint32_t)_timer->getLapTotal();

    String url = String("/api/multinode/laps?node=") + String(_myNodeId) +
                 "&since=" + String(since) +
                 "&limit=" + String(_resyncChunk);

    String resp;
    if (!_getFromMaster(url, resp)) {
        DEBUG("[LAPSYNC] Resync pull failed\n");
        return;
    }

    DynamicJsonDocument doc(512 + LAPSYNC_CHUNK_IDLE * 96);
    if (deserializeJson(doc, resp)) return;

    // Epoch check — a chunk from another race must never be merged.
    const uint32_t chunkRaceId = doc["raceId"] | 0u;
    if (chunkRaceId != _raceId) {
        DEBUG("[LAPSYNC] Resync chunk epoch %u != %u — ignoring\n",
              (unsigned)chunkRaceId, (unsigned)_raceId);
        return;
    }

    JsonArray arr = doc["laps"].as<JsonArray>();
    uint32_t applied = 0;
    for (JsonObject lo : arr) {
        LapSyncRecord rec;
        rec.seq           = lo["seq"]           | 0u;
        rec.lapTimeMs     = lo["lapTimeMs"]     | 0u;
        rec.raceElapsedMs = lo["raceElapsedMs"] | 0u;
        if (rec.seq < (uint32_t)_timer->getLapTotal()) continue;   // already held
        _timer->appendRestoredLap(rec);
        applied++;
    }

    const bool more = doc["more"] | false;
    if (!more) {
        // ── Convergence check ───────────────────────────────────────────
        // `more == false` means the master has nothing further to send; it
        // does NOT mean we arrived where the master is.  buildLapChunk()
        // answers an out-of-window `since` with an empty array and
        // more == false, so a client holding MORE laps than the master would
        // reach this point having applied nothing — and then overwrite its
        // own fastest lap and totals with the master's smaller summary, after
        // which the digests falsely agree and nothing re-triggers.
        //
        // Adopt only on an exact match.  Anything else leaves the grant held,
        // so the master's stall timer retries and, failing that, marks the
        // node DIVERGENT — a visible non-convergence is the right outcome
        // here, and silently corrupting the replica is not.
        const uint32_t masterCount = doc["count"] | 0u;
        if ((uint32_t)_timer->getLapTotal() != masterCount) {
            DEBUG("[LAPSYNC] Restore short: hold %u, master %u — not adopting\n",
                  (unsigned)_timer->getLapTotal(), (unsigned)masterCount);
            return;
        }

        // ── CRC adoption (§4) ───────────────────────────────────────────
        // Only at the END of the restore, and only verbatim.  This device is
        // now holding laps it never witnessed — the evicted ones — so folding
        // its own window would produce a digest that can never match the
        // master's, and every later compare would re-trigger a resync.
        RaceSummary s = {};
        JsonObject sj = doc["summary"].as<JsonObject>();
        s.lapTotal         = (uint16_t)(sj["lapTotal"]         | 0u);
        s.fastestLapMs     =           (sj["fastestLapMs"]     | 0u);
        s.fastestLapNumber = (uint16_t)(sj["fastestLapNumber"] | 0u);
        s.slowestLapMs     =           (sj["slowestLapMs"]     | 0u);
        s.sumLapMs         =           (sj["sumLapMs"]         | 0u);
        s.timedLapCount    = (uint16_t)(sj["timedLapCount"]    | 0u);

        _timer->adoptSyncState(doc["crc"] | 0u, s);
        _resyncGranted = false;
        DEBUG("[LAPSYNC] Restore complete — adopted crc %08X, %u lap(s) this chunk\n",
              (unsigned)(doc["crc"] | 0u), (unsigned)applied);
    }
}

void MultiNodeManager::_consumeHeartbeatResponse(const String& resp) {
    if (resp.isEmpty()) return;
    DynamicJsonDocument rdoc(320);
    if (deserializeJson(rdoc, resp)) return;

    // Epoch first — everything below is meaningless across a race boundary.
    if (rdoc["raceId"].is<uint32_t>()) {
        const uint32_t rid = rdoc["raceId"].as<uint32_t>();
        if (rid != 0 && rid != _raceId) setRaceId(rid);
    }

    if (rdoc["ackSeq"].is<uint32_t>()) {
        const uint32_t ack = rdoc["ackSeq"].as<uint32_t>();
        if (!_hasAckedSeq || ack >= _ackedSeq) { _ackedSeq = ack; _hasAckedSeq = true; }
    }
    if (rdoc["oldestSeq"].is<uint32_t>()) _masterOldest = rdoc["oldestSeq"].as<uint32_t>();

    // wantFrom >= 0 means the master is short and knows exactly where the gap
    // starts.  Cheaper than a resync: the normal lap report simply resumes
    // from there on the next tick.
    if (rdoc["wantFrom"].is<int32_t>()) {
        const int32_t wf = rdoc["wantFrom"].as<int32_t>();
        if (wf >= 0) { _masterWantsFrom = true; _masterWantFromSeq = (uint32_t)wf; }
        else          { _masterWantsFrom = false; }
    }

    // §7 — the master issues at most one grant fleet-wide.  Holding it is what
    // permits this node to pull its own laps back after a reboot.
    _resyncGranted   = rdoc["resyncGrant"] | false;
    _resyncChunk     = rdoc["chunkLimit"]  | (uint32_t)LAPSYNC_CHUNK_RACING;
    if (_resyncChunk == 0) _resyncChunk = 1;
}

void MultiNodeManager::_processQueuedLap() {
    // NOTE the guard that is NOT here.  The old code returned early when
    // !_masterConnected, which discarded every lap detected during a dropout —
    // permanently, because nothing else held them.  Laps now live in
    // LapTimer's ring the moment they are detected, so a disconnect costs
    // nothing but a delay: the unacked window simply drains when the link
    // returns.
    if (_myNodeId == 0 || !_timer) return;
    if (!_masterConnected) return;   // nothing to send TO — laps are safe in the ring

    const uint16_t retained = _timer->getRetainedLapCount();
    if (retained == 0) return;

    // The unacked window is [_ackedSeq+1 .. lapTotal-1], clamped to what the
    // ring still holds and to what the master has not already retired.
    const uint32_t oldest = _timer->getOldestSeq();
    uint32_t from = _hasAckedSeq ? (_ackedSeq + 1) : oldest;

    // Never resend below the master's window floor.  Those laps aged out of
    // its ring; they are RETIRED, not missing, and re-sending them would loop
    // forever because the master can never ack a lap it has evicted (§6.1).
    if (from < _masterOldest) from = _masterOldest;
    if (from < oldest)        from = oldest;

    // If the master named a resume point, honour it in BOTH directions — but
    // never below either window floor.  The previous form only accepted a
    // request that moved `from` forward, which made wantFrom inert in exactly
    // the case it was added for: a master that is short and knows where the
    // gap starts is asking for an EARLIER seq than our ack implies.
    if (_masterWantsFrom) {
        uint32_t want = _masterWantFromSeq;
        if (want < _masterOldest) want = _masterOldest;
        if (want < oldest)        want = oldest;
        from = want;
    }

    const uint32_t lapTotal = (uint32_t)_timer->getLapTotal();
    if (from >= lapTotal) return;    // nothing outstanding

    // Cap the drain (§6.1).  An uncapped window would dump every lap
    // accumulated during a long dropout in a single POST, at exactly the
    // moment the master is also handling this node's re-registration.
    uint32_t toSend = lapTotal - from;
    if (toSend > LAPSYNC_MAX_UNACKED_PER_REPORT) toSend = LAPSYNC_MAX_UNACKED_PER_REPORT;

    // ~90 bytes per lap plus a small header; sized for the cap so the
    // document never has to grow.
    DynamicJsonDocument doc(256 + LAPSYNC_MAX_UNACKED_PER_REPORT * 96);
    doc["nodeId"]    = _myNodeId;
    doc["raceId"]    = _raceId;
    doc["bootId"]    = _bootId;
    doc["oldestSeq"] = oldest;
    JsonArray arr    = doc.createNestedArray("laps");

    for (uint32_t s = from; s < from + toSend; s++) {
        // Translate seq -> ring index. getRetainedRecord() walks oldest-first.
        if (s < oldest) continue;
        const uint16_t idx = (uint16_t)(s - oldest);
        LapSyncRecord rec;
        if (!_timer->getRetainedRecord(idx, &rec)) break;

        JsonObject o = arr.createNestedObject();
        o["seq"]           = rec.seq;
        o["lapTimeMs"]     = rec.lapTimeMs;
        o["raceElapsedMs"] = rec.raceElapsedMs;
    }
    if (arr.size() == 0) return;

    String body;
    serializeJson(doc, body);

    String resp;
    if (!_postToMasterWithResponse("/api/multinode/lap", body, resp)) {
        // No ack, so _ackedSeq does not move and these laps are still
        // outstanding.  They ride the next report; nothing is lost and no
        // counter has been advanced on a send that did not land.
        DEBUG("[LAPSYNC] Lap report failed — %u lap(s) still unacked\n", (unsigned)arr.size());
        return;
    }

    DynamicJsonDocument rdoc(192);
    if (deserializeJson(rdoc, resp)) return;   // malformed: treat as unacked

    if (rdoc["ackSeq"].is<uint32_t>()) {
        const uint32_t ack = rdoc["ackSeq"].as<uint32_t>();
        if (!_hasAckedSeq || ack >= _ackedSeq) {
            _ackedSeq    = ack;
            _hasAckedSeq = true;
        }
    }
    if (rdoc["oldestSeq"].is<uint32_t>()) {
        _masterOldest = rdoc["oldestSeq"].as<uint32_t>();
    }
    // Gap request satisfied for now; the heartbeat digest will re-raise it if
    // the master is still short.
    _masterWantsFrom = false;
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

bool MultiNodeManager::_getFromMaster(const String& endpoint, String& response) {
    HTTPClient http;
    String url = String("http://") + MULTINODE_MASTER_IP + endpoint;
    if (!http.begin(url)) return false;
    http.setTimeout(800);
    // Paired with setTimeout deliberately: setTimeout does NOT cover the
    // connect phase, so an unreachable peer costs the full default 5000 ms and
    // stalls this whole tick.
    http.setConnectTimeout(800);
    int code = http.GET();
    bool ok = (code == 200);
    if (ok) response = http.getString();
    http.end();
    return ok;
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
    http.setConnectTimeout(800);
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
        // A slot the director just kicked is already gone as far as matching
        // is concerned — it simply has not been reaped yet.  Without this, a
        // client whose pauseReconnect never landed (which is the normal case,
        // since we kick clients precisely when they are unreachable) could
        // re-register into its old slot in the millisecond before the reap
        // and undo the removal.
        if (n.pendingRemoval) continue;
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
                (n.pilotName     != pilotName)     ||
                (n.pilotColor    != pilotColor)    ||
                (n.bandIndex     != bandIndex)     ||
                (n.channelIndex  != channelIndex)  ||
                (n.frequency     != frequency)     ||
                (n.enterRssi     != enterRssi)     ||
                (n.exitRssi      != exitRssi)      ||
                (n.clientIP      != clientIP)      ||
                (n.staIP         != staIP)         ||
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

    // Hard capacity guard, and it must stay keyed on size() rather than on a
    // live-slot count.  reserve() claimed exactly MULTINODE_MAX_NODES, so
    // admitting an eighth element — even to replace one that is merely
    // awaiting reap — would reallocate the vector and hand parallelTask a
    // dangling pointer, which is the entire failure this reserve() prevents.
    //
    // A node marked for removal therefore still costs its slot until the next
    // parallelTask tick.  That is a sub-tick delay before a replacement can
    // register, against a 5 s client retry — invisible in practice.
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
    _nodes.push_back(n);

    assignedNodeId = newId;
    DEBUG("[MULTINODE] New node %c (mac=%s ip=%s suffix=%s): %s\n",
          slotLetter(newId), macAddress.c_str(), staIP.c_str(), apSuffix.c_str(), pilotName.c_str());
    stateChanged = true;   // brand-new slot occupant — broadcast so all clients learn about it
    return true;
}

const NodeInfo* MultiNodeManager::findNode(uint8_t nodeId) const {
    for (const auto& n : _nodes) {
        if (n.nodeId == nodeId) return &n;
    }
    return nullptr;
}

bool MultiNodeManager::handleLapBatch(uint8_t nodeId, uint32_t raceId, uint32_t bootId,
                                      const LapSyncRecord* laps, size_t count,
                                      uint32_t& outAckSeq, uint32_t& outOldestSeq) {
    if (_pausedForOta) return false;   // master is OTA-busy

    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;

        // Epoch check first (§5).  Laps from a stale race must never merge
        // into the current one — that is exactly how run 2 inherited run 1's
        // laps and doubled the payload.
        if (raceId != n.raceId) {
            if (raceId != 0 && n.raceId != 0) {
                DEBUG("[LAPSYNC] Node %c raceId %u != %u — dropping stale batch\n",
                      slotLetter(nodeId), raceId, n.raceId);
                outAckSeq    = n.hasAck ? n.ackSeq : 0;
                outOldestSeq = n.laps.oldestSeq();
                n.lastSeen   = millis();
                return true;   // handled; the client will resync on the digest
            }
            if (raceId == 0 && n.raceId != 0) {
                // We have an epoch and this client has not adopted it yet —
                // it reconnected, or its masterStart POST never landed.  This
                // must NOT fall through to the reset below: doing so wiped
                // the master's copy of that node's race, which is the exact
                // backup the protocol exists to keep.  Hold the batch; the
                // raceId already in this reply brings the client forward and
                // it resends on the next tick.
                outAckSeq    = n.hasAck ? n.ackSeq : 0;
                outOldestSeq = n.laps.oldestSeq();
                n.lastSeen   = millis();
                return true;
            }
            // We have no epoch of our own — adopt the client's and start clean.
            n.raceId = raceId;
            n.laps.reset();
            n.hasAck = false;
            n.ackSeq = 0;
        }

        // A changed bootId means this node restarted.  Its laps are still
        // valid (we hold them); what changed is that IT may have lost them.
        // Record it so the governor can offer a restore (§7 trigger).
        if (bootId != 0 && n.bootId != 0 && bootId != n.bootId) {
            DEBUG("[LAPSYNC] Node %c bootId changed %u -> %u — reboot detected\n",
                  slotLetter(nodeId), n.bootId, bootId);
            n.resyncState      = LAPSYNC_NEEDED;
            n.resyncNeededAtMs = millis();
        }
        if (bootId != 0) n.bootId = bootId;

        for (size_t i = 0; i < count; i++) {
            const LapSyncRecord& lap = laps[i];

            // Idempotency: anything at or below the ack is already stored.
            // This is what makes a retry after a lost response safe.
            if (n.hasAck && lap.seq <= n.ackSeq) continue;

            // Only append the lap that continues the sequence.  A gap means
            // the client is ahead of us; we ack what we have and the client
            // resends from there on its next report.
            // An empty ring has no sequence to continue, so the first lap to
            // arrive DEFINES the base — that is how a master which rebooted
            // mid-race picks the fleet back up at seq 61 instead of demanding
            // laps 0-60 that nobody is going to resend.  Every lap after it is
            // gap-checked, because oldestSeq() reads total - retained() and is
            // only the true base while appends stay contiguous.
            const bool defining = (!n.hasAck && n.laps.retained() == 0);
            const uint32_t expected = n.hasAck ? (n.ackSeq + 1) : n.laps.oldestSeq();
            if (lap.seq != expected && !defining) {
                DEBUG("[LAPSYNC] Node %c gap: got seq %u, expected %u\n",
                      slotLetter(nodeId), lap.seq, expected);
                break;
            }

            if (!n.laps.append(lap)) {
                DEBUG("[LAPSYNC] Node %c ring alloc failed — dropping lap %u\n",
                      slotLetter(nodeId), lap.seq);
                break;
            }
            n.ackSeq   = lap.seq;
            n.hasAck   = true;

            // §6.5 — queue for the next directorState so peers learn of it.
            _queueLapDelta(n, lap);
        }

        n.lastSeen   = millis();
        // Quiet-gate reference (§7 guard 3).  Any real lap pushes resync back,
        // so repair traffic can never land inside a crossing.
        _lastLapHandledMs = n.lastSeen;
        outAckSeq    = n.hasAck ? n.ackSeq : 0;
        outOldestSeq = n.laps.oldestSeq();
        return true;
    }
    return false;
}

bool MultiNodeManager::handleLap(uint8_t nodeId, uint32_t lapTimeMs, uint8_t lapNumber) {
    // Legacy shape (§10): no seq, no epoch, no timestamp.  Synthesise what we
    // can — the seq becomes our own next slot, and raceElapsedMs is left zero
    // because this client did not send one and inventing an arrival time here
    // is precisely the defect this protocol removes (§8).  Such a node is
    // stored and displayed but is never anchored for cross-pilot ordering.
    if (_pausedForOta) return false;

    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;

        LapSyncRecord lap;
        lap.lapTimeMs     = lapTimeMs;
        lap.raceElapsedMs = 0;
        lap.seq           = n.hasAck ? (n.ackSeq + 1) : 0;
        (void)lapNumber;   // 8-bit, wraps at 255 — deliberately not trusted

        if (!n.laps.append(lap)) return false;
        n.ackSeq   = lap.seq;
        n.hasAck   = true;
        n.anchored = false;            // legacy nodes cannot be ordered (§8)
        n.lastSeen = millis();
        _queueLapDelta(n, lap);
        return true;
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

bool MultiNodeManager::handleHeartbeatSync(uint8_t nodeId, uint32_t raceId, uint32_t bootId,
                                           bool lapSyncCapable,
                                           uint32_t count, uint32_t crc, uint32_t oldestSeq,
                                           HeartbeatSyncReply& out) {
    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;

        // Heard-from is what makes the absence of lapSync meaningful: only a
        // node we have actually processed a heartbeat for can be classified
        // as pre-protocol.  See anyNodeNeedsLegacyLaps().
        n.lapSyncHeard   = true;
        n.lapSyncCapable = lapSyncCapable;
        n.reportedCount  = count;
        n.reportedCrc    = crc;
        n.reportedOldest = oldestSeq;

        out.raceId    = _masterRaceId;
        out.ackSeq    = n.hasAck ? n.ackSeq : 0;
        out.oldestSeq = n.laps.oldestSeq();

        // ── Capability (§10) ────────────────────────────────────────────
        // A node that does not speak the protocol reports no digest, so its
        // count and crc arrive as 0 — which is indistinguishable from "I lost
        // every lap".  Left ungated, a single legacy client would trip the
        // restore trigger below, be handed the one fleet-wide resync grant it
        // cannot act on, hold that grant until the 120 s stall timeout, and
        // block every other node's repair in the meantime.  It cannot be
        // healed by a mechanism it does not implement, so it never enters the
        // state machine at all.
        if (!lapSyncCapable) {
            out.wantFrom    = -1;
            out.resyncGrant = false;
            return true;
        }

        // ── Epoch (§5) ──────────────────────────────────────────────────
        // Digests from different races are incomparable.  Say nothing about
        // gaps or resync until the client has adopted the current epoch —
        // it will, from the raceId in this very reply.
        if (raceId != _masterRaceId) {
            out.wantFrom = -1;
            return true;
        }

        // ── bootId (§5) ─────────────────────────────────────────────────
        // A changed bootId with an unchanged raceId is the one case that is
        // NOT "the race just started": this node restarted mid-race and may
        // have lost laps we still hold.
        if (bootId != 0 && n.bootId != 0 && bootId != n.bootId &&
            n.resyncState == LAPSYNC_IDLE && n.laps.total > 0) {
            DEBUG("[LAPSYNC] Node %c rebooted mid-race — restore needed\n", slotLetter(nodeId));
            n.resyncState      = LAPSYNC_NEEDED;
            n.resyncNeededAtMs = millis();
            n.resyncAttempts   = 0;
        }
        if (bootId != 0) n.bootId = bootId;

        // ── Gap detection ───────────────────────────────────────────────
        // The client is ahead of us: ask for the continuation on the fast
        // path rather than spending a resync grant on it.
        const uint32_t haveNext = n.hasAck ? (n.ackSeq + 1) : n.laps.oldestSeq();
        if (count > haveNext) {
            out.wantFrom = (int32_t)(haveNext < oldestSeq ? oldestSeq : haveNext);
        } else {
            out.wantFrom = -1;
        }

        // ── Divergence (§7 triggers) ────────────────────────────────────
        // Same count, different digest: silent corruption that a count check
        // alone would never see.  This is what the CRC is for.
        if (n.resyncState == LAPSYNC_IDLE &&
            count == n.laps.total && count > 0 && crc != n.laps.crc) {
            DEBUG("[LAPSYNC] Node %c digest mismatch (count %u, crc %08X vs %08X)\n",
                  slotLetter(nodeId), (unsigned)count, (unsigned)crc, (unsigned)n.laps.crc);
            n.resyncState      = LAPSYNC_NEEDED;
            n.resyncNeededAtMs = millis();
            n.resyncAttempts   = 0;
        }

        // The client holds fewer laps than we do — it lost them.  It cannot
        // simply be lagging: `count` is the client's own lapTotal and we only
        // hold what it sent us, so our total can never legitimately run ahead
        // of its own.
        //
        // This used to require count == 0, which healed a rebooted client but
        // left a PARTIAL loss stranded forever: at count 5 against our 20 the
        // CRC trigger below never fires (it needs equal counts) and wantFrom
        // never fires (it needs the client ahead), so nothing repaired it.
        if (n.resyncState == LAPSYNC_IDLE && n.laps.total > 0 && count < n.laps.total) {
            DEBUG("[LAPSYNC] Node %c holds %u laps, master holds %u — restore needed\n",
                  slotLetter(nodeId), (unsigned)count, (unsigned)n.laps.total);
            n.resyncState      = LAPSYNC_NEEDED;
            n.resyncNeededAtMs = millis();
            n.resyncAttempts   = 0;
        }

        // ── Verify (§7) ─────────────────────────────────────────────────
        // A streaming node that now matches is healed.
        if (n.resyncState == LAPSYNC_STREAMING &&
            count == n.laps.total && crc == n.laps.crc) {
            DEBUG("[LAPSYNC] Node %c resync verified\n", slotLetter(nodeId));
            n.resyncState    = LAPSYNC_IDLE;
            n.resyncDoneAtMs = millis();
            n.resyncAttempts = 0;
            if (_resyncGrantNodeId == nodeId) _resyncGrantNodeId = 0;
        }

        out.resyncGrant = (_resyncGrantNodeId == nodeId);
        if (out.resyncGrant) {
            // Adaptive budget: the constraint only exists while racing.
            const bool racing = _anyNodeRunning();
            const bool escaped = (millis() - n.resyncNeededAtMs) >= LAPSYNC_ESCAPE_VALVE_MS;
            out.chunkLimit = escaped ? LAPSYNC_CHUNK_ESCAPE
                                     : (racing ? LAPSYNC_CHUNK_RACING : LAPSYNC_CHUNK_IDLE);
            if (n.resyncState == LAPSYNC_GRANTED) n.resyncState = LAPSYNC_STREAMING;
        }
        return true;
    }
    return false;
}

bool MultiNodeManager::buildLapChunk(uint8_t nodeId, uint32_t since, uint32_t limit,
                                     String& out) const {
    const NodeInfo* n = findNode(nodeId);
    if (!n) return false;

    if (limit == 0 || limit > LAPSYNC_MAX_LAPS) limit = LAPSYNC_CHUNK_IDLE;

    const uint32_t oldest = n->laps.oldestSeq();
    // A request below the window is answered from the window floor.  Refusing
    // would stall a restore forever over laps that are retired, not lost.
    if (since < oldest) since = oldest;

    const uint16_t retained = n->laps.retained();
    uint16_t startIdx = (uint16_t)(since - oldest);

    out = "";
    out.reserve(256 + limit * 96);
    out += "{\"nodeId\":";      out += (int)nodeId;
    out += ",\"raceId\":";      out += (uint32_t)n->raceId;
    out += ",\"oldestSeq\":";   out += (uint32_t)oldest;
    out += ",\"count\":";       out += (uint32_t)n->laps.total;
    // Authoritative digest — the recipient ADOPTS this rather than folding its
    // own window, which it cannot do for laps it never witnessed (§4).
    out += ",\"crc\":";         out += (uint32_t)n->laps.crc;
    out += ",\"summary\":{\"fastestLapMs\":"; out += (uint32_t)n->laps.summary.fastestLapMs;
    out += ",\"fastestLapNumber\":";          out += (uint32_t)n->laps.summary.fastestLapNumber;
    out += ",\"slowestLapMs\":";              out += (uint32_t)n->laps.summary.slowestLapMs;
    out += ",\"sumLapMs\":";                  out += (uint32_t)n->laps.summary.sumLapMs;
    out += ",\"timedLapCount\":";             out += (uint32_t)n->laps.summary.timedLapCount;
    out += ",\"lapTotal\":";                  out += (uint32_t)n->laps.summary.lapTotal;
    out += "},\"laps\":[";

    uint32_t sent = 0;
    for (uint16_t i = startIdx; i < retained && sent < limit; i++, sent++) {
        LapSyncRecord rec;
        if (!n->laps.get(i, &rec)) break;
        if (sent) out += ',';
        out += "{\"seq\":";            out += (uint32_t)rec.seq;
        out += ",\"lapTimeMs\":";      out += (uint32_t)rec.lapTimeMs;
        out += ",\"raceElapsedMs\":";  out += (uint32_t)rec.raceElapsedMs;
        out += '}';
    }
    out += "],\"more\":";
    out += ((uint32_t)startIdx + sent < retained) ? "true" : "false";
    out += "}";
    return true;
}

bool MultiNodeManager::_anyNodeRunning() const {
    if (_nodes.empty()) return false;
    for (const auto& n : _nodes) if (n.online && n.running) return true;
    return false;
}

void MultiNodeManager::_runResyncGovernor(uint32_t nowMs) {
    if (!isMasterMode()) return;

    // Guard 3 — quiet gate.  Any real lap resets the reference, so the stream
    // finds actual lulls empirically instead of trying to predict them.
    const bool quiet = (uint32_t)(nowMs - _lastLapHandledMs) >= LAPSYNC_QUIET_GATE_MS;

    // A grant is outstanding: let it run, but do not let it wedge the single
    // fleet-wide slot forever if the node went away mid-stream.
    if (_resyncGrantNodeId != 0) {
        NodeInfo* g = nullptr;
        for (auto& n : _nodes) if (n.nodeId == _resyncGrantNodeId) { g = &n; break; }

        if (!g || !g->online ||
            (g->resyncState != LAPSYNC_GRANTED && g->resyncState != LAPSYNC_STREAMING)) {
            _resyncGrantNodeId = 0;
        } else if ((uint32_t)(nowMs - _resyncGrantAtMs) > 120000u) {
            // Stalled stream. Count it as a failed attempt so a node that can
            // never converge reaches DIVERGENT instead of holding the slot.
            if (++g->resyncAttempts >= LAPSYNC_VERIFY_ATTEMPTS) {
                DEBUG("[LAPSYNC] Node %c DIVERGENT after %u attempts — giving up\n",
                      slotLetter(g->nodeId), g->resyncAttempts);
                g->resyncState = LAPSYNC_DIVERGENT;
            } else {
                g->resyncState = LAPSYNC_NEEDED;
            }
            g->resyncDoneAtMs  = nowMs;
            _resyncGrantNodeId = 0;
        }
        return;
    }

    // No grant outstanding — pick at most one candidate.
    for (auto& n : _nodes) {
        if (!n.online || n.resyncState != LAPSYNC_NEEDED) continue;
        // Belt and braces alongside the capability check in
        // handleHeartbeatSync: the single fleet-wide grant must never be
        // spent on a node that cannot consume it.
        if (!n.lapSyncCapable) { n.resyncState = LAPSYNC_IDLE; continue; }

        // Guard 4 — escape valve.  A saturated race must not starve repair
        // indefinitely; slow repair beats no repair.
        const bool escaped = (uint32_t)(nowMs - n.resyncNeededAtMs) >= LAPSYNC_ESCAPE_VALVE_MS;

        // Guard 1 — per-node cooldown.  A node rebooting every ten seconds
        // costs one stream per cooldown, not one per boot.
        if (n.resyncDoneAtMs != 0 &&
            (uint32_t)(nowMs - n.resyncDoneAtMs) < LAPSYNC_RESYNC_COOLDOWN_MS) continue;

        if (!quiet && !escaped) continue;

        n.resyncState      = LAPSYNC_GRANTED;
        _resyncGrantNodeId = n.nodeId;
        _resyncGrantAtMs   = nowMs;
        DEBUG("[LAPSYNC] Resync grant -> node %c (%s)\n",
              slotLetter(n.nodeId), escaped ? "escape valve" : "quiet");
        return;   // Guard 2 — exactly one grant fleet-wide
    }
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
        n.laps.reset();
        n.ackSeq                 = 0;
        n.hasAck                 = false;
        n.reportedCrc            = 0;
        n.reportedCount          = 0;
        n.reportedOldest         = 0;
        n.resyncState            = LAPSYNC_IDLE;
        n.resyncAttempts         = 0;
        n.running                = false;
        n.quitEarly              = false;
        n.excludedFromCurrentRace = false;
        // Anchors belong to a race, not to a node — a cleared race has none.
        // The measured clock offset and drift fit are NOT cleared: those
        // describe the crystals, not the race.
        n.anchored               = false;
        n.anchorUs               = 0;
        n.anchorRttMs            = 0xFFFFFFFF;
    }
    _excludeNodes.clear();
    _lapDeltas.clear();
    DEBUG("[MULTINODE] All laps cleared\n");
}

// ── Lap Sync: epochs, anchors and the peer delta feed ───────────────────

uint32_t MultiNodeManager::beginRaceEpoch() {
    // A fresh, non-zero id per race.  esp_random() is seeded from hardware, so
    // two masters powering up together do not collide the way millis() would.
    do { _masterRaceId = esp_random(); } while (_masterRaceId == 0);

    for (auto& n : _nodes) {
        n.raceId         = _masterRaceId;
        n.laps.reset();
        n.ackSeq         = 0;
        n.hasAck         = false;
        n.reportedCrc    = 0;
        n.reportedCount  = 0;
        n.reportedOldest = 0;
        n.resyncState    = LAPSYNC_IDLE;
        n.resyncAttempts = 0;
        // The ANCHOR belongs to a race and is re-derived at each start.  The
        // clock OFFSET and drift fit do not — they are properties of the two
        // crystals and survive across races, which is what lets a node that
        // misses one pre-arm still be reasonably placed.
        n.anchored       = false;
        n.anchorUs       = 0;
        n.anchorRttMs    = 0xFFFFFFFF;
        // Per-race start bookkeeping: last race's ack must not vouch for this
        // one, and last race's lateness must not be reported against it.
        n.startAcked     = false;
        n.startLateMs    = 0;
    }
    _lapDeltas.clear();
    DEBUG("[LAPSYNC] Race epoch %u begun\n", _masterRaceId);
    return _masterRaceId;
}

// ── Clock sync (§8) ─────────────────────────────────────────────────────
//
// One four-timestamp exchange, the NTP way:
//
//   t1  master sends          t2  client receives
//   t4  master receives       t3  client replies
//
//   offset = ((t2 - t1) + (t3 - t4)) / 2     client's clock, ahead of ours
//   delay  = (t4 - t1) - (t3 - t2)           round trip, client's work removed
//
// Subtracting (t3 - t2) is what makes this worth building.  The previous
// anchor measured the reply of /timer/masterStart — a handler that clears lap
// data, starts a timer and fires an SSE broadcast before answering — so all of
// that landed inside the number being called network latency.
// Wire format, deliberately fixed-width and tiny:
//   REQ  [magic:4][seq:4]                        = 8 bytes
//   RSP  [magic:4][seq:4][t2:8][t3:8]            = 24 bytes
struct ClockProbeReq { uint32_t magic; uint32_t seq; } __attribute__((packed));
struct ClockProbeRsp { uint32_t magic; uint32_t seq; int64_t t2; int64_t t3; } __attribute__((packed));
//   GO   [magic:4][raceId:4][startAtUs:8]         = 16 bytes
struct ClockStartMsg { uint32_t magic; uint32_t raceId; int64_t startAtUs; } __attribute__((packed));
//   ACK  [magic:4][raceId:4][nodeId:1]                = 9 bytes
struct ClockStartAck { uint32_t magic; uint32_t raceId; uint8_t nodeId; } __attribute__((packed));

void MultiNodeManager::_initClockUdp() {
    if (_clockUdpReady) return;
    if (!_clockUdp.listen(CLOCK_PROBE_UDP_PORT)) {
        DEBUG("[CLOCK] UDP listen on %u failed — probes disabled\n", CLOCK_PROBE_UDP_PORT);
        return;
    }
    _clockUdp.onPacket([this](AsyncUDPPacket packet) {
        // Runs in the LwIP task.  Stamp FIRST, think second — every
        // instruction before the stamp is error attributed to the network.
        const int64_t stamp = esp_timer_get_time();
        const size_t  len   = packet.length();
        if (len < sizeof(ClockProbeReq)) return;

        uint32_t magic;
        memcpy(&magic, packet.data(), sizeof(magic));

        if (magic == CLOCK_PROBE_MAGIC_REQ && len >= sizeof(ClockProbeReq)) {
            // Client role: answer immediately, in this callback.  Nothing
            // else may be added here — the gap between t2 and t3 is time the
            // master subtracts out, but only if we report it honestly.
            ClockProbeReq req;
            memcpy(&req, packet.data(), sizeof(req));
            ClockProbeRsp rsp;
            rsp.magic = CLOCK_PROBE_MAGIC_RSP;
            rsp.seq   = req.seq;
            rsp.t2    = stamp;
            rsp.t3    = esp_timer_get_time();
            _clockUdp.writeTo((const uint8_t*)&rsp, sizeof(rsp),
                              packet.remoteIP(), packet.remotePort());
            return;
        }

        if (magic == CLOCK_START_MAGIC && len >= sizeof(ClockStartMsg)) {
            // Client role: GO.  The only work done here is storing a target —
            // scheduleStart() never starts inline, precisely so this callback
            // stays safe to run in the LwIP task.
            ClockStartMsg msg;
            memcpy(&msg, packet.data(), sizeof(msg));
            // Trust it only if it belongs to the epoch we were armed for.  A
            // stray or replayed datagram from a previous race is ignored, and
            // an unarmed node (raceId 0) accepts nothing at all.
            if (_raceId == 0 || msg.raceId != _raceId) return;
            if (!_timer) return;
            // Repeats are expected: three copies of the same GO arrive ~3 ms
            // apart.  Re-arming the same instant is idempotent, and once the
            // race is running a later copy must not restart it.
            if (!_timer->isRunning()) _timer->scheduleStart(msg.startAtUs);
            // Acknowledge every copy, including duplicates — the master is
            // listening for ANY ack, and answering all of them costs 9 bytes
            // while raising the odds that at least one gets home.
            ClockStartAck ack;
            ack.magic  = CLOCK_START_ACK_MAGIC;
            ack.raceId = msg.raceId;
            ack.nodeId = _myNodeId;
            _clockUdp.writeTo((const uint8_t*)&ack, sizeof(ack),
                              packet.remoteIP(), packet.remotePort());
            return;
        }

        if (magic == CLOCK_START_ACK_MAGIC && len >= sizeof(ClockStartAck)) {
            // Master role: mark this node as having received its GO.
            ClockStartAck ack;
            memcpy(&ack, packet.data(), sizeof(ack));
            if (ack.raceId != _masterRaceId) return;
            for (auto& n : _nodes) {
                if (n.nodeId == ack.nodeId) { n.startAcked = true; break; }
            }
            return;
        }

        if (magic == CLOCK_PROBE_MAGIC_RSP && len >= sizeof(ClockProbeRsp)) {
            // Master role: t4 is `stamp`, taken before any parsing.
            ClockProbeRsp rsp;
            memcpy(&rsp, packet.data(), sizeof(rsp));
            if (rsp.seq != _probeSeq) return;   // stale reply from a timed-out probe
            _probeT2 = rsp.t2;
            _probeT3 = rsp.t3;
            _probeT4 = stamp;
            _probeReady = true;                 // set LAST — publishes the three above
        }
    });
    _clockUdpReady = true;
    DEBUG("[CLOCK] UDP probe socket listening on %u\n", CLOCK_PROBE_UDP_PORT);
}

bool MultiNodeManager::_probeNodeClock(NodeInfo& n) {
    if (!n.online || n.staIP.isEmpty()) return false;
    if (!_clockUdpReady) return false;

    IPAddress dst;
    if (!dst.fromString(n.staIP)) return false;

    // Bump the sequence before arming so a late reply to the PREVIOUS probe
    // can't be mistaken for this one's.
    const uint32_t seq = _probeSeq + 1;
    _probeSeq   = seq;
    _probeReady = false;

    ClockProbeReq req;
    req.magic = CLOCK_PROBE_MAGIC_REQ;
    req.seq   = seq;

    const int64_t t1 = esp_timer_get_time();
    if (!_clockUdp.writeTo((const uint8_t*)&req, sizeof(req), dst, CLOCK_PROBE_UDP_PORT)) {
        return false;
    }

    // Poll for the callback's flag.  Poll granularity costs nothing here:
    // t4 was already stamped in the LwIP task the moment the datagram landed.
    const uint32_t deadline = millis() + CLOCK_PROBE_TIMEOUT_MS;
    while (!_probeReady && (int32_t)(millis() - deadline) < 0) vTaskDelay(1);
    if (!_probeReady) return false;

    const int64_t t2 = _probeT2, t3 = _probeT3, t4 = _probeT4;
    if (t2 <= 0 || t3 < t2) return false;

    const int64_t offsetUs = ((t2 - t1) + (t3 - t4)) / 2;
    const int64_t delayUs  = (t4 - t1) - (t3 - t2);
    // A negative delay is arithmetically impossible and means one of the two
    // clocks jumped mid-exchange.  Discard rather than reason about it.
    if (delayUs < 0) return false;

    _recordClockSample(n, offsetUs, delayUs);
    return true;
}

void MultiNodeManager::_recordClockSample(NodeInfo& n, int64_t offsetUs, int64_t delayUs) {
    n.clockSamples++;
    // Published unfiltered so an external logger can fit drift over a long
    // baseline rather than inheriting our 8-sample window.
    n.rawOffsetUs = offsetUs;
    n.rawDelayUs  = (uint32_t)delayUs;
    n.rawAtUs     = esp_timer_get_time();

    // Keep the least-queued sample.  The NTP formula assumes the outbound and
    // return legs took equal time; that assumption is least wrong when the
    // round trip was shortest, so min-delay is the filter that matters.
    if (!n.clockValid || (uint32_t)delayUs < n.clockDelayUs) {
        n.clockOffsetUs = offsetUs;
        n.clockDelayUs  = (uint32_t)delayUs;
        n.clockValid    = true;
    }

    // Quality gate.  A sample's offset error is roughly half its round trip,
    // so a 63 ms sample carries ~30 ms of error into a fit that is trying to
    // resolve single-digit ppm.  Admitting it costs more than the coverage it
    // adds.  Judged against this node's own best so a good link is not held to
    // a bad one's standard, with an absolute ceiling so a bad link cannot
    // quietly raise its own bar.
    const uint32_t admitUs = (n.clockDelayUs == 0xFFFFFFFF)
                           ? CLOCK_DRIFT_MAX_DELAY_US
                           : min(n.clockDelayUs * CLOCK_DRIFT_DELAY_MULT,
                                 CLOCK_DRIFT_MAX_DELAY_US);
    if ((uint32_t)delayUs > admitUs) return;

    // The drift window is fed at a RATE LIMIT, not on every sample.
    //
    // This matters more than it looks.  The pre-arm burst fires a probe every
    // 120 ms, so without this guard eight burst samples would flush the entire
    // window and leave it spanning ~1 s — below CLOCK_DRIFT_MIN_SPAN_US, so
    // driftValid would drop to false at the exact moment a race starts, having
    // just discarded a slope built over the previous several minutes.  The
    // burst is for measuring the OFFSET; the window is for measuring the RATE,
    // and a rate needs a long baseline.
    const int64_t nowUs = esp_timer_get_time();
    const uint8_t last  = (uint8_t)((n.driftHead + NodeInfo::CLOCK_DRIFT_SAMPLES - 1)
                                    % NodeInfo::CLOCK_DRIFT_SAMPLES);
    if (n.driftCount > 0 && (nowUs - n.driftAtUs[last]) < CLOCK_DRIFT_SPACING_US) return;

    n.driftAtUs[n.driftHead]  = nowUs;
    n.driftOffUs[n.driftHead] = offsetUs;
    n.driftHead = (uint8_t)((n.driftHead + 1) % NodeInfo::CLOCK_DRIFT_SAMPLES);
    if (n.driftCount < NodeInfo::CLOCK_DRIFT_SAMPLES) n.driftCount++;

    _fitClockDrift(n);
}

// Ordinary least squares on (master time, offset).  Slope is the relative rate
// between this node's crystal and ours, in ppm.
void MultiNodeManager::_fitClockDrift(NodeInfo& n) {
    if (n.driftCount < CLOCK_DRIFT_MIN_SAMPLES) { n.driftValid = false; return; }

    // Work relative to the first sample so the sums stay small — absolute
    // esp_timer values are ~1e10 µs and squaring them overflows fast.
    int64_t tMin = INT64_MAX, tMax = INT64_MIN;
    for (uint8_t i = 0; i < n.driftCount; i++) {
        if (n.driftAtUs[i] < tMin) tMin = n.driftAtUs[i];
        if (n.driftAtUs[i] > tMax) tMax = n.driftAtUs[i];
    }
    // A slope fitted over a few seconds is noise wearing a trend's clothing.
    if (tMax - tMin < CLOCK_DRIFT_MIN_SPAN_US) { n.driftValid = false; return; }

    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const double nD = (double)n.driftCount;
    for (uint8_t i = 0; i < n.driftCount; i++) {
        const double x = (double)(n.driftAtUs[i] - tMin);
        const double y = (double)n.driftOffUs[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const double denom = nD * sxx - sx * sx;
    if (denom == 0.0) { n.driftValid = false; return; }

    // slope is µs of offset per µs of master time — i.e. already a rate.
    const double slope = (nD * sxy - sx * sy) / denom;
    int32_t ppm = (int32_t)llround(slope * 1000000.0);

    // Clamp rather than reject: a genuine unit in the sun sits well inside
    // ±100 ppm, so anything beyond it is a bad probe, and letting a bad probe
    // set the rate would corrupt every remaining lap of the race.
    if (ppm >  CLOCK_MAX_DRIFT_PPM) ppm =  CLOCK_MAX_DRIFT_PPM;
    if (ppm < -CLOCK_MAX_DRIFT_PPM) ppm = -CLOCK_MAX_DRIFT_PPM;

    n.driftPpm   = ppm;
    n.driftValid = true;
}

// Master-time µs of a lap that this node timed at raceElapsedMs on its own
// clock.  Every part of the sync chain converges here.
//
//   client_us_at_lap = raceStartUs + elapsed
//   master_us_at_lap = client_us_at_lap - offset(t)
//                    = anchorUs + elapsed - elapsed × rate
//
// The correction is applied FORWARD only, to laps as they are appended.  A lap
// already published keeps the orderMs it shipped with: the delta log is
// append-only and acked, and rewriting history to chase microseconds would
// break that for no observable gain.  Safe because the corrections (µs) sit
// three orders of magnitude below lap spacing (seconds), so no adjustment can
// ever reorder two laps.
// Note the correction is zero at elapsed == 0 and grows linearly from there.
// The initial offset is already fully absorbed by anchorUs and is never
// re-applied here — drift is a RATE, and the constant term would be double
// counting.  (Which is also why the huge boot-skew offsets — one node showed
// -1362 s simply because it powered up 22 minutes after the master — cannot
// leak into the rate: a constant contributes nothing to a slope.)
int64_t MultiNodeManager::_lapOrderUs(const NodeInfo& n, uint32_t raceElapsedMs) const {
    const int64_t elapsedUs = (int64_t)raceElapsedMs * 1000LL;
    int64_t orderUs = n.anchorUs + elapsedUs;
    if (CLOCK_APPLY_DRIFT && n.driftValid) {
        orderUs -= (elapsedUs * (int64_t)n.driftPpm) / 1000000LL;
    }
    return orderUs;
}

bool MultiNodeManager::recordAnchorFromReport(uint8_t nodeId, int64_t clientRaceStartUs) {
    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;
        if (!n.clockValid || clientRaceStartUs <= 0) return false;

        // master_us = client_us - offset.  Exact: the client told us when its
        // clock hit zero, and we measured how far its clock sits from ours.
        n.anchorUs = clientRaceStartUs - n.clockOffsetUs;
        n.anchored = true;
        DEBUG("[CLOCK] Node %c anchored from report: offset=%lld us delay=%u us "
              "samples=%u drift=%d ppm%s\n",
              slotLetter(n.nodeId), (long long)n.clockOffsetUs, n.clockDelayUs,
              n.clockSamples, n.driftPpm, n.driftValid ? "" : " (unfitted)");
        return true;
    }
    return false;
}

void MultiNodeManager::recordAnchorFromRtt(uint8_t nodeId, uint32_t masterAckMs, uint32_t rttMs) {
    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;
        if (n.anchored && rttMs >= n.anchorRttMs) return;

        n.anchorUs    = (int64_t)(masterAckMs - (rttMs / 2)) * 1000LL;
        n.anchorRttMs = rttMs;
        n.anchored    = true;
        DEBUG("[CLOCK] Node %c anchored from RTT fallback: rtt=%u ms "
              "(no clock offset — node missed the pre-arm sync)\n",
              slotLetter(n.nodeId), rttMs);
        return;
    }
}

void MultiNodeManager::_queueLapDelta(const NodeInfo& n, const LapSyncRecord& lap) {
    // Bounded (§6.5).  Dropping past the cap is deliberate: peer lap data is a
    // cache, and the recipient detects the shortfall through the per-node
    // digest carried in the same payload.  Growing this vector under load is
    // exactly the behaviour this protocol exists to remove.
    if (_lapDeltas.size() >= (size_t)LAPSYNC_MAX_LAP_DELTAS) return;

    LapDelta d;
    d.nodeId        = n.nodeId;
    d.seq           = lap.seq;
    d.lapTimeMs     = lap.lapTimeMs;
    d.raceElapsedMs = lap.raceElapsedMs;
    d.ordered       = n.anchored;
    // Unanchored nodes (solo racers, legacy clients) are stored and displayed
    // but cannot be placed on the field's timeline, so they carry no ordering
    // key rather than a fabricated one (§8).
    //
    // Rounded to ms exactly once, here.  The offset and drift arithmetic
    // behind _lapOrderUs runs in µs so a 12 ms whole-race drift correction
    // isn't quantized away before it can be applied — but the published key
    // stays ms, which is already below the ~1 ms uncertainty of RSSI peak
    // detection itself.
    d.orderMs       = n.anchored
                    ? (uint32_t)(_lapOrderUs(n, lap.raceElapsedMs) / 1000LL)
                    : 0u;
    _lapDeltas.push_back(d);
}

void MultiNodeManager::setRaceId(uint32_t raceId) {
    if (raceId == _raceId) return;
    _raceId          = raceId;
    // Acks are only meaningful inside one epoch.
    _ackedSeq        = 0;
    _hasAckedSeq     = false;
    _masterOldest    = 0;
    _masterWantsFrom = false;
    DEBUG("[LAPSYNC] Adopted race epoch %u\n", raceId);
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
        http.setConnectTimeout(800);
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
    // MARK, don't erase.
    //
    // This runs on the async_tcp task (the /kickNode and /removeNode
    // handlers).  It used to call _nodes.erase() directly, which shifts every
    // later element down and invalidates any iterator into the vector — while
    // parallelTask may be part-way through _checkNodeTimeouts, a fanout
    // builder, or the resync governor's raw NodeInfo*.  That is undefined
    // behaviour, and "Disconnect from Slot" is the one control that reaches
    // it: clicking it on an already-dead client could take the whole master
    // down rather than free a slot.
    //
    // The actual erase now happens in process(), on parallelTask, which is
    // the only task that iterates _nodes — so it can never run mid-walk.
    for (auto& n : _nodes) {
        if (n.nodeId == nodeId) {
            DEBUG("[MULTINODE] Node %c (%s) marked for removal\n", slotLetter(n.nodeId), n.pilotName.c_str());
            // Drop it out of every "is this node live" test immediately, so
            // the UI and the fanout stop treating it as present in the tick
            // or two before the reap.
            n.online         = false;
            n.running        = false;
            n.pendingRemoval = true;
            return true;
        }
    }
    return false;
}

// Reap nodes marked by removeNode().  parallelTask ONLY — see removeNode().
void MultiNodeManager::_reapRemovedNodes() {
    for (auto it = _nodes.begin(); it != _nodes.end(); ) {
        if (it->pendingRemoval) {
            DEBUG("[MULTINODE] Node %c slot freed\n", slotLetter(it->nodeId));
            // Release the grant if the departing node was holding it,
            // otherwise the single fleet-wide slot stays wedged until the
            // 120 s stall timeout.
            if (_resyncGrantNodeId == it->nodeId) _resyncGrantNodeId = 0;
            it = _nodes.erase(it);
        } else {
            ++it;
        }
    }
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
    // Mint the epoch HERE, not at GO (§5, §8).  Clients adopt it during the
    // countdown so the UDP start message can be authenticated against it —
    // and so the expensive setup happens while there is time to spare.
    // A cancelled countdown simply leaves an unused epoch behind; the next
    // pre-arm mints another.
    const uint32_t raceId = beginRaceEpoch();

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
            http.setConnectTimeout(500);
            int code = http.POST("");
            http.end();
            DEBUG("[MULTINODE] Race pre-arm → node %c (%s): HTTP %d\n", slotLetter(n.nodeId), n.staIP.c_str(), code);
        }
        vTaskDelay(1);

        // ARM: clear laps and adopt the epoch now, while the countdown gives
        // us seconds to do it in.  This is the work that used to happen at GO
        // and forced the start margin to cover six sequential HTTP round trips.
        HTTPClient arm;
        String armUrl = "http://" + n.staIP + "/timer/masterArm?raceId=" + String(raceId);
        if (arm.begin(armUrl)) {
            arm.setTimeout(500);
            arm.setConnectTimeout(500);
            int code = arm.POST("");
            arm.end();
            DEBUG("[MULTINODE] Race arm → node %c: HTTP %d\n", slotLetter(n.nodeId), code);
        }
        vTaskDelay(1);
    }

    // Open the clock-sync window (§8).  What follows this broadcast is the
    // countdown — 6-9 s during which nobody is racing and the network is
    // otherwise idle.  That is the moment to measure, not the Start All
    // fanout, where seven sequential POSTs are contending with each other.
    for (auto& n : _nodes) {
        // Retire the previous burst's best sample.  An offset measured 20
        // minutes ago is stale by ~24 ms at ±20 ppm, and the anchor is built
        // from it — so each race re-establishes the offset from scratch
        // rather than inheriting a lucky old sample.  clockValid stays set so
        // a node whose probes all fail still has something to fall back on.
        n.clockDelayUs = 0xFFFFFFFF;
        n.clockSamples = 0;
    }
    // Ship ONE directorState before going quiet.
    //
    // The sync window suppresses the fanout, and "Arm your quad" is carried by
    // that fanout (race.prearmActive) — so suppressing it for the whole
    // countdown meant clients never learned they were in pre-arm at all, and
    // then received the deferred pre-arm payload at GO, showing the banner at
    // the exact moment the race started.  The banner has to lead the race, not
    // trail it.
    //
    // One fanout costs ~500 ms of a window that is now the entire countdown,
    // and it is the only state clients genuinely need before the start.
    if (_webserver) {
        _webserver->pushMultiNodeState();
        if (_directorStatePayloadValid) {
            _directorStateBroadcastPending = false;
            _lastDirectorBroadcastMs       = millis();
            _broadcastDirectorState();
        }
    }

    // Open-ended: this closes when _broadcastRaceStart() runs, so it covers
    // the countdown however long the announcer takes.  The constant is only a
    // safety cap for a countdown that never completes.
    _clockSyncUntilMs = millis() + CLOCK_SYNC_MAX_MS;
    _lastClockProbeMs = 0;   // start probing on the very next tick
    DEBUG("[CLOCK] Pre-arm sync window open (closes at race start, cap %u ms)\n",
          CLOCK_SYNC_MAX_MS);
}

void MultiNodeManager::setExcludeNodes(const std::vector<uint8_t>& ids) {
    _excludeNodes = ids;
}

// Instrumentation for the clock chain (§8).  Everything needed to judge
// whether a race's ordering can be trusted, without opening a serial monitor:
//
//   offsetUs   how far this node's clock sits from ours
//   delayUs    round trip of the sample that offset came from — the honest
//              error bar.  A 2 ms delay means roughly ±1 ms of offset error.
//   samples    probes landed in the current burst; ~8 is a healthy countdown
//   driftPpm   fitted relative rate; also a hardware canary.  A unit reading
//              80 ppm while its neighbours read 15 is either cooking in the
//              sun or has a marginal crystal, and that is worth knowing before
//              somebody questions the results.
String MultiNodeManager::buildClockReport() const {
    String out;
    out.reserve(160 + _nodes.size() * 180);
    out  = "{\"masterUs\":"; out += (long long)esp_timer_get_time();
    out += ",\"syncing\":";  out += ((int32_t)(_clockSyncUntilMs - millis()) > 0) ? "true" : "false";
    // Start-margin telemetry: what the last race cost, what we allowed for it,
    // and how much healing headroom is currently being carried.
    out += ",\"startMarginUs\":"; out += (long long)_lastStartMarginUs;
    out += ",\"lastFanoutUs\":";  out += (long long)_lastFanoutUs;
    out += ",\"startBoostUs\":";  out += (long long)_startBoostUs;
    out += ",\"nextMarginUs\":";  out += (long long)_computeStartMarginUs();
    out += ",\"nodes\":[";
    bool first = true;
    for (const auto& n : _nodes) {
        if (n.pendingRemoval) continue;
        if (!first) out += ',';
        first = false;
        out += "{\"nodeId\":";      out += (int)n.nodeId;
        out += ",\"online\":";      out += n.online ? "true" : "false";
        out += ",\"clockValid\":";  out += n.clockValid ? "true" : "false";
        out += ",\"offsetUs\":";    out += (long long)n.clockOffsetUs;
        out += ",\"delayUs\":";     out += (n.clockDelayUs == 0xFFFFFFFF) ? -1 : (int32_t)n.clockDelayUs;
        // Unfiltered latest sample — the series an external logger fits.
        out += ",\"rawOffsetUs\":"; out += (long long)n.rawOffsetUs;
        out += ",\"rawDelayUs\":";  out += (uint32_t)n.rawDelayUs;
        out += ",\"rawAtUs\":";     out += (long long)n.rawAtUs;
        out += ",\"samples\":";     out += (int)n.clockSamples;
        out += ",\"driftPpm\":";    out += (int)n.driftPpm;
        out += ",\"driftValid\":";  out += n.driftValid ? "true" : "false";
        out += ",\"driftSamples\":";out += (int)n.driftCount;
        out += ",\"anchored\":";    out += n.anchored ? "true" : "false";
        out += ",\"anchorUs\":";    out += (long long)n.anchorUs;
        out += ",\"startAcked\":";  out += n.startAcked ? "true" : "false";
        out += ",\"startLateMs\":"; out += (uint32_t)n.startLateMs;
        out += '}';
    }
    out += "]}";
    return out;
}

void MultiNodeManager::_broadcastRaceStart() {
    // The countdown is over, so the sync burst is over: close the window
    // explicitly rather than letting it lapse.  Two reasons — the anchor is
    // taken right here and further burst probes cannot improve it, and the
    // directorState fanout (suppressed while the window is open) should resume
    // the instant there is race state worth pushing.  Drift keeps accruing
    // from the steady-state re-probe.
    _clockSyncUntilMs = 0;

    // Drop whatever was queued during the countdown.  It was built at pre-arm
    // and says prearmActive=true — shipping it now, the instant the window
    // reopens, is what put "Arm your quad" on screen at the moment the race
    // began.  A fresh payload is pushed from the main loop when the scheduled
    // start actually fires, and that one describes the race that is running.
    _directorStateBroadcastPending = false;

    // The epoch was minted at PRE-ARM and the clients adopted it then (§5, §8),
    // which is what lets the GO datagram be authenticated without a round trip.
    // A start that somehow arrives without a pre-arm still gets a valid epoch
    // rather than racing under 0.
    const uint32_t raceId = (_masterRaceId != 0) ? _masterRaceId : beginRaceEpoch();

    // ONE instant, in master time, that the whole fleet will start at (§8).
    // Everything below converts this single number into each client's clock.
    const int64_t marginUs       = _computeStartMarginUs();
    const int64_t fanoutBeganUs  = esp_timer_get_time();
    const int64_t targetMasterUs = fanoutBeganUs + marginUs;
    _lastStartMarginUs  = marginUs;
    _lastStartTargetUs  = targetMasterUs;   // kept for the repair path
    DEBUG("[CLOCK] Fleet start target +%lld ms (fanout last time %lld ms, boost %lld ms)\n",
          (long long)(marginUs / 1000), (long long)(_lastFanoutUs / 1000),
          (long long)(_startBoostUs / 1000));

    // The master is a pilot too, and it is already IN master time — its target
    // needs no conversion.  Scheduled before the fanout so that if anything
    // below stalls, the host still starts on the instant it published.
    //
    // This is why /api/multinode/race/start no longer starts the timer itself:
    // doing so would have put the host a full RACE_START_MARGIN_US ahead of
    // every client it just told to wait.
    if (_timer) _timer->scheduleStart(targetMasterUs);

    uint8_t missed = 0;   // clients whose target had already passed on arrival

    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        // Skip excluded nodes (e.g., solo racers the director chose to leave running)
        bool excluded = false;
        for (uint8_t id : _excludeNodes) { if (id == n.nodeId) { excluded = true; break; } }
        n.excludedFromCurrentRace = excluded;
        if (excluded) {
            // No start ack means no anchor, so this node is unanchored for the
            // whole race: stored and displayed, but never placed on the
            // field's timeline (§8).  Anchors are never back-filled — a
            // fabricated one would misplace every lap it covers.
            n.anchored = false;
            DEBUG("[MULTINODE] Race start → node %c (%s): SKIPPED (excluded)\n", slotLetter(n.nodeId), n.staIP.c_str());
            continue;
        }
        // ── Fast path: GO as a datagram ─────────────────────────────────
        // The client was armed during the countdown, so all that is left to
        // send is the instant to start at, expressed in ITS clock:
        //     master_us = client_us - offset  =>  client_us = target + offset
        //
        // Sent CLOCK_START_REPEATS times a few ms apart.  There is no ack —
        // waiting for one would put us back on round trips, which is the cost
        // this removes — so redundancy substitutes for acknowledgement.  A
        // duplicate is harmless: the client ignores a GO once it is running.
        if (n.clockValid) {
            IPAddress dst;
            if (dst.fromString(n.staIP) && _clockUdpReady) {
                ClockStartMsg msg;
                msg.magic     = CLOCK_START_MAGIC;
                msg.raceId    = raceId;
                msg.startAtUs = targetMasterUs + n.clockOffsetUs;
                for (uint8_t r = 0; r < CLOCK_START_REPEATS; r++) {
                    _clockUdp.writeTo((const uint8_t*)&msg, sizeof(msg),
                                      dst, CLOCK_PROBE_UDP_PORT);
                    if (r + 1 < CLOCK_START_REPEATS) delayMicroseconds(CLOCK_START_REPEAT_GAP_US);
                }
                // Exact by construction: we COMMANDED this node to start at
                // targetMasterUs in our own time, so that is its anchor.  No
                // measurement, no inference, no reply needed.
                n.anchorUs = targetMasterUs;
                n.anchored = true;
                n.startAcked = false;   // set by the ack datagram, checked below
                DEBUG("[MULTINODE] Race GO → node %c (%s): UDP x%u\n",
                      slotLetter(n.nodeId), n.staIP.c_str(), CLOCK_START_REPEATS);
                continue;
            }
        }

        // ── Fallback: a node with no measured clock offset ───────────────
        // Registered during the countdown, or its probes all failed.  It gets
        // the original HTTP start and the accuracy that came with it.
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStart?raceId=" + String(raceId);
        const bool scheduled = false;
        int64_t clientStartUs = 0;
        if (http.begin(url)) {
            http.setTimeout(500);
            http.setConnectTimeout(500);
            const uint32_t t1 = millis();
            int code = http.POST("");
            const uint32_t t4 = millis();
            String reply = (code > 0 && code < 400) ? http.getString() : String();
            http.end();

            if (code > 0 && code < 400) {
                // Anchor from the start instant the client REPORTED, converted
                // with the offset measured during pre-arm (§8).  No inference:
                // when the start was scheduled the client echoes back the
                // target it committed to, so the anchor is exact by
                // construction; when it started on arrival it reports the
                // instant it actually did.
                bool anchored = false;
                const int ip = reply.indexOf("\"raceStartUs\":");
                if (ip >= 0) {
                    const int64_t reportedUs =
                        strtoll(reply.c_str() + ip + 14, nullptr, 10);
                    anchored = recordAnchorFromReport(n.nodeId, reportedUs);

                    // Did the client honour the schedule?  A mismatch means it
                    // fell back to starting on arrival — the target had already
                    // passed, i.e. RACE_START_MARGIN_US was too small for this
                    // fanout.  Worth shouting about: it silently reintroduces
                    // exactly the start spread this mechanism removes.
                    if (scheduled) {
                        const int64_t deltaUs = reportedUs - clientStartUs;
                        if (reply.indexOf("\"scheduled\":true") < 0) {
                            missed++;
                            DEBUG("[CLOCK] Node %c did NOT honour the schedule "
                                  "(off by %+lld us) — margin too small\n",
                                  slotLetter(n.nodeId), (long long)deltaUs);
                        }
                    }
                }
                // Degraded path: a node that registered during the countdown
                // has no offset yet, and an older client sends no raceStartUs
                // at all.  Both still race — they just carry the old estimate's
                // accuracy until the next pre-arm syncs them.
                if (!anchored) recordAnchorFromRtt(n.nodeId, t4, t4 - t1);
            } else {
                n.anchored = false;
            }
            DEBUG("[MULTINODE] Race start → node %c (%s): HTTP %d rtt=%u\n",
                  slotLetter(n.nodeId), n.staIP.c_str(), code, (unsigned)(t4 - t1));
        }
        vTaskDelay(1);  // yield between nodes so async_tcp stays fed
    }
    _excludeNodes.clear();  // consumed — reset for next race

    // ── Fast repair: who did NOT acknowledge? ───────────────────────────
    // Acks arrive asynchronously on the UDP callback, so a short wait here
    // costs one delay rather than a round trip per node.  Anyone still silent
    // gets the GO again immediately — which is far better than discovering it
    // three seconds later from a heartbeat, by which time they have missed
    // gates rather than milliseconds.
    vTaskDelay(pdMS_TO_TICKS(START_ACK_WAIT_MS));
    for (auto& n : _nodes) {
        if (n.pendingRemoval || !n.online || n.staIP.isEmpty()) continue;
        if (n.excludedFromCurrentRace || !n.clockValid || n.startAcked) continue;
        IPAddress dst;
        if (!dst.fromString(n.staIP) || !_clockUdpReady) continue;
        ClockStartMsg msg;
        msg.magic     = CLOCK_START_MAGIC;
        msg.raceId    = raceId;
        msg.startAtUs = targetMasterUs + n.clockOffsetUs;
        for (uint8_t r = 0; r < CLOCK_START_REPEATS; r++) {
            _clockUdp.writeTo((const uint8_t*)&msg, sizeof(msg), dst, CLOCK_PROBE_UDP_PORT);
            if (r + 1 < CLOCK_START_REPEATS) delayMicroseconds(CLOCK_START_REPEAT_GAP_US);
        }
        DEBUG("[CLOCK] Node %c did not ack GO — resent\n", slotLetter(n.nodeId));
    }

    // ── Measure, then heal ──────────────────────────────────────────────
    // How long distribution ACTUALLY took, at this venue, with this many
    // pilots, through today's RF.  Next race's margin is computed from it.
    _lastFanoutUs = esp_timer_get_time() - fanoutBeganUs;

    // Nothing acknowledges a GO datagram, so schedule a check instead: once
    // the target has passed and a heartbeat has had time to tell us the truth,
    // anyone who should be racing and is not gets repaired.
    _startVerifyAtMs = millis() + (uint32_t)(marginUs / 1000) + START_VERIFY_GRACE_MS;

    if (missed > 0) {
        // Someone got a target that had already passed.  Buy headroom now —
        // a start that is slightly late is invisible, a start that is missed
        // puts that pilot back on the old per-node spread.
        _startBoostUs += START_BOOST_STEP_US;
        if (_startBoostUs > START_BOOST_MAX_US) _startBoostUs = START_BOOST_MAX_US;
        DEBUG("[CLOCK] %u node(s) missed the start target — boost now %lld ms\n",
              missed, (long long)(_startBoostUs / 1000));
    } else if (_startBoostUs > 0) {
        // Clean race.  Give a little back so a one-off bad race does not
        // permanently tax every race after it.
        _startBoostUs -= START_BOOST_DECAY_US;
        if (_startBoostUs < 0) _startBoostUs = 0;
    }
    DEBUG("[CLOCK] Start fanout took %lld ms (margin allowed %lld ms)\n",
          (long long)(_lastFanoutUs / 1000), (long long)(_lastStartMarginUs / 1000));
}

// A GO that never landed is the one failure a pilot cannot recover from, and
// UDP gives us no ack to notice it.  So we check the outcome instead of the
// delivery: by now every non-excluded node should be reporting `running`.
void MultiNodeManager::_verifyRaceStarts() {
    _startVerifyAtMs = 0;

    uint8_t repaired = 0;
    for (auto& n : _nodes) {
        if (n.pendingRemoval || !n.online || n.staIP.isEmpty()) continue;
        if (n.excludedFromCurrentRace) continue;
        if (n.running) continue;                 // heartbeat says it is racing

        // It should be racing and it is not.  Start it over HTTP, carrying the
        // ORIGINAL target — now several seconds in the past.  The client
        // backdates its race clock to it, so this pilot joins with the same
        // elapsed as everyone else rather than a clock that reads zero while
        // the field is at four seconds.
        //
        // They genuinely lost those seconds of flying, and their clock says so.
        // What they keep is comparability: their laps still sit on the field's
        // timeline, so the anchor set at GO remains true and they stay in the
        // standings.
        HTTPClient http;
        String url = "http://" + n.staIP + "/timer/masterStart?raceId=" + String(_masterRaceId);
        if (n.clockValid && _lastStartTargetUs != 0) {
            url += "&startAtUs=" + String((long long)(_lastStartTargetUs + n.clockOffsetUs));
        }
        if (http.begin(url)) {
            http.setTimeout(500);
            http.setConnectTimeout(500);
            const int code = http.POST("");
            http.end();
            DEBUG("[CLOCK] Node %c missed GO — repaired over HTTP (%d)\n",
                  slotLetter(n.nodeId), code);
        }
        // The anchor stays valid: the client backdates to the same target we
        // anchored against, so its laps remain comparable.  Only a node with
        // no clock offset (no target to backdate to) loses its place.
        if (!n.clockValid || _lastStartTargetUs == 0) n.anchored = false;
        repaired++;
        vTaskDelay(1);
    }

    if (repaired > 0) {
        // Feed the healing loop: next race buys more headroom.
        _startBoostUs += START_BOOST_STEP_US;
        if (_startBoostUs > START_BOOST_MAX_US) _startBoostUs = START_BOOST_MAX_US;
        DEBUG("[CLOCK] %u node(s) needed start repair — boost now %lld ms\n",
              repaired, (long long)(_startBoostUs / 1000));
    }
}

// Derived entirely from measurement — see the notes on START_MARGIN_MIN_US.
// The only judgement encoded here is the safety factors, and those are
// multipliers on measured quantities rather than substitutes for them.
int64_t MultiNodeManager::_computeStartMarginUs() const {
    int64_t worstOneWayUs = 0;
    uint8_t online = 0;
    for (const auto& n : _nodes) {
        if (!n.online || n.pendingRemoval) continue;
        online++;
        // clockDelayUs is a round trip, and only half of it lies between us
        // and the client.  This is the same measurement the clock offset is
        // built from, so it already reflects today's RF conditions.
        if (n.clockValid) {
            const int64_t oneWay = (int64_t)n.clockDelayUs / 2;
            if (oneWay > worstOneWayUs) worstOneWayUs = oneWay;
        }
    }

    // First race after boot: nothing measured yet, so scale by fleet size
    // rather than guessing a single number that is wrong for 2 pilots and
    // also wrong for 7.
    const int64_t fanoutUs = (_lastFanoutUs > 0)
                           ? _lastFanoutUs
                           : (int64_t)online * START_MARGIN_PER_NODE_US;

    int64_t margin = (fanoutUs * 3) / 2      // distribution, with 50% headroom
                   + worstOneWayUs * 3       // flight of the last datagram
                   + START_MARGIN_SLACK_US   // scheduling granularity
                   + _startBoostUs;          // healing

    if (margin < START_MARGIN_MIN_US) margin = START_MARGIN_MIN_US;
    if (margin > START_MARGIN_MAX_US) margin = START_MARGIN_MAX_US;
    return margin;
}

void MultiNodeManager::setNodeStartLate(uint8_t nodeId, uint32_t lateMs) {
    for (auto& n : _nodes) {
        if (n.nodeId != nodeId) continue;
        if (n.startLateMs != lateMs && lateMs > 0) {
            DEBUG("[CLOCK] Node %c reports it started %u ms late — gate "
                  "crossings in that window were not recorded\n",
                  slotLetter(nodeId), lateMs);
        }
        n.startLateMs = lateMs;
        return;
    }
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

bool MultiNodeManager::directorBroadcastDue(uint32_t nowMs) {
    if (!isMasterMode()) return false;
    return (nowMs - _lastDirectorBroadcastMs) >= MIN_DIRECTOR_BROADCAST_INTERVAL_MS;
}

void MultiNodeManager::queueDirectorStateBroadcast(const String& payload) {
    if (!isMasterMode()) return;
    // Assigning into the retained buffer rather than a fresh String: capacity
    // is reused when it is already large enough, so the steady state does no
    // allocation at all.
    _directorStatePayload          = payload;     // overwrites any pending payload — newest wins
    _directorStatePayloadValid     = true;
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
    if (!_directorStatePayloadValid || _directorStatePayload.isEmpty()) return;
    for (auto& n : _nodes) {
        if (!n.online || n.staIP.isEmpty()) continue;
        HTTPClient http;
        String url = "http://" + n.staIP + "/api/multinode/directorState";
        if (http.begin(url)) {
            // BOTH are required.  setTimeout() bounds only the READ phase —
            // HTTPClient keeps a separate _connectTimeout defaulting to
            // HTTPCLIENT_DEFAULT_TCP_TIMEOUT (5000 ms), and connect() uses
            // that one.  Against an UNREACHABLE client the connect alone
            // blocked for 5 s regardless of the line above, turning a single
            // dropped client into a 5 s Core-0 stall during which further
            // clients missed their heartbeat window and dropped too.
            // Measured 2026-08-07: stalls of 5004/5008 ms under 7-client load.
            http.setTimeout(300);
            http.setConnectTimeout(300);
            http.addHeader("Content-Type", "application/json");
            http.POST(_directorStatePayload);
            http.end();
        }
        vTaskDelay(1);
    }
    // Empty WITHOUT releasing the buffer — see the note on _directorStatePayload
    // in multinode.h.  `= String()` here used to hand back ~17 KB after every
    // fanout, guaranteeing the next one had to re-find a contiguous block.
    _directorStatePayload      = "";
    _directorStatePayloadValid = false;
}

void MultiNodeManager::_broadcastRaceStop() {
    _startVerifyAtMs = 0;   // race is over; nothing left to verify or repair
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
            http.setConnectTimeout(500);
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
