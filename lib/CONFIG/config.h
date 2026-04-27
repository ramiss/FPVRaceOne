#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <stdint.h>

// Firmware version — overridable from CI build flags so the git tag is the
// single source of truth.  Local builds fall back to the placeholder below.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.0.0-dev"
#endif

// GitHub OTA update target.  Override at build time with -DUPDATE_REPO if you
// fork the project.  Asset names are matched verbatim against release assets.
#ifndef UPDATE_REPO
#define UPDATE_REPO "ramiss/FPVRaceOne"
#endif
#define UPDATE_FW_ASSET "FPVRaceOne-firmware.bin"
#define UPDATE_FS_ASSET "FPVRaceOne-littlefs.bin"

// RSSI Logging for Dev only. Set to 0 for production
// Set to 1 to enable dedicated RSSI logging SD card (see lib/RSSILOG/)
// Set to 0 to compile it out entirely
// VCC           →   3.3 V
// GND           →   GND
// CS            →   GPIO 15
// SCK           →   GPIO 14
// MOSI          →   GPIO 13
// MISO          →   GPIO 25   ← must be 25, NOT 12 (GPIO12 is a boot-strapping pin)
#define RSSI_LOGGING_ENABLED 0

// USB Serial RSSI stream for diagnostics.
// Set to 1 to compile in the stream feature; toggle at runtime via POST /api/rssistream.
// Set to 0 to compile it out entirely for production builds.
#define RSSI_STREAM_ENABLED 0

//ESP23-C3
#if defined(ESP32C3)

#define PIN_LED 1
#define PIN_VBAT 0
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 3
#define PIN_RX5808_DATA 6     //CH1
#define PIN_RX5808_SELECT 7   //CH2
#define PIN_RX5808_CLOCK 4    //CH3
#define PIN_BUZZER 5
#define BUZZER_INVERTED false
#define PIN_MODE_SWITCH 1     // Mode selection: LOW=WiFi, HIGH=RotorHazard

//ESP32-S3
#elif defined(ESP32S3)

#define PIN_LED 2
#define PIN_RGB_LED 48         // WS2812 RGB LED on ESP32-S3-DevKitC-1
#define PIN_VBAT 1
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 4      // RSSI on Pin 4 (GPIO3 is a strapping pin - causes boot issues!)
#define PIN_RX5808_DATA 10     // CH1 on Pin 10
#define PIN_RX5808_SELECT 11   // CH2 on Pin 11
#define PIN_RX5808_CLOCK 12    // CH3 on Pin 12
#define PIN_BUZZER 5
#define BUZZER_INVERTED false
#define PIN_MODE_SWITCH 9      // Mode selection: LOW=WiFi, HIGH=RotorHazard
// SD Card SPI pins (tested and working configuration)
#define PIN_SD_CS 39
#define PIN_SD_SCK 36
#define PIN_SD_MOSI 35
#define PIN_SD_MISO 37

// FPV Scanner Hardware (XIAO ESP32C6)
#elif defined(APP_BOARD_XIAO_C6)

#define USE_EXT_ANTENNA true
#define WIFI_POWER WIFI_POWER_21dBm    // Max power for AP mode to improve range
//#define PIN_LED 0
//#define PIN_VBAT 35
//#define VBAT_SCALE 2
//#define VBAT_ADD 2
#define PIN_RX5808_RSSI A2
#define PIN_RX5808_DATA D10  // CH1
#define PIN_RX5808_SELECT A1  // CH2
#define PIN_RX5808_CLOCK D8  // CH3
//#define PIN_BUZZER 27
//#define BUZZER_INVERTED false
//#define PIN_MODE_SWITCH 33   // Mode selection: LOW=WiFi, HIGH=RotorHazard
// SD Card SPI pins (tested and working configuration)
//#define PIN_SD_CS 39
//#define PIN_SD_SCK 36
//#define PIN_SD_MOSI 35
//#define PIN_SD_MISO 37

//ESP32
#else

#define PIN_LED 21
#define PIN_VBAT 35
#define VBAT_SCALE 2
#define VBAT_ADD 2
#define PIN_RX5808_RSSI 33
#define PIN_RX5808_DATA 19   //CH1
#define PIN_RX5808_SELECT 22 //CH2
#define PIN_RX5808_CLOCK 23  //CH3
#define PIN_BUZZER 27
#define BUZZER_INVERTED false
#define PIN_MODE_SWITCH 33   // Mode selection: LOW=WiFi, HIGH=RotorHazard

#endif

// Mode selection constants
#define WIFI_MODE LOW          // GND on switch pin = WiFi/Standalone mode
#define ROTORHAZARD_MODE HIGH  // HIGH (floating/pullup) = RotorHazard node mode

#define EEPROM_RESERVED_SIZE 512
#define CONFIG_MAGIC_MASK (0b11U << 30)
#define CONFIG_MAGIC (0b01U << 30)
#define CONFIG_VERSION 17

#define EEPROM_CHECK_TIME_MS 1000

typedef struct {
    uint8_t bandIndex;    
    uint8_t channelIndex; 
    uint32_t version;
    uint16_t frequency;
    uint8_t minLap;
    uint8_t alarm;
    uint8_t announcerType;
    uint8_t announcerRate;
    uint8_t enterRssi;
    uint8_t exitRssi;
    uint8_t rssiSens;          // 0=Normal(Legacy), 1=High(1.5x Boost)
    uint8_t maxLaps;
    uint8_t ledMode;           // 0=off, 1=solid, 2=pulse, 3=rainbow (legacy, kept for migration)
    uint8_t ledBrightness;     // 0-255
    uint32_t ledColor;         // RGB as 0xRRGGBB (solid color)
    uint8_t ledPreset;         // 0-9 (new preset system)
    uint8_t ledSpeed;          // 1-20 (animation speed)
    uint32_t ledFadeColor;     // RGB for COLOR_FADE preset
    uint32_t ledStrobeColor;   // RGB for STROBE preset
    uint8_t ledManualOverride; // Manual override flag (0=off, 1=on)
    uint8_t operationMode;     // 0=WiFi, 1=RotorHazard (software switch)
    uint8_t tracksEnabled;     // Track feature enabled (0=disabled, 1=enabled)
    uint32_t selectedTrackId;  // Currently selected track (0=none)
    uint8_t webhooksEnabled;   // Webhooks enabled (0=disabled, 1=enabled)
    char webhookIPs[10][16];   // Up to 10 webhook IPs (xxx.xxx.xxx.xxx format)
    uint8_t webhookCount;      // Number of configured webhooks
    uint8_t gateLEDsEnabled;   // Gate LEDs feature enabled (0=disabled, 1=enabled)
    uint8_t webhookRaceStart;  // Send /RaceStart webhook (0=disabled, 1=enabled)
    uint8_t webhookRaceStop;   // Send /RaceStop webhook (0=disabled, 1=enabled)
    uint8_t webhookLap;        // Send /Lap webhook (0=disabled, 1=enabled)
    char pilotName[21];
    uint32_t pilotColor;       // Pilot color (0xRRGGBB)
    char theme[21];            // UI theme name
    char selectedVoice[21];    // Voice selection (default, rachel, piper, etc)
    uint8_t voiceEnabled;      // 0 = off, 1 = on
    char lapFormat[11];        // Lap announcement format (full, laptime, timeonly)
    char ssid[33];
    char password[33];
    uint8_t wifiExtAntenna;     // 0=internal, 1=external antenna (takes effect on next boot)
    uint8_t wifiTxPower;        // WiFi TX power in dBm (2–21, takes effect on next boot)
    uint8_t filterMode;         // 0=V1 FPVRaceOne (Kalman+EMA pipeline), 1=V2 RotorHazard (raw passthrough)
    uint8_t besselLevel;        // Independent Bessel post-stage: 0=off, 1..10 increasing smoothing
    uint8_t enterHoldSamples;   // V1: consecutive samples at/above enter threshold before gate entry (1-20)
    uint8_t exitConfirmSamples; // V1: consecutive samples below exit threshold to confirm exit (1-10)
    uint8_t nodeMode;           // 0=single (default), 1=master, 2=client
    char masterSSID[33];        // SSID of master to connect to (client mode only)
    char masterPassword[33];    // Password for master AP (default "fpvraceone")
    uint8_t mnSkipMasterStart;  // 1=skip master Start All if race already running on this node
    uint8_t devMode;            // 0=off, 1=dev mode (simulate laps by clicking pilot name)
} laptimer_config_t;

class Storage;  // Forward declaration

class Config {
   public:
    void init();
    void load();
    void write();
    void toJson(AsyncResponseStream& destination);
    void toJsonString(char* buf);
    void fromJson(JsonObject source);
    void handleEeprom(uint32_t currentTimeMs);
    
    // SD card backup/restore
    void setStorage(Storage* stor) { storage = stor; }
    bool saveToSD();
    bool loadFromSD();

    // getters and setters
    uint8_t getBandIndex();
    uint8_t getChannelIndex();
    uint16_t getFrequency();
    uint32_t getMinLapMs();
    uint8_t getAlarmThreshold();
    uint8_t getEnterRssi();
    uint8_t getExitRssi();
    uint8_t getMaxLaps();
    uint8_t getLedMode();
    uint8_t getLedBrightness();
    uint32_t getLedColor();
    uint8_t getLedPreset();
    uint8_t getLedSpeed();
    uint32_t getLedFadeColor();
    uint32_t getLedStrobeColor();
    uint8_t getLedManualOverride();
    uint8_t getTracksEnabled();
    uint32_t getSelectedTrackId();
    uint8_t getWebhooksEnabled();
    uint8_t getWebhookCount();
    const char* getWebhookIP(uint8_t index);
    uint8_t getGateLEDsEnabled();
    uint8_t getWebhookRaceStart();
    uint8_t getWebhookRaceStop();
    uint8_t getWebhookLap();
    char* getSsid();
    char* getPassword();
    uint8_t getOperationMode();
    uint32_t getPilotColor();
    char* getTheme();
    char* getSelectedVoice();
    uint8_t getVoiceEnabled();
    char* getLapFormat();
    uint8_t getWifiExtAntenna();
    uint8_t getWifiTxPower();
    uint8_t getFilterMode();
    uint8_t getBesselLevel();
    void    setBesselLevel(uint8_t level);
    uint8_t getEnterHoldSamples();
    uint8_t getExitConfirmSamples();
    char*   getPilotName();
    uint8_t getNodeMode();
    char*   getMasterSSID();
    char*   getMasterPassword();
    void    setNodeMode(uint8_t mode);
    bool    getMnSkipMasterStart();
    void    setMnSkipMasterStart(bool skip);
    uint8_t getDevMode();
    void    setDevMode(uint8_t mode);

    // added because we use multiple bands with the same channels and revere freq lookup no longer works
    void setBandIndex(uint8_t band);
    void setChannelIndex(uint8_t ch);
    // Setters for RotorHazard node mode
    void setFrequency(uint16_t freq);
    void setEnterRssi(uint8_t rssi);
    void setExitRssi(uint8_t rssi);
    void setOperationMode(uint8_t mode);
    
    // LED setters
    void setLedPreset(uint8_t preset);
    void setLedBrightness(uint8_t brightness);
    void setLedSpeed(uint8_t speed);
    void setLedColor(uint32_t color);
    void setLedFadeColor(uint32_t color);
    void setLedStrobeColor(uint32_t color);
    void setLedManualOverride(uint8_t override);
    void setTracksEnabled(uint8_t enabled);
    void setSelectedTrackId(uint32_t trackId);
    void setWebhooksEnabled(uint8_t enabled);
    bool addWebhookIP(const char* ip);
    bool removeWebhookIP(const char* ip);
    void clearWebhookIPs();
    void setGateLEDsEnabled(uint8_t enabled);
    void setWebhookRaceStart(uint8_t enabled);
    void setWebhookRaceStop(uint8_t enabled);
    void setWebhookLap(uint8_t enabled);
    void setVoiceEnabled(uint8_t enabled);

   private:
    laptimer_config_t conf;
    bool modified;
    volatile uint32_t checkTimeMs = 0;
    Storage* storage = nullptr;
    void setDefaults();
};

#endif // CONFIG_H
