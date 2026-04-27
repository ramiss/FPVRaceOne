#include "ota.h"

#include "config.h"
#include "laptimer.h"
#include "debug.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

OtaManager otaManager;

// Strip a leading "v" or "V" from a tag name so "v1.2.3" compares equal to "1.2.3".
static String stripVersionPrefix(const String& s) {
    if (s.length() > 0 && (s[0] == 'v' || s[0] == 'V')) return s.substring(1);
    return s;
}

// Returns -1 if a < b, 0 if equal, +1 if a > b.  Compares major.minor.patch
// numerically; missing components are treated as 0.  Pre-release suffixes
// (e.g. "1.2.3-rc1") are ignored — sscanf stops at the first non-numeric.
static int compareSemver(const String& a, const String& b) {
    String sa = stripVersionPrefix(a);
    String sb = stripVersionPrefix(b);
    int aMaj = 0, aMin = 0, aPat = 0;
    int bMaj = 0, bMin = 0, bPat = 0;
    sscanf(sa.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
    sscanf(sb.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);
    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aPat != bPat) return aPat < bPat ? -1 : 1;
    return 0;
}

void OtaManager::init(Config* config, LapTimer* timer, AsyncEventSource* events) {
    _config = config;
    _timer  = timer;
    _events = events;
    _state  = STATE_IDLE;
    _progressPercent = 0;
    _statusMessage   = "";
}

void OtaManager::setState(State s, const String& msg) {
    _state = s;
    _statusMessage = msg;
    emitProgress();
}

void OtaManager::setProgress(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    _progressPercent = percent;
    emitProgress();
}

void OtaManager::emitProgress() {
    if (!_events) return;
    // Escape double quotes in the message for JSON safety.  Status messages are
    // short and we control all sources, but this guards against future drift.
    String msg = _statusMessage;
    msg.replace("\\", "\\\\");
    msg.replace("\"", "\\\"");
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"state\":%d,\"progress\":%d,\"message\":\"%s\"}",
             (int)_state, _progressPercent, msg.c_str());
    _events->send(buf, "updateProgress");
}

bool OtaManager::connectToHomeWifi(uint32_t timeoutMs, String& err) {
    if (!_config) { err = "Config not initialised"; return false; }
    const char* ssid = _config->getSsid();
    const char* pwd  = _config->getPassword();
    if (!ssid || ssid[0] == 0) {
        err = "Home WiFi SSID not configured";
        return false;
    }

    DEBUG("[OTA] Switching to AP+STA, connecting to '%s'\n", ssid);

    // AP+STA keeps the user's browser connected to our AP while we use STA to
    // reach the internet.  ESP32 will retune the AP onto the STA's channel
    // when STA associates — clients may briefly drop and reassociate.
    WiFi.mode(WIFI_AP_STA);
    delay(50);
    WiFi.setAutoReconnect(false);
    WiFi.begin(ssid, pwd);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            err = "Timed out connecting to home WiFi (check SSID/password)";
            DEBUG("[OTA] %s\n", err.c_str());
            return false;
        }
        delay(100);
    }

    DEBUG("[OTA] STA connected — IP: %s, RSSI: %d dBm\n",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

void OtaManager::disconnectFromHomeWifi() {
    DEBUG("[OTA] Disconnecting STA, returning to AP-only\n");
    WiFi.disconnect(false);   // disconnect but don't erase stored creds
    WiFi.mode(WIFI_AP);
}

bool OtaManager::checkForUpdate(UpdateInfo& out, String& err) {
    if (_timer && _timer->isRunning()) {
        err = "Cannot check for updates while a race is running";
        return false;
    }
    if (isInProgress()) {
        err = "Another update operation is already in progress";
        return false;
    }

    setState(STATE_CONNECTING, "Connecting to home WiFi...");
    setProgress(10);
    if (!connectToHomeWifi(15000, err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    setState(STATE_CHECKING, "Querying GitHub for latest release...");
    setProgress(40);

    WiFiClientSecure client;
    client.setInsecure();   // GitHub release downloads redirect through CDNs;
                            // pinning a cert chain across providers is fragile.
    HTTPClient http;
    http.setUserAgent("FPVRaceOne-OTA");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);

    String apiUrl = String("https://api.github.com/repos/") + UPDATE_REPO + "/releases/latest";
    if (!http.begin(client, apiUrl)) {
        err = "HTTP client init failed";
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        err = "GitHub API returned HTTP " + String(code);
        http.end();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    String body = http.getString();
    http.end();

    DynamicJsonDocument doc(8192);
    DeserializationError jerr = deserializeJson(doc, body);
    if (jerr) {
        err = String("JSON parse error: ") + jerr.c_str();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    out.currentVersion = FIRMWARE_VERSION;
    out.latestVersion  = doc["tag_name"].as<String>();
    out.releaseNotes   = doc["body"].as<String>();
    out.firmwareUrl    = "";
    out.filesystemUrl  = "";

    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        String name = asset["name"].as<String>();
        String aurl = asset["browser_download_url"].as<String>();
        if (name == UPDATE_FW_ASSET) out.firmwareUrl   = aurl;
        else if (name == UPDATE_FS_ASSET) out.filesystemUrl = aurl;
    }

    if (out.firmwareUrl.length() == 0 || out.filesystemUrl.length() == 0) {
        err = String("Release '") + out.latestVersion + "' is missing required assets ("
              + UPDATE_FW_ASSET + ", " + UPDATE_FS_ASSET + ")";
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    out.available = compareSemver(out.currentVersion, out.latestVersion) < 0;

    if (out.available) {
        setState(STATE_UPDATE_AVAILABLE, String("Update available: ") + out.latestVersion);
    } else {
        setState(STATE_UP_TO_DATE, String("Already on latest (") + out.currentVersion + ")");
    }
    setProgress(100);

    // Drop STA — apply phase will reconnect when the user confirms.
    disconnectFromHomeWifi();
    return true;
}

void OtaManager::requestApply(const String& fwUrl, const String& fsUrl) {
    _pendingFwUrl = fwUrl;
    _pendingFsUrl = fsUrl;
    _pendingApply = true;
}

bool OtaManager::downloadAndFlash(const String& url, int target, const char* label, String& err) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);   // seconds — TLS handshake + read

    httpUpdate.rebootOnUpdate(false);    // we do one reboot at the very end
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    httpUpdate.onProgress([this, label](int cur, int total) {
        int pct = total > 0 ? (int)((int64_t)cur * 100 / total) : 0;
        _progressPercent = pct;
        char msg[80];
        snprintf(msg, sizeof(msg), "%s: %d%% (%d / %d KB)", label, pct, cur / 1024, total / 1024);
        _statusMessage = msg;
        emitProgress();
    });

    t_httpUpdate_return ret;
    if (target == U_FLASH) {
        ret = httpUpdate.update(client, url);
    } else {
        ret = httpUpdate.updateSpiffs(client, url);
    }

    switch (ret) {
        case HTTP_UPDATE_OK:
            return true;
        case HTTP_UPDATE_NO_UPDATES:
            err = String(label) + ": no update applied";
            return false;
        case HTTP_UPDATE_FAILED:
        default:
            err = String(label) + " download failed: " + httpUpdate.getLastErrorString()
                  + " (code " + String(httpUpdate.getLastError()) + ")";
            return false;
    }
}

void OtaManager::loop() {
    if (!_pendingApply) return;
    _pendingApply = false;

    String fwUrl = _pendingFwUrl;
    String fsUrl = _pendingFsUrl;
    _pendingFwUrl = "";
    _pendingFsUrl = "";

    if (_timer && _timer->isRunning()) {
        setState(STATE_ERROR, "Cannot update while a race is running");
        return;
    }
    if (fwUrl.length() == 0 || fsUrl.length() == 0) {
        setState(STATE_ERROR, "Missing firmware or filesystem URL");
        return;
    }

    String err;
    setState(STATE_CONNECTING, "Connecting to home WiFi for download...");
    setProgress(0);
    if (!connectToHomeWifi(20000, err)) {
        setState(STATE_ERROR, err);
        // Recover gracefully — failed before any flash write, safe to keep running.
        disconnectFromHomeWifi();
        return;
    }

    // Filesystem first.  If something goes wrong here the firmware partition is
    // still untouched; the device boots cleanly on the existing app + (now
    // half-written) SPIFFS.  SPIFFS is recoverable via re-running the update.
    setState(STATE_DOWNLOADING_FS, "Downloading filesystem image...");
    setProgress(0);
    if (!downloadAndFlash(fsUrl, U_SPIFFS, "Filesystem", err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return;
    }

    // Firmware last.  HTTPUpdate writes the new app to the inactive OTA slot
    // and flips the boot pointer.  If power is lost mid-write, the bootloader
    // falls back to the previously-good slot we're currently running from.
    setState(STATE_DOWNLOADING_FW, "Downloading firmware image...");
    setProgress(0);
    if (!downloadAndFlash(fwUrl, U_FLASH, "Firmware", err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return;
    }

    setState(STATE_REBOOTING, "Update complete — rebooting in 3 seconds...");
    setProgress(100);

    // Give SSE a chance to flush, AsyncTCP a chance to send the last events,
    // and the user a moment to read the message before the device reboots.
    delay(3000);
    DEBUG("[OTA] Rebooting after successful update\n");
    ESP.restart();
}
