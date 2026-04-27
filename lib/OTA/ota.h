#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

class Config;
class LapTimer;
class AsyncEventSource;

// Drives the GitHub-based OTA update flow.
//
// Two phases, both initiated from the web UI:
//   1. Check  — connects to the user's home WiFi (STA, alongside the AP),
//               queries the GitHub Releases API for the latest tag, and reports
//               whether an update is available.  Caller then prompts the user.
//   2. Apply  — re-connects to home WiFi, streams the LittleFS image into the
//               SPIFFS partition, then streams the firmware image into the
//               inactive OTA app slot.  Reboots once at the end.
//
// All state-changes emit `updateProgress` SSE events so the UI can render a
// progress bar without polling.  `loop()` must be called from the parallel
// task; the actual download work runs there (blocking) when `requestApply()`
// has been called.
class OtaManager {
public:
    enum State {
        STATE_IDLE              = 0,
        STATE_CONNECTING        = 1,  // associating with home WiFi
        STATE_CHECKING          = 2,  // hitting GitHub API
        STATE_UPDATE_AVAILABLE  = 3,  // newer release found, waiting on user
        STATE_UP_TO_DATE        = 4,  // already on latest
        STATE_DOWNLOADING_FS    = 5,  // streaming littlefs.bin into SPIFFS partition
        STATE_DOWNLOADING_FW    = 6,  // streaming firmware.bin into OTA app partition
        STATE_REBOOTING         = 7,  // both flashes complete; about to ESP.restart()
        STATE_ERROR             = 99
    };

    struct UpdateInfo {
        String currentVersion;   // FIRMWARE_VERSION
        String latestVersion;    // tag_name from GitHub
        bool   available;        // currentVersion < latestVersion
        String releaseNotes;     // release body (markdown, may be empty)
        String firmwareUrl;      // browser_download_url for firmware asset
        String filesystemUrl;    // browser_download_url for littlefs asset
    };

    void init(Config* config, LapTimer* timer, AsyncEventSource* events);
    void loop();  // call from parallel task — drains pending apply work

    // Synchronous: connects, queries GitHub, fills `out`.
    // Returns false on error and sets `errorMessage`.
    bool checkForUpdate(UpdateInfo& out, String& errorMessage);

    // Schedules the apply phase.  Returns immediately; actual work happens in
    // loop().  Caller should have already received URLs from a check.
    void requestApply(const String& fwUrl, const String& fsUrl);

    State   getState()           const { return _state; }
    int     getProgressPercent() const { return _progressPercent; }
    String  getStatusMessage()   const { return _statusMessage; }
    bool    isInProgress()       const {
        return _state == STATE_CONNECTING || _state == STATE_CHECKING ||
               _state == STATE_DOWNLOADING_FS || _state == STATE_DOWNLOADING_FW ||
               _state == STATE_REBOOTING;
    }

private:
    Config*           _config = nullptr;
    LapTimer*         _timer  = nullptr;
    AsyncEventSource* _events = nullptr;

    State   _state           = STATE_IDLE;
    int     _progressPercent = 0;
    String  _statusMessage;

    // Pending apply request — set by requestApply(), consumed by loop().
    bool    _pendingApply = false;
    String  _pendingFwUrl;
    String  _pendingFsUrl;

    bool connectToHomeWifi(uint32_t timeoutMs, String& errorMessage);
    void disconnectFromHomeWifi();
    bool downloadAndFlash(const String& url, int target, const char* label, String& errorMessage);

    void setState(State s, const String& msg);
    void setProgress(int percent);
    void emitProgress();
};

extern OtaManager otaManager;

#endif // OTA_H
