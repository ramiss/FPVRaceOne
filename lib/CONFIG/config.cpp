#include "config.h"

#include <EEPROM.h>
#include <LittleFS.h>

#include "debug.h"
#include "storage.h"

#ifdef ESP32S3
#include <SD.h>
#endif

#define CONFIG_BACKUP_PATH "/config_backup.bin"

void Config::init(void) {
    if (sizeof(laptimer_config_t) > EEPROM_RESERVED_SIZE) {
        DEBUG("Config size too big, adjust reserved EEPROM size\n");
        return;
    }

    EEPROM.begin(EEPROM_RESERVED_SIZE);  // Size of EEPROM
    load();                              // Override default settings from EEPROM

    checkTimeMs = millis();

    DEBUG("EEPROM Init Successful\n");
}

void Config::load(void) {
    modified = false;
    EEPROM.get(0, conf);

    // Ensure all char arrays are null-terminated regardless of EEPROM content
    conf.pilotName[sizeof(conf.pilotName)-1]         = '\0';
    conf.theme[sizeof(conf.theme)-1]                 = '\0';
    conf.selectedVoice[sizeof(conf.selectedVoice)-1] = '\0';
    conf.lapFormat[sizeof(conf.lapFormat)-1]         = '\0';
    conf.ssid[sizeof(conf.ssid)-1]                   = '\0';
    conf.password[sizeof(conf.password)-1]            = '\0';
    conf.masterSSID[sizeof(conf.masterSSID)-1]       = '\0';
    conf.masterPassword[sizeof(conf.masterPassword)-1] = '\0';

    uint32_t version = 0xFFFFFFFF;
    if ((conf.version & CONFIG_MAGIC_MASK) == CONFIG_MAGIC) {
        version = conf.version & ~CONFIG_MAGIC_MASK;
    }

    DEBUG("EEPROM raw: version=%u (expected=%u) voiceEnabled=%u wifiExtAntenna=%u wifiTxPower=%u\n",
          version, CONFIG_VERSION, conf.voiceEnabled, conf.wifiExtAntenna, conf.wifiTxPower);

    // If version is not current, try to restore from SD backup
    if (version != CONFIG_VERSION) {
        DEBUG("EEPROM config invalid (version=%u, expected=%u)\n", version, CONFIG_VERSION);
        if (loadFromSD()) {
            DEBUG("Successfully restored config from SD card backup\n");
            modified = true;  // Mark as modified to write back to EEPROM
            write();  // Write restored config to EEPROM
        } else {
            DEBUG("No SD backup found, using defaults\n");
            setDefaults();
        }
    }

    // Sanity: announcerRate stored as x10 (1–20). If invalid, reset to default (10).
    if (conf.announcerRate < 1 || conf.announcerRate > 20) {
        DEBUG("Invalid announcerRate=%u; resetting to default 10\n", conf.announcerRate);
        conf.announcerRate = 10;
        modified = true;
    }
}

void Config::write(void) {
    static bool inWrite = false;
    static uint32_t writeCount = 0;

    if (!modified) return;

    // Prevent re-entry/races
    if (inWrite) {
        //DEBUG("Writing to EEPROM skipped (re-entry)\n");
        return;
    }
    inWrite = true;

    writeCount++;
    DEBUG("Writing to EEPROM");

    EEPROM.put(0, conf);
    EEPROM.commit();

    DEBUG("Writing to EEPROM done");

    // Mark unmodified BEFORE SD backup to prevent back-to-back writes
    modified = false;

    // Also backup to SD card if available
    if (saveToSD()) {
        DEBUG("Config backed up to SD card\n");
    }

    inWrite = false;
}


void Config::toJson(AsyncResponseStream& destination) {
    // ~51 fields + 10 webhook IPs. On ESP32 each slot = 16 bytes + possible string copies.
    // 2048 gives a large margin to prevent silent overflow dropping fields.
    DynamicJsonDocument config(2048);
    config["band"] = conf.bandIndex;
    config["chan"] = conf.channelIndex;
    config["freq"] = conf.frequency;
    config["minLap"] = conf.minLap;
    config["alarm"] = conf.alarm;
    config["anType"] = conf.announcerType;
    config["anRate"] = conf.announcerRate;
    config["enterRssi"] = conf.enterRssi;
    config["exitRssi"] = conf.exitRssi;
    config["rssiSens"] = conf.rssiSens;
    config["maxLaps"] = conf.maxLaps;
    config["ledMode"] = conf.ledMode;
    config["ledBrightness"] = conf.ledBrightness;
    config["ledColor"] = conf.ledColor;
    config["ledPreset"] = conf.ledPreset;
    config["ledSpeed"] = conf.ledSpeed;
    config["ledFadeColor"] = conf.ledFadeColor;
    config["ledStrobeColor"] = conf.ledStrobeColor;
    config["ledManualOverride"] = conf.ledManualOverride;
    config["opMode"] = conf.operationMode;
    config["tracksEnabled"] = conf.tracksEnabled;
    config["selectedTrackId"] = conf.selectedTrackId;
    config["webhooksEnabled"] = conf.webhooksEnabled;
    config["webhookCount"] = conf.webhookCount;
    JsonArray webhooks = config.createNestedArray("webhookIPs");
    for (uint8_t i = 0; i < conf.webhookCount; i++) {
        webhooks.add(conf.webhookIPs[i]);
    }
    config["gateLEDsEnabled"] = conf.gateLEDsEnabled;
    config["webhookRaceStart"] = conf.webhookRaceStart;
    config["webhookRaceStop"] = conf.webhookRaceStop;
    config["webhookLap"] = conf.webhookLap;
    config["name"] = conf.pilotName;
    config["pilotColor"] = conf.pilotColor;
    config["theme"] = conf.theme;
    config["selectedVoice"] = conf.selectedVoice;
    config["voiceEnabled"] = conf.voiceEnabled;
    config["lapFormat"] = conf.lapFormat;
    config["ssid"] = conf.ssid;
    config["pwd"] = conf.password;
    config["wifiExtAntenna"] = conf.wifiExtAntenna;
    config["wifiTxPower"] = conf.wifiTxPower;
    config["filterMode"] = conf.filterMode;
    config["besselLevel"] = conf.besselLevel;
    config["nodeMode"] = conf.nodeMode;
    config["masterSSID"] = conf.masterSSID;
    config["masterPassword"] = conf.masterPassword;
    config["mnSkipMasterStart"] = conf.mnSkipMasterStart;
    config["devMode"] = conf.devMode;

    #ifdef PIN_VBAT
        config["hasVbat"] = true;
    #else
        config["hasVbat"] = false;
    #endif

    #ifdef PIN_LED
        config["hasLed"] = true;
    #else
        config["hasLed"] = false;
    #endif

    serializeJson(config, destination);
}

void Config::toJsonString(char* buf) {
    DynamicJsonDocument config(2048);
    config["band"] = conf.bandIndex;
    config["chan"] = conf.channelIndex;
    config["freq"] = conf.frequency;
    config["minLap"] = conf.minLap;
    config["alarm"] = conf.alarm;
    config["anType"] = conf.announcerType;
    config["anRate"] = conf.announcerRate;
    config["enterRssi"] = conf.enterRssi;
    config["exitRssi"] = conf.exitRssi;
    config["rssiSens"] = conf.rssiSens;
    config["maxLaps"] = conf.maxLaps;
    config["ledMode"] = conf.ledMode;
    config["ledBrightness"] = conf.ledBrightness;
    config["ledColor"] = conf.ledColor;
    config["ledPreset"] = conf.ledPreset;
    config["ledSpeed"] = conf.ledSpeed;
    config["ledFadeColor"] = conf.ledFadeColor;
    config["ledStrobeColor"] = conf.ledStrobeColor;
    config["ledManualOverride"] = conf.ledManualOverride;
    config["opMode"] = conf.operationMode;
    config["tracksEnabled"] = conf.tracksEnabled;
    config["selectedTrackId"] = conf.selectedTrackId;
    config["name"] = conf.pilotName;
    config["ssid"] = conf.ssid;
    config["pwd"] = conf.password;
    config["filterMode"] = conf.filterMode;
    config["besselLevel"] = conf.besselLevel;
    config["nodeMode"] = conf.nodeMode;
    config["masterSSID"] = conf.masterSSID;
    config["masterPassword"] = conf.masterPassword;
    config["mnSkipMasterStart"] = conf.mnSkipMasterStart;
    config["devMode"] = conf.devMode;

    #ifdef PIN_VBAT
        config["hasVbat"] = true;
    #else
        config["hasVbat"] = false;
    #endif

    #ifdef PIN_LED
        config["hasLed"] = true;
    #else
        config["hasLed"] = false;
    #endif

    config["voiceEnabled"] = conf.voiceEnabled;
    config["wifiExtAntenna"] = conf.wifiExtAntenna;
    config["wifiTxPower"] = conf.wifiTxPower;

    serializeJsonPretty(config, buf, 2048);
}

void Config::fromJson(JsonObject source) {
    // Helpers to avoid "missing key reads as 0/empty" and to prevent false modified=true.
    auto setU8 = [&](const char* key, uint8_t& dst, uint8_t minV, uint8_t maxV) {
        if (!source.containsKey(key)) return;
        int v = source[key].as<int>();
        if (v < (int)minV) v = minV;
        if (v > (int)maxV) v = maxV;
        uint8_t nv = (uint8_t)v;
        if (dst != nv) { dst = nv; modified = true; }
    };

    auto setU16 = [&](const char* key, uint16_t& dst, uint16_t minV, uint16_t maxV) {
        if (!source.containsKey(key)) return;
        int v = source[key].as<int>();
        if (v < (int)minV) v = minV;
        if (v > (int)maxV) v = maxV;
        uint16_t nv = (uint16_t)v;
        if (dst != nv) { dst = nv; modified = true; }
    };

    auto setU32 = [&](const char* key, uint32_t& dst) {
        if (!source.containsKey(key)) return;
        uint32_t nv = source[key].as<uint32_t>();
        if (dst != nv) { dst = nv; modified = true; }
    };

    auto setBool01 = [&](const char* key, uint8_t& dst) {
        if (!source.containsKey(key)) return;
        int v = source[key].as<int>();
        uint8_t nv = (v != 0) ? 1 : 0;
        if (dst != nv) { dst = nv; modified = true; }
    };

    auto setStr = [&](const char* key, char* dst, size_t dstSize) {
        if (!source.containsKey(key)) return;
        const char* v = source[key] | "";
        if (strcmp(v, dst) != 0) {
            strlcpy(dst, v, dstSize);
            modified = true;
        }
    };

    if (source.containsKey("band")) {
        int b = source["band"].as<int>();
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        setBandIndex((uint8_t)b);
    }

    if (source.containsKey("chan")) {
        int c = source["chan"].as<int>();
        if (c < 0) c = 0;
        if (c > 7) c = 7;
        setChannelIndex((uint8_t)c);
    }

    // ===== Core timing / RF =====
    // Frequency (MHz). Allow 0 only if you really use it; otherwise clamp to plausible band.
    if (source.containsKey("freq")) {
        int f = source["freq"].as<int>();
        if (f < 0) f = 0;
        if (f > 7000) f = 7000;
        uint16_t nf = (uint16_t)f;
        if (conf.frequency != nf) { conf.frequency = nf; modified = true; }
    }

    // Units: stored x10 (0.1s steps) per your UI, but kept as uint8
    setU8("minLap",   conf.minLap,        0, 255);
    setU8("alarm",    conf.alarm,         0, 255);

    // Announcer
    setU8("anType",   conf.announcerType, 0, 20);

    // IMPORTANT: rate is x10 (0.1–2.0 => 1–20); default 10
    if (source.containsKey("anRate")) {
        int r = source["anRate"].as<int>();
        if (r < 1) r = 10;     // fall back to default 1.0 if invalid
        if (r > 20) r = 20;
        uint8_t nr = (uint8_t)r;
        if (conf.announcerRate != nr) { conf.announcerRate = nr; modified = true; }
    }

    // RSSI + race settings
    setU8("enterRssi", conf.enterRssi,    0, 255);
    setU8("exitRssi",  conf.exitRssi,     0, 255);
    setU8("rssiSens", conf.rssiSens,      0, 1);
    setU8("maxLaps",   conf.maxLaps,      0, 255);

    // ===== LED settings =====
    if (source.containsKey("ledMode"))           setU8("ledMode",           conf.ledMode,           0, 10);
    if (source.containsKey("ledBrightness"))     setU8("ledBrightness",     conf.ledBrightness,     0, 255);
    if (source.containsKey("ledColor"))          setU32("ledColor",         conf.ledColor);
    if (source.containsKey("ledPreset"))         setU8("ledPreset",         conf.ledPreset,         0, 50);
    if (source.containsKey("ledSpeed"))          setU8("ledSpeed",          conf.ledSpeed,          1, 20);
    if (source.containsKey("ledFadeColor"))      setU32("ledFadeColor",     conf.ledFadeColor);
    if (source.containsKey("ledStrobeColor"))    setU32("ledStrobeColor",   conf.ledStrobeColor);
    if (source.containsKey("ledManualOverride")) setBool01("ledManualOverride", conf.ledManualOverride);

    // ===== System =====
    if (source.containsKey("opMode")) setU8("opMode", conf.operationMode, 0, 1);

    // ===== Tracks =====
    if (source.containsKey("tracksEnabled")) setBool01("tracksEnabled", conf.tracksEnabled);
    if (source.containsKey("selectedTrackId")) setU32("selectedTrackId", conf.selectedTrackId);

    // ===== Gate LED options + webhook event toggles =====
    if (source.containsKey("gateLEDsEnabled"))  setBool01("gateLEDsEnabled", conf.gateLEDsEnabled);
    if (source.containsKey("webhookRaceStart")) setBool01("webhookRaceStart", conf.webhookRaceStart);
    if (source.containsKey("webhookRaceStop"))  setBool01("webhookRaceStop", conf.webhookRaceStop);
    if (source.containsKey("webhookLap"))       setBool01("webhookLap", conf.webhookLap);

    // ===== Webhooks master enable =====
    if (source.containsKey("webhooksEnabled")) setBool01("webhooksEnabled", conf.webhooksEnabled);

    // ===== Webhook IP list (only mark modified if list actually changed) =====
    if (source.containsKey("webhookIPs")) {
        JsonArray arr = source["webhookIPs"].as<JsonArray>();

        // Compare current list vs incoming
        bool changed = false;

        uint8_t incomingCount = 0;
        for (JsonVariant ipV : arr) {
            if (incomingCount >= 10) break;
            incomingCount++;
        }

        if (incomingCount != conf.webhookCount) {
            changed = true;
        } else {
            uint8_t i = 0;
            for (JsonVariant ipV : arr) {
                if (i >= 10) break;
                const char* ipStr = ipV.as<const char*>() ? ipV.as<const char*>() : "";
                if (strcmp(conf.webhookIPs[i], ipStr) != 0) {
                    changed = true;
                    break;
                }
                i++;
            }
        }

        if (changed) {
            memset(conf.webhookIPs, 0, sizeof(conf.webhookIPs));
            conf.webhookCount = 0;

            for (JsonVariant ipV : arr) {
                if (conf.webhookCount >= 10) break;
                const char* ipStr = ipV.as<const char*>() ? ipV.as<const char*>() : "";
                strlcpy(conf.webhookIPs[conf.webhookCount], ipStr, 16);
                conf.webhookCount++;
            }

            modified = true;
        }
    }

    // ===== Pilot/UI strings =====
    setStr("name",          conf.pilotName,     sizeof(conf.pilotName));

    if (source.containsKey("pilotColor")) {
        uint32_t c = source["pilotColor"].as<uint32_t>();
        if (conf.pilotColor != c) { conf.pilotColor = c; modified = true; }
    }

    if (source.containsKey("theme"))        setStr("theme",        conf.theme,        sizeof(conf.theme));
    if (source.containsKey("selectedVoice"))setStr("selectedVoice",conf.selectedVoice,sizeof(conf.selectedVoice));
    if (source.containsKey("voiceEnabled")) {
        int ve = source["voiceEnabled"].as<int>();
        uint8_t nve = (ve != 0) ? 1 : 0;
        if (conf.voiceEnabled != nve) { conf.voiceEnabled = nve; modified = true; }
    }
    if (source.containsKey("lapFormat"))    setStr("lapFormat",    conf.lapFormat,    sizeof(conf.lapFormat));

    // ===== WiFi credentials (CRITICAL: must be guarded) =====
    if (source.containsKey("ssid")) setStr("ssid", conf.ssid, sizeof(conf.ssid));
    if (source.containsKey("pwd"))  setStr("pwd",  conf.password, sizeof(conf.password));

    // ===== WiFi antenna / power (next-boot settings) =====
    if (source.containsKey("wifiExtAntenna")) setBool01("wifiExtAntenna", conf.wifiExtAntenna);
    if (source.containsKey("wifiTxPower"))    setU8("wifiTxPower", conf.wifiTxPower, 2, 21);

    // ===== Signal processing mode =====
    if (source.containsKey("filterMode"))       setU8("filterMode",       conf.filterMode,       0, 1);
    if (source.containsKey("besselLevel"))      setU8("besselLevel",      conf.besselLevel,      0, 10);

    // ===== Multi-node =====
    if (source.containsKey("nodeMode"))           setU8("nodeMode", conf.nodeMode, 0, 2);
    if (source.containsKey("masterSSID"))         setStr("masterSSID",     conf.masterSSID,     sizeof(conf.masterSSID));
    if (source.containsKey("masterPassword"))     setStr("masterPassword", conf.masterPassword, sizeof(conf.masterPassword));
    if (source.containsKey("mnSkipMasterStart"))  setU8("mnSkipMasterStart", conf.mnSkipMasterStart, 0, 1);
    if (source.containsKey("devMode"))            setU8("devMode", conf.devMode, 0, 1);
}


/*  - old version with unnecessary eeprom saves
void Config::fromJson(JsonObject source) {
    if (source["freq"] != conf.frequency) {
        conf.frequency = source["freq"];
        modified = true;
    }
    if (source["minLap"] != conf.minLap) {
        conf.minLap = source["minLap"];
        modified = true;
    }
    if (source["alarm"] != conf.alarm) {
        conf.alarm = source["alarm"];
        modified = true;
    }
    if (source["anType"] != conf.announcerType) {
        conf.announcerType = source["anType"];
        modified = true;
    }
    if (source.containsKey("anRate")) {
        int r = source["anRate"].as<int>();

        // UI range is 0.1–2.0, stored as x10 => 1–20
        if (r < 1) r = 10;     // fall back to default 1.0
        if (r > 20) r = 20;

        if ((uint8_t)r != conf.announcerRate) {
            conf.announcerRate = (uint8_t)r;
            modified = true;
        }
    }
    if (source["enterRssi"] != conf.enterRssi) {
        conf.enterRssi = source["enterRssi"];
        modified = true;
    }
    if (source["exitRssi"] != conf.exitRssi) {
        conf.exitRssi = source["exitRssi"];
        modified = true;
    }
    if (source["maxLaps"] != conf.maxLaps) {
        conf.maxLaps = source["maxLaps"];
        modified = true;
    }
    if (source.containsKey("ledMode") && source["ledMode"] != conf.ledMode) {
        conf.ledMode = source["ledMode"];
        modified = true;
    }
    if (source.containsKey("ledBrightness") && source["ledBrightness"] != conf.ledBrightness) {
        conf.ledBrightness = source["ledBrightness"];
        modified = true;
    }
    if (source.containsKey("ledColor") && source["ledColor"] != conf.ledColor) {
        conf.ledColor = source["ledColor"];
        modified = true;
    }
    if (source.containsKey("ledPreset") && source["ledPreset"] != conf.ledPreset) {
        conf.ledPreset = source["ledPreset"];
        modified = true;
    }
    if (source.containsKey("ledSpeed") && source["ledSpeed"] != conf.ledSpeed) {
        conf.ledSpeed = source["ledSpeed"];
        modified = true;
    }
    if (source.containsKey("ledFadeColor") && source["ledFadeColor"] != conf.ledFadeColor) {
        conf.ledFadeColor = source["ledFadeColor"];
        modified = true;
    }
    if (source.containsKey("ledStrobeColor") && source["ledStrobeColor"] != conf.ledStrobeColor) {
        conf.ledStrobeColor = source["ledStrobeColor"];
        modified = true;
    }
    if (source.containsKey("ledManualOverride") && source["ledManualOverride"] != conf.ledManualOverride) {
        conf.ledManualOverride = source["ledManualOverride"];
        modified = true;
    }
    if (source.containsKey("opMode") && source["opMode"] != conf.operationMode) {
        conf.operationMode = source["opMode"];
        modified = true;
    }
    if (source.containsKey("tracksEnabled") && source["tracksEnabled"] != conf.tracksEnabled) {
        conf.tracksEnabled = source["tracksEnabled"];
        modified = true;
    }
    if (source.containsKey("selectedTrackId") && source["selectedTrackId"] != conf.selectedTrackId) {
        conf.selectedTrackId = source["selectedTrackId"];
        modified = true;
    }
    if (source.containsKey("gateLEDsEnabled") && source["gateLEDsEnabled"] != conf.gateLEDsEnabled) {
        conf.gateLEDsEnabled = source["gateLEDsEnabled"];
        modified = true;
    }
    if (source.containsKey("webhookRaceStart") && source["webhookRaceStart"] != conf.webhookRaceStart) {
        conf.webhookRaceStart = source["webhookRaceStart"];
        modified = true;
    }
    if (source.containsKey("webhookRaceStop") && source["webhookRaceStop"] != conf.webhookRaceStop) {
        conf.webhookRaceStop = source["webhookRaceStop"];
        modified = true;
    }
    if (source.containsKey("webhookLap") && source["webhookLap"] != conf.webhookLap) {
        conf.webhookLap = source["webhookLap"];
        modified = true;
    }
    // Webhook IPs and enabled state
    if (source.containsKey("webhooksEnabled") && source["webhooksEnabled"] != conf.webhooksEnabled) {
        conf.webhooksEnabled = source["webhooksEnabled"];
        modified = true;
    }
    if (source.containsKey("webhookIPs")) {
        JsonArray webhookArray = source["webhookIPs"].as<JsonArray>();

        bool changed = false;

        // Compare count
        if (webhookArray.size() != conf.webhookCount) {
            changed = true;
        } else {
            // Compare entries
            uint8_t i = 0;
            for (JsonVariant ip : webhookArray) {
                const char* ipStr = ip.as<const char*>() ? ip.as<const char*>() : "";
                if (strcmp(conf.webhookIPs[i], ipStr) != 0) {
                    changed = true;
                    break;
                }
                i++;
            }
        }

        if (changed) {
            memset(conf.webhookIPs, 0, sizeof(conf.webhookIPs));
            conf.webhookCount = 0;

            for (JsonVariant ip : webhookArray) {
                if (conf.webhookCount < 10) {
                    const char* ipStr = ip.as<const char*>() ? ip.as<const char*>() : "";
                    strlcpy(conf.webhookIPs[conf.webhookCount], ipStr, 16);
                    conf.webhookCount++;
                }
            }
            modified = true;
        }
    }

    if (source.containsKey("name")) {
        const char* v = source["name"] | "";
        if (strcmp(v, conf.pilotName) != 0) {
            strlcpy(conf.pilotName, v, sizeof(conf.pilotName));
            modified = true;
        }
    }
    if (source.containsKey("pilotColor") && source["pilotColor"] != conf.pilotColor) {
        conf.pilotColor = source["pilotColor"];
        modified = true;
    }
    if (source.containsKey("theme") && source["theme"] != conf.theme) {
        strlcpy(conf.theme, source["theme"] | "oceanic", sizeof(conf.theme));
        modified = true;
    }
    if (source.containsKey("selectedVoice") && source["selectedVoice"] != conf.selectedVoice) {
        strlcpy(conf.selectedVoice, source["selectedVoice"] | "default", sizeof(conf.selectedVoice));
        modified = true;
    }
    if (source.containsKey("lapFormat") && source["lapFormat"] != conf.lapFormat) {
        strlcpy(conf.lapFormat, source["lapFormat"] | "pilottime", sizeof(conf.lapFormat));
        modified = true;
    }
    if (source.containsKey("ssid")) {
        const char* v = source["ssid"] | "";
        if (strcmp(v, conf.ssid) != 0) {
            strlcpy(conf.ssid, v, sizeof(conf.ssid));
            modified = true;
        }
    }
    if (source.containsKey("pwd")) {
        const char* v = source["pwd"] | "";
        if (strcmp(v, conf.password) != 0) {
            strlcpy(conf.password, v, sizeof(conf.password));
            modified = true;
        }
    }
}
    */

uint8_t Config::getBandIndex() {
  return conf.bandIndex;
}

uint8_t Config::getChannelIndex() {
  return conf.channelIndex;
}

uint16_t Config::getFrequency() {
    // === TEMPORARY HARDCODE FOR RX5808 CH1 PIN ISSUE ===
    // Hardcoded to R1 (5658 MHz) - Raceband Channel 1
    // TODO: Remove this once CH1 pin is fixed and revert to: return conf.frequency;
    // return 5658;
    // === END TEMPORARY HARDCODE ===
    return conf.frequency;
}

uint32_t Config::getMinLapMs() {
    return conf.minLap * 100;
}

uint8_t Config::getAlarmThreshold() {
    return conf.alarm;
}

uint8_t Config::getEnterRssi() {
    return conf.enterRssi;
}

uint8_t Config::getExitRssi() {
    return conf.exitRssi;
}

char* Config::getSsid() {
    return conf.ssid;
}

char* Config::getPassword() {
    return conf.password;
}

uint8_t Config::getMaxLaps() {
    return conf.maxLaps;
}

uint8_t Config::getLedMode() {
    return conf.ledMode;
}

uint8_t Config::getLedBrightness() {
    return conf.ledBrightness;
}

uint32_t Config::getLedColor() {
    return conf.ledColor;
}

uint8_t Config::getLedPreset() {
    return conf.ledPreset;
}

uint8_t Config::getLedSpeed() {
    return conf.ledSpeed;
}

uint32_t Config::getLedFadeColor() {
    return conf.ledFadeColor;
}

uint32_t Config::getLedStrobeColor() {
    return conf.ledStrobeColor;
}

uint8_t Config::getLedManualOverride() {
    return conf.ledManualOverride;
}

uint8_t Config::getOperationMode() {
    return conf.operationMode;
}

uint8_t Config::getTracksEnabled() {
    return conf.tracksEnabled;
}

uint32_t Config::getSelectedTrackId() {
    return conf.selectedTrackId;
}

uint8_t Config::getWebhooksEnabled() {
    return conf.webhooksEnabled;
}

uint8_t Config::getWebhookCount() {
    return conf.webhookCount;
}

const char* Config::getWebhookIP(uint8_t index) {
    if (index < conf.webhookCount) {
        return conf.webhookIPs[index];
    }
    return nullptr;
}

uint8_t Config::getGateLEDsEnabled() {
    return conf.gateLEDsEnabled;
}

uint8_t Config::getWebhookRaceStart() {
    return conf.webhookRaceStart;
}

uint8_t Config::getWebhookRaceStop() {
    return conf.webhookRaceStop;
}

uint8_t Config::getWebhookLap() {
    return conf.webhookLap;
}

uint32_t Config::getPilotColor() {
    return conf.pilotColor;
}

char* Config::getTheme() {
    return conf.theme;
}

char* Config::getSelectedVoice() {
    return conf.selectedVoice;
}

uint8_t Config::getVoiceEnabled() {
    return conf.voiceEnabled;
}

char* Config::getLapFormat() {
    return conf.lapFormat;
}

uint8_t Config::getWifiExtAntenna() {
    return conf.wifiExtAntenna;
}

uint8_t Config::getWifiTxPower() {
    return conf.wifiTxPower;
}

uint8_t Config::getFilterMode() {
    return conf.filterMode;
}

uint8_t Config::getBesselLevel() {
    return conf.besselLevel;
}

void Config::setBesselLevel(uint8_t level) {
    if (level > 10) level = 10;
    if (conf.besselLevel != level) {
        conf.besselLevel = level;
        modified = true;
    }
}

char* Config::getPilotName() {
    return conf.pilotName;
}

uint8_t Config::getNodeMode() {
    return conf.nodeMode;
}

char* Config::getMasterSSID() {
    return conf.masterSSID;
}

char* Config::getMasterPassword() {
    return conf.masterPassword;
}

void Config::setNodeMode(uint8_t mode) {
    if (mode > 2) mode = 0;
    if (conf.nodeMode != mode) {
        conf.nodeMode = mode;
        modified = true;
    }
}

bool Config::getMnSkipMasterStart() {
    return conf.mnSkipMasterStart != 0;
}

void Config::setMnSkipMasterStart(bool skip) {
    uint8_t val = skip ? 1 : 0;
    if (conf.mnSkipMasterStart != val) {
        conf.mnSkipMasterStart = val;
        modified = true;
    }
}

uint8_t Config::getDevMode() {
    return conf.devMode;
}

void Config::setDevMode(uint8_t mode) {
    if (conf.devMode != mode) {
        conf.devMode = mode;
        modified = true;
    }
}

void Config::setBandIndex(uint8_t band) {
  if (conf.bandIndex != band) {
    conf.bandIndex = band;
    modified = true;
  }
}

void Config::setChannelIndex(uint8_t ch) {
  if (conf.channelIndex != ch) {
    conf.channelIndex = ch;
    modified = true;
  }
}

// Setters for RotorHazard node mode
void Config::setFrequency(uint16_t freq) {
    if (conf.frequency != freq) {
        conf.frequency = freq;
        modified = true;
    }
}

void Config::setEnterRssi(uint8_t rssi) {
    if (conf.enterRssi != rssi) {
        conf.enterRssi = rssi;
        modified = true;
    }
}

void Config::setExitRssi(uint8_t rssi) {
    if (conf.exitRssi != rssi) {
        conf.exitRssi = rssi;
        modified = true;
    }
}

void Config::setOperationMode(uint8_t mode) {
    if (conf.operationMode != mode) {
        conf.operationMode = mode;
        modified = true;
    }
}

void Config::setLedPreset(uint8_t preset) {
    if (conf.ledPreset != preset) {
        conf.ledPreset = preset;
        modified = true;
    }
}

void Config::setLedBrightness(uint8_t brightness) {
    if (conf.ledBrightness != brightness) {
        conf.ledBrightness = brightness;
        modified = true;
    }
}

void Config::setLedSpeed(uint8_t speed) {
    if (conf.ledSpeed != speed) {
        conf.ledSpeed = speed;
        modified = true;
    }
}

void Config::setLedColor(uint32_t color) {
    if (conf.ledColor != color) {
        conf.ledColor = color;
        modified = true;
    }
}

void Config::setLedFadeColor(uint32_t color) {
    if (conf.ledFadeColor != color) {
        conf.ledFadeColor = color;
        modified = true;
    }
}

void Config::setLedStrobeColor(uint32_t color) {
    if (conf.ledStrobeColor != color) {
        conf.ledStrobeColor = color;
        modified = true;
    }
}

void Config::setLedManualOverride(uint8_t override) {
    if (conf.ledManualOverride != override) {
        conf.ledManualOverride = override;
        modified = true;
    }
}

void Config::setTracksEnabled(uint8_t enabled) {
    if (conf.tracksEnabled != enabled) {
        conf.tracksEnabled = enabled;
        modified = true;
    }
}

void Config::setSelectedTrackId(uint32_t trackId) {
    if (conf.selectedTrackId != trackId) {
        conf.selectedTrackId = trackId;
        modified = true;
    }
}

void Config::setWebhooksEnabled(uint8_t enabled) {
    if (conf.webhooksEnabled != enabled) {
        conf.webhooksEnabled = enabled;
        modified = true;
    }
}

void Config::setVoiceEnabled(uint8_t enabled) {
    if (conf.voiceEnabled != enabled) {
        conf.voiceEnabled = enabled;
        modified = true;
    }
}

bool Config::addWebhookIP(const char* ip) {
    if (conf.webhookCount >= 10) {
        DEBUG("Max webhooks reached\n");
        return false;
    }
    
    // Check if IP already exists
    for (uint8_t i = 0; i < conf.webhookCount; i++) {
        if (strcmp(conf.webhookIPs[i], ip) == 0) {
            DEBUG("Webhook IP already exists\n");
            return false;
        }
    }
    
    strlcpy(conf.webhookIPs[conf.webhookCount], ip, 16);
    conf.webhookCount++;
    modified = true;
    return true;
}

bool Config::removeWebhookIP(const char* ip) {
    for (uint8_t i = 0; i < conf.webhookCount; i++) {
        if (strcmp(conf.webhookIPs[i], ip) == 0) {
            // Shift remaining IPs down
            for (uint8_t j = i; j < conf.webhookCount - 1; j++) {
                strlcpy(conf.webhookIPs[j], conf.webhookIPs[j + 1], 16);
            }
            conf.webhookCount--;
            memset(conf.webhookIPs[conf.webhookCount], 0, 16);  // Clear last entry
            modified = true;
            return true;
        }
    }
    return false;
}

void Config::clearWebhookIPs() {
    memset(conf.webhookIPs, 0, sizeof(conf.webhookIPs));
    conf.webhookCount = 0;
    modified = true;
}

void Config::setGateLEDsEnabled(uint8_t enabled) {
    if (conf.gateLEDsEnabled != enabled) {
        conf.gateLEDsEnabled = enabled;
        modified = true;
    }
}

void Config::setWebhookRaceStart(uint8_t enabled) {
    if (conf.webhookRaceStart != enabled) {
        conf.webhookRaceStart = enabled;
        modified = true;
    }
}

void Config::setWebhookRaceStop(uint8_t enabled) {
    if (conf.webhookRaceStop != enabled) {
        conf.webhookRaceStop = enabled;
        modified = true;
    }
}

void Config::setWebhookLap(uint8_t enabled) {
    if (conf.webhookLap != enabled) {
        conf.webhookLap = enabled;
        modified = true;
    }
}

void Config::setDefaults(void) {
    DEBUG("Setting EEPROM defaults\n");
    // Reset everything to 0/false and then just set anything that zero is not appropriate
    memset(&conf, 0, sizeof(conf));
    conf.version = CONFIG_VERSION | CONFIG_MAGIC;
    conf.bandIndex = 4;  // RaceBand
    conf.channelIndex = 0;  // Channel 1
    conf.frequency = 5658;  // RaceBand Channel 1 (R1) - 5658 MHz
    conf.minLap = 50;  // 5.0 seconds
    conf.alarm = 0;  // Alarm disabled
    conf.announcerType = 2;
    conf.announcerRate = 10;
    conf.enterRssi = 72;
    conf.exitRssi = 68;
    conf.rssiSens = 0;  // Normal sensitivity (Legacy)
    conf.maxLaps = 0;
    conf.ledMode = 3;  // Rainbow wave by default (legacy)
    conf.ledBrightness = 120;
    conf.ledColor = 14492325;  // 0xDD00A5 (purple-pink)
    conf.ledPreset = 3;  // Color fade preset by default
    conf.ledSpeed = 5;  // Medium speed
    conf.ledFadeColor = 0x0080FF;  // Blue for fade
    conf.ledStrobeColor = 0xFFFFFF;  // White for strobe
    conf.ledManualOverride = 0;  // Manual override off by default
    conf.operationMode = 0;  // WiFi mode by default
    conf.tracksEnabled = 0;  // Tracks enabled by default
    conf.selectedTrackId = 0;  // No track selected by default (will be set on first track creation)
    conf.webhooksEnabled = 0;  // Webhooks disabled by default (no IPs configured)
    conf.webhookCount = 0;  // No webhooks configured
    memset(conf.webhookIPs, 0, sizeof(conf.webhookIPs));  // Clear all webhook IPs
    conf.gateLEDsEnabled = 0;  // Gate LEDs enabled by default
    conf.webhookRaceStart = 1;  // Race start enabled by default
    conf.webhookRaceStop = 1;  // Race stop enabled by default
    conf.webhookLap = 1;  // Lap enabled by default
    strlcpy(conf.pilotName, "Pilot", sizeof(conf.pilotName));  // Default pilot name
    conf.pilotColor = 0x0080FF;  // Default blue color
    strlcpy(conf.theme, "oceanic", sizeof(conf.theme));  // Default theme
    strlcpy(conf.selectedVoice, "piper", sizeof(conf.selectedVoice));  // Default voice
    conf.voiceEnabled = 1;   // default ON 
    strlcpy(conf.lapFormat, "pilottime", sizeof(conf.lapFormat));  // Default lap format
    strlcpy(conf.ssid, "", sizeof(conf.ssid));  // Empty WiFi credentials
    strlcpy(conf.password, "", sizeof(conf.password));  // Empty WiFi credentials
    conf.wifiExtAntenna = 1;  // External antenna by default (matches hardware)
    conf.wifiTxPower = 21;    // Maximum TX power by default
    conf.filterMode = 0;          // V1 (FPVRaceOne pipeline) by default
    conf.besselLevel = 0;         // Bessel post-stage off by default; calibration wizard will recommend
    conf.nodeMode = 0;            // Single node (standalone) by default
    memset(conf.masterSSID, 0, sizeof(conf.masterSSID));
    strlcpy(conf.masterPassword, "fpvraceone", sizeof(conf.masterPassword));
    conf.mnSkipMasterStart = 0;
    conf.devMode = 0;
    modified = true;
    write();
}

void Config::handleEeprom(uint32_t currentTimeMs) {
    if (modified && ((currentTimeMs - checkTimeMs) > EEPROM_CHECK_TIME_MS)) {
        checkTimeMs = currentTimeMs;
        write();
    }
}

bool Config::saveToSD() {
    if (!storage || !storage->isSDAvailable()) {
        return false;
    }
    
    DEBUG("Saving config to SD: %s\n", CONFIG_BACKUP_PATH);
    
#ifdef ESP32S3
    File file = SD.open(CONFIG_BACKUP_PATH, FILE_WRITE);
    if (!file) {
        DEBUG("Failed to open config backup file for writing\n");
        return false;
    }
    
    size_t written = file.write((uint8_t*)&conf, sizeof(laptimer_config_t));
    file.close();
    
    if (written != sizeof(laptimer_config_t)) {
        DEBUG("Failed to write complete config (wrote %d of %d bytes)\n", written, sizeof(laptimer_config_t));
        return false;
    }
    
    DEBUG("Config saved to SD (%d bytes)\n", written);
    return true;
#else
    return false;
#endif
}

bool Config::loadFromSD() {
    if (!storage || !storage->isSDAvailable()) {
        return false;
    }
    
    DEBUG("Attempting to load config from SD: %s\n", CONFIG_BACKUP_PATH);
    
#ifdef ESP32S3
    if (!SD.exists(CONFIG_BACKUP_PATH)) {
        DEBUG("No config backup file found on SD\n");
        return false;
    }
    
    File file = SD.open(CONFIG_BACKUP_PATH, FILE_READ);
    if (!file) {
        DEBUG("Failed to open config backup file for reading\n");
        return false;
    }
    
    size_t fileSize = file.size();
    if (fileSize != sizeof(laptimer_config_t)) {
        DEBUG("Config backup file size mismatch (found %d, expected %d)\n", fileSize, sizeof(laptimer_config_t));
        file.close();
        return false;
    }
    
    laptimer_config_t temp_conf;
    size_t bytesRead = file.read((uint8_t*)&temp_conf, sizeof(laptimer_config_t));
    file.close();
    
    if (bytesRead != sizeof(laptimer_config_t)) {
        DEBUG("Failed to read complete config (read %d of %d bytes)\n", bytesRead, sizeof(laptimer_config_t));
        return false;
    }
    
    // Validate the loaded config
    uint32_t version = 0xFFFFFFFF;
    if ((temp_conf.version & CONFIG_MAGIC_MASK) == CONFIG_MAGIC) {
        version = temp_conf.version & ~CONFIG_MAGIC_MASK;
    }
    
    if (version != CONFIG_VERSION) {
        DEBUG("SD config version mismatch (found %u, expected %u)\n", version, CONFIG_VERSION);
        return false;
    }
    
    // Config is valid, use it
    memcpy(&conf, &temp_conf, sizeof(laptimer_config_t));
    DEBUG("Config loaded from SD successfully\n");
    return true;
#else
    return false;
#endif
}
