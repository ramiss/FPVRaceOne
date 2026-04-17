#include "debug.h"
#include "led.h"
#include "multinode.h"
#include "webserver.h"
#include "racehistory.h"
#include "storage.h"
#include "selftest.h"
#include "transport.h"
#include "trackmanager.h"
#include "usb.h"
#include "webhook.h"
// DISABLED FOR NOW: #include "nodemode.h"  // Uncomment to re-enable RotorHazard support
#include <ElegantOTA.h>
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
static TrackManager trackManager;
static WebhookManager webhookManager;
static MultiNodeManager multiNodeManager;
#ifdef ESP32S3
static RgbLed rgbLed;
RgbLed* g_rgbLed = &rgbLed;
#else
void* g_rgbLed = nullptr;
#endif
static LapTimer timer;
// Battery monitoring removed - legacy feature no longer used
// static BatteryMonitor monitor;

static TaskHandle_t xTimerTask = NULL;
static bool sdInitAttempted = false;

static void parallelTask(void *pvArgs) {
    for (;;) {
        uint32_t currentTimeMs = millis();
        buzzer.handleBuzzer(currentTimeMs);
        led.handleLed(currentTimeMs);
#ifdef ESP32S3
        rgbLed.handleRgbLed(currentTimeMs);
#endif
        ws.handleWebUpdate(currentTimeMs);
        usbTransport.update(currentTimeMs);
        config.handleEeprom(currentTimeMs);
        rx.handleFrequencyChange(currentTimeMs, config.getFrequency());
        webhookManager.process();      // HTTP I/O belongs on Core 0, not in the RSSI loop
        multiNodeManager.process(currentTimeMs);  // Client: heartbeats/registration; Master: timeout checks
        // Battery monitoring removed
        // monitor.checkBatteryState(currentTimeMs, config.getAlarmThreshold());
        
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

    // ====================================================================
    // ROTORHAZARD MODE DETECTION - CURRENTLY DISABLED
    // Mode switching has been disabled - system now runs in WiFi mode only
    // To re-enable: uncomment the mode detection block below
    // ====================================================================

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
    
    // Initialize serial (115200)
    Serial.begin(115200);
    delay(100);
    
    // Clear serial buffer
    while (Serial.available()) {
        Serial.read();
    }
    
    // Always enable debug output (WiFi mode only)
    DEBUG_INIT;
    
    // Suppress VFS file-not-found errors (reduces spam from API endpoint checks)
    esp_log_level_set("vfs_api", ESP_LOG_NONE);
    
#ifdef ESP32S3
        DEBUG("ESP32S3 build detected - WiFi Mode\n");
#else
        DEBUG("Generic ESP32 build - WiFi Mode\n");
#endif
    
    // Note: config.init() already called above
    rx.init();
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
    
    // Initialize track manager
    if (trackManager.init(&storage)) {
        DEBUG("Track manager initialized, %d tracks loaded\n", trackManager.getTrackCount());
    } else {
        DEBUG("Track manager initialization failed\n");
    }
    
    // Load selected track if tracks are enabled
    if (config.getTracksEnabled() && config.getSelectedTrackId() != 0) {
        Track* selectedTrack = trackManager.getTrackById(config.getSelectedTrackId());
        if (selectedTrack) {
            timer.setTrack(selectedTrack);
            DEBUG("Selected track loaded: %s\n", selectedTrack->name.c_str());
        }
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
    
    // Initialize multi-node manager
    multiNodeManager.init(&config);

    ws.init(&config, &timer, nullptr, &buzzer, &led, &raceHistory, &storage, &selfTest, &rx, &trackManager, &webhookManager, &multiNodeManager);
    
    // Initialize USB transport
    usbTransport.init(&config, &timer, nullptr, &buzzer, &led, &raceHistory, &storage, &selfTest, &rx, &trackManager);
    
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
    
    // Broadcast lap events to all transports (WiFi + USB)
    if (timer.isLapAvailable()) {
        uint32_t lapTime = timer.getLapTime();
        transportManager.broadcastLapEvent(lapTime);
        // In client mode, also queue the lap to be forwarded to the master node
        multiNodeManager.queueLap(lapTime);
    }
    
    // WiFi mode - original behavior (RotorHazard mode disabled)
    ElegantOTA.loop();
    
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
            
            // Reload tracks from SD card
            if (trackManager.loadTracks()) {
                DEBUG("Tracks reloaded from SD card, %d tracks available\n", trackManager.getTrackCount());
                
                // Reload selected track if tracks are enabled
                if (config.getTracksEnabled() && config.getSelectedTrackId() != 0) {
                    Track* selectedTrack = trackManager.getTrackById(config.getSelectedTrackId());
                    if (selectedTrack) {
                        timer.setTrack(selectedTrack);
                        DEBUG("Selected track reloaded: %s\n", selectedTrack->name.c_str());
                    }
                }
            } else {
                DEBUG("Tracks reload from SD card failed\n");
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
