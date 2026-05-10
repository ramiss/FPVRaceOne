# FPVRaceOne Features Reference

In-depth technical documentation of all FPVRaceOne capabilities.

**Navigation:** [Home](../README.md) | [Getting Started](GETTING_STARTED.md) | [User Guide](USER_GUIDE.md)

---

## Table of Contents

1. [Connectivity](#connectivity)
2. [Signal Processing](#signal-processing)
3. [Lap Detection](#lap-detection)
4. [Calibration Wizard](#calibration-wizard)
5. [Voice Announcements](#voice-announcements)
6. [Race Analysis](#race-analysis)
7. [Race History & Data Management](#race-history--data-management)
8. [Multi-Node Racing](#multi-node-racing)
9. [Configuration Management](#configuration-management)
10. [OTA Updates](#ota-updates)
11. [Self-Test System](#self-test-system)
12. [Technical Architecture](#technical-architecture)

---

## Connectivity

### WiFi Access Point

FPVRaceOne creates its own WiFi network — no router required.

| Property | Value |
|----------|-------|
| SSID | `FPVRaceOne_XXXX` (last 4 MAC digits) |
| Security | WPA2-PSK |
| Default password | `fpvraceone` |
| IP address | `192.168.4.1` (static) |
| Band | 2.4 GHz |
| Max clients | 8 simultaneous |

No captive DNS — connected devices retain their cellular internet connection.

### USB Serial CDC

Direct connection via USB-C for zero-latency operation.

| Property | Value |
|----------|-------|
| Interface | USB Serial CDC |
| Protocol | JSON over serial |
| Latency | < 5 ms (vs 20–30 ms WiFi) |

Commands are sent as JSON objects; events are broadcast with an `EVENT:` prefix.

### Hybrid Mode

USB and WiFi operate simultaneously. A race director can control the timer via USB (low latency) while spectators view live data via WiFi.

---

## Signal Processing

A single RSSI processing pipeline based verbatim on the upstream FPVGate algorithm.

### ADC Input

- **Hardware:** Seeed XIAO ESP32-C6 12-bit ADC
- **Attenuation:** 6 dB (input range 0–1.75 V)
- **Scale:** 1 V ≈ 2340 counts; full-scale divisor = 2400
- **Output:** 0–255 (8-bit, clamped)

### Multi-Stage Pipeline

A sequential chain of five filter stages:

| Stage | Filter | Parameters |
|-------|--------|------------|
| 1 | Kalman | Process noise = 0.002, Measurement noise = 9.0 (upstream FPVGate gains) |
| 2 | Median-of-3 | Rolling 3-sample window |
| 3 | Moving average | 7-sample window |
| 4 | EMA | α = 0.03–0.50, user-tunable via Pipeline Smoothing slider (default 0.15) |
| 5 | Step limiter | Max ±12 counts per sample |

### Gate Detection

- **4-sample enter debounce** — consecutive samples at or above Enter RSSI before peak tracking starts
- **2-sample raw exit confirm** — direct comparison against the unsmoothed sample buffer to avoid slow-falling smoothed signals masking the exit
- **Peak must exceed exit by ≥5 counts** to be considered a valid lap
- **Ceiling-drift watchdog** — if "in gate" for >3 s without an exit, state is force-reset (the antenna RSSI must have drifted up to enter)

### Optional Gate-1 Bootstrap

When enabled, the first lap of a race is special-cased so a drone already inside the gate at race start still produces a clean first lap:

- Relaxed effective enter threshold (~exit + 4 counts)
- Lower 2-sample debounce
- Lower 3-count peak-above-exit margin
- If RSSI at race start is already ≥ enter, the peak is seeded so the first confirmed exit counts as Gate 1

---

## Lap Detection

### State Machine

```
IDLE
 │ (RSSI > Enter for N hold samples)
 ▼
ENTERED — tracking peak
 │ (RSSI falls below Exit for M confirm samples)
 ▼
LAP CONFIRMED → record timestamp, trigger events
 │
COOLDOWN (minimum lap time)
 │
IDLE
```

### Timing Accuracy

| Factor | Impact | Notes |
|--------|--------|-------|
| RSSI sampling rate | ~10 ms | 100 Hz loop |
| Peak detection | ±10 ms | Per-sample resolution |
| System timer | < 1 ms | `millis()` |
| Lap timestamp | At peak sample | Recorded at the sample where the peak was reached |

### Minimum Lap Time

Laps triggered within the minimum lap time are silently ignored. This prevents double-counting when the drone lingers near the gate or crashes close to it.

---

## Calibration Wizard

### Recording Phase

- Firmware stores up to 5000 RSSI samples with millisecond timestamps in a dedicated calibration buffer
- The wizard records the **final pipeline output** (post Kalman → Median → MA → EMA → step limiter) — what the lap detector actually sees
- One sample every 20 ms (~50 Hz)
- Multiple fly-over passes can be recorded in a single session, but **3 passes is the sweet spot** for the threshold calculator

### Analysis Phase

After recording stops, the full dataset is streamed to the browser in pages (500 samples/page) and rendered on a chart.

The wizard auto-detects the three highest peaks and overlays markers; any marker can be dragged manually to correct a missed detection.

**Threshold calculation:**

| Threshold | Formula | Reason |
|-----------|---------|--------|
| Enter RSSI | `round(0.95 × min(peakA, peakB, peakC))` | 5 % headroom keyed to the *weakest* peak so even the weakest pass triggers |
| Exit RSSI | `Enter − 7`, raised above noise floor | Tight gap for tiny-whoop tracks; never below 35th-percentile noise + 3 |

Both values are clamped to `[30, 255]` and Exit is forced ≥ 5 below Enter as a fallback hysteresis.

### Peak-Spread Warning

If the three peaks differ in height by more than ~15 % (weakest / strongest < 0.85), the wizard surfaces a non-blocking warning recommending a re-fly. False positives are cheap (the user can ignore and continue); false negatives leave the pilot with sub-optimal calibration without ever knowing.

### Overview Mode

- After recording, the calibration chart replaces the live scanner with the full recorded dataset
- The live scanner can be restored at any time by exiting overview mode
- The live signal can be **paused** to examine a specific moment; Enter/Exit lines move in real time as sliders are adjusted even while paused

---

## Voice Announcements

### TTS Engines

#### ElevenLabs (Pre-recorded)

High-quality neural TTS pre-generated as MP3 files stored in LittleFS.

**Voices:** Sarah (energetic), Rachel (calm), Adam (deep male), Antoni (friendly male)

**Fallback:** If audio files are missing or unreadable, the system automatically falls back to PiperTTS.

#### PiperTTS (Real-time)

Fast lightweight neural synthesis — no pre-recorded files required.

- ~200 ms faster than ElevenLabs (no file I/O)
- Slightly more synthetic character
- Suitable when LittleFS space is limited or for custom pilot names

### Announcement Formats

| Format | Example |
|--------|---------|
| Full (name + lap + time) | "Louis Lap 5, 12.34" |
| Lap + Time | "Lap 5, 12.34" |
| Time Only | "12.34" |

### Announcer Types

| Type | Behaviour |
|------|-----------|
| None | Silent — only buzzer for race start |
| Beep | Short beep per lap, no speech |
| Lap Time | Announces every lap (recommended) |
| 2 Consecutive | Announces every 2 laps as a pair |
| 3 Consecutive | Announces every 3 laps (RaceGOW format) |

### Phonetic Name

If the Phonetic Name field is populated, it is used in place of the Pilot Name for TTS. This allows correct pronunciation of non-phonetic names without affecting the display name.

---

## Race Analysis

### Statistics

| Metric | Calculation |
|--------|-------------|
| Fastest Lap | Minimum lap time, excluding Gate 1 |
| Median Lap | Middle value of sorted lap times |
| Best 3 Laps | Sum of 3 lowest individual lap times |
| Fastest 3 Consecutive | Minimum sum of any 3 back-to-back laps |

### Charts

- **Lap History** — Horizontal bars for last 10 laps; bar width proportional to time; each lap a distinct colour
- **Fastest Round** — Vertical bars for the 3-consecutive laps with the best combined time

The fastest lap row is highlighted gold in the lap table.

---

## Race History & Data Management

### Automatic Saving

Races are saved automatically when:
- "Stop Race" is clicked
- "Clear Laps" is clicked (if laps exist)
- Max lap count is reached (auto-stop)

### Storage

Races are stored as individual JSON files in LittleFS under `/races/race_<timestamp>.json` with an `/races/races_index.json` manifest. They persist across reboots and firmware updates as long as the filesystem partition isn't erased.

### JSON Format

```json
{
  "id": "race_1703001234567",
  "timestamp": "2024-12-04T14:30:45Z",
  "pilot": { "name": "Pilot Name", "callsign": "Callsign", "color": "#0080FF" },
  "frequency": { "band": "R", "channel": 1, "mhz": 5658 },
  "laps": [
    { "number": 0, "time": 8450, "name": "Gate 1" },
    { "number": 1, "time": 12340, "name": "Lap 1" }
  ],
  "stats": {
    "fastest_lap": 12340,
    "fastest_lap_number": 1,
    "median": 12340,
    "best_3_sum": 0,
    "best_consecutive_3": 0
  }
}
```

### Marshalling Mode

Laps can be added or removed from any saved race after the fact. All statistics and charts recalculate immediately. Changes are saved automatically.

### Export / Import

- Export individual races or full history as JSON
- Import merges races by ID — existing races are not overwritten

---

## Multi-Node Racing

Network up to **8 FPVRaceOne devices** together — one **Master** + up to seven **Clients** — without a router. The master runs an Access Point that every client joins; lap events stream back over plain HTTP and Server-Sent Events.

### Roles

| Role | Behaviour |
|------|-----------|
| **Single** (default) | Standalone — no inter-device traffic |
| **Master** | Race director. Hosts the WiFi AP that clients join; renders the multi-node Race tab; broadcasts Start All / Stop All |
| **Client** | Joins the master's AP. Presents the same race UI as standalone, plus a card on the master with live state |

Switching roles requires a reboot — the **Apply Multi-Node & Reboot** button only enables when Node Mode actually changes, so other multi-node settings can be tweaked without forcing a restart.

### Discovery

Clients can scan for available masters in range (`mnScanNetworks()`): every `FPVRaceOne_*` AP within signal is listed and tappable to autofill the Master SSID field.

### Master Race Tab

Each connected client appears as a card showing pilot name, running indicator, lap count, and last lap time. State updates stream from clients via 1 Hz heartbeats and are pushed to the browser as Server-Sent Events (`multiNodeState` event).

| Indicator | Meaning |
|-----------|---------|
| ● **Running** | Client's lap timer is running |
| ○ **Stopped** | Client is idle |
| ⚠ **DNF** | Client pressed Stop locally during a master-broadcast race (early quit) |

### Start / Stop Broadcast

- **Start All** sends `POST /timer/masterStart` to every online client in parallel — sequential calls in firmware land within ~350 ms of each other (acceptable for current head-to-head precision; can be moved to true-parallel HTTP if tighter sync is needed later)
- **Stop All** sends `POST /timer/masterStop`
- Each client decides what to do with the broadcast based on its own *Ignore Race Director Start/Stop if already racing* toggle (default off → honour broadcast)
- The client pushes `masterRaceState` SSE to its browser so the UI flips into race-running state — Start button looks pressed, Stop becomes the active button — same as if the pilot had pressed Start locally

### Early Quit Notification

When a client's pilot presses **Stop** locally during an active master-broadcast race, the client posts `/api/multinode/quit` to the master. The master sets `quitEarly = true` on that node and the card flips to **DNF** until the next master-broadcast Start All.

This is what lets a director keep a heat running cleanly when one pilot crashes or batteries out — they tap Stop on their own device, the rest of the field continues racing without interruption, and the director sees exactly who DNF'd.

### Solo-Practice Override

Each client has an **Ignore Race Director Start/Stop if already racing** toggle (`mnSkipMasterStart` in config). When on:

- A master Start All broadcast is silently ignored if the client's timer is already running locally
- The pilot can keep practising while a director runs heats on the rest of the field
- Toggle off to rejoin the directed race format

---

## Configuration Management

### Storage

All device configuration is stored in EEPROM with a version stamp. When the version doesn't match (e.g. after a firmware update that adds new fields), the device loads defaults.

A backup is also written to LittleFS which can be restored if EEPROM is invalidated.

### Versioning

`CONFIG_VERSION` is incremented whenever the config struct changes. When the stored version doesn't match firmware expectation, the device loads defaults (no destructive migration). The current version is defined in `lib/CONFIG/config.h`.

### Settings Save Behaviour

| Setting | How it saves |
|---------|-------------|
| Band / Channel | Auto-saved on selection |
| RSSI thresholds (sliders) | Staged; saved by "Save RSSI Thresholds" button |
| All Settings tab fields | Staged; saved by "Save Configuration" button |
| Calibration thresholds | Saved immediately by wizard "Apply" button |
| WiFi credentials | Saved by "Apply WiFi & Reboot" button |

The **Save Configuration** button highlights orange with a pulse animation when there are unsaved staged changes. It returns to its inactive (greyed-out) state after a successful save.

### Config Fields

Key config fields stored per device:

- Band, channel, frequency
- Enter RSSI, Exit RSSI
- Min lap time, max laps
- Announcer type, rate, voice, lap format, phonetic name
- Pilot name, callsign, colour
- LED preset, brightness, speed, colours, manual override
- WiFi (AP + home) SSID, password, external antenna, TX power
- RSSI sensitivity
- Pipeline Smoothing level (`v1Smoothing`), Gate-1 Bootstrap toggle
- Selected track ID
- Multi-node: node mode, master SSID, skip-master-start toggle
- Theme, selected voice

---

## OTA Updates

The device pulls release artefacts directly from GitHub Releases. There is no PlatformIO requirement and no manual upload step for normal updates.

### End-User Flow

1. **Settings → Firmware Update** — enter Home WiFi credentials (saved permanently after the first time)
2. Tap **Check for Updates** — the device briefly drops the AP, joins the home network, hits `https://api.github.com/repos/<owner>/<repo>/releases/latest`, and reads the asset list
3. If the published `tag_name` is newer than the installed `firmware_version.h` build, the user is prompted with the version + release notes
4. Tap **Update Now** — the device downloads `FPVRaceOne-littlefs.bin`, flashes the LittleFS partition, then downloads `FPVRaceOne-firmware.bin`, flashes the OTA partition, and reboots once
5. If any download fails, the previous firmware/filesystem is preserved — the device cannot be bricked from a flaky network

### Release Asset Naming

The CI workflow produces two binaries per release:

- `FPVRaceOne-firmware.bin` — application image (flashed to OTA_0/1)
- `FPVRaceOne-littlefs.bin` — filesystem image (flashed to LittleFS)

### Manual Flashing (Recovery)

For first-time setup of new hardware or recovery if the device won't boot, `pio run -e seeed_xiao_esp32c6 -t upload` and `-t uploadfs` are still supported. See [FLASHING_OPTIONAL.md](FLASHING_OPTIONAL.md).

### Race-Time Safety

Updates are blocked while a race is running. Attempting to start an update during a race returns an error.

---

## Self-Test System

Access via **Settings → Diagnostics → Run All Tests**.

The self-test exercises hardware, storage, and software paths and reports pass/fail with detail text. Results can be downloaded as a diagnostic log to attach to a GitHub issue.

Categories covered include the RX5808 SPI driver, EEPROM, LittleFS, WiFi AP / station, USB CDC transport, lap timer, race history, web server file presence, and OTA partition health.

---

## Technical Architecture

### Firmware Structure

```
src/
└── main.cpp              — Entry point; sampling loop on Core 1, WiFi/HTTP on Core 0

lib/
├── CONFIG/               — Configuration struct, EEPROM, serialisation
├── DEBUG/                — Debug logger (web-streamed, no hot-path delays)
├── KALMAN/               — Kalman filter
├── LAPTIMER/             — Single 5-stage filter pipeline + gate state machine + lap events
├── RX5808/               — RX5808 SPI driver and ADC read
├── MULTINODE/            — Master/client coordination (heartbeats, broadcasts, quit notifications)
├── RACEHISTORY/          — Per-race JSON storage with index file
├── TRACKMANAGER/         — Track CRUD + distance integration
├── WEBSERVER/            — ESPAsyncWebServer HTTP + SSE
└── ...
```

### Web Interface

```
data/
├── index.html            — Single-page application
├── style.css             — Responsive styles, multiple themes, save-button dirty state
├── script.js             — UI logic, staged config, race control, calibration, multi-node
└── usb-transport.js      — USB Serial CDC communication layer
```

### Two-Core Architecture

| Core | Responsibilities |
|------|-----------------|
| Core 1 (main loop) | RSSI sampling, full filter pipeline, gate detection, lap events, audio |
| Core 0 (parallel task) | WiFi stack, HTTP server, SSE streaming, multi-node POSTs, OTA download |

Network calls run on Core 0 specifically so they never stall RSSI reads during lap events.

### SSE Streaming

The browser receives several streams of Server-Sent Events:
- Live RSSI samples (high-frequency)
- Race state changes (`raceState`, `masterRaceState`)
- Multi-node node updates (`multiNodeState`)
- Debug log lines (when the diagnostics tab is open)

SSE is one-way (server → client) and avoids WebSocket overhead for the high-frequency RSSI feed.

### Memory Layout (XIAO ESP32-C6)

| Partition | Size | Contents |
|-----------|------|----------|
| Bootloader | ~32 KB | ESP32 bootloader |
| Partition table | 4 KB | Partition layout |
| OTA_0 (App0) | 2 MB | Primary firmware |
| OTA_1 (App1) | 2 MB | OTA update staging |
| LittleFS | ~1.5 MB | Web files, race history, audio |

---

**Questions or issues? [Open a GitHub issue](https://github.com/ramiss/FPVRaceOne/issues)**
