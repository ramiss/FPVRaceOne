#include "ota.h"

#include "config.h"
#include "laptimer.h"
#include "debug.h"
#include "multinode.h"
#include "fpv_webserver.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

OtaManager otaManager;

// ── Filesystem-update sentinel (NVS) ────────────────────────────────────────
// The spiffs/LittleFS partition has no A/B copy, so its image is written in place;
// a power-loss or failed download mid-write leaves it corrupt with no rollback.
// Record the write window in NVS so an interrupted update is detectable on the next
// boot (surfaced via OtaManager::isFsRecoveryPending(); re-running the update heals
// it). Writes are tiny and infrequent — no flash-wear concern.
namespace {
constexpr char    OTA_NVS_NS[]       = "ota";
constexpr char    OTA_KEY_FSSTATE[]  = "fsState";
constexpr char    OTA_KEY_FSURL[]    = "fsUrl";
constexpr char    OTA_KEY_FSVER[]    = "fsVer";
constexpr uint8_t OTA_FS_OK            = 0;  // filesystem consistent
constexpr uint8_t OTA_FS_WRITING       = 1;  // write started, not yet confirmed complete
constexpr uint8_t OTA_FS_PENDING_RETRY = 2;  // write failed/interrupted — re-run update

void otaSetFsState(uint8_t state, const String& url = String(), const String& ver = String()) {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, false)) return;   // read-write
    p.putUChar(OTA_KEY_FSSTATE, state);
    if (url.length()) p.putString(OTA_KEY_FSURL, url);
    if (ver.length()) p.putString(OTA_KEY_FSVER, ver);
    p.end();
}

uint8_t otaGetFsState() {
    Preferences p;
    if (!p.begin(OTA_NVS_NS, true)) return OTA_FS_OK;   // read-only; absent namespace => OK
    uint8_t state = p.getUChar(OTA_KEY_FSSTATE, OTA_FS_OK);
    p.end();
    return state;
}
} // namespace

bool OtaManager::filesystemUpdateIncomplete() {
    const uint8_t st = otaGetFsState();
    return (st == OTA_FS_WRITING || st == OTA_FS_PENDING_RETRY);
}

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

// HTTPClient::GET() returns a positive HTTP status code on success (200, 404, …)
// or a negative HTTPC_ERROR_* on a pre-response failure (TLS handshake, DNS,
// timeout, captive portal, etc.).  Raw "-1"/"-7" codes mean nothing to a pilot;
// translate them into actionable English.
static String describeHttpFailure(int code, const char* what) {
    if (code > 0) {
        // Positive == real HTTP status.  Leave the number — these are
        // recognisable and Google-able (403, 404, 500…).
        return String(what) + " returned HTTP " + String(code);
    }
    switch (code) {
        case HTTPC_ERROR_CONNECTION_REFUSED:
            return String("Could not reach ") + what +
                   " — your home WiFi may have no internet (or a captive portal is blocking it). "
                   "Verify the saved network is online and try again.";
        case HTTPC_ERROR_NOT_CONNECTED:
        case HTTPC_ERROR_CONNECTION_LOST:
            return String("Network connection to ") + what + " dropped mid-request. "
                   "Check your home WiFi signal and try again.";
        case HTTPC_ERROR_READ_TIMEOUT:
            return String(what) + " did not respond in time. "
                   "The network is slow or unreachable — try again, or use a stronger signal.";
        case HTTPC_ERROR_SEND_HEADER_FAILED:
        case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
            return String("Could not send request to ") + what +
                   ". This usually means a TLS handshake failed or the WiFi requires a captive-portal login.";
        case HTTPC_ERROR_NO_HTTP_SERVER:
            return String("Reached the network but no HTTPS server answered for ") + what +
                   ". The home WiFi may be a captive portal.";
        case HTTPC_ERROR_TOO_LESS_RAM:
            return "Not enough memory to complete the update request. Reboot the device and try again.";
        default:
            return String(what) + " network error (" +
                   HTTPClient::errorToString(code) + "). Check that your home WiFi has internet access.";
    }
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

void OtaManager::init(Config* config, LapTimer* timer, AsyncEventSource* events,
                      MultiNodeManager* multinode, Webserver* webserver) {
    _config    = config;
    _timer     = timer;
    _events    = events;
    _multinode = multinode;
    _webserver = webserver;
    _state  = STATE_IDLE;
    _progressPercent = 0;
    _statusMessage   = "";

    // Detect an interrupted filesystem update from a previous boot. WRITING means we
    // lost power mid-write, so the partition is presumed corrupt — promote it to
    // PENDING_RETRY. Either way, flag it so the rest of the system can surface that
    // the web assets may be incomplete and the update should be re-run.
    uint8_t fsState = otaGetFsState();
    if (fsState == OTA_FS_WRITING) {
        DEBUG("[OTA] Filesystem update was interrupted (sentinel=WRITING); marking for retry\n");
        otaSetFsState(OTA_FS_PENDING_RETRY);
        _fsRecoveryPending = true;
    } else if (fsState == OTA_FS_PENDING_RETRY) {
        _fsRecoveryPending = true;
    }
    if (_fsRecoveryPending) {
        DEBUG("[OTA] Filesystem may be incomplete — re-run the update to restore web assets.\n");
    }
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

void OtaManager::resumeMultinodeIfPaused() {
    if (_multinode) _multinode->resumeFromOta();
}

bool OtaManager::connectToHomeWifi(uint32_t timeoutMs, String& err) {
    if (!_config) { err = "Config not initialised"; return false; }
    const char* ssid = _config->getSsid();
    const char* pwd  = _config->getPassword();
    if (!ssid || ssid[0] == 0) {
        err = "Home WiFi SSID not configured";
        return false;
    }

    // Uniform AP+STA for every mode.  The aggressive WIFI_OFF→WIFI_STA
    // teardown (Recruit-style) was originally added because the master-mode
    // check crashed the AsyncWebServer request thread mid-WiFi-change; that
    // root cause is now fixed by running the check on Core 0 via
    // requestCheck() + loop(), so the simpler AP+STA path works in every
    // mode.  Multinode pause (in client mode the STA's already disconnected
    // from the master; in master mode client polls are rejected fast) keeps
    // TCP slot pressure off the radio.
    DEBUG("[OTA] Switching to AP+STA, connecting to '%s'\n", ssid);
    WiFi.mode(WIFI_AP_STA);
    delay(50);
    WiFi.setAutoReconnect(false);

    // Retry the association up to 3 times.  WiFi.begin's first attempt
    // intermittently fails on the C6 after a mode transition (master/client
    // observed ~20-30%) — the radio hasn't fully settled before begin()
    // fires, the SSID isn't found on the cached channel, or DHCP times out.
    // Each retry restarts the WPA handshake cleanly; a brief disconnect
    // between attempts clears any half-state from the previous try.
    constexpr uint8_t  WIFI_ATTEMPTS         = 3;
    const uint32_t     perAttemptTimeoutMs   = timeoutMs;   // each attempt gets the full budget
    for (uint8_t attempt = 1; attempt <= WIFI_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            DEBUG("[OTA] Retry attempt %u/%u — restarting WiFi.begin\n", attempt, WIFI_ATTEMPTS);
            setState(STATE_CONNECTING,
                     String("Connecting to home WiFi… (attempt ") + attempt + "/" + WIFI_ATTEMPTS + ")");
            WiFi.disconnect(false);
            delay(300);
        }
        WiFi.begin(ssid, pwd);

        uint32_t start = millis();
        bool connected = false;
        while (millis() - start < perAttemptTimeoutMs) {
            if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
            delay(100);
        }
        if (connected) {
            DEBUG("[OTA] STA connected — IP: %s, RSSI: %d dBm (attempt %u)\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), attempt);
            return true;
        }
        DEBUG("[OTA] Attempt %u/%u: WiFi.status()=%d, not connected after %u ms\n",
              attempt, WIFI_ATTEMPTS, (int)WiFi.status(), (unsigned)perAttemptTimeoutMs);
    }

    err = "Timed out connecting to home WiFi after " + String(WIFI_ATTEMPTS) +
          " attempts (check SSID/password, or signal strength)";
    DEBUG("[OTA] %s\n", err.c_str());
    return false;
}

void OtaManager::disconnectFromHomeWifi() {
    DEBUG("[OTA] Disconnecting STA after home-WiFi excursion\n");
    WiFi.disconnect(false);   // drop STA; keep stored creds

    const bool isClient = (_multinode && _multinode->isClientMode());

    if (isClient && _config) {
        // Client mode: the STA was associated to home WiFi, not the master.
        // The webserver's STA reconnect logic uses WiFi.reconnect(), which
        // would retry the home SSID.  Explicitly re-issue WiFi.begin() with
        // the master's credentials so the next reconnect tick targets the
        // right network.
        DEBUG("[OTA] Re-associating client STA to master after OTA\n");
        const char* masterSsid = _config->getMasterSSID();
        const char* masterPwd  = _config->getMasterPassword();
        if (masterSsid && masterSsid[0]) {
            WiFi.begin(masterSsid, (masterPwd && masterPwd[0]) ? masterPwd : "fpvraceone");
        }
    } else if (_multinode && _multinode->isMasterMode()) {
        // Master mode: AP+STA was kept up the whole time; just drop the
        // STA side and stay in AP-only so the master can resume serving its
        // own AP at 192.168.5.1.  Don't go through startAP() — the AP never
        // came down, so a full restart would needlessly drop the director's
        // browser connection.
        WiFi.mode(WIFI_AP);
    } else {
        // Single mode: plain AP-only.
        WiFi.mode(WIFI_AP);
    }
}

bool OtaManager::checkForUpdate(UpdateInfo& out, String& err) {
    if (_timer && _timer->isRunning()) {
        err = "Cannot check for updates while a race is running";
        return false;
    }
    // No isInProgress() guard here — when called from loop() via the async
    // requestCheck() path, requestCheck has already pre-set state to
    // CONNECTING (so the frontend's polling doesn't mistake a stale
    // UP_TO_DATE for "the new check finished"); that would otherwise self-
    // reject the very call we just queued.  requestCheck does its own
    // in-progress and race-running guards at the public entry.

    // Invalidate any previous cached result before we start.  If the check
    // succeeds we'll repopulate `_lastInfo` below.
    _lastInfoValid = false;

    // Set the CONNECTING state FIRST, before any WiFi-mode change.  In master
    // and client mode the next steps disrupt connectivity (AP teardown or
    // master-STA disconnect), so the SSE event has to be queued on the page
    // socket while the socket is still healthy.
    setState(STATE_CONNECTING, "Connecting to home WiFi...");
    setProgress(10);

    // Pause multinode networking for the duration of the home-WiFi excursion.
    // Idempotent — no-op if we're already in single mode.  Left paused on the
    // STATE_UPDATE_AVAILABLE exit so the apply phase can keep using the freed
    // STA radio / TCP slots; resumed on every other exit.
    if (_multinode) _multinode->pauseForOta();

    if (!connectToHomeWifi(15000, err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return false;
    }

    // ── Pre-flight network diagnostics ───────────────────────────────────
    // Read-only probes that run once per check, dumped to the serial log so
    // any future HTTPC_ERROR_CONNECTION_REFUSED (-1) can be triaged at a
    // glance: was it DNS, TCP-blocking, TLS, or memory?
    //
    //   1. STA cfg dump — confirms DHCP gave us a usable router + DNS.
    //      If gateway or dns1 read 0.0.0.0, the network never finished
    //      handing us configuration and the rest will fail.
    //   2. DNS probe — resolves api.github.com via WiFi.hostByName.  A
    //      timeout / failure here is the most common cause of -1 on
    //      home networks (ISP DNS unreachable, captive portal, IPv6-only
    //      DNS responses).
    //   3. Raw TCP-443 probe — opens a plain socket (no TLS) to the
    //      resolved IP.  If this fails, outbound 443 is blocked at the
    //      router or ISP and HTTPS can't possibly work.  If it succeeds,
    //      any later failure is TLS-side.
    //   4. Free-heap snapshot — mbedTLS needs ~30-40 KB for handshake
    //      buffers.  In master mode AsyncWebServer ties up RAM and
    //      handshake allocations can fail mid-way.
    DEBUG("[OTA] STA cfg: IP=%s gw=%s mask=%s dns1=%s dns2=%s ch=%d RSSI=%d dBm freeHeap=%u maxBlk=%u\n",
          WiFi.localIP().toString().c_str(),
          WiFi.gatewayIP().toString().c_str(),
          WiFi.subnetMask().toString().c_str(),
          WiFi.dnsIP(0).toString().c_str(),
          WiFi.dnsIP(1).toString().c_str(),
          WiFi.channel(), WiFi.RSSI(),
          (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
    {
        IPAddress ghIp;
        uint32_t dnsT0 = millis();
        int dnsOk = WiFi.hostByName("api.github.com", ghIp);
        uint32_t dnsMs = millis() - dnsT0;
        if (dnsOk > 0) {
            DEBUG("[OTA] DNS api.github.com → %s (%u ms)\n",
                  ghIp.toString().c_str(), (unsigned)dnsMs);
            WiFiClient tcp;
            uint32_t tcpT0 = millis();
            bool tcpOk = tcp.connect(ghIp, 443, 5000);
            uint32_t tcpMs = millis() - tcpT0;
            if (tcpOk) {
                DEBUG("[OTA] TCP probe %s:443 OK (%u ms) — outbound HTTPS reachable\n",
                      ghIp.toString().c_str(), (unsigned)tcpMs);
                tcp.stop();
            } else {
                DEBUG("[OTA] TCP probe %s:443 FAILED (%u ms) — router or ISP is blocking outbound 443\n",
                      ghIp.toString().c_str(), (unsigned)tcpMs);
            }
        } else {
            DEBUG("[OTA] DNS api.github.com FAILED (rc=%d, %u ms) — DHCP DNS server unreachable, filtered, or returning IPv6-only\n",
                  dnsOk, (unsigned)dnsMs);
        }
    }

    setState(STATE_CHECKING, "Querying GitHub for latest release...");
    setProgress(40);

    // Release the DebugLogger ring buffer's backing allocation (~26 KB when
    // full) right before TLS opens.  mbedTLS handshake needs ~16 KB of
    // CONTIGUOUS heap for its IN buffer, and on master mode the heap is
    // fragmented enough that ESP.getFreeHeap() reporting 70+ KB still can't
    // satisfy that — visible as MBEDTLS_ERR_SSL_ALLOC_FAILED on the lastError
    // line.  The log buffer is one large vector allocation, so dropping it
    // returns a contiguous hole that mbedTLS can use.  We lose the in-RAM log
    // history during the OTA window; UART output continues normally so nothing
    // is lost for serial-attached debugging.  The buffer regrows as future
    // DEBUG() calls push entries.
    DEBUG("[OTA] freeing DebugLogger ring (freeHeap=%u maxBlk=%u before)\n",
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    DebugLogger::getInstance().releaseBuffer();
    DEBUG("[OTA] DebugLogger released (freeHeap=%u maxBlk=%u after)\n",
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    WiFiClientSecure client;
    client.setInsecure();   // GitHub release downloads redirect through CDNs;
                            // pinning a cert chain across providers is fragile.
    HTTPClient http;
    http.setUserAgent("FPVRaceOne-OTA");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    // Bumped from 10s → 20s because the prerelease endpoint streams up to
    // ~30 KB (5 releases × ~6 KB of metadata) and the parser reads from the
    // stream as it arrives.  At -75 dBm RSSI, the full receive can take 12+ s.
    // The earlier 10 s ceiling produced intermittent "JSON parse error:
    // IncompleteInput" when the read window expired mid-stream.
    http.setTimeout(20000);

    // Always hit the array endpoint; we apply the channel filter (Published
    // vs Pre-release) ourselves below.  GitHub's /releases/latest endpoint
    // would server-side filter out prereleases and drafts, but its single-
    // object shape can't surface a 5-entry picker, and switching to the
    // array endpoint costs us essentially nothing now that the response is
    // chunk-decoded into a String and filter-parsed into a small doc.
    //
    // per_page=5 matches the picker size.  If your repo alternates stable
    // and pre-release tags heavily, this may not yield 5 of the selected
    // channel.  That's accurate (you really don't have 5 stable releases
    // yet); bump per_page later if it becomes an issue.
    //
    // _config->getOtaIncludePrereleases() is now interpreted as a channel
    // selector: 0=Published (stable releases only), 1=Pre-releases (only).
    // Either/or rather than the old "also include" toggle semantics.
    const bool prereleaseChannel = _config && _config->getOtaIncludePrereleases();
    String apiUrl = String("https://api.github.com/repos/") + UPDATE_REPO +
                    "/releases?per_page=5";
    // Build the JSON filter ONCE before the retry loop — the same filter
    // shape applies to every attempt.  Whitelisting only the fields we
    // actually read drops ~80% of the payload at parse-time (release-note
    // bodies, author objects, URL variants, reactions) so the doc stays tiny.
    // Both Published and Pre-release channels use the array endpoint now,
    // so the filter is always the array shape: filter[0]["..."] applies to
    // every entry in the array.
    DynamicJsonDocument filter(512);
    filter[0]["tag_name"]   = true;
    filter[0]["draft"]      = true;
    filter[0]["prerelease"] = true;
    filter[0]["assets"][0]["name"]                 = true;
    filter[0]["assets"][0]["browser_download_url"] = true;
    DynamicJsonDocument doc(4096);

    // Retry up to 3 times.  The retry covers BOTH layers:
    //   • HTTPClient pre-response failures (TLS handshake, DNS, timeout)
    //     return negative codes — typically transient.
    //   • Parse failures after a 200 response — usually IncompleteInput
    //     when the receive window expired or the stream returned short.
    //     Also transient (the user observed: retrying produces a real
    //     response).
    // Positive HTTP codes that aren't 200 (404, 403, 5xx) are NOT retried —
    // those are real server verdicts handled by the caller below.
    constexpr uint8_t GITHUB_ATTEMPTS = 3;
    int code = -1;
    DeserializationError jerr = DeserializationError::EmptyInput;
    for (uint8_t attempt = 1; attempt <= GITHUB_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            DEBUG("[OTA] GitHub retry attempt %u/%u\n", attempt, GITHUB_ATTEMPTS);
            setState(STATE_CHECKING,
                     String("Querying GitHub for latest release… (attempt ") +
                     attempt + "/" + GITHUB_ATTEMPTS + ")");
            delay(500);
        }
        if (!http.begin(client, apiUrl)) {
            err = "HTTP client init failed";
            setState(STATE_ERROR, err);
            disconnectFromHomeWifi();
            if (_multinode) _multinode->resumeFromOta();
            return false;
        }
        uint32_t getT0 = millis();
        code = http.GET();
        uint32_t getMs = millis() - getT0;
        if (code > 0) {
            DEBUG("[OTA] Attempt %u/%u HTTP %d (%u ms)\n",
                  attempt, GITHUB_ATTEMPTS, code, (unsigned)getMs);
            if (code != 200) {
                // Non-200 with a real HTTP code (404, 403, rate-limit) — not
                // a retry case.  Let the post-loop handlers deal with it.
                break;
            }
            // 200 — read the body via getString() so HTTPClient handles
            // any Transfer-Encoding: chunked framing for us, then parse from
            // the String.  Earlier we stream-parsed via http.getStream() to
            // save memory, but that returns the RAW TCP/TLS stream with the
            // chunk-size hex headers (e.g. "1FE0\r\n[{...") still in-band —
            // ArduinoJson sees those as garbage and reports IncompleteInput
            // within 20 ms.  getString() decodes chunks transparently.
            //
            // Memory: 5 releases of /releases output is ~25-30 KB wire size.
            // The DebugLogger release above frees ~26 KB of contiguous heap,
            // bringing maxBlk to 65-75 KB — comfortably enough for the
            // String + the filtered 4 KB doc.  Reset the doc on retries so a
            // partial-parse from the previous attempt doesn't leak in.
            uint32_t parseT0 = millis();
            String body = http.getString();
            http.end();
            doc.clear();
            jerr = deserializeJson(doc, body,
                                   DeserializationOption::Filter(filter));
            uint32_t parseMs = millis() - parseT0;
            if (!jerr) {
                DEBUG("[OTA] Attempt %u/%u parsed OK (%u bytes, %u ms)\n",
                      attempt, GITHUB_ATTEMPTS, (unsigned)body.length(),
                      (unsigned)parseMs);
                break;
            }
            // Parse failed — could be truncation on a heap-pressure
            // getString() (rare now), or a malformed response from GitHub.
            // Treat as transient and retry the whole GET.
            DEBUG("[OTA] Attempt %u/%u parse failed (%s, body=%u bytes, %u ms) — will retry\n",
                  attempt, GITHUB_ATTEMPTS, jerr.c_str(),
                  (unsigned)body.length(), (unsigned)parseMs);
            code = -1;   // force the post-loop guard to treat this as a failure
            continue;
        }
        DEBUG("[OTA] Attempt %u/%u failed (code=%d, %s, %u ms)\n",
              attempt, GITHUB_ATTEMPTS, code,
              HTTPClient::errorToString(code).c_str(), (unsigned)getMs);
        // Extra TLS-layer detail: HTTPClient's -1/-3/-11 codes are catch-all
        // "pre-response failure" buckets.  WiFiClientSecure::lastError() reports
        // the underlying mbedTLS code + message so we can distinguish:
        //   • handshake out-of-memory (free heap small)
        //   • cert chain rejection
        //   • peer closed before reply (firewall/captive portal)
        //   • DNS or routing issue (no socket ever opened)
        // Heap snapshot at failure shows whether mbedTLS handshake exhausted RAM.
        {
            char tlsBuf[128] = {0};
            int tlsErr = client.lastError(tlsBuf, sizeof(tlsBuf));
            DEBUG("[OTA] Attempt %u/%u TLS: lastError=%d (%s), freeHeap=%u maxBlk=%u\n",
                  attempt, GITHUB_ATTEMPTS, tlsErr,
                  tlsBuf[0] ? tlsBuf : "(no message)",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
        }
        http.end();
    }

    // 404 on the array endpoint only happens for a non-existent repo —
    // empty-array (no releases) returns HTTP 200 with [], handled after parse.
    if (code != 200) {
        // Any other non-200 (rate limit, auth, server error) is a real failure.
        // Negative codes are HTTPClient pre-response errors (TLS, DNS, timeout);
        // describeHttpFailure translates them into pilot-readable English.
        err = describeHttpFailure(code, "GitHub releases API");
        http.end();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return false;
    }

    // Final parse-success check.  jerr is updated on every attempt inside
    // the loop; if all three attempts failed to parse, surface the last
    // error message (typically "IncompleteInput") to the UI.
    if (jerr) {
        err = String("JSON parse error: ") + jerr.c_str();
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return false;
    }

    // Pick the first release object that matches the selected channel.  The
    // response is always an array of recent releases (newest-first); we walk
    // it and pick the first non-draft whose `prerelease` flag matches the
    // channel selector.  Drafts are private placeholders and never
    // installable, so they're always skipped.
    JsonArray arr = doc.as<JsonArray>();
    // Diagnostic: log every entry GitHub returned with its draft +
    // prerelease + tag.  If pilots see "No releases" while they have
    // releases on the repo, this tells us exactly why — empty array,
    // all drafts, or all on the wrong channel.
    DEBUG("[OTA] /releases returned %u entries (channel=%s)\n",
          (unsigned)arr.size(), prereleaseChannel ? "Pre-releases" : "Published");
    unsigned idx = 0;
    for (JsonObject r : arr) {
        DEBUG("[OTA]   [%u] tag=%s draft=%d prerelease=%d assets=%u\n",
              idx++,
              r["tag_name"].as<const char*>() ? r["tag_name"].as<const char*>() : "(none)",
              r["draft"].as<bool>() ? 1 : 0,
              r["prerelease"].as<bool>() ? 1 : 0,
              (unsigned)r["assets"].as<JsonArray>().size());
    }
    JsonObject release;
    for (JsonObject r : arr) {
        if (r["draft"].as<bool>()) continue;
        if (r["prerelease"].as<bool>() != prereleaseChannel) continue;
        release = r;
        break;
    }
    if (release.isNull()) {
        // No entry matched (empty array, all drafts, or every entry was on
        // the OTHER channel).  Treat as "no releases yet on this channel".
        out.currentVersion = FIRMWARE_VERSION;
        out.latestVersion  = "";
        out.releaseNotes   = "";
        out.firmwareUrl    = "";
        out.filesystemUrl  = "";
        out.available      = false;
        _lastInfo      = out;
        _lastInfoValid = true;
        setState(STATE_UP_TO_DATE,
                 prereleaseChannel
                     ? "No pre-releases published yet for this device's firmware repo."
                     : "No published releases yet for this device's firmware repo. "
                       "If pre-releases are available, switch Release Channel to \"Pre-releases\" above to see them.");
        setProgress(100);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return true;
    }

    out.currentVersion = FIRMWARE_VERSION;
    out.latestVersion  = release["tag_name"].as<String>();
    // releaseNotes intentionally left empty — the JSON filter above drops the
    // `body` field before parse to keep the response small.  The UI's "Release
    // Notes ↗" link surfaces the full notes via the GitHub web page instead.
    out.releaseNotes   = "";
    out.firmwareUrl    = "";
    out.filesystemUrl  = "";
    out.options.clear();

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
        if (_multinode) _multinode->resumeFromOta();
        return false;
    }

    // Build the full options list — every non-draft entry with both required
    // assets becomes an installable choice the UI can offer.  In stable mode
    // we only have one release so options[] is a single CURRENT/UPGRADE row.
    // In prerelease mode the array can hold up to 5 (per_page=5 set above).
    auto kindFor = [&](const String& tag) {
        int c = compareSemver(out.currentVersion, tag);
        if (c < 0) return ReleaseOption::UPGRADE;
        if (c > 0) return ReleaseOption::DOWNGRADE;
        return ReleaseOption::CURRENT;
    };
    auto pushOption = [&](JsonObject r) {
        ReleaseOption opt;
        opt.tag = r["tag_name"].as<String>();
        JsonArray a = r["assets"].as<JsonArray>();
        for (JsonObject asset : a) {
            String name = asset["name"].as<String>();
            String aurl = asset["browser_download_url"].as<String>();
            if (name == UPDATE_FW_ASSET) opt.firmwareUrl   = aurl;
            else if (name == UPDATE_FS_ASSET) opt.filesystemUrl = aurl;
        }
        // Skip rows that aren't installable — missing either asset means
        // the release isn't a complete pair and Install would fail later.
        if (opt.firmwareUrl.length() == 0 || opt.filesystemUrl.length() == 0) return;
        opt.kind = kindFor(opt.tag);
        out.options.push_back(opt);
    };
    // Walk the array, push every non-draft entry on the matching channel.
    // Drafts and entries on the other channel are silently skipped.  `arr`
    // is the same JsonArray we already iterated for diagnostic logging above.
    for (JsonObject r : arr) {
        if (r["draft"].as<bool>()) continue;
        if (r["prerelease"].as<bool>() != prereleaseChannel) continue;
        pushOption(r);
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
    // Leave multinode paused IFF an update is available, so the apply phase can
    // reuse the freed STA radio / TCP slots without a races-with-clients gap
    // between check and apply.  If we're up-to-date, resume now — the user is
    // done with home WiFi.
    if (_state != STATE_UPDATE_AVAILABLE && _multinode) {
        _multinode->resumeFromOta();
    }
    return true;
}

void OtaManager::requestApply(const String& fwUrl, const String& fsUrl) {
    _pendingFwUrl = fwUrl;
    _pendingFsUrl = fsUrl;
    _pendingApply = true;
}

void OtaManager::requestCheck() {
    // Guard rails surface as terminal-state messages so the polling frontend
    // sees them immediately on /api/update/status without waiting for loop().
    if (_timer && _timer->isRunning()) {
        setState(STATE_ERROR, "Cannot check for updates while a race is running");
        return;
    }
    if (isInProgress()) {
        setState(STATE_ERROR, "Another update operation is already in progress");
        return;
    }
    // Initial state so a poll between requestCheck() and loop()'s first tick
    // sees something more useful than IDLE.
    setState(STATE_CONNECTING, "Update check queued — starting…");
    setProgress(0);
    _pendingCheck = true;
}

bool OtaManager::downloadAndFlash(const String& url, int target, const char* label, String& err) {
    // Same rationale as checkForUpdate — free the log ring buffer to give
    // mbedTLS contiguous heap for its handshake IN buffer.
    DebugLogger::getInstance().releaseBuffer();
    DEBUG("[OTA] %s: starting download (freeHeap=%u maxBlk=%u)\n",
          label, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
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
            // getLastErrorString() returns things like "Stream Read Timeout" or
            // "Wrong HTTP code: 404" — usable but cryptic on its own. Add a
            // plain-English next step so a pilot knows what to do.
            err = String(label) + " download failed (" + httpUpdate.getLastErrorString() +
                  "). Likely cause: home WiFi dropped, GitHub blocked the request, "
                  "or the device ran low on memory. Re-run the update.";
            return false;
    }
}

bool OtaManager::preflightCheck(const String& fwUrl, const String& fsUrl, String& err) {
    // Fetch headers only (GET, then end() without reading the body) for BOTH assets
    // and confirm each resolves to a 200 of a plausible size. This is the guard
    // against the catastrophic case: wiping the filesystem and only then discovering
    // the firmware asset is missing/404, leaving the device with neither image.
    struct Target { const String& url; const char* label; long minBytes; };
    const Target targets[] = {
        { fwUrl, "firmware",   400L * 1024 },
        { fsUrl, "filesystem",  64L * 1024 },
    };

    for (const Target& t : targets) {
        if (t.url.length() == 0) {
            err = String("Pre-flight: missing ") + t.label + " URL";
            return false;
        }
        // Same rationale as checkForUpdate — drop the log ring buffer so the
        // mbedTLS handshake has a contiguous IN buffer to allocate.
        DebugLogger::getInstance().releaseBuffer();
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(15);   // seconds
        HTTPClient http;
        http.setUserAgent("FPVRaceOne-OTA");
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub → CDN redirect
        http.setTimeout(10000);
        if (!http.begin(client, t.url)) {
            err = String("Pre-flight: HTTP init failed for ") + t.label + " asset";
            return false;
        }
        int  code = http.GET();
        long len  = http.getSize();   // final Content-Length after redirects, or -1 if unknown
        http.end();

        if (code != HTTP_CODE_OK) {
            const String what = String(t.label) + " asset download";
            err = String("Pre-flight: ") + describeHttpFailure(code, what.c_str());
            if (code > 0 && code != HTTP_CODE_OK) {
                // For real HTTP statuses (404 etc.) tack on the most likely cause.
                err += " — the release may be missing its " + String(t.label) + " image.";
            }
            return false;
        }
        if (len >= 0 && len < t.minBytes) {
            err = String("Pre-flight: ") + t.label + " asset is implausibly small ("
                  + String(len) + " bytes)";
            return false;
        }
        DEBUG("[OTA] Pre-flight OK: %s asset reachable (%ld bytes)\n", t.label, len);
    }
    return true;
}

void OtaManager::loop() {
    // Pending check — runs the same checkForUpdate() as before, but on Core 0
    // where blocking WiFi/HTTPS calls and AP teardown don't crash the
    // AsyncWebServer request thread.  Out params are discarded; the frontend
    // reads results via /api/update/status (which serves the cached UpdateInfo).
    if (_pendingCheck) {
        _pendingCheck = false;
        UpdateInfo info;
        String err;
        checkForUpdate(info, err);
        // checkForUpdate sets state to UPDATE_AVAILABLE / UP_TO_DATE / ERROR
        // on every exit; nothing more for us to do.
    }

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

    // Defensive pause — the check path already paused if it found an update,
    // but if the user took longer than the 5-minute safety auto-resume to
    // click Update Now, or somehow reached apply without going through check,
    // we need to pause again.  Idempotent — no-op if already paused.
    if (_multinode) _multinode->pauseForOta();

    setState(STATE_CONNECTING, "Connecting to home WiFi for download...");
    setProgress(0);
    if (!connectToHomeWifi(20000, err)) {
        setState(STATE_ERROR, err);
        // Recover gracefully — failed before any flash write, safe to keep running.
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return;
    }

    // Pre-flight: confirm BOTH assets are reachable and sanely sized *before* writing
    // any partition. Prevents the worst case — wiping the filesystem, then finding the
    // firmware asset is a 404 and ending up with neither a working app nor a UI.
    setState(STATE_CHECKING, "Verifying update assets...");
    setProgress(0);
    if (!preflightCheck(fwUrl, fsUrl, err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return;
    }

    // Firmware FIRST (was: filesystem first). HTTPUpdate streams the app into the
    // *inactive* OTA slot and only flips the boot pointer on a fully MD5-verified
    // image, so a failure here leaves the filesystem completely untouched and we
    // abort cleanly on the current image. Firmware is also the larger, more
    // failure-prone download, so doing it while the FS is still pristine shrinks the
    // window in which a network drop can leave a corrupt filesystem.
    setState(STATE_DOWNLOADING_FW, "Downloading firmware image...");
    setProgress(0);
    if (!downloadAndFlash(fwUrl, U_FLASH, "Firmware", err)) {
        setState(STATE_ERROR, err);
        disconnectFromHomeWifi();
        if (_multinode) _multinode->resumeFromOta();
        return;
    }

    // Filesystem LAST. The spiffs/LittleFS partition has no A/B copy — this write is
    // in place and destructive, with no bootloader rollback. Bracket it with the NVS
    // sentinel so a power-loss or failure mid-write is detectable on the next boot
    // (OtaManager::isFsRecoveryPending()).
    otaSetFsState(OTA_FS_WRITING, fsUrl,
                  _lastInfoValid ? _lastInfo.latestVersion : String());
    setState(STATE_DOWNLOADING_FS, "Downloading filesystem image...");
    setProgress(0);
    if (!downloadAndFlash(fsUrl, U_SPIFFS, "Filesystem", err)) {
        // The firmware slot is already updated and its boot pointer flipped, so the
        // safest recovery is to re-run the update (which re-flashes the filesystem)
        // BEFORE power-cycling. Record the pending state and say so.
        otaSetFsState(OTA_FS_PENDING_RETRY, fsUrl);
        setState(STATE_ERROR, err + " — filesystem update incomplete. Re-run the update now (do not reboot first).");
        disconnectFromHomeWifi();
        // Leave multinode PAUSED on this specific failure: the user is expected
        // to immediately re-run the update without rebooting, and we want the
        // STA radio / TCP slots free for that retry.  The 5-minute safety
        // auto-resume will rescue them if they walk away instead.
        return;
    }
    otaSetFsState(OTA_FS_OK);

    // Success — device reboots in 3 s; the boot path restores the persisted
    // multinode mode from NVS naturally, so no explicit resume needed here.
    setState(STATE_REBOOTING, "Update complete — rebooting in 3 seconds...");
    setProgress(100);

    // Give SSE a chance to flush, AsyncTCP a chance to send the last events,
    // and the user a moment to read the message before the device reboots.
    delay(3000);
    DEBUG("[OTA] Rebooting after successful update\n");
    ESP.restart();
}
