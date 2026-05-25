#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "battery.h"
#include "laptimer.h"
#include "multinode.h"
#include "racehistory.h"
#include "storage.h"
#include "selftest.h"
#include "transport.h"
#include "webhook.h"

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
};
