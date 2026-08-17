#include "debug.h"
#include "led.h"
#if RSSI_LOGGING_ENABLED
#include "rssilog.h"
#endif
#include "buzzer.h"
#include "config.h"
#include "cpumon.h"
#include "laptimer.h"
#include "multinode.h"
#include "racehistory.h"
#include "RX5808.h"
#include "selftest.h"
#include "storage.h"
#include "transport.h"
#include "usb.h"
#include "webhook.h"
#include "fpv_webserver.h"
// DISABLED FOR NOW: #include "nodemode.h"  // Uncomment to re-enable RotorHazard support
#include "ota.h"
#ifdef ESP32S3
#include "rgbled.h"
#endif

// ====================================================================
// ROTORHAZARD MODE - CURRENTLY DISABLED
// To re-enable RotorHazard support in the future:
// 1. Uncomment the OperationMode enum below
// 2. Uncomment nodeMode initialization and usage throughout this file
// 3. Uncomment the mode detection logic in setup()
// 4. Uncomment the mode-specific code in loop()
// ====================================================================

// DISABLED: Operation mode enumeration
// enum OperationMode {
//     MODE_WIFI,
//     MODE_ROTORHAZARD
// };
// 
// OperationMode currentMode = MODE_WIFI;
// static NodeMode nodeMode;  // RotorHazard node mode controller

// Mode Switching Information:
// =========================
// SOFTWARE MODE SWITCH:
// - Change "opMode" in config via web interface (0=WiFi, 1=RotorHazard)
// - Requires REBOOT to take effect
// - Setting is stored in EEPROM and persists across reboots
//
// PHYSICAL MODE SWITCH (when installed):
// - GPIO9 to GND = Force WiFi mode (overrides software setting)
// - GPIO9 floating = Use software config setting
// - Hardware switch always takes priority over software setting

static RX5808 rx(PIN_RX5808_RSSI, PIN_RX5808_DATA, PIN_RX5808_SELECT, PIN_RX5808_CLOCK);
static Config config;
static Storage storage;
static SelfTest selfTest;
static Webserver ws;
static USBTransport usbTransport;
static TransportManager transportManager;
static Buzzer buzzer;
static Led led;
static RaceHistory raceHistory;
static WebhookManager webhookManager;
static MultiNodeManager multiNodeManager;
#ifdef ESP32S3
static RgbLed rgbLed;
RgbLed* g_rgbLed = &rgbLed;
#else
void* g_rgbLed = nullptr;
#endif
static LapTimer timer;
#if RSSI_LOGGING_ENABLED
static RssiLogger rssiLogger;
#endif
// Battery monitoring removed - legacy feature no longer used
// static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;
static bool sdInitAttempted = false;

// Core-0 stall instrumentation.  Tracks the worst single sub-call duration
// across each 10 s window plus the worst gap between consecutive ticks (the
// latter catches FreeRTOS preemption by higher-priority tasks like AsyncTCP).
// If a sub-call takes ≥100 ms it's logged immediately with its name; the 10 s
// summary line then shows the worst seen across the window even if it was
// under that loud-log threshold.
//
// Why this matters: the multi-node timeout symptom is "4 clients flagged
// offline in the same millisecond, all recovering 2 s later" — only possible
// if Core 0 didn't run multiNodeManager.process() for 3+ s.  Finding the
// offender call is the prerequisite for fixing it.
struct Core0CallSample { const char* name = nullptr; uint32_t ms = 0; };
static Core0CallSample worstCallThisWindow;
static uint32_t        worstTickGapThisWindow = 0;
static uint32_t        lastTickEndMs          = 0;
static uint32_t        coreReportLastMs       = 0;

static inline void timeCall(const char* name, void (*invoke)()) {
    // Helper unused — we expand the timing inline below to avoid forcing
    // every sub-call through an indirection.  Kept as documentation.
    (void)name; (void)invoke;
}

// Bring-up window.  Boot legitimately contains multi-second blocking work that
// can never happen again: startAP() runs a full WiFi channel scan (~2.5 s,
// 120 ms x 13 channels), and that scan is load-bearing — clients carry a whole
// tier-3 reconnect path precisely because the master re-picks its channel every
// boot.  Reported identically to a mid-race stall, it teaches you to skim past
// [CORE0] lines, and a 2.5 s stall during a race is a serious event that must
// not look like routine boot noise.  So it is still printed in full, just
// labelled for what it is.
#define CORE0_BOOT_SETTLE_MS 15000

#define CORE0_TIME(NAME, EXPR) do {                                         \
    uint32_t _t0 = millis();                                                \
    EXPR;                                                                   \
    uint32_t _dt = millis() - _t0;                                          \
    if (_dt > worstCallThisWindow.ms) {                                     \
        worstCallThisWindow.ms   = _dt;                                     \
        worstCallThisWindow.name = NAME;                                    \
    }                                                                       \
    if (_dt >= 100) {                                                       \
        DEBUG("[CORE0] %s blocked for %u ms at t=%us%s\n", NAME,            \
              (unsigned)_dt, (unsigned)(_t0 / 1000),                        \
              (_t0 < CORE0_BOOT_SETTLE_MS) ? "  (boot bring-up)" : "");     \
    }                                                                       \
} while (0)

static void parallelTask(void *pvArgs) {
    for (;;) {
        uint32_t tickStart = millis();
        if (lastTickEndMs != 0) {
            uint32_t gap = tickStart - lastTickEndMs;
            if (gap > worstTickGapThisWindow) worstTickGapThisWindow = gap;
        }

        uint32_t currentTimeMs = tickStart;
        CORE0_TIME("buzzer",     buzzer.handleBuzzer(currentTimeMs));
        CORE0_TIME("led",        led.handleLed(currentTimeMs));
#ifdef ESP32S3
        CORE0_TIME("rgbLed",     rgbLed.handleRgbLed(currentTimeMs));
#endif
        // OTA update work runs here (blocking) when an apply is pending.
        //
        // NOTE: the ESP32-C6 is SINGLE-core.  An earlier comment here claimed
        // "the RSSI loop on Core 1 is unaffected" — that was never true on
        // this chip.  This task and the Arduino loop task (which samples RSSI)
        // share the one HP core; this task runs at priority 2, loop() at 1.
        // A long blocking call here therefore CAN delay sampling.  In practice
        // most of the blocking is socket I/O, during which the task yields and
        // loop() runs — but that is a property of what we block on, not of
        // core isolation.  Measure it, don't assume it: TIMING_STATS_ENABLED
        // reports the worst sample gap, and the [CORE0] lines below report the
        // worst sub-call duration.
        CORE0_TIME("ota",        otaManager.loop());
        CORE0_TIME("webUpdate",  ws.handleWebUpdate(currentTimeMs));
        CORE0_TIME("usb",        usbTransport.update(currentTimeMs));
        CORE0_TIME("eeprom",     config.handleEeprom(currentTimeMs));
        CORE0_TIME("rxFreq",     rx.handleFrequencyChange(currentTimeMs, config.getFrequency()));
        CORE0_TIME("webhooks",   webhookManager.process());
        CORE0_TIME("multinode",  multiNodeManager.process(currentTimeMs));
#if CPU_MONITOR_ENABLED
        // Closes a load window every CPU_MONITOR_WINDOW_MS and publishes it.
        // Cheap: two uxTaskGetSystemState() calls per window, not per tick.
        CORE0_TIME("cpumon",     CpuMonitor::getInstance().tick(currentTimeMs));
#endif

        uint32_t tickEnd = millis();
        lastTickEndMs = tickEnd;

        // Roll up the window every 10 s.  Even if no individual sub-call
        // crossed the 100 ms loud threshold, this line surfaces the worst
        // measured tick + the worst preemption gap so you can see whether
        // Core 0 is healthy across the whole period.
        if (tickEnd - coreReportLastMs >= 10000) {
            coreReportLastMs = tickEnd;
            DEBUG("[CORE0] window 10s: longest sub-call %s=%u ms, longest tick gap=%u ms\n",
                  worstCallThisWindow.name ? worstCallThisWindow.name : "(idle)",
                  (unsigned)worstCallThisWindow.ms,
                  (unsigned)worstTickGapThisWindow);
            worstCallThisWindow.ms   = 0;
            worstCallThisWindow.name = nullptr;
            worstTickGapThisWindow   = 0;

#if CPU_MONITOR_ENABLED
            // Stack headroom, on the same 10 s cadence.  This costs nothing to
            // collect: uxTaskGetSystemState() already fills usStackHighWaterMark
            // for every task and cpumon was discarding it.
            //
            // Read it as "worst free bytes, ever, on the tightest task".  Unlike
            // heap figures it never recovers — it is a high-water mark — so a
            // number that stops falling is a task that has found its true depth.
            // Under ~2000 bytes on any task, stop shrinking stacks: an overflow
            // here is an immediate crash with no warning and no diagnostic.
            const CpuMonitor::Snapshot& cs = CpuMonitor::getInstance().getLast();
            if (cs.valid && cs.minStackFreeBytes != 0xFFFF) {
                DEBUG("[STACK] tightest: %s=%u B free | loop=%u parallel=%u asyncTcp=%u\n",
                      cs.minStackTask, (unsigned)cs.minStackFreeBytes,
                      (unsigned)cs.stackLoopTask,
                      (unsigned)cs.stackParallelTask,
                      (unsigned)cs.stackAsyncTcp);
            }
#endif
        }

        // Let other tasks run (WiFi/AsyncWebServer/etc.)
        vTaskDelay(1);
    }
}

static void initParallelTask() {
    disableCore0WDT();

    // Priority 2 so it reliably runs even when loop() is busy.
    // (Arduino loop task is typically priority 1.)
    xTaskCreatePinnedToCore(
        parallelTask,
        "parallelTask",
        8192,
        NULL,
        2,
        &xTimerTask,
        0
    );
}


void setup() {

    // Serial must be first so all subsequent DEBUG() calls are visible
    Serial.begin(115200);
    delay(100);
    while (Serial.available()) { Serial.read(); }
    DEBUG_INIT;

    // Initialize storage first (LittleFS only at boot)
    storage.init();

    // Initialize config and connect to storage for SD backup/restore
    config.setStorage(&storage);
    config.init();

    // Set antenna option from persisted config (must run before WiFi starts)
    if (config.getWifiExtAntenna()) {
        pinMode(WIFI_ENABLE, OUTPUT);
        digitalWrite(WIFI_ENABLE, LOW);   // Activate RF switch control
        delay(200);
        pinMode(WIFI_ANT_CONFIG, OUTPUT);
        digitalWrite(WIFI_ANT_CONFIG, HIGH); // Use external antenna
    }
    
    /* DISABLED: RotorHazard mode detection
    // Check physical mode switch
    pinMode(PIN_MODE_SWITCH, INPUT_PULLUP);
    delay(10);  // Allow pin to settle
    
    int modePin = digitalRead(PIN_MODE_SWITCH);
    
    // Physical switch overrides software setting
    // If pin is explicitly pulled LOW (to GND), force WiFi mode
    // If pin reads HIGH (floating with pullup), use software config
    if (modePin == WIFI_MODE) {
        // Physical switch connected to GND = force WiFi mode
        currentMode = MODE_WIFI;
    } else {
        // Pin is HIGH (floating) = use software config
        uint8_t configMode = config.getOperationMode();
        if (configMode == 0) {
            currentMode = MODE_WIFI;
        } else {
            currentMode = MODE_ROTORHAZARD;
        }
    }
    */

    // set LED pin
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    // Suppress VFS file-not-found errors (reduces spam from API endpoint checks)
    esp_log_level_set("vfs_api", ESP_LOG_NONE);
    
#ifdef ESP32S3
        DEBUG("ESP32S3 build detected - WiFi Mode\n");
#else
        DEBUG("Generic ESP32 build - WiFi Mode\n");
#endif
    
    // Note: config.init() already called above.
    // adcMode is latched here for the run — 0 = polled analogRead,
    // 1 = DMA continuous with peak-hold.  Changing it requires a reboot,
    // which is why it is read once at init rather than polled per sample.
    rx.init(config.getAdcMode());
#ifdef PIN_BUZZER
    buzzer.init(PIN_BUZZER, BUZZER_INVERTED);
#endif
#ifdef PIN_LED
    led.init(PIN_LED, false);
#endif
#ifdef ESP32S3
    rgbLed.init();
    // Apply saved LED configuration from config
    rgbLed.setBrightness(config.getLedBrightness());
    rgbLed.setEffectSpeed(config.getLedSpeed());
    rgbLed.setManualColor(config.getLedColor());
    rgbLed.setFadeColor(config.getLedFadeColor());
    rgbLed.setStrobeColor(config.getLedStrobeColor());
    rgbLed.enableManualOverride(config.getLedManualOverride());
    // Apply preset last so all colors are set
    rgbLed.setPreset((led_preset_e)config.getLedPreset());
#endif
    timer.init(&config, &rx, &buzzer, &led, &webhookManager);
#if RSSI_LOGGING_ENABLED
    rssiLogger.init();
#endif
    // Battery monitoring removed
    // monitor.init(PIN_VBAT, VBAT_SCALE, VBAT_ADD, &buzzer, &led);
    
    // WiFi mode initialization (RotorHazard mode disabled)
    selfTest.init(&storage);
    
    // Initialize race history with storage backend
    // Note: This uses LittleFS initially; SD card will be mounted later in loop()
    if (raceHistory.init(&storage)) {
        DEBUG("Race history initialized, %d races loaded\n", raceHistory.getRaceCount());
    } else {
        DEBUG("Race history initialization failed\n");
    }
    
    // Initialize webhook manager and load webhooks from config
    webhookManager.setEnabled(config.getWebhooksEnabled());
    for (uint8_t i = 0; i < config.getWebhookCount(); i++) {
        const char* ip = config.getWebhookIP(i);
        if (ip) {
            webhookManager.addWebhook(ip);
            DEBUG("Loaded webhook: %s\n", ip);
        }
    }
    
    // Initialize multi-node manager — pass LED and webserver so the recruit
    // job can hold the LED solid-on and restore the AP after STA work.
    multiNodeManager.init(&config, &led, &ws);
    // The client's own laps live in LapTimer's ring and are authoritative
    // (§3).  Handing the sync layer that pointer is what lets it read the
    // unacked window straight out of the ring instead of keeping a second,
    // lossy copy — see the notes on _timer in multinode.h.
    multiNodeManager.setLapTimer(&timer);

    ws.init(&config, &timer, nullptr, &buzzer, &led, &raceHistory, &storage, &selfTest, &rx, &webhookManager, &multiNodeManager);

    // OTA manager — uses the webserver's SSE channel for progress events,
    // pauses multinode networking during home-WiFi excursions, and in master
    // mode tears the AP fully down (and restarts it via ws.startAP()) for the
    // duration of the excursion so the STA radio gets clean time for the
    // outbound HTTPS handshake.
    otaManager.init(&config, &timer, ws.getEvents(), &multiNodeManager, &ws);

    // Initialize USB transport
    usbTransport.init(&config, &timer, nullptr, &buzzer, &led, &raceHistory, &storage, &selfTest, &rx);
    
    // Register transports with TransportManager
    transportManager.addTransport(&ws);
    transportManager.addTransport(&usbTransport);
    
    // Set TransportManager in webserver for event broadcasting
    ws.setTransportManager(&transportManager);
    
    DEBUG("Transport system initialized (WiFi + USB)\n");
    
    #ifdef PIN_LED
        led.on(400);
    #endif
    #ifdef PIN_BUZZER
        buzzer.beep(200);
        buzzer.beep(200);
    #endif
    initParallelTask();  // Start Core 0 task
    
    /* DISABLED: RotorHazard mode initialization
    if (currentMode == MODE_WIFI) {
        // WiFi mode - start web server and services
        selfTest.init(&storage);
        ws.init(&config, &timer, &monitor, &buzzer, &led, &raceHistory, &storage, &selfTest, &rx);
        led.on(400);
        buzzer.beep(200);
        initParallelTask();  // Start Core 0 task
    } else {
        // RotorHazard mode - start node protocol
        nodeMode.begin(&timer, &config);
        led.blink(100, 1900);  // Slow blink = node mode active (100ms on, 1900ms off)
        // NO parallel task (avoid WiFi interference)
        // NO buzzer beep (silent operation)
    }
    */
}

void loop() {
    uint32_t currentTimeMs = millis();

    // LED Flashing
    static bool led_on = false;
    static uint32_t t = 0;
    
    if (ws.servicesStarted) {
        if (currentTimeMs - t > 500) {
            t = millis();
            led_on = !led_on;
            digitalWrite(LED_BUILTIN, led_on ? HIGH : LOW);
        }
    } else {
        digitalWrite(LED_BUILTIN, HIGH); // LED off when services not started
    }
    
    // Timing always runs
    timer.handleLapTimerUpdate(currentTimeMs);
#if RSSI_LOGGING_ENABLED
    rssiLogger.log(timer.snapshot);
#endif
    
    // A scheduled race start just fired (§8).  Tell the UI at the instant the
    // race ACTUALLY began, not when the start command arrived — the command
    // lands up to RACE_START_MARGIN_US early, and at a different moment on
    // every node because the fanout is sequential.  Emitting here is what
    // keeps the displays as synchronised as the timers underneath them.
    if (timer.consumeStartEvent()) {
        // A GO that arrived over UDP did none of the bookkeeping the old HTTP
        // masterStart handler used to do — it only carried a timestamp.  All
        // of it belongs here anyway, at the real start:
        if (multiNodeManager.isClientMode()) {
            multiNodeManager.setTimerRunning(true);    // heartbeat now reports racing
            multiNodeManager.setMasterRaceActive(true);
            // A client's Race View listens on masterRaceState, not raceState,
            // so both have to be released or the pilot's own display never
            // starts at all.
            ws.getEvents()->send("started", "masterRaceState");
        }
        transportManager.broadcastRaceStateEvent("started");
        ws.pushMultiNodeState();
    }

    // Broadcast lap events to all transports (WiFi + USB)
    if (timer.isLapAvailable()) {
        uint32_t lapTime   = timer.getLapTime();
        uint8_t  peakRssi  = timer.getLastLapPeakRssi();
        transportManager.broadcastLapEvent(lapTime, peakRssi);
        // In client mode, also queue the lap to be forwarded to the master node
        multiNodeManager.queueLap(lapTime);
    }
    
    // OTA update progress is driven from parallelTask (otaManager.loop());
    // no per-loop work needed here.

    // Initialize SD card after boot (deferred to prevent watchdog timeout)
    // Try once after 5 seconds of uptime
    if (!sdInitAttempted && currentTimeMs > 5000) {
        sdInitAttempted = true;
        DEBUG("\n=== Deferred SD card initialization ===\n");
        
        if (storage.initSDDeferred()) {
            DEBUG("SD card ready!\n");
            
            // Try to restore config from SD backup if EEPROM was invalid
            // (This handles the case where config was reset to defaults during boot)
            DEBUG("Checking for config backup on SD card...\n");
            if (config.loadFromSD()) {
                DEBUG("Config restored from SD backup after SD mount\n");
            }
            
            // Migrate sounds from LittleFS to SD card
            if (storage.migrateSoundsToSD()) {
                DEBUG("Sound files migrated successfully!\n");
                DEBUG("Recommend: delete /sounds from LittleFS to reclaim space\n");
            }
            
            // Reload race history from SD card
            if (raceHistory.loadRaces()) {
                DEBUG("Race history reloaded from SD card, %d races available\n", raceHistory.getRaceCount());
            } else {
                DEBUG("Race history reload from SD card failed\n");
            }
        } else {
            DEBUG("SD card not available - using LittleFS only\n");
        }
    }
    
    /* DISABLED: RotorHazard mode loop
    if (currentMode == MODE_WIFI) {
        // WiFi mode - original behavior
        ElegantOTA.loop();
        
        // ... SD card initialization code ...
    } else {
        // RotorHazard mode - run node protocol
        nodeMode.process();
        
        // Still update hardware (LED, buzzer) but NOT web server
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
#ifdef ESP32S3
        rgbLed.handleRgbLed(currentTimeMs);
#endif
        rx.handleFrequencyChange(currentTimeMs, config.getFrequency());
        monitor.checkBatteryState(currentTimeMs, config.getAlarmThreshold());
    }
    */

    vTaskDelay(1); // so we don't hog the CPU
}
