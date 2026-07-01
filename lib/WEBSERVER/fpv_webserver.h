#include <ESPAsyncWebServer.h>
#include <WiFi.h>

// transport.h provides TransportInterface — the Webserver class's base class,
// which requires the full definition.  Every other library type used in this
// header is a pointer-only reference, so forward-declared below to keep
// transitively-including translation units (e.g. OtaManager) from needing
// every webserver dependency on their own library search path.
#include "transport.h"

class Config;
class LapTimer;
class BatteryMonitor;
class Buzzer;
class Led;
class RaceHistory;
class Storage;
class SelfTest;
class RX5808;
class WebhookManager;
class MultiNodeManager;
class TransportManager;

#define WIFI_CONNECTION_TIMEOUT_MS 30000
#define WIFI_RECONNECT_TIMEOUT_MS 500
#define WEB_RSSI_SEND_TIMEOUT_MS 100
#define WEB_SSE_KEEPALIVE_MS 5000

class Webserver : public TransportInterface {
   public:
    void init(Config *config, LapTimer *lapTimer, BatteryMonitor *batMonitor, Buzzer *buzzer, Led *l, RaceHistory *raceHist, Storage *stor, SelfTest *test, RX5808 *rx5808, WebhookManager *webhookMgr, MultiNodeManager *multiNodeMgr = nullptr);
    void setTransportManager(TransportManager *tm);
    void handleWebUpdate(uint32_t currentTimeMs);

    // Exposes the SSE channel so other modules (e.g. OtaManager) can publish
    // progress events to connected browsers.  Returns nullptr until init().
    AsyncEventSource* getEvents();

    // Build the multi-node "director state" payload (nodes + race state), push
    // it via local SSE as "multiNodeState" (master's own browser) AND queue it
    // for HTTP broadcast to all online clients so their read-only Race View tab
    // can mirror the master's view. Master-mode only; no-op otherwise.
    void pushMultiNodeState();

    // (Re-)start the device's own AP using the cached / scanned channel.
    // Called from handleWebUpdate's WIFI_AP transition AND from the recruit job
    // (in MultiNodeManager) after it returns from STA-only mode.  Safe to call
    // multiple times — startServices() is idempotent.
    void startAP();

    // TransportInterface implementation
    void sendLapEvent(uint32_t lapTimeMs, uint8_t peakRssi = 0) override;
    void sendRssiEvent(uint8_t rssi) override;
    void sendRaceStateEvent(const char* state) override;
    bool isConnected() override;
    void update(uint32_t currentTimeMs) override;
    bool servicesStarted = false;
    bool wifiConnected = false;

   private:
    void startServices();

    Config *conf;
    LapTimer *timer;
    BatteryMonitor *monitor;
    Buzzer *buz;
    Led *led;
    RaceHistory *history;
    Storage *storage;
    SelfTest *selftest;
    RX5808 *rx;
    WebhookManager *webhooks;
    MultiNodeManager *multiNode;
    TransportManager *transportMgr;

    wifi_mode_t wifiMode = WIFI_OFF;
    wl_status_t lastStatus = WL_IDLE_STATUS;
    volatile wifi_mode_t changeMode = WIFI_OFF;
    volatile uint32_t changeTimeMs = 0;
    bool sendRssi = false;
    uint32_t rssiSentMs = 0;
    uint32_t sseKeepaliveMs = 0;
    uint8_t apChannel = 0;  // 0 = not yet scanned; first AP start picks the least-congested 1/6/11

    // Client-mode STA reconnect backoff.  When the master can't be reached
    // we stretch the reconnect interval after consecutive failures so a
    // fleet of stale ex-clients doesn't pound an unreachable AP at 5 s
    // intervals forever.  Resets the instant the STA reassociates.
    uint32_t staFailedReconnects = 0;
    uint32_t staBackoffMs        = 5000;

    // Last director-state payload the client received from master via the
    // /api/multinode/directorState POST endpoint.  Replayed to any newly-
    // connected browser SSE client so a late-loading Multi Race tab sees
    // current mesh state instead of the "Race Director" fallback.  Empty
    // string means no push has been received yet on this boot.
    String cachedDirectorState;
};
