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
    // ── Director-state build coalescing ──────────────────────────────────
    // pushMultiNodeState() used to BUILD the full payload inline, on whichever
    // AsyncWebServer request thread called it.  During a realistic pack
    // crossing seven lap POSTs arrive within ~10 ms, so seven request threads
    // each built a payload concurrently.
    //
    // That payload is pre-sized to 400 + clients*400 + laps*40 bytes — about
    // 7.7 KB with 7 clients and ~110 laps, and it GROWS as a race progresses.
    // Each request then needed that block for the build, another for the SSE
    // send, and another for the queueDirectorStateBroadcast copy: roughly
    // 23 KB per request, ~160 KB across seven, against a largest-free-block
    // that was measured at 16 KB.
    //
    // Measured consequence (2026-08-08 rig, 7 clients, burst load): free heap
    // reached a minimum of 304 bytes, AsyncTCP stopped accepting connections
    // (browser showed "disconnected" with WiFi up, 70-91% of injected lap
    // POSTs timed out, clients dropped en masse), and when a dip lasted past
    // HEAP_REBOOT_AFTER the heap watchdog restarted the device.
    //
    // Now callers only set a dirty flag; the payload is built ONCE per
    // interval from handleWebUpdate(), i.e. on parallelTask.  Seven concurrent
    // builds become one sequential build.  It also removes a cross-task race
    // for free: _directorStatePayload is now written and read by the same
    // task, where before it was written from request threads and read by
    // parallelTask with no lock (String assignment does malloc/free).
    volatile bool _mnStateDirty       = false;
    uint32_t      _mnStateLastBuildMs = 0;
    // 250 ms is imperceptible for a browser state view and collapses an entire
    // pack crossing into a single build.
    static constexpr uint32_t MN_STATE_BUILD_INTERVAL_MS = 250;
    void _flushMultiNodeState(uint32_t currentTimeMs);

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

    // Multi-tab prearm heartbeat.  When /timer/prearm is POSTed we set an
    // absolute deadline (millis()+~6000).  process() re-broadcasts
    // raceState=prearming at 500 ms intervals while millis() < deadline so
    // any browser that dropped the first SSE frame (Safari and some Chrome
    // builds silently miss a lone rapid event on AsyncEventSource) catches
    // the next one during the 5-second countdown window.
    uint32_t prearmHeartbeatUntilMs = 0;
    uint32_t prearmHeartbeatNextMs  = 0;
};
