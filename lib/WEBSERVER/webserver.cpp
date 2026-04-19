#include "webserver.h"
#include <ElegantOTA.h>
#include <HTTPClient.h>

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <esp_wifi.h>
extern "C" {
  #include "lwip/dhcp.h"
  #include "lwip/netif.h"
}
#include "esp_netif.h"
#include "debug.h"
#include "config.h"

#ifdef ESP32S3
#include "rgbled.h"
extern RgbLed* g_rgbLed;
#include <SD.h>
#endif

static bool captiveDnsEnabled = false; // default OFF to preserve Android cellular internet

// Map a user-facing dBm value (2–21) to the nearest wifi_power_t enum constant.
static wifi_power_t dBmToWifiPower(uint8_t dbm) {
    if (dbm >= 21) return WIFI_POWER_21dBm;
    if (dbm >= 20) return WIFI_POWER_20dBm;
    if (dbm >= 19) return WIFI_POWER_19dBm;
    if (dbm >= 18) return WIFI_POWER_18_5dBm;
    if (dbm >= 17) return WIFI_POWER_17dBm;
    if (dbm >= 15) return WIFI_POWER_15dBm;
    if (dbm >= 13) return WIFI_POWER_13dBm;
    if (dbm >= 11) return WIFI_POWER_11dBm;
    if (dbm >= 8)  return WIFI_POWER_8_5dBm;
    if (dbm >= 7)  return WIFI_POWER_7dBm;
    if (dbm >= 5)  return WIFI_POWER_5dBm;
    return WIFI_POWER_2dBm;
}

// Global storage pointer for static functions
static Storage* g_storage = nullptr;

static const uint8_t DNS_PORT = 53;
static IPAddress netMsk(255, 255, 255, 0);
static DNSServer dnsServer;
static IPAddress ipAddress;
static AsyncWebServer server(80);
static AsyncEventSource events("/events");
// Randomised at boot — changes on every reboot so the browser always fetches
// fresh JS/CSS after a filesystem or firmware upload (both require a reboot).
static String _bootToken;

static const char *wifi_hostname = "FPVRaceOne";
static const char *wifi_ap_ssid_prefix = "FPVRaceOne";
static const char *wifi_ap_password = "fpvraceone";
static const char *wifi_ap_address = "192.168.4.1";
String wifi_ap_ssid;

// Scan 2.4GHz channels and return the least-congested non-overlapping channel (1, 6, or 11).
// Scoring: each nearby AP adds 1 point plus a RSSI weight (stronger = more interference).
// Lower score = better channel. Called once per boot before starting the AP.
static uint8_t selectBestWifiChannel() {
    const uint8_t candidates[] = {1, 6, 11};
    int score[14] = {};  // index 1-13; 0 unused

    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true, false, 120);  // 120ms per channel max
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            int ch = WiFi.channel(i);
            int rssi = WiFi.RSSI(i);  // negative, e.g. -70
            if (ch >= 1 && ch <= 13) {
                score[ch] += 1 + (rssi + 100) / 20;  // stronger signal = higher score
            }
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);

    uint8_t best = candidates[0];
    int bestScore = INT_MAX;
    for (uint8_t ch : candidates) {
        if (score[ch] < bestScore) {
            bestScore = score[ch];
            best = ch;
        }
    }
    DEBUG("WiFi channel scan: ch1=%d ch6=%d ch11=%d -> selected ch%d\n",
          score[1], score[6], score[11], best);
    return best;
}

void Webserver::init(Config *config, LapTimer *lapTimer, BatteryMonitor *batMonitor, Buzzer *buzzer, Led *l, RaceHistory *raceHist, Storage *stor, SelfTest *test, RX5808 *rx5808, TrackManager *trackMgr, WebhookManager *webhookMgr, MultiNodeManager *multiNodeMgr) {

    ipAddress.fromString(wifi_ap_address);

    conf = config;
    timer = lapTimer;
    monitor = batMonitor;
    buz = buzzer;
    led = l;
    history = raceHist;
    storage = stor;
    g_storage = stor;  // Set global pointer for static functions
    selftest = test;
    rx = rx5808;
    trackManager = trackMgr;
    webhooks = webhookMgr;
    multiNode = multiNodeMgr;
    transportMgr = nullptr;

    uint64_t mac = ESP.getEfuseMac();  // bytes 0-2 = OUI (same on all units), bytes 3-5 = unique
    char macStr[7];
    snprintf(macStr, sizeof(macStr), "%06llX", (mac >> 24) & 0xFFFFFF);
    wifi_ap_ssid = String(wifi_ap_ssid_prefix) + "_" + macStr;
    DEBUG("WiFi AP SSID configured: %s\n", wifi_ap_ssid.c_str());

    WiFi.persistent(false);
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);

    // Determine initial WiFi mode based on node mode and config
    uint8_t nodeMode = conf->getNodeMode();
    if (nodeMode == 2 && conf->getMasterSSID()[0] != 0) {
        // Client mode: AP+STA (own AP for pilot + STA to master)
        changeMode = WIFI_AP_STA;
    } else if (conf->getSsid()[0] == 0) {
        changeMode = WIFI_AP;
    } else {
        changeMode = WIFI_STA;
    }
    // Pre-expire the delay so the first handleWebUpdate() starts WiFi immediately
    // without waiting WIFI_RECONNECT_TIMEOUT_MS on every boot.
    changeTimeMs = millis() - WIFI_RECONNECT_TIMEOUT_MS;
    lastStatus = WL_DISCONNECTED;
}

void Webserver::setTransportManager(TransportManager *tm) {
    transportMgr = tm;
}

// TransportInterface implementation
void Webserver::sendLapEvent(uint32_t lapTimeMs) {
    if (!servicesStarted) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", lapTimeMs);
    events.send(buf, "lap");
}

void Webserver::sendRssiEvent(uint8_t rssi) {
    if (!servicesStarted) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", rssi);
    events.send(buf, "rssi");
}

void Webserver::sendRaceStateEvent(const char* state) {
    if (!servicesStarted) return;
    events.send(state, "raceState");
}

bool Webserver::isConnected() {
    // WiFi transport is always "connected" if services are started
    // Individual clients connect/disconnect via SSE but that's transparent
    return servicesStarted;
}

void Webserver::update(uint32_t currentTimeMs) {
    handleWebUpdate(currentTimeMs);
}

void Webserver::handleWebUpdate(uint32_t currentTimeMs) {
    // Note: Lap events are now broadcast via TransportManager in main.cpp
    // This method only handles WiFi-specific logic

    if (sendRssi && ((currentTimeMs - rssiSentMs) > WEB_RSSI_SEND_TIMEOUT_MS)) {
        sendRssiEvent(timer->getRssi());
        rssiSentMs = currentTimeMs;
    }

    // Send SSE keepalive ping to prevent connection timeout
    if (servicesStarted && ((currentTimeMs - sseKeepaliveMs) > WEB_SSE_KEEPALIVE_MS)) {
        events.send("ping", "keepalive", millis());
        sseKeepaliveMs = currentTimeMs;
    }

    // Push multiNodeClientState SSE to client browser when connection state changes
    if (servicesStarted && multiNode && multiNode->isClientMode() &&
        multiNode->consumeClientStateChanged()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"connected\":%s,\"nodeId\":%u}",
                 multiNode->isMasterConnected() ? "true" : "false",
                 multiNode->getMyNodeId());
        events.send(buf, "multiNodeClientState");
    }

    wl_status_t status = WiFi.status();

    if (status != lastStatus && (wifiMode == WIFI_STA || wifiMode == WIFI_AP_STA)) {
        DEBUG("WiFi STA status = %u\n", status);
        switch (status) {
            case WL_NO_SSID_AVAIL:
            case WL_CONNECT_FAILED:
            case WL_CONNECTION_LOST:
                if (wifiMode == WIFI_AP_STA) {
                    // Client node: keep AP up, just mark master as unreachable
                    DEBUG("[MULTINODE] STA lost — will retry registration\n");
                } else {
                    changeTimeMs = currentTimeMs;
                    changeMode = WIFI_AP;
                }
                break;
            case WL_DISCONNECTED:  // try reconnection
                changeTimeMs = currentTimeMs;
                break;
            case WL_CONNECTED:
                buz->beep(200);
                if (wifiMode != WIFI_AP_STA) led->off();
                wifiConnected = true;
                DEBUG("WiFi STA connected! IP: %s SSID: %s\n",
                      WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
#ifdef ESP32S3
                if (g_rgbLed) g_rgbLed->setStatus(STATUS_USER_CONNECTED);
#endif
                break;
            default:
                break;
        }
        lastStatus = status;
    }
    if (status != WL_CONNECTED && wifiMode == WIFI_STA && (currentTimeMs - changeTimeMs) > WIFI_CONNECTION_TIMEOUT_MS) {
        changeTimeMs = currentTimeMs;
        if (!wifiConnected) {
            changeMode = WIFI_AP;  // if we didnt manage to ever connect to wifi network
            // Signal WiFi connection failure - set orange color briefly
#ifdef ESP32S3
            if (g_rgbLed) {
                g_rgbLed->setColor(CRGB::Orange, RGB_SOLID);
            }
#endif
        } else {
            DEBUG("WiFi Connection failed, reconnecting\n");
            WiFi.reconnect();
            startServices();
            buz->beep(100);
            led->blink(200);
        }
    }
    if (changeMode != wifiMode && changeMode != WIFI_OFF && (currentTimeMs - changeTimeMs) > WIFI_RECONNECT_TIMEOUT_MS) {
        switch (changeMode) {
            case WIFI_AP: {
                // Scan for the least-congested non-overlapping channel once per boot
                static uint8_t apChannel = 0;
                if (apChannel == 0) {
                    apChannel = selectBestWifiChannel();
                }

                DEBUG("Changing to WiFi AP mode on ch%d\n", apChannel);

                WiFi.disconnect();
                wifiMode = WIFI_AP;
                WiFi.setHostname(wifi_hostname);  // must be set before WiFi.mode()
                WiFi.mode(WIFI_AP);
                changeTimeMs = currentTimeMs;

                WiFi.setTxPower(dBmToWifiPower(conf->getWifiTxPower()));

                // Master node uses 192.168.5.1 so client nodes can keep the standard 192.168.4.1
                if (conf->getNodeMode() == 1) {
                    ipAddress.fromString("192.168.5.1");
                }
                // Advertise NO default gateway — helps Android keep cellular data for internet
                WiFi.softAPConfig(ipAddress, IPAddress(0, 0, 0, 0), netMsk);

                DEBUG("Starting WiFi AP: %s with password: %s on ch%d\n", wifi_ap_ssid.c_str(), wifi_ap_password, apChannel);
                // Master mode supports up to 8 clients; single mode supports 4
                uint8_t maxConn = (conf->getNodeMode() == 1) ? 8 : 4;
                WiFi.softAP(wifi_ap_ssid.c_str(), wifi_ap_password, apChannel, 0, maxConn);

                // Disable modem power save — without this the AP sleeps between beacons
                // and clients drop ~50% of the time on ESP32C6.
                esp_wifi_set_ps(WIFI_PS_NONE);

                // Force HT20 (20 MHz) to reduce adjacent-channel interference
                esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
                // Standard 802.11 b/g/n — no proprietary LR (LR only works between ESP32 devices)
                esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

                DEBUG("WiFi AP started: SSID=%s ch=%d HT20\n", WiFi.softAPSSID().c_str(), apChannel);
                startServices();
                buz->beep(1000);
                led->on(1000);
                break;
            }
            case WIFI_AP_STA: {
                // Client node: own AP for pilot phone + STA connected to master
                DEBUG("[MULTINODE] Starting AP+STA mode (client node)\n");

                wifiMode = WIFI_AP_STA;
                WiFi.setHostname(wifi_hostname);
                WiFi.mode(WIFI_AP_STA);

                // Client AP stays on 192.168.4.1 (pilots use the same IP as always).
                // Master uses 192.168.5.1, so there is no subnet conflict.
                WiFi.softAPConfig(ipAddress, IPAddress(0, 0, 0, 0), netMsk);
                WiFi.softAP(wifi_ap_ssid.c_str(), wifi_ap_password, 0, 0, 4);

                // Disable modem power save on both interfaces for AP+STA stability
                esp_wifi_set_ps(WIFI_PS_NONE);

                esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
                esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
                esp_wifi_set_protocol(WIFI_IF_AP,  WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
                esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

                changeTimeMs = currentTimeMs;
                WiFi.setAutoReconnect(true);
                WiFi.begin(conf->getMasterSSID(), conf->getMasterPassword());
                startServices();
                buz->beep(500);
                led->blink(100);
                DEBUG("[MULTINODE] Client AP: 192.168.4.1  Connecting to master: %s\n", conf->getMasterSSID());
                break;
            }
            case WIFI_STA:
                DEBUG("Connecting to WiFi network\n");
                wifiMode = WIFI_STA;
                WiFi.setHostname(wifi_hostname);  // must be set before WiFi.mode()
                WiFi.mode(WIFI_STA);
                // Force HT20 and standard protocols for STA mode
                esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
                esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
                changeTimeMs = currentTimeMs;
                WiFi.setAutoReconnect(true);
                WiFi.begin(conf->getSsid(), conf->getPassword());
                startServices();
                led->blink(200);
            default:
                break;
        }

        changeMode = WIFI_OFF;
    }

    // Always process DNS in AP mode so connectivity probes get fast NXDOMAIN replies.
    // Without this, Android waits 5-10 s for DNS timeouts before deciding to use
    // cellular for internet — during which it may drop or freeze mobile data.
    if (servicesStarted && (wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA)) {
        dnsServer.processNextRequest();
    }
}

/** Is this an IP? */
static boolean isIp(String str) {
    for (size_t i = 0; i < str.length(); i++) {
        int c = str.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

/** IP to String? */
static String toStringIp(IPAddress ip) {
    String res = "";
    for (int i = 0; i < 3; i++) {
        res += String((ip >> (8 * i)) & 0xFF) + ".";
    }
    res += String(((ip >> 8 * 3)) & 0xFF);
    return res;
}

static bool captivePortal(AsyncWebServerRequest *request) {
    extern const char *wifi_hostname;

    // Allow fpvraceone.xyz, IP addresses, and hostname.local without redirecting
    if (!isIp(request->host()) && 
        request->host() != (String(wifi_hostname) + ".local") &&
        request->host() != "fpvraceone.xyz" &&
        request->host() != "www.fpvraceone.xyz") {
        DEBUG("Request redirected to captive portal\n");
        request->redirect(String("http://") + toStringIp(request->client()->localIP()));
        return true;
    }
    return false;
}

static void handleRoot(AsyncWebServerRequest *request) {
    if (captiveDnsEnabled && captivePortal(request)) {  // Only redirect in captive portal mode.
        return;
    }
#ifdef ESP32S3
    // Flash green when user accesses web interface
    extern RgbLed* g_rgbLed;
    if (g_rgbLed) g_rgbLed->flashGreen();
#endif

    if (!LittleFS.begin(false) || !LittleFS.exists("/index.html")) {
        request->send(500, "text/plain",
            "Web UI not found. LittleFS not mounted or /index.html missing.\n"
            "Did you add a LittleFS partition + run uploadfs?");
        return;
    }

    request->send(LittleFS, "/index.html", "text/html", false, [](const String& var) -> String {
        if (var == "BUILD_TIME") return _bootToken;
        return String();
    });
}

static void handleNotFound(AsyncWebServerRequest *request) {
    if (captiveDnsEnabled && captivePortal(request)) {  // Only redirect in captive portal mode.
        return;
    }

    String path = request->url();

#ifdef ESP32S3
    // Try SD card as a fallback for any unknown path (e.g. /sounds_*/file.mp3)
    if (g_storage && g_storage->isSDAvailable() && SD.exists(path)) {
        const char* contentType = "application/octet-stream";
        if (path.endsWith(".mp3")) contentType = "audio/mpeg";
        else if (path.endsWith(".svg")) contentType = "image/svg+xml";
        else if (path.endsWith(".ico")) contentType = "image/x-icon";
        else if (path.endsWith(".json")) contentType = "application/json";
        else if (path.endsWith(".txt")) contentType = "text/plain";
        DEBUG("[404->SD] Serving from SD fallback: %s\n", path.c_str());
        request->send(SD, path, contentType);
        return;
    }
#endif

    String message = F("File Not Found\n\n");
    message += F("URI: ");
    message += path;
    message += F("\nMethod: ");
    message += (request->method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += request->args();
    message += F("\n");

    for (uint8_t i = 0; i < request->args(); i++) {
        message += String(F(" ")) + request->argName(i) + F(": ") + request->arg(i) + F("\n");
    }
    AsyncWebServerResponse *response = request->beginResponse(404, "text/plain", message);
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "-1");
    request->send(response);
}

static bool startLittleFS() {
    Serial.println("[INFO] Attempting to mount LittleFS...");
    if (!LittleFS.begin(false)) {
        Serial.println("[WARN] LittleFS mount failed, attempting to format...");
        DEBUG("LittleFS mount failed, attempting to format...\n");
        if (!LittleFS.begin(true)) {
            Serial.println("[ERROR] LittleFS format failed!");
            DEBUG("LittleFS format failed\n");
            return false;
        }
        Serial.println("[INFO] LittleFS formatted and mounted");
        DEBUG("LittleFS formatted and mounted\n");
        return true;
    }
    Serial.println("[INFO] LittleFS mounted successfully");
    DEBUG("LittleFS mounted successfully\n");
    return true;
}

static void startMDNS() {
    DEBUG("Starting mDNS with hostname: %s\n", wifi_hostname);
    
    if (!MDNS.begin(wifi_hostname)) {
        DEBUG("ERROR: mDNS failed to start!\n");
        return;
    }

    String instance = String(wifi_hostname) + "_" + WiFi.macAddress();
    instance.replace(":", "");
    MDNS.setInstanceName(instance);
    MDNS.addService("http", "tcp", 80);
    
    DEBUG("mDNS started successfully\n");
    DEBUG("  Hostname: %s.local\n", wifi_hostname);
    DEBUG("  Instance: %s\n", instance.c_str());
    DEBUG("  HTTP service advertised on port 80\n");
}

void Webserver::startServices() {
    if (servicesStarted) {
        if (captiveDnsEnabled) {
            // Restart mDNS when WiFi mode changes
            MDNS.end();
            delay(100);  // Give mDNS time to shut down
            startMDNS();
            DEBUG("mDNS restarted for mode change\n");
        }
        return;
    }

    startLittleFS();
    _bootToken = String(esp_random(), HEX);  // unique per boot → cache-busts JS/CSS on every reboot

    // Initialize storage (SD card or LittleFS fallback)
    storage->init();
    
    // Initialize race history after storage is ready
    history->init(storage);

    server.on("/", handleRoot);
    if (captiveDnsEnabled) {
        server.on("/generate_204", handleRoot);  // handle Andriod phones doing shit to detect if there is 'real' internet and possibly dropping conn.
        server.on("/gen_204", handleRoot);
        server.on("/library/test/success.html", handleRoot);
        server.on("/hotspot-detect.html", handleRoot);
        server.on("/connectivity-check.html", handleRoot);
        server.on("/check_network_status.txt", handleRoot);
        server.on("/ncsi.txt", handleRoot);
        server.on("/fwlink", handleRoot);
    }

    server.on("/api/version", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = String("{\"firmwareVersion\":\"") + FIRMWARE_VERSION + "\"}";
        request->send(200, "application/json", json);
    });

    server.on("/status", [this](AsyncWebServerRequest *request) {
        char buf[1536];
        char configBuf[512];
        conf->toJsonString(configBuf);
        const char *format =
            "\
Heap:\n\
\tFree:\t%i\n\
\tMin:\t%i\n\
\tSize:\t%i\n\
\tAlloc:\t%i\n\
Storage:\n\
\tType:\t%s\n\
\tUsed:\t%llu\n\
\tTotal:\t%llu\n\
\tFree:\t%llu\n\
Chip:\n\
\tModel:\t%s Rev %i, %i Cores, SDK %s\n\
\tFlashSize:\t%i\n\
\tFlashSpeed:\t%iMHz\n\
\tCPU Speed:\t%iMHz\n\
Network:\n\
\tIP:\t%s\n\
\tMAC:\t%s\n\
EEPROM:\n\
%s";

        snprintf(buf, sizeof(buf), format,
                 ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getHeapSize(), ESP.getMaxAllocHeap(),
                 storage->getStorageType().c_str(), storage->getUsedBytes(), storage->getTotalBytes(), storage->getFreeBytes(),
                 ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), ESP.getSdkVersion(), ESP.getFlashChipSize(), ESP.getFlashChipSpeed() / 1000000, getCpuFrequencyMhz(),
                 WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str(), configBuf);
        request->send(200, "text/plain", buf);
        led->on(200);
    });

    server.on("/tuningstatus", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const char* status = (rx && rx->isSettingFrequency()) ? "setting" : "idle";
        String json = String("{\"tuningstatus\":\"") + status + "\"}";
        request->send(200, "application/json", json);
    });

    server.on("/timer/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
        timer->start();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("started");
        if (multiNode && multiNode->isClientMode()) multiNode->setTimerRunning(true);
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    server.on("/timer/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        timer->stop();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("stopped");
        if (multiNode && multiNode->isClientMode()) {
            if (multiNode->isMasterRaceActive()) {
                multiNode->setQuitPending();   // notify master the pilot quit early
            }
            multiNode->setTimerRunning(false);
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // /timer/masterPreArm — called by master before countdown; clients flash Start button
    server.on("/timer/masterPreArm", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (multiNode && multiNode->isClientMode()) {
            events.send("prearming", "masterRaceState");
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // /timer/masterStart — called by master broadcast; respects client skip toggle
    server.on("/timer/masterStart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (multiNode && multiNode->isClientMode() &&
            conf->getMnSkipMasterStart() && timer->isRunning()) {
            // Client opted to skip master start when already running
            request->send(200, "application/json", "{\"status\": \"SKIPPED\"}");
            return;
        }
        timer->start();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("started");
        if (multiNode && multiNode->isClientMode()) {
            multiNode->setTimerRunning(true);
            multiNode->setMasterRaceActive(true);
            events.send("started", "masterRaceState");
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // /timer/masterStop — called by master broadcast; clears masterRaceActive (no quit)
    server.on("/timer/masterStop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        timer->stop();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("stopped");
        if (multiNode && multiNode->isClientMode()) {
            multiNode->setTimerRunning(false);
            multiNode->setMasterRaceActive(false);
            events.send("stopped", "masterRaceState");
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    server.on("/timer/lap", HTTP_POST, [this](AsyncWebServerRequest *request) {
#ifdef ESP32S3
        if (g_rgbLed) {
            g_rgbLed->flashLap();
        }
#endif
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });

    // Manual lap addition - broadcasts lap event to all clients
    AsyncCallbackJsonWebHandler *addLapHandler = new AsyncCallbackJsonWebHandler("/timer/addLap", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        if (jsonObj.containsKey("lapTime")) {
            uint32_t lapTimeMs = jsonObj["lapTime"].as<uint32_t>();
            if (transportMgr) {
                transportMgr->broadcastLapEvent(lapTimeMs);
            }
#ifdef ESP32S3
            if (g_rgbLed) {
                g_rgbLed->flashLap();
            }
#endif
            // Trigger lap webhook if Gate LEDs enabled and Lap enabled
            if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookLap()) {
                webhooks->triggerLap();
            }
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.addHandler(addLapHandler);

    // Playback endpoints - replay saved races
    AsyncCallbackJsonWebHandler *playbackStartHandler = new AsyncCallbackJsonWebHandler("/timer/playbackStart", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (transportMgr) {
            transportMgr->broadcastRaceStateEvent("started");
        }
        // Trigger race start webhook if Gate LEDs enabled
        if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookRaceStart()) {
            webhooks->triggerRaceStart();
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.addHandler(playbackStartHandler);

    AsyncCallbackJsonWebHandler *playbackLapHandler = new AsyncCallbackJsonWebHandler("/timer/playbackLap", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        if (jsonObj.containsKey("lapTime")) {
            uint32_t lapTimeMs = jsonObj["lapTime"].as<uint32_t>();
            if (transportMgr) {
                transportMgr->broadcastLapEvent(lapTimeMs);
            }
#ifdef ESP32S3
            if (g_rgbLed) {
                g_rgbLed->flashLap();
            }
#endif
            // Trigger lap webhook if Gate LEDs enabled and Lap enabled
            if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookLap()) {
                webhooks->triggerLap();
            }
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.addHandler(playbackLapHandler);

    AsyncCallbackJsonWebHandler *playbackStopHandler = new AsyncCallbackJsonWebHandler("/timer/playbackStop", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (transportMgr) {
            transportMgr->broadcastRaceStateEvent("stopped");
        }
        // Trigger race stop webhook if Gate LEDs enabled
        if (webhooks && conf->getGateLEDsEnabled() && conf->getWebhookRaceStop()) {
            webhooks->triggerRaceStop();
        }
        request->send(200, "application/json", "{\"status\": \"OK\"}");
    });
    server.addHandler(playbackStopHandler);

    server.on("/timer/rssiStart", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi = true;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/timer/rssiStop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        sendRssi = false;
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/config", HTTP_GET, [this](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        conf->toJson(*response);
        request->send(response);
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *configJsonHandler = new AsyncCallbackJsonWebHandler("/config", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
#ifdef DEBUG_OUT
        serializeJsonPretty(jsonObj, DEBUG_OUT);
        DEBUG("\n");
#endif
        conf->fromJson(jsonObj);

        // This endpoint is only hit on an explicit user "Save" action.
        // Persist immediately so /config reflects the new values right away
        // and we don't lose changes if the unit reboots before the periodic
        // EEPROM handler runs.
        conf->write();

        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    // On-demand config dump to serial (called when user opens Configuration tab)
    server.on("/api/debugconfig", HTTP_GET, [this](AsyncWebServerRequest *request) {
        char buf[2048];
        conf->toJsonString(buf);
        Serial.println("[DEBUGCONFIG]");
        Serial.println(buf);
        request->send(200, "text/plain", "printed to serial");
    });

    // Serve audio files from SD card voice directories (sounds_default, sounds_rachel, etc.)
    server.on("^\\/sounds_.+\\/.+\\.mp3$", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String path = request->url();
        
#ifdef ESP32S3
        // Try SD card first if available
        if (storage->isSDAvailable() && SD.exists(path)) {
            DEBUG("Serving audio from SD: %s\n", path.c_str());
            request->send(SD, path, "audio/mpeg");
            return;
        }
#endif
        
        // Fall back to LittleFS
        if (LittleFS.exists(path)) {
            DEBUG("Serving audio from LittleFS: %s\n", path.c_str());
            request->send(LittleFS, path, "audio/mpeg");
            return;
        }
        
        // File not found
        DEBUG("Audio file not found: %s\n", path.c_str());
        request->send(404, "text/plain", "Audio file not found");
    });
    
    // Serve audio files from SD /sounds/ directory (legacy/fallback)
    server.on("^\\/sounds\\/.+\\.mp3$", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String path = request->url();
        
#ifdef ESP32S3
        // Try SD card first if available
        if (storage->isSDAvailable() && SD.exists(path)) {
            DEBUG("Serving audio from SD: %s\n", path.c_str());
            request->send(SD, path, "audio/mpeg");
            return;
        }
#endif
        
        // Fall back to LittleFS
        if (LittleFS.exists(path)) {
            DEBUG("Serving audio from LittleFS: %s\n", path.c_str());
            request->send(LittleFS, path, "audio/mpeg");
            return;
        }
        
        // File not found
        DEBUG("Audio file not found: %s\n", path.c_str());
        request->send(404, "text/plain", "Audio file not found");
    });
    
    // WiFi status endpoint (register before serveStatic to prevent VFS errors)
    server.on("/api/wifi", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        
        // Get WiFi mode
        wifi_mode_t mode = WiFi.getMode();
        if (mode == WIFI_AP) {
            doc["mode"] = "AP";
            doc["ssid"] = WiFi.softAPSSID();
            doc["ip"] = WiFi.softAPIP().toString();
            doc["clients"] = WiFi.softAPgetStationNum();
            doc["rssi"] = 0;
        } else if (mode == WIFI_STA) {
            doc["mode"] = "STA";
            doc["ssid"] = WiFi.SSID();
            doc["ip"] = WiFi.localIP().toString();
            doc["clients"] = 0;
            doc["rssi"] = WiFi.RSSI();
            doc["connected"] = WiFi.status() == WL_CONNECTED;
        } else {
            doc["mode"] = "OFF";
            doc["ssid"] = "";
            doc["ip"] = "";
            doc["clients"] = 0;
            doc["rssi"] = 0;
            doc["connected"] = false;
        }
        
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
        led->on(200);
    });
    
    // index.html: no-cache so OTA updates are reflected immediately.
    // script.js / style.css: 5-minute TTL so the browser reuses them within a session
    // without burning a fresh TCP connection on every SSE reconnect.
    server.serveStatic("/index.html",       LittleFS, "/index.html")      .setCacheControl("no-cache, no-store, must-revalidate");
    server.serveStatic("/script.js",        LittleFS, "/script.js")       .setCacheControl("max-age=300");
    server.serveStatic("/style.css",        LittleFS, "/style.css")       .setCacheControl("max-age=300");
    server.serveStatic("/audio-announcer.js", LittleFS, "/audio-announcer.js").setCacheControl("max-age=300");
    server.serveStatic("/smoothie.js",      LittleFS, "/smoothie.js")     .setCacheControl("max-age=300");
    server.serveStatic("/usb-transport.js", LittleFS, "/usb-transport.js").setCacheControl("max-age=300");
    server.serveStatic("/", LittleFS, "/").setCacheControl("no-cache, no-store, must-revalidate");

    events.onConnect([this](AsyncEventSourceClient *client) {
        if (client->lastId()) {
            DEBUG("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
        }
        client->send("start", NULL, millis(), 1000);
        led->on(200);
    });

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "600");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "POST,GET,OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "*");

    server.onNotFound(handleNotFound);

    server.addHandler(&events);
    server.addHandler(configJsonHandler);

    // Race history endpoints
    server.on("/races", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String json = history->toJsonString();
        request->send(200, "application/json", json);
        led->on(200);
    });

    server.on("/api/races/download", HTTP_GET, [this](AsyncWebServerRequest *request) {

        String json = history->toJsonString();

        AsyncResponseStream *response =
            request->beginResponseStream("application/octet-stream");

        response->addHeader("Content-Disposition",
                            "attachment; filename=\"races.json\"");
        response->addHeader("X-Content-Type-Options", "nosniff");
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");

        response->print(json);
        request->send(response);

        led->on(200);
    });

    AsyncCallbackJsonWebHandler *raceSaveHandler = new AsyncCallbackJsonWebHandler("/races/save", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        RaceSession race;
        race.timestamp = jsonObj["timestamp"];
        race.fastestLap = jsonObj["fastestLap"];
        race.medianLap = jsonObj["medianLap"];
        race.best3LapsTotal = jsonObj["best3LapsTotal"];
        race.pilotName = jsonObj["pilotName"] | "";
        race.frequency = jsonObj["frequency"] | 0;
        race.band = jsonObj["band"] | "";
        race.channel = jsonObj["channel"] | 0;
        
        DEBUG("Parsing race save: trackId=%u\n", (uint32_t)(jsonObj["trackId"] | 0));
        race.trackId = jsonObj["trackId"] | 0;
        race.trackName = jsonObj["trackName"] | "";
        DEBUG("Parsing totalDistance...\n");
        race.totalDistance = jsonObj["totalDistance"] | 0.0;
        DEBUG("Parsed totalDistance=%.2f\n", race.totalDistance);
        
        JsonArray lapsArray = jsonObj["lapTimes"];
        for (uint32_t lap : lapsArray) {
            race.lapTimes.push_back(lap);
        }
        
        bool success = history->saveRace(race);
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *raceUploadHandler = new AsyncCallbackJsonWebHandler("/races/upload", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        String jsonString;
        serializeJson(json, jsonString);
        bool success = history->fromJsonString(jsonString);
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    server.on("/races/delete", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("timestamp", true)) {
            uint32_t timestamp = request->getParam("timestamp", true)->value().toInt();
            bool success = history->deleteRace(timestamp);
            request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing timestamp\"}");
        }
        led->on(200);
    });

    server.on("/races/clear", HTTP_POST, [this](AsyncWebServerRequest *request) {
        bool success = history->clearAll();
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    server.on("/races/update", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("timestamp", true) && 
            request->hasParam("name", true) && 
            request->hasParam("tag", true)) {
            uint32_t timestamp = request->getParam("timestamp", true)->value().toInt();
            String name = request->getParam("name", true)->value();
            String tag = request->getParam("tag", true)->value();
            float totalDistance = -1.0f;
            if (request->hasParam("totalDistance", true)) {
                totalDistance = request->getParam("totalDistance", true)->value().toFloat();
            }
            bool success = history->updateRace(timestamp, name, tag, totalDistance);
            request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing parameters\"}");
        }
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *updateLapsHandler = new AsyncCallbackJsonWebHandler("/races/updateLaps", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        if (!jsonObj.containsKey("timestamp") || !jsonObj.containsKey("lapTimes")) {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing parameters\"}");
            return;
        }
        
        uint32_t timestamp = jsonObj["timestamp"];
        JsonArray lapsArray = jsonObj["lapTimes"];
        
        std::vector<uint32_t> lapTimes;
        for (JsonVariant lap : lapsArray) {
            lapTimes.push_back(lap.as<uint32_t>());
        }
        
        bool success = history->updateLaps(timestamp, lapTimes);
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    server.on("/races/downloadOne", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("timestamp")) {
            uint32_t timestamp = request->getParam("timestamp")->value().toInt();
            
            // Find the race
            const auto& races = history->getRaces();
            for (const auto& race : races) {
                if (race.timestamp == timestamp) {
                    // Create JSON for single race
                    DynamicJsonDocument doc(16384);
                    JsonArray racesArray = doc.createNestedArray("races");
                    JsonObject raceObj = racesArray.createNestedObject();
                    raceObj["timestamp"] = race.timestamp;
                    raceObj["fastestLap"] = race.fastestLap;
                    raceObj["medianLap"] = race.medianLap;
                    raceObj["best3LapsTotal"] = race.best3LapsTotal;
                    raceObj["name"] = race.name;
                    raceObj["tag"] = race.tag;
                    raceObj["pilotName"] = race.pilotName;
                    raceObj["frequency"] = race.frequency;
                    raceObj["band"] = race.band;
                    raceObj["channel"] = race.channel;
                    JsonArray lapsArray = raceObj.createNestedArray("lapTimes");
                    for (uint32_t lap : race.lapTimes) {
                        lapsArray.add(lap);
                    }
                    
                    String json;
                    serializeJson(doc, json);
                    
                    String filename = "race_" + String(timestamp) + ".json";
                    AsyncWebServerResponse *response = request->beginResponse(200, "application/octet-stream", json);
                    response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
                    response->addHeader("Content-Type", "application/json");
                    request->send(response);
                    led->on(200);
                    return;
                }
            }
            request->send(404, "application/json", "{\"status\": \"ERROR\", \"message\": \"Race not found\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing timestamp\"}");
        }
        led->on(200);
    });

    server.addHandler(raceSaveHandler);
    server.addHandler(raceUploadHandler);
    server.addHandler(updateLapsHandler);

    // Track endpoints
    server.on("/tracks", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String json = trackManager->toJsonString();
        request->send(200, "application/json", json);
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *trackCreateHandler = new AsyncCallbackJsonWebHandler("/tracks/create", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        Track track;
        track.trackId = jsonObj["trackId"];
        track.name = jsonObj["name"] | "";
        track.tags = jsonObj["tags"] | "";
        track.distance = jsonObj["distance"] | 0.0f;
        track.notes = jsonObj["notes"] | "";
        track.imagePath = "";
        
        bool success = trackManager->createTrack(track);
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    AsyncCallbackJsonWebHandler *trackUpdateHandler = new AsyncCallbackJsonWebHandler("/tracks/update", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        if (!jsonObj.containsKey("trackId")) {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing trackId\"}");
            return;
        }
        
        uint32_t trackId = jsonObj["trackId"];
        Track updatedTrack;
        updatedTrack.trackId = trackId;
        updatedTrack.name = jsonObj["name"] | "";
        updatedTrack.tags = jsonObj["tags"] | "";
        updatedTrack.distance = jsonObj["distance"] | 0.0f;
        updatedTrack.notes = jsonObj["notes"] | "";
        
        bool success = trackManager->updateTrack(trackId, updatedTrack);
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    server.on("/tracks/delete", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("trackId", true)) {
            uint32_t trackId = request->getParam("trackId", true)->value().toInt();
            bool success = trackManager->deleteTrack(trackId);
            
            // If deleted track was selected, deselect it
            if (success && conf->getSelectedTrackId() == trackId) {
                conf->setSelectedTrackId(0);
                timer->setTrack(nullptr);
            }
            
            request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing trackId\"}");
        }
        led->on(200);
    });

    server.on("/tracks/select", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("trackId", true)) {
            uint32_t trackId = request->getParam("trackId", true)->value().toInt();
            
            if (trackId == 0) {
                // Deselect track
                conf->setSelectedTrackId(0);
                timer->setTrack(nullptr);
                request->send(200, "application/json", "{\"status\": \"OK\"}");
            } else {
                Track* track = trackManager->getTrackById(trackId);
                if (track) {
                    conf->setSelectedTrackId(trackId);
                    timer->setTrack(track);
                    request->send(200, "application/json", "{\"status\": \"OK\"}");
                } else {
                    request->send(404, "application/json", "{\"status\": \"ERROR\", \"message\": \"Track not found\"}");
                }
            }
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing trackId\"}");
        }
        led->on(200);
    });

    server.on("/tracks/clear", HTTP_POST, [this](AsyncWebServerRequest *request) {
        bool success = trackManager->clearAll();
        
        // Clear selected track if any
        if (success && conf->getSelectedTrackId() != 0) {
            conf->setSelectedTrackId(0);
            timer->setTrack(nullptr);
        }
        
        request->send(200, "application/json", success ? "{\"status\": \"OK\"}" : "{\"status\": \"ERROR\"}");
        led->on(200);
    });

    server.on("/timer/distance", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["totalDistance"] = timer->getTotalDistance();
        doc["distanceRemaining"] = timer->getDistanceRemaining();
        
        Track* selectedTrack = timer->getSelectedTrack();
        if (selectedTrack) {
            doc["trackId"] = selectedTrack->trackId;
            doc["trackName"] = selectedTrack->name;
            doc["trackDistance"] = selectedTrack->distance;
        } else {
            doc["trackId"] = 0;
            doc["trackName"] = "";
            doc["trackDistance"] = 0.0f;
        }
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    server.addHandler(trackCreateHandler);
    server.addHandler(trackUpdateHandler);

    /*  // NOTE: /api/selftest is defined later with the full per-module test suite (RX5808, timer, etc).
        // Keeping only one registration prevents route conflicts.
    // Self-test endpoint
    server.on("/api/selftest", HTTP_GET, [this](AsyncWebServerRequest *request) {
        selftest->runAllTests();
        String json = selftest->getResultsJSON();
        request->send(200, "application/json", json);
        led->on(200);
    });
    */
    
    // Debug log endpoint for serial monitor
    server.on("/api/debuglog", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const auto& buffer = DebugLogger::getInstance().getBuffer();
        DynamicJsonDocument doc(8192);
        JsonArray logs = doc.createNestedArray("logs");
        
        for (const auto& entry : buffer) {
            JsonObject log = logs.createNestedObject();
            log["timestamp"] = entry.timestamp;
            log["message"] = entry.message;
        }
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
        led->on(200);
    });
    
    // Reboot endpoint
    server.on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
        request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"Rebooting...\"}");
        led->on(200);
        // Restart immediately without delay to avoid blocking async_tcp task
        ESP.restart();
    });

    // SD card initialization endpoint
    server.on("/storage/initsd", HTTP_POST, [this](AsyncWebServerRequest *request) {
        bool success = storage->initSDDeferred();
        String json = success ? "{\"status\":\"OK\",\"message\":\"SD card initialized\"}" : 
                               "{\"status\":\"ERROR\",\"message\":\"SD card init failed\"}";
        request->send(success ? 200 : 500, "application/json", json);
        led->on(200);
    });
    
    // SD card test endpoint - list files
    server.on("/storage/sdtest", HTTP_GET, [this](AsyncWebServerRequest *request) {
#ifdef ESP32S3
        String response = "SD Card Test:\n\n";
        response += "Available: " + String(storage->isSDAvailable() ? "YES" : "NO") + "\n";
        response += "Storage Type: " + storage->getStorageType() + "\n";
        
        if (storage->isSDAvailable()) {
            response += "\nRoot directories:\n";
            File root = SD.open("/");
            if (root) {
                File entry = root.openNextFile();
                while (entry) {
                    if (entry.isDirectory()) {
                        response += "  [DIR] " + String(entry.name()) + "\n";
                        
                        // List first 5 files in sounds_* directories
                        String entryName = String(entry.name());
                        if (entryName.startsWith("/sounds_")) {
                            File subdir = SD.open(entry.name());
                            if (subdir) {
                                int count = 0;
                                File file = subdir.openNextFile();
                                while (file && count < 5) {
                                    response += "    - " + String(file.name()) + " (" + String(file.size()) + " bytes)\n";
                                    file = subdir.openNextFile();
                                    count++;
                                }
                                if (count == 5) response += "    ... (more files)\n";
                                subdir.close();
                            }
                        }
                    } else {
                        response += "  [FILE] " + String(entry.name()) + " (" + String(entry.size()) + " bytes)\n";
                    }
                    entry = root.openNextFile();
                }
                root.close();
            } else {
                response += "ERROR: Could not open root directory\n";
            }
            
            // Test specific file
            response += "\nTest file access:\n";
            const char* testFile = "/sounds_adam/gate_1.mp3";
            response += "  " + String(testFile) + ": ";
            if (SD.exists(testFile)) {
                File f = SD.open(testFile);
                if (f) {
                    response += "EXISTS, size=" + String(f.size()) + " bytes\n";
                    f.close();
                } else {
                    response += "EXISTS but CANNOT OPEN\n";
                }
            } else {
                response += "NOT FOUND\n";
            }
        } else {
            response += "\nSD card not available!\n";
        }
        
        request->send(200, "text/plain", response);
#else
        request->send(200, "text/plain", "SD card not supported on this platform");
#endif
        led->on(200);
    });

#ifdef ESP32S3
    // LED control endpoints
    server.on("/led/color", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("color", true)) {
            String colorStr = request->getParam("color", true)->value();
            uint32_t color = strtol(colorStr.c_str(), NULL, 16);
            if (g_rgbLed) {
                g_rgbLed->setManualColor(color);
            }
            conf->setLedColor(color);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing color\"}");
        }
        led->on(200);
    });

    server.on("/led/mode", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("mode", true)) {
            uint8_t mode = request->getParam("mode", true)->value().toInt();
            if (g_rgbLed) {
                if (mode == 0) {
                    g_rgbLed->off();
                } else if (mode == 1) {
                    g_rgbLed->setManualMode(RGB_SOLID);
                } else if (mode == 2) {
                    g_rgbLed->setManualMode(RGB_PULSE);
                } else if (mode == 3) {
                    g_rgbLed->setRainbowWave();
                }
            }
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing mode\"}");
        }
        led->on(200);
    });

    server.on("/led/brightness", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("brightness", true)) {
            uint8_t brightness = request->getParam("brightness", true)->value().toInt();
            if (g_rgbLed) {
                g_rgbLed->setBrightness(brightness);
            }
            conf->setLedBrightness(brightness);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing brightness\"}");
        }
        led->on(200);
    });

    server.on("/led/preset", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("preset", true)) {
            uint8_t preset = request->getParam("preset", true)->value().toInt();
            if (g_rgbLed) {
                g_rgbLed->setPreset((led_preset_e)preset);
            }
            conf->setLedPreset(preset);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing preset\"}");
        }
        led->on(200);
    });

    server.on("/led/override", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("enable", true)) {
            bool enable = request->getParam("enable", true)->value() == "1";
            if (g_rgbLed) {
                g_rgbLed->enableManualOverride(enable);
            }
            conf->setLedManualOverride(enable ? 1 : 0);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing enable\"}");
        }
        led->on(200);
    });

    server.on("/led/error", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("code", true)) {
            uint8_t code = request->getParam("code", true)->value().toInt();
            if (g_rgbLed) {
                g_rgbLed->showErrorCode(code);
            }
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing code\"}");
        }
        led->on(200);
    });

    server.on("/led/speed", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("speed", true)) {
            uint8_t speed = request->getParam("speed", true)->value().toInt();
            if (g_rgbLed) {
                g_rgbLed->setEffectSpeed(speed);
            }
            conf->setLedSpeed(speed);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing speed\"}");
        }
        led->on(200);
    });

    server.on("/led/fadecolor", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("color", true)) {
            String colorStr = request->getParam("color", true)->value();
            uint32_t color = strtol(colorStr.c_str(), NULL, 16);
            if (g_rgbLed) {
                g_rgbLed->setFadeColor(color);
            }
            conf->setLedFadeColor(color);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing color\"}");
        }
        led->on(200);
    });

    server.on("/led/strobecolor", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("color", true)) {
            String colorStr = request->getParam("color", true)->value();
            uint32_t color = strtol(colorStr.c_str(), NULL, 16);
            if (g_rgbLed) {
                g_rgbLed->setStrobeColor(color);
            }
            conf->setLedStrobeColor(color);
            request->send(200, "application/json", "{\"status\": \"OK\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing color\"}");
        }
        led->on(200);
    });
#endif

    // Calibration wizard endpoints
    server.on("/calibration/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
        timer->startCalibrationWizard();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    server.on("/calibration/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        timer->stopCalibrationWizard();
        request->send(200, "application/json", "{\"status\": \"OK\"}");
        led->on(200);
    });

    // updated to serve paged calibration data
    server.on("/calibration/data", HTTP_GET, [this](AsyncWebServerRequest *request) {
        const uint16_t total = timer->getCalibrationRssiCount();

        // Optional query params:
        //   offset = starting index (default 0)
        //   limit  = number of samples to return (default 500)
        // Special:
        //   limit=0 returns meta only (just total)
        uint32_t offset = 0;
        uint32_t limit  = 500;

        if (request->hasParam("offset")) {
            offset = request->getParam("offset")->value().toInt();
        }
        if (request->hasParam("limit")) {
            limit = request->getParam("limit")->value().toInt();
        }

        if (offset > total) offset = total;

        // Meta-only mode (fast for wizard sample count polling)
        if (limit == 0) {
            String meta = String("{\"total\":") + total + "}";
            request->send(200, "application/json", meta);
            led->on(50);
            return;
        }

        // Clamp limit to a sane upper bound to avoid huge responses
        if (limit > 1000) limit = 1000;

        const uint32_t end = (offset + limit > total) ? total : (offset + limit);
        const uint32_t count = (end > offset) ? (end - offset) : 0;

        // Build a paged JSON response
        // Shape:
        // {
        //   "total": 5000,
        //   "offset": 0,
        //   "limit": 500,
        //   "count": 500,
        //   "data":[{"rssi":58,"time":123}, ...]
        // }
        String json;
        json.reserve(64 + (count * 28)); // rough reserve to reduce fragmentation

        json += "{\"total\":";
        json += total;
        json += ",\"offset\":";
        json += offset;
        json += ",\"limit\":";
        json += limit;
        json += ",\"count\":";
        json += count;
        json += ",\"data\":[";

        for (uint32_t i = offset; i < end; i++) {
            if (i > offset) json += ",";
            json += "{\"rssi\":";
            json += timer->getCalibrationRssi((uint16_t)i);
            json += ",\"time\":";
            json += timer->getCalibrationTimestamp((uint16_t)i);
            json += "}";
        }

        json += "]}";

        request->send(200, "application/json", json);
        led->on(50);
    });


    // Self-test endpoint
    server.on("/api/selftest", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // Run RX5808 test
        TestResult rxTest = selftest->testRX5808(rx);
        
        // Run Lap Timer test
        TestResult timerTest = selftest->testLapTimer(timer);
        
        // Run Audio test
        TestResult audioTest = selftest->testAudio(buz);
        
        // Run Config test
        TestResult configTest = selftest->testConfig(conf);
        
        #ifdef PIN_SD_CS
            // Run Race History test
            TestResult historyTest = selftest->testRaceHistory(history);
        #endif
        
        // Run Web Server test
        TestResult webTest = selftest->testWebServer();
        
        // Run OTA test
        TestResult otaTest = selftest->testOTA();
        
        #ifdef PIN_SD_CS
            // Run Storage test
            TestResult storageTest = selftest->testStorage();
        #endif
        
        // Run LittleFS test
        TestResult littleFSTest = selftest->testLittleFS();
        
        // Run EEPROM test
        TestResult eepromTest = selftest->testEEPROM();
        
        // Run WiFi test
        TestResult wifiTest = selftest->testWiFi();
        
        #ifdef PIN_VBAT
            // Run Battery test
            TestResult batteryTest = selftest->testBattery();
        #endif
        
        #ifdef PIN_SD_CS
        // Run Track Manager test
        TestResult trackTest = selftest->testTrackManager();
        #endif
        
        // Run Webhooks test
        TestResult webhookTest = selftest->testWebhooks();
        
        // Run Transport test
        TestResult transportTest = selftest->testTransport();
        
#ifdef ESP32S3
        // Run RGB LED test
        TestResult ledTest = selftest->testRGBLED(g_rgbLed);
        
        // Run SD Card test
        TestResult sdTest = selftest->testSDCard();
#endif
        
        // Build JSON response
        String json = "{\"tests\":[";
        
        auto addTest = [&json](const TestResult& test, bool first = false) {
            if (!first) json += ",";
            json += "{\"name\":\"" + test.name + "\",\"passed\":" + String(test.passed ? "true" : "false") + 
                   ",\"details\":\"" + test.details + "\",\"duration\":" + String(test.duration_ms) + "}";
        };
        
        addTest(rxTest, true);
        addTest(timerTest);
        addTest(audioTest);
        addTest(configTest);
        #ifdef PIN_SD_CS
            addTest(historyTest);
        #endif
        addTest(webTest);
        addTest(otaTest);
        #ifdef PIN_SD_CS
            addTest(storageTest);
        #endif
        addTest(littleFSTest);
        addTest(eepromTest);
        addTest(wifiTest);
        #ifdef PIN_VBAT
            addTest(batteryTest);
        #endif
        #ifdef PIN_SD_CS
            addTest(trackTest);
        #endif
        addTest(webhookTest);
        addTest(transportTest);
#ifdef ESP32S3
        addTest(ledTest);
        addTest(sdTest);
#endif
        
        json += "]}";
        
        request->send(200, "application/json", json);
        led->on(200);
    });

    // Webhook management endpoints
    server.on("/webhooks", HTTP_GET, [this](AsyncWebServerRequest *request) {
        String json = "{\"enabled\":" + String(webhooks ? (webhooks->isEnabled() ? "true" : "false") : "false") + ",\"webhooks\":[";
        if (webhooks) {
            uint8_t count = webhooks->getWebhookCount();
            for (uint8_t i = 0; i < count; i++) {
                if (i > 0) json += ",";
                json += "\"" + String(webhooks->getWebhookIP(i)) + "\"";
            }
        }
        json += "]}";
        request->send(200, "application/json", json);
        led->on(200);
    });

    server.on("/webhooks/add", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("ip", true)) {
            String ip = request->getParam("ip", true)->value();
            if (webhooks && webhooks->addWebhook(ip.c_str())) {
                conf->addWebhookIP(ip.c_str());
                request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"Webhook added\"}");
            } else {
                request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Failed to add webhook\"}");
            }
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing ip\"}");
        }
        led->on(200);
    });

    server.on("/webhooks/remove", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("ip", true)) {
            String ip = request->getParam("ip", true)->value();
            if (webhooks && webhooks->removeWebhook(ip.c_str())) {
                conf->removeWebhookIP(ip.c_str());
                request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"Webhook removed\"}");
            } else {
                request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Webhook not found\"}");
            }
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing ip\"}");
        }
        led->on(200);
    });

    server.on("/webhooks/clear", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (webhooks) {
            webhooks->clearWebhooks();
            conf->clearWebhookIPs();
            request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"All webhooks cleared\"}");
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Webhooks not initialized\"}");
        }
        led->on(200);
    });

    server.on("/webhooks/enable", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (request->hasParam("enabled", true)) {
            bool enabled = request->getParam("enabled", true)->value() == "1";
            if (webhooks) {
                webhooks->setEnabled(enabled);
                conf->setWebhooksEnabled(enabled ? 1 : 0);
                request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"Webhooks " + String(enabled ? "enabled" : "disabled") + "\"}");
            } else {
                request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Webhooks not initialized\"}");
            }
        } else {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Missing enabled\"}");
        }
        led->on(200);
    });

    // Manual webhook triggers (for testing)
    server.on("/webhooks/trigger/flash", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!webhooks) {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Webhooks not initialized\"}");
            return;
        }
        
        if (!webhooks->isEnabled()) {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"Webhooks are disabled\"}");
            return;
        }
        
        uint8_t hookCount = webhooks->getWebhookCount();
        if (hookCount == 0) {
            request->send(400, "application/json", "{\"status\": \"ERROR\", \"message\": \"No webhooks configured\"}");
            return;
        }
        
        DEBUG("Triggering flash webhook to %d endpoints\n", hookCount);
        
        // Send response before triggering webhooks to avoid blocking
        request->send(200, "application/json", "{\"status\": \"OK\", \"message\": \"Flash triggered\"}");
        led->on(200);
        
        // Trigger webhooks after response sent
        webhooks->triggerFlash();
    });

    // ── Multi-Node API ──────────────────────────────────────────────────
    // /api/mode — returns current node mode and status
    server.on("/api/mode", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(256);
        uint8_t nodeMode = conf->getNodeMode();
        doc["nodeMode"] = nodeMode;
        doc["modeName"] = (nodeMode == 1) ? "master" : (nodeMode == 2) ? "client" : "single";
        doc["ssid"] = wifi_ap_ssid;
        doc["sdAvailable"] = storage ? storage->isSDAvailable() : false;
        doc["devMode"] = conf->getDevMode();
        doc["timerRunning"]  = timer ? timer->isRunning() : false;
        doc["raceElapsedMs"] = timer ? (uint32_t)timer->getElapsedMs() : 0;
        if (multiNode) {
            doc["masterConnected"]  = multiNode->isMasterConnected();
            doc["myNodeId"]         = multiNode->getMyNodeId();
            doc["masterRaceActive"] = multiNode->isMasterRaceActive();
        }
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // /api/laps/current — returns in-progress lap data for page reload restore
    server.on("/api/laps/current", HTTP_GET, [this](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        bool running   = timer ? timer->isRunning() : false;
        uint8_t count  = (running && timer) ? timer->getLapCount() : 0;
        doc["running"]  = running;
        doc["lapCount"] = count;
        JsonArray arr = doc.createNestedArray("laps");
        for (uint8_t i = 0; i < count; i++) {
            JsonObject lap = arr.createNestedObject();
            lap["lapNumber"] = i + 1;
            lap["lapTimeMs"] = timer->getLapTimeAt(i);
        }
        String out; serializeJson(doc, out);
        request->send(200, "application/json", out);
    });

    // /api/multinode/nodes — master returns all registered client nodes
    server.on("/api/multinode/nodes", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!multiNode) {
            request->send(200, "application/json", "{\"nodes\":[]}");
            return;
        }
        request->send(200, "application/json", multiNode->getNodesToJson());
    });

    // /api/multinode/clearLaps — master clears all stored laps for all nodes
    server.on("/api/multinode/clearLaps", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!multiNode || !multiNode->isMasterMode()) {
            request->send(403, "application/json", "{\"error\":\"not master\"}");
            return;
        }
        multiNode->clearAllLaps();
        // Push updated (empty) node state to all SSE clients
        String nodesJson = multiNode->getNodesToJson();
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", nodesJson.c_str());
        events.send(buf, "multiNodeState", millis());
        request->send(200, "application/json", "{\"ok\":true}");
    });

    // /api/multinode/scan — scan for nearby FPVRaceOne_ SSIDs
    server.on("/api/multinode/scan", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!multiNode) {
            request->send(200, "application/json", "{\"networks\":[]}");
            return;
        }
        String json = multiNode->scanForNodesJson();
        request->send(200, "application/json", json);
    });

    // /api/multinode/register — client registers with master (POST JSON)
    AsyncCallbackJsonWebHandler *mnRegisterHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/register",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj          = json.as<JsonObject>();
            String pilotName        = obj["pilotName"]     | "";
            uint32_t pilotColor     = obj["pilotColor"]    | 0x0080FFu;
            uint8_t  bandIndex      = obj["band"]          | 0;
            uint8_t  channelIndex   = obj["chan"]          | 0;
            uint16_t freq           = obj["freq"]          | 0;
            String clientIP         = obj["clientIP"]      | "";
            String staIP            = request->client()->remoteIP().toString();

            String macAddress   = obj["mac"]      | "";
            uint8_t assignedId  = obj["nodeId"]   | 0;  // client's self-reported nodeId (0 on first registration)
            bool ok = multiNode->handleRegister(pilotName,
                                                 pilotColor, bandIndex, channelIndex, freq,
                                                 staIP, clientIP,
                                                 macAddress, assignedId);

            DynamicJsonDocument resp(64);
            if (ok) {
                resp["status"] = "OK";
                resp["nodeId"] = assignedId;
            } else {
                resp["status"] = "ERROR";
                resp["nodeId"] = 0;
            }
            String out;
            serializeJson(resp, out);
            request->send(ok ? 200 : 409, "application/json", out);
        });
    server.addHandler(mnRegisterHandler);

    // /api/multinode/lap — client reports a lap to master
    AsyncCallbackJsonWebHandler *mnLapHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/lap",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj      = json.as<JsonObject>();
            uint8_t  nodeId     = obj["nodeId"]    | 0;
            uint32_t lapTimeMs  = obj["lapTimeMs"] | 0u;
            uint8_t  lapNumber  = obj["lapNumber"] | 0;

            bool ok = multiNode->handleLap(nodeId, lapTimeMs, lapNumber);

            // Also push to SSE so master's browser updates in real time
            if (ok && servicesStarted) {
                char buf[64];
                snprintf(buf, sizeof(buf), "{\"node\":%u,\"lap\":%u,\"ms\":%u}",
                         nodeId, lapNumber, lapTimeMs);
                events.send(buf, "multiNodeLap");
            }
            request->send(ok ? 200 : 404, "application/json",
                          ok ? "{\"status\":\"OK\"}" : "{\"status\":\"ERROR\"}");
        });
    server.addHandler(mnLapHandler);

    // /api/multinode/heartbeat — client sends periodic heartbeat to master
    AsyncCallbackJsonWebHandler *mnHeartbeatHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/heartbeat",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj    = json.as<JsonObject>();
            uint8_t nodeId    = obj["nodeId"] | 0;
            bool running      = obj["running"] | false;
            bool stateChanged = false;
            bool ok = multiNode->handleHeartbeat(nodeId, running, stateChanged);
            if (ok && stateChanged) {
                // Push updated node list to master browser
                String nodesJson = multiNode->getNodesToJson();
                events.send(nodesJson.c_str(), "multiNodeState");
            }
            request->send(ok ? 200 : 404, "application/json",
                          ok ? "{\"status\":\"OK\"}" : "{\"status\":\"NOT_FOUND\"}");
        });
    server.addHandler(mnHeartbeatHandler);

    // /api/multinode/quit — client notifies master that pilot quit the race early
    AsyncCallbackJsonWebHandler *mnQuitHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/quit",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj = json.as<JsonObject>();
            uint8_t nodeId = obj["nodeId"] | 0;
            bool ok = multiNode->handleQuit(nodeId);
            if (ok) {
                String nodesJson = multiNode->getNodesToJson();
                events.send(nodesJson.c_str(), "multiNodeState");
            }
            request->send(ok ? 200 : 404, "application/json",
                          ok ? "{\"status\":\"OK\"}" : "{\"status\":\"NOT_FOUND\"}");
        });
    server.addHandler(mnQuitHandler);

    // /api/multinode/removeNode — master manually removes a node slot
    AsyncCallbackJsonWebHandler *mnRemoveHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/removeNode",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj = json.as<JsonObject>();
            uint8_t nodeId = obj["nodeId"] | 0;
            bool ok = multiNode->removeNode(nodeId);
            if (ok) {
                String nodesJson = multiNode->getNodesToJson();
                events.send(nodesJson.c_str(), "multiNodeState");
            }
            request->send(ok ? 200 : 404, "application/json",
                          ok ? "{\"status\":\"OK\"}" : "{\"status\":\"NOT_FOUND\"}");
        });
    server.addHandler(mnRemoveHandler);

    // /api/multinode/editName — master pushes a new pilot name to a client node
    AsyncCallbackJsonWebHandler *mnEditNameHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/editName",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj       = json.as<JsonObject>();
            uint8_t    nodeId    = obj["nodeId"]    | 0;
            String     pilotName = obj["pilotName"] | "";

            // Find node's STA IP to proxy the request (staIP = client's DHCP IP on master's network)
            String staIP;
            const auto& nodes = multiNode->getNodes();
            for (const auto& n : nodes) {
                if (n.nodeId == nodeId) { staIP = n.staIP; break; }
            }
            if (staIP.isEmpty()) {
                request->send(404, "application/json", "{\"status\":\"NOT_FOUND\"}");
                return;
            }

            // Proxy the name change to the client's /api/pilotName endpoint
            HTTPClient http;
            String url = "http://" + staIP + "/api/pilotName";
            bool sent = false;
            if (http.begin(url)) {
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(2000);
                DynamicJsonDocument body(128);
                body["pilotName"] = pilotName;
                String bodyStr;
                serializeJson(body, bodyStr);
                sent = (http.POST(bodyStr) == 200);
                http.end();
            }
            request->send(sent ? 200 : 502, "application/json",
                          sent ? "{\"status\":\"OK\"}" : "{\"status\":\"PROXY_FAILED\"}");
        });
    server.addHandler(mnEditNameHandler);

    // /api/multinode/editPilot — master pushes pilot name + color to a client node
    AsyncCallbackJsonWebHandler *mnEditPilotHandler = new AsyncCallbackJsonWebHandler(
        "/api/multinode/editPilot",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!multiNode) {
                request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
                return;
            }
            JsonObject obj        = json.as<JsonObject>();
            uint8_t    nodeId     = obj["nodeId"]     | 0;
            String     pilotName  = obj["pilotName"]  | "";
            uint32_t   pilotColor = obj["pilotColor"] | 0x0080FFu;
            bool       hasBand    = obj.containsKey("band");
            uint8_t    bandIndex  = obj["band"]        | 0;
            uint8_t    chanIndex  = obj["chan"]         | 0;
            uint16_t   freq       = obj["freq"]        | 0;

            String staIP;
            const auto& nodes = multiNode->getNodes();
            for (const auto& n : nodes) {
                if (n.nodeId == nodeId) { staIP = n.staIP; break; }
            }
            if (staIP.isEmpty()) {
                request->send(404, "application/json", "{\"status\":\"NOT_FOUND\"}");
                return;
            }

            HTTPClient http;
            String url = "http://" + staIP + "/api/pilotInfo";
            bool sent = false;
            if (http.begin(url)) {
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(2000);
                DynamicJsonDocument body(256);
                body["pilotName"]  = pilotName;
                body["pilotColor"] = pilotColor;
                if (hasBand) {
                    body["band"] = bandIndex;
                    body["chan"] = chanIndex;
                    body["freq"] = freq;
                }
                String bodyStr;
                serializeJson(body, bodyStr);
                sent = (http.POST(bodyStr) == 200);
                http.end();
            }
            if (sent) {
                multiNode->updateNodePilot(nodeId, pilotName, pilotColor);
                if (hasBand) multiNode->updateNodeChannel(nodeId, bandIndex, chanIndex, freq);
            }
            request->send(sent ? 200 : 502, "application/json",
                          sent ? "{\"status\":\"OK\"}" : "{\"status\":\"PROXY_FAILED\"}");
        });
    server.addHandler(mnEditPilotHandler);

    // /api/pilotName — client receives a name update from master
    AsyncCallbackJsonWebHandler *pilotNameHandler = new AsyncCallbackJsonWebHandler(
        "/api/pilotName",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            JsonObject obj       = json.as<JsonObject>();
            String     pilotName = obj["pilotName"] | "";
            if (pilotName.isEmpty() || !conf) {
                request->send(400, "application/json", "{\"status\":\"INVALID\"}");
                return;
            }
            DynamicJsonDocument patch(64);
            patch["name"] = pilotName;
            conf->fromJson(patch.as<JsonObject>());
            conf->write();
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        });
    server.addHandler(pilotNameHandler);

    // /api/pilotInfo — client receives name + color update from master
    AsyncCallbackJsonWebHandler *pilotInfoHandler = new AsyncCallbackJsonWebHandler(
        "/api/pilotInfo",
        [this](AsyncWebServerRequest *request, JsonVariant &json) {
            if (!conf) {
                request->send(503, "application/json", "{\"error\":\"no config\"}");
                return;
            }
            JsonObject obj = json.as<JsonObject>();
            DynamicJsonDocument patch(256);
            if (obj.containsKey("pilotName"))  patch["name"]       = obj["pilotName"].as<String>();
            if (obj.containsKey("pilotColor")) patch["pilotColor"] = obj["pilotColor"].as<uint32_t>();
            if (obj.containsKey("band"))       patch["band"]       = obj["band"].as<int>();
            if (obj.containsKey("chan"))       patch["chan"]       = obj["chan"].as<int>();
            if (obj.containsKey("freq"))       patch["freq"]       = obj["freq"].as<int>();
            conf->fromJson(patch.as<JsonObject>());
            conf->write();
            // Push update to this client's browser so name/color reflect immediately
            DynamicJsonDocument notify(128);
            notify["name"]       = conf->getPilotName();
            notify["pilotColor"] = conf->getPilotColor();
            String notifyStr;
            serializeJson(notify, notifyStr);
            events.send(notifyStr.c_str(), "pilotInfoChanged");
            request->send(200, "application/json", "{\"status\":\"OK\"}");
        });
    server.addHandler(pilotInfoHandler);

    // /api/multinode/race/prearm — broadcast pre-arm signal to all clients (flashes their Start button)
    server.on("/api/multinode/race/prearm", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!multiNode) {
            request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
            return;
        }
        multiNode->queueRacePreArm();
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });

    // /api/multinode/race/start — start master timer immediately, queue client POSTs
    // to parallelTask so async_tcp is never blocked by outbound HTTP calls
    server.on("/api/multinode/race/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!multiNode) {
            request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
            return;
        }
        if (timer) timer->start();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("started");
        multiNode->queueRaceStart();
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });

    // /api/multinode/race/stop
    server.on("/api/multinode/race/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!multiNode) {
            request->send(503, "application/json", "{\"error\":\"multinode disabled\"}");
            return;
        }
        if (timer) timer->stop();
        if (transportMgr) transportMgr->broadcastRaceStateEvent("stopped");
        multiNode->queueRaceStop();
        request->send(200, "application/json", "{\"status\":\"OK\"}");
    });
    // ────────────────────────────────────────────────────────────────────

    ElegantOTA.setAutoReboot(true);
    ElegantOTA.begin(&server);

    server.begin();

    if (captiveDnsEnabled) {
        // Full captive-portal mode: wildcard redirect to our UI.
        // Not used by default — triggers Android "Sign in to network" prompt.
        dnsServer.start(DNS_PORT, "*", ipAddress);
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        startMDNS();
    } else if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
        // Always run DNS in AP or AP+STA mode, but only answer for our own hostname.
        // Everything else (e.g. connectivitycheck.gstatic.com) gets NXDOMAIN
        // immediately, so Android detects "no internet" in milliseconds and
        // falls back to cellular — rather than waiting 5-10 s for DNS timeouts.
        dnsServer.setErrorReplyCode(DNSReplyCode::NonExistentDomain);
        dnsServer.start(DNS_PORT, wifi_hostname, ipAddress);
        DEBUG("[DNS] Selective DNS started: '%s' -> %s, all others -> NXDOMAIN\n",
              wifi_hostname, ipAddress.toString().c_str());
    }

    

    servicesStarted = true;
}
