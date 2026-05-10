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

static bool isAllDigits(const String& s) {
    if (s.length() == 0) return false;
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// `git describe --tags --always --dirty` produces strings like
// "1.2.3-87-g51aa636-dirty" for "87 commits past v1.2.3, working tree dirty".
// For OTA precedence we want to compare against the *base tag* (1.2.3 here),
// because the -N-gSHA-dirty suffix is git artefact, not a semver prerelease.
// Without this, a dev build of v0.1.2-beta.1 would look (under semver rules)
// "newer" than v0.1.2-beta.5 — `1-dirty` is alphanumeric, `5` is numeric,
// and per semver §11 alphanumeric > numeric.  That's clearly wrong for our
// usage where the dev build is in fact behind the published beta.
//
// Strips, in order:
//   1. trailing "-dirty"
//   2. trailing "-N-gHEX" where N is digits and HEX is hex chars (≥1)
static String stripGitDescribeNoise(const String& s) {
    String r = s;

    // 1. Trim "-dirty" suffix.
    if (r.length() >= 6 && r.endsWith("-dirty")) {
        r = r.substring(0, r.length() - 6);
    }

    // 2. Trim trailing "-N-gHEX" where N is digits and HEX is 1+ hex chars.
    int lastDash = r.lastIndexOf('-');
    if (lastDash > 0) {
        String last = r.substring(lastDash + 1);
        if (last.length() >= 2 && (last[0] == 'g' || last[0] == 'G')) {
            bool allHex = true;
            for (size_t i = 1; i < last.length(); i++) {
                char c = last[i];
                if (!((c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F'))) {
                    allHex = false;
                    break;
                }
            }
            if (allHex) {
                int prevDash = r.lastIndexOf('-', lastDash - 1);
                if (prevDash >= 0) {
                    String n = r.substring(prevDash + 1, lastDash);
                    if (isAllDigits(n)) {
                        r = r.substring(0, prevDash);
                    }
                }
            }
        }
    }

    return r;
}

// Compare two prerelease identifier strings ("beta.1" vs "beta.5", "rc.2"
// vs "beta.5", etc.) per SemVer §11.4.  Identifiers are split on ".";
// numeric identifiers compare numerically and have lower precedence than
// alphanumeric ones; fewer identifiers lose to a larger set if all earlier
// ones match.  Returns -1/0/+1.
static int comparePrereleaseIds(const String& a, const String& b) {
    int i = 0, j = 0;
    const int la = (int)a.length();
    const int lb = (int)b.length();
    while (i < la || j < lb) {
        int endI = a.indexOf('.', i);
        if (endI < 0 || endI > la) endI = la;
        int endJ = b.indexOf('.', j);
        if (endJ < 0 || endJ > lb) endJ = lb;

        bool aDone = (i >= la);
        bool bDone = (j >= lb);
        if (aDone && bDone) return 0;
        if (aDone) return -1;   // fewer identifiers => lower precedence
        if (bDone) return 1;

        String idA = a.substring(i, endI);
        String idB = b.substring(j, endJ);

        bool aNum = isAllDigits(idA);
        bool bNum = isAllDigits(idB);

        int cmp = 0;
        if (aNum && bNum) {
            long va = idA.toInt();
            long vb = idB.toInt();
            cmp = (va < vb) ? -1 : (va > vb ? 1 : 0);
        } else if (aNum && !bNum) {
            cmp = -1;   // numeric has lower precedence than alphanumeric
        } else if (!aNum && bNum) {
            cmp = 1;
        } else {
            int s = strcmp(idA.c_str(), idB.c_str());
            cmp = (s < 0) ? -1 : (s > 0 ? 1 : 0);
        }
        if (cmp != 0) return cmp;

        i = endI + 1;
        j = endJ + 1;
    }
    return 0;
}

// Returns -1 if a < b, 0 if equal, +1 if a > b.  Implements SemVer 2.0
// precedence rules:
//   * MAJOR.MINOR.PATCH compared numerically (missing components = 0)
//   * Build metadata after `+` is ignored (we never emit any)
//   * Prerelease handling:
//       - a version WITHOUT prerelease > the same version WITH prerelease
//         (e.g. 1.0.0 > 1.0.0-beta.5)
//       - prereleases compared identifier-by-identifier per comparePrereleaseIds
//   * git-describe noise (-N-gSHA, -dirty) is stripped first so dev builds
//     compare against their underlying tag, not a misleading longer suffix.
static int compareSemver(const String& a, const String& b) {
    String sa = stripGitDescribeNoise(stripVersionPrefix(a));
    String sb = stripGitDescribeNoise(stripVersionPrefix(b));

    // Drop build metadata (anything after '+'); it has no effect on precedence.
    int plusA = sa.indexOf('+');
    if (plusA >= 0) sa = sa.substring(0, plusA);
    int plusB = sb.indexOf('+');
    if (plusB >= 0) sb = sb.substring(0, plusB);

    // Split into core "X.Y.Z" and optional prerelease tail (everything after
    // the first '-' in the cleaned-up string).
    int dashA = sa.indexOf('-');
    int dashB = sb.indexOf('-');
    String coreA = (dashA >= 0) ? sa.substring(0, dashA) : sa;
    String coreB = (dashB >= 0) ? sb.substring(0, dashB) : sb;
    String preA  = (dashA >= 0) ? sa.substring(dashA + 1) : String("");
    String preB  = (dashB >= 0) ? sb.substring(dashB + 1) : String("");

    int aMaj = 0, aMin = 0, aPat = 0;
    int bMaj = 0, bMin = 0, bPat = 0;
    sscanf(coreA.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPat);
    sscanf(coreB.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPat);
    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aPat != bPat) return aPat < bPat ? -1 : 1;

    // Cores match — apply prerelease precedence.
    if (preA.length() == 0 && preB.length() == 0) return 0;
    if (preA.length() == 0) return  1;   // stable > prerelease
    if (preB.length() == 0) return -1;   // prerelease < stable
    return comparePrereleaseIds(preA, preB);
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

    // Invalidate any previous cached result before we start.  If the check
    // succeeds we'll repopulate `_lastInfo` below.
    _lastInfoValid = false;

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

    // Two endpoints, two response shapes:
    //   * /releases/latest — single object, server-side filters out drafts AND
    //     prereleases.  Returns HTTP 404 if no qualifying release exists.
    //   * /releases?per_page=10 — array of recent releases, includes drafts
    //     and prereleases.  Always 200 (with possibly empty array).
    const bool includePre = _config && _config->getOtaIncludePrereleases();
    String apiUrl = String("https://api.github.com/repos/") + UPDATE_REPO;
    if (includePre) {
        // 10 entries is plenty — the first non-draft is what we want.  Drafts
        // are extremely rare in practice (only created via the web UI), so
        // even one entry would usually suffice; 10 buys headroom.
        apiUrl += "/releases?per_page=10";
    } else {
        apiUrl += "/releases/latest";
    }
    if (!http.begin(client, apiUrl)) {
        err = "HTTP client init failed";
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    int code = http.GET();
    // Stable channel only: 404 means no qualifying full release exists.
    // Pre-release channel uses the array endpoint, which never 404s for an
    // existing repo — an empty array is handled below.
    if (code == 404 && !includePre) {
        http.end();
        out.currentVersion = FIRMWARE_VERSION;
        out.latestVersion  = "";
        out.releaseNotes   = "";
        out.firmwareUrl    = "";
        out.filesystemUrl  = "";
        out.available      = false;
        _lastInfo      = out;
        _lastInfoValid = true;
        setState(STATE_UP_TO_DATE, "No releases published yet for this device's firmware repo.");
        setProgress(100);
        disconnectFromHomeWifi();
        return true;
    }
    if (code != 200) {
        // Any other non-200 (rate limit, auth, server error) is a real failure.
        err = "GitHub API returned HTTP " + String(code);
        http.end();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    String body = http.getString();
    http.end();

    // Larger doc to fit a 10-entry releases array.  Stable channel uses far
    // less; sizing for the worst case keeps the code paths uniform.
    DynamicJsonDocument doc(16384);
    DeserializationError jerr = deserializeJson(doc, body);
    if (jerr) {
        err = String("JSON parse error: ") + jerr.c_str();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        return false;
    }

    // Pick the release object to inspect.  In stable mode the response is a
    // single object.  In prerelease mode it's an array; pick the first
    // non-draft entry (drafts are private placeholders, never installable).
    JsonObject release;
    if (includePre) {
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject r : arr) {
            if (!r["draft"].as<bool>()) {
                release = r;
                break;
            }
        }
        if (release.isNull()) {
            // Empty array, or every entry was a draft.  Treat as "no releases
            // yet" — same UX as the stable channel's 404 path.
            out.currentVersion = FIRMWARE_VERSION;
            out.latestVersion  = "";
            out.releaseNotes   = "";
            out.firmwareUrl    = "";
            out.filesystemUrl  = "";
            out.available      = false;
            _lastInfo      = out;
            _lastInfoValid = true;
            setState(STATE_UP_TO_DATE, "No releases published yet for this device's firmware repo.");
            setProgress(100);
            disconnectFromHomeWifi();
            return true;
        }
    } else {
        release = doc.as<JsonObject>();
    }

    out.currentVersion = FIRMWARE_VERSION;
    out.latestVersion  = release["tag_name"].as<String>();
    out.releaseNotes   = release["body"].as<String>();
    out.firmwareUrl    = "";
    out.filesystemUrl  = "";

    JsonArray assets = release["assets"].as<JsonArray>();
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

    // Single semver comparison drives both the `available` flag and the
    // displayed message — using the result rather than string equality means
    // an exact-tag match like "0.1.2-beta.5" vs "v0.1.2-beta.5" is recognised
    // as equivalent (the `v` prefix would otherwise wrongly trip the "dev
    // build ahead" branch).
    const int cmp = compareSemver(out.currentVersion, out.latestVersion);
    out.available = (cmp < 0);

    // Cache the result so /api/update/status can serve it even if the
    // original /api/update/check HTTP response gets dropped during the
    // AP-retune disconnect that happens when STA associates.
    _lastInfo      = out;
    _lastInfoValid = true;

    if (cmp < 0) {
        setState(STATE_UPDATE_AVAILABLE, String("Update available: ") + out.latestVersion);
    } else if (cmp == 0) {
        setState(STATE_UP_TO_DATE, String("You're on the latest release (") + out.latestVersion + ").");
    } else {
        // Running version's semver is strictly newer than the latest published
        // release — local dev build past the most recent tag.
        setState(STATE_UP_TO_DATE, String("You're on a development build (") + out.currentVersion +
                                   ") ahead of the latest release (" + out.latestVersion + ").");
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
