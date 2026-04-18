#include "racehistory.h"
#include <algorithm>
#include <time.h>
#include "debug.h"

RaceHistory::RaceHistory() : storage(nullptr) {
}

bool RaceHistory::isPersistenceEnabled() const {
    #ifdef PIN_SD_CS
        return true;
    #else
        return false;
    #endif
}

bool RaceHistory::init(Storage* storageBackend) {
    storage = storageBackend;

    #ifndef PIN_SD_CS
        // No SD support on this build: keep only the current race in RAM and do not persist to flash.
        races.clear();
        return true;
    #endif

    if (!storage) {
        DEBUG("RaceHistory: Storage backend is null!\n");
        return false;
    }
    
    // Create races directory if it doesn't exist
    storage->mkdir("/races");
    
    return loadRaces();
}

bool RaceHistory::saveRace(const RaceSession& race) {
    #ifndef PIN_SD_CS
        // RAM-only mode: keep just the most recent race in memory.
        races.clear();
        races.push_back(race);
        return true;
    #endif
    
    
    // Generate filename from timestamp: DDMMYY-HrMinSec.json
    time_t timestamp = race.timestamp;
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);
    
    char filename[32];
    strftime(filename, sizeof(filename), "%d%m%y-%H%M%S.json", &timeinfo);
    String filepath = String(RACES_DIR) + "/" + String(filename);
    
    // Create JSON for single race
    DynamicJsonDocument doc(16384);
    JsonObject raceObj = doc.to<JsonObject>();
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
    raceObj["trackId"] = race.trackId;
    raceObj["trackName"] = race.trackName;
    raceObj["totalDistance"] = race.totalDistance;
    
    JsonArray lapsArray = raceObj.createNestedArray("lapTimes");
    for (uint32_t lap : race.lapTimes) {
        lapsArray.add(lap);
    }
    
    String json;
    serializeJson(doc, json);
    
    DEBUG("Saving race JSON: totalDistance=%.2f\n", race.totalDistance);
    DEBUG("JSON content: %s\n", json.c_str());
    
    bool success = storage->writeFile(filepath, json);
    if (success) {
        DEBUG("Saved race to %s (%d bytes)\n", filepath.c_str(), json.length());
        
        // Add to in-memory list
        races.insert(races.begin(), race);
        if (races.size() > MAX_RACES) {
            races.resize(MAX_RACES);
        }
    } else {
        DEBUG("Failed to save race to %s\n", filepath.c_str());
    }
    
    return success;
}

bool RaceHistory::loadRaces() {
    #ifndef PIN_SD_CS
        // RAM-only mode: nothing to load from storage.
        races.clear();
        return true;
    #endif
    
    if (!storage) {
        DEBUG("RaceHistory: Storage backend is null!\n");
        return false;
    }
    
    races.clear();
    
    // List all JSON files in races directory
    std::vector<String> files;
    if (!storage->listDir(RACES_DIR, files)) {
        DEBUG("Races directory does not exist or is empty\n");
        return true;
    }
    
    // Load each race file
    for (const String& filename : files) {
        if (!filename.endsWith(".json")) {
            continue;
        }
        
        String filepath = String(RACES_DIR) + "/" + filename;
        String json;
        if (!storage->readFile(filepath, json)) {
            DEBUG("Failed to read %s\n", filepath.c_str());
            continue;
        }
        
        DynamicJsonDocument doc(16384);
        DeserializationError error = deserializeJson(doc, json);
        if (error) {
            DEBUG("Failed to parse %s: %s\n", filepath.c_str(), error.c_str());
            continue;
        }
        
        RaceSession race;
        race.timestamp = doc["timestamp"];
        race.fastestLap = doc["fastestLap"];
        race.medianLap = doc["medianLap"];
        race.best3LapsTotal = doc["best3LapsTotal"];
        race.name = doc["name"] | "";
        race.tag = doc["tag"] | "";
        { String pn = doc["pilotName"] | ""; race.pilotName = pn.length() ? pn : (doc["pilotCallsign"] | ""); }
        race.frequency = doc["frequency"] | 0;
        race.band = doc["band"] | "";
        race.channel = doc["channel"] | 0;
        race.trackId = doc["trackId"] | 0;
        race.trackName = doc["trackName"] | "";
        race.totalDistance = doc["totalDistance"] | 0.0f;
        
        JsonArray lapsArray = doc["lapTimes"];
        for (uint32_t lap : lapsArray) {
            race.lapTimes.push_back(lap);
        }
        
        races.push_back(race);
    }
    
    // Sort by timestamp (newest first)
    std::sort(races.begin(), races.end(), 
        [](const RaceSession& a, const RaceSession& b) { return a.timestamp > b.timestamp; });
    
    // Keep only MAX_RACES
    if (races.size() > MAX_RACES) {
        races.resize(MAX_RACES);
    }
    
    DEBUG("Loaded %d races from individual files\n", races.size());
    return true;
}

bool RaceHistory::deleteRace(uint32_t timestamp) {
    #ifndef PIN_SD_CS
        auto ramIt = std::find_if(races.begin(), races.end(),
            [timestamp](const RaceSession& r) { return r.timestamp == timestamp; });
        if (ramIt != races.end()) {
            races.erase(ramIt);
            return true;
        }
        return false;
    #endif    
    
    // Find and delete the file
    time_t ts = timestamp;
    struct tm timeinfo;
    localtime_r(&ts, &timeinfo);
    
    char filename[32];
    strftime(filename, sizeof(filename), "%d%m%y-%H%M%S.json", &timeinfo);
    String filepath = String(RACES_DIR) + "/" + String(filename);
    
    bool fileDeleted = storage->deleteFile(filepath);
    
    // Remove from in-memory list
    auto it = std::remove_if(races.begin(), races.end(),
        [timestamp](const RaceSession& r) { return r.timestamp == timestamp; });
    
    if (it != races.end()) {
        races.erase(it, races.end());
        return fileDeleted;
    }
    
    return false;
}

bool RaceHistory::updateRace(uint32_t timestamp, const String& name, const String& tag, float totalDistance) {
    #ifndef PIN_SD_CS
        // RAM-only mode: update in-memory entry only.
        for (auto& race : races) {
            if (race.timestamp == timestamp) {
                race.name = name;
                race.tag = tag;
                if (totalDistance >= 0.0f) {
                    race.totalDistance = totalDistance;
                }
                return true;
            }
        }
        return false;
    #endif
    
    // Update in-memory race
    RaceSession* targetRace = nullptr;
    for (auto& race : races) {
        if (race.timestamp == timestamp) {
            race.name = name;
            race.tag = tag;
            if (totalDistance >= 0.0f) {
                race.totalDistance = totalDistance;
            }
            targetRace = &race;
            break;
        }
    }
    
    if (!targetRace) {
        return false;
    }
    
    // Regenerate file with updated data
    time_t ts = timestamp;
    struct tm timeinfo;
    localtime_r(&ts, &timeinfo);
    
    char filename[32];
    strftime(filename, sizeof(filename), "%d%m%y-%H%M%S.json", &timeinfo);
    String filepath = String(RACES_DIR) + "/" + String(filename);
    
    // Create JSON
    DynamicJsonDocument doc(16384);
    JsonObject raceObj = doc.to<JsonObject>();
    raceObj["timestamp"] = targetRace->timestamp;
    raceObj["fastestLap"] = targetRace->fastestLap;
    raceObj["medianLap"] = targetRace->medianLap;
    raceObj["best3LapsTotal"] = targetRace->best3LapsTotal;
    raceObj["name"] = targetRace->name;
    raceObj["tag"] = targetRace->tag;
    raceObj["pilotName"] = targetRace->pilotName;
    raceObj["frequency"] = targetRace->frequency;
    raceObj["band"] = targetRace->band;
    raceObj["channel"] = targetRace->channel;
    raceObj["trackId"] = targetRace->trackId;
    raceObj["trackName"] = targetRace->trackName;
    raceObj["totalDistance"] = targetRace->totalDistance;
    
    JsonArray lapsArray = raceObj.createNestedArray("lapTimes");
    for (uint32_t lap : targetRace->lapTimes) {
        lapsArray.add(lap);
    }
    
    String json;
    serializeJson(doc, json);
    
    return storage->writeFile(filepath, json);
}

bool RaceHistory::updateLaps(uint32_t timestamp, const std::vector<uint32_t>& newLapTimes) {
    #ifndef PIN_SD_CS
        // RAM-only mode: update in-memory entry only.
        for (auto& race : races) {
            if (race.timestamp == timestamp) {
                race.lapTimes = newLapTimes;
                // (Optional) recompute fastest/median/best3 here if you want.
                return true;
            }
        }
        return false;
    #endif
    
    // Validate lap times
    if (newLapTimes.empty()) {
        DEBUG("Cannot update race with empty lap times\n");
        return false;
    }
    
    // Find the race in memory
    RaceSession* targetRace = nullptr;
    for (auto& race : races) {
        if (race.timestamp == timestamp) {
            targetRace = &race;
            break;
        }
    }
    
    if (!targetRace) {
        DEBUG("Race with timestamp %u not found\n", timestamp);
        return false;
    }
    
    // Update lap times
    targetRace->lapTimes = newLapTimes;
    
    // Recalculate statistics
    // Fastest lap
    targetRace->fastestLap = *std::min_element(newLapTimes.begin(), newLapTimes.end());
    
    // Median lap
    std::vector<uint32_t> sorted = newLapTimes;
    std::sort(sorted.begin(), sorted.end());
    size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        targetRace->medianLap = (sorted[mid - 1] + sorted[mid]) / 2;
    } else {
        targetRace->medianLap = sorted[mid];
    }
    
    // Best 3 laps total
    if (newLapTimes.size() >= 3) {
        targetRace->best3LapsTotal = sorted[0] + sorted[1] + sorted[2];
    } else {
        targetRace->best3LapsTotal = 0;
        for (uint32_t lap : sorted) {
            targetRace->best3LapsTotal += lap;
        }
    }
    
    // Write updated race to file
    time_t ts = timestamp;
    struct tm timeinfo;
    localtime_r(&ts, &timeinfo);
    
    char filename[32];
    strftime(filename, sizeof(filename), "%d%m%y-%H%M%S.json", &timeinfo);
    String filepath = String(RACES_DIR) + "/" + String(filename);
    
    // Create JSON
    DynamicJsonDocument doc(16384);
    JsonObject raceObj = doc.to<JsonObject>();
    raceObj["timestamp"] = targetRace->timestamp;
    raceObj["fastestLap"] = targetRace->fastestLap;
    raceObj["medianLap"] = targetRace->medianLap;
    raceObj["best3LapsTotal"] = targetRace->best3LapsTotal;
    raceObj["name"] = targetRace->name;
    raceObj["tag"] = targetRace->tag;
    raceObj["pilotName"] = targetRace->pilotName;
    raceObj["frequency"] = targetRace->frequency;
    raceObj["band"] = targetRace->band;
    raceObj["channel"] = targetRace->channel;
    raceObj["trackId"] = targetRace->trackId;
    raceObj["trackName"] = targetRace->trackName;
    raceObj["totalDistance"] = targetRace->totalDistance;
    
    JsonArray lapsArray = raceObj.createNestedArray("lapTimes");
    for (uint32_t lap : targetRace->lapTimes) {
        lapsArray.add(lap);
    }
    
    String json;
    serializeJson(doc, json);
    
    bool success = storage->writeFile(filepath, json);
    if (success) {
        DEBUG("Updated laps for race %u\n", timestamp);
    }
    return success;
}

bool RaceHistory::clearAll() {
    #ifndef PIN_SD_CS
        // RAM-only mode: just clear memory.
        races.clear();
        return true;
    #endif
    
    // Delete all race files
    std::vector<String> files;
    if (storage->listDir(RACES_DIR, files)) {
        for (const String& filename : files) {
            if (filename.endsWith(".json")) {
                String filepath = String(RACES_DIR) + "/" + filename;
                storage->deleteFile(filepath);
            }
        }
    }
    
    races.clear();
    return true;
}

String RaceHistory::toJsonString() {
    DynamicJsonDocument doc(32768);
    JsonArray racesArray = doc.createNestedArray("races");
    
    doc["persistent"] = isPersistenceEnabled();
    doc["storage"] = isPersistenceEnabled() ? "sd" : "ram";

    for (const auto& race : races) {
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
        raceObj["trackId"] = race.trackId;
        raceObj["trackName"] = race.trackName;
        raceObj["totalDistance"] = race.totalDistance;
        
        JsonArray lapsArray = raceObj.createNestedArray("lapTimes");
        for (uint32_t lap : race.lapTimes) {
            lapsArray.add(lap);
        }
    }
    
    String output;
    serializeJson(doc, output);
    return output;
}

bool RaceHistory::fromJsonString(const String& json) {
    DynamicJsonDocument doc(32768);
    DeserializationError error = deserializeJson(doc, json);
    
    if (error) {
        DEBUG("Failed to parse races JSON: %s\n", error.c_str());
        return false;
    }
    
    // Import races from JSON array
    JsonArray racesArray = doc["races"];
    int importedCount = 0;

    #ifndef PIN_SD_CS
        // RAM-only mode: load just the first race into memory ("current race").
        if (racesArray.isNull() || racesArray.size() == 0) {
            races.clear();
            return true;
        }
        {
            JsonObject raceObj = racesArray[0];
            RaceSession race;
            race.timestamp = raceObj["timestamp"] | 0;
            race.fastestLap = raceObj["fastestLap"] | 0;
            race.medianLap = raceObj["medianLap"] | 0;
            race.best3LapsTotal = raceObj["best3LapsTotal"] | 0;
            race.name = (const char*)(raceObj["name"] | "");
            race.tag = (const char*)(raceObj["tag"] | "");
            { const char* pn = raceObj["pilotName"] | ""; race.pilotName = strlen(pn) ? pn : (const char*)(raceObj["pilotCallsign"] | ""); }
            race.frequency = raceObj["frequency"] | 0;
            race.band = (const char*)(raceObj["band"] | "");
            race.channel = raceObj["channel"] | 0;
            race.trackId = raceObj["trackId"] | 0;
            race.trackName = (const char*)(raceObj["trackName"] | "");
            race.totalDistance = raceObj["totalDistance"] | 0.0f;

            race.lapTimes.clear();
            if (raceObj.containsKey("lapTimes")) {
                for (JsonVariant lap : raceObj["lapTimes"].as<JsonArray>()) {
                    race.lapTimes.push_back(lap.as<uint32_t>());
                }
            }

            races.clear();
            races.push_back(race);
        }
        return true;
    #endif
    
    for (JsonObject raceObj : racesArray) {
        RaceSession race;
        race.timestamp = raceObj["timestamp"];
        race.fastestLap = raceObj["fastestLap"];
        race.medianLap = raceObj["medianLap"];
        race.best3LapsTotal = raceObj["best3LapsTotal"];
        race.name = raceObj["name"] | "";
        race.tag = raceObj["tag"] | "";
        { const char* pn = raceObj["pilotName"] | ""; race.pilotName = strlen(pn) ? pn : (const char*)(raceObj["pilotCallsign"] | ""); }
        race.frequency = raceObj["frequency"] | 0;
        race.band = raceObj["band"] | "";
        race.channel = raceObj["channel"] | 0;
        race.trackId = raceObj["trackId"] | 0;
        race.trackName = raceObj["trackName"] | "";
        race.totalDistance = raceObj["totalDistance"] | 0.0f;
        
        JsonArray lapsArray = raceObj["lapTimes"];
        for (uint32_t lap : lapsArray) {
            race.lapTimes.push_back(lap);
        }
        
        // Check if race with this timestamp already exists
        bool exists = false;
        for (const auto& existingRace : races) {
            if (existingRace.timestamp == race.timestamp) {
                exists = true;
                break;
            }
        }
        
        // Save to individual file if it doesn't exist
        if (!exists && saveRace(race)) {
            importedCount++;
        }
    }
    
    // Reload all races to update in-memory list
    loadRaces();
    
    DEBUG("Imported %d races\n", importedCount);
    return true;
}

