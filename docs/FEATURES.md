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
| IP address | `192.168.4.1` (Single / Client), `192.168.5.1` (Master) |
| Band | 2.4 GHz |
| Max clients | 9 simultaneous |
| AP inactivity timeout | 60 s — a silent station drops within a minute so its slot frees up |

No captive DNS — connected devices retain their cellular internet connection on most platforms (Samsung devices block this and force a choice).

The USB-C connector on the device is used for **power and flashing only** — there is no user-facing USB control protocol in the current firmware.

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
| Exit RSSI | `Enter − 4`, raised above noise floor | Tight hysteresis tuned for close-pattern tracks; never below 35th-percentile noise + 3 |

Both values are clamped to `[30, 255]` and Exit is forced ≥ 4 below Enter as a fallback hysteresis.

### Peak-Spread Warning

If the three peaks differ in height by more than ~15 % (weakest / strongest < 0.85), the wizard surfaces a non-blocking warning recommending a re-fly. False positives are cheap (the user can ignore and continue); false negatives leave the pilot with sub-optimal calibration without ever knowing.

### Overview Mode

- After recording, the calibration chart replaces the live scanner with the full recorded dataset
- The live scanner can be restored at any time by exiting overview mode
- The live signal can be **paused** to examine a specific moment; Enter/Exit lines move in real time as sliders are adjusted even while paused

---

## Voice Announcements

Lap announcements are spoken by the **browser** that's logged into the web UI, not by the device. The web app uses the standard **Web Speech API** (`SpeechSynthesisUtterance`) so the actual voice quality and accent depends on the OS the browser is running on — modern Chrome/Edge/Safari all ship a usable English voice.

Implication: whichever device has the web UI open with "Enable Voice" turned on is the speaker. The race director's laptop is usually the right choice; pilot phones can stay silent so the gate doesn't sound like a chorus.

### Announcement Formats

| Format | Example |
|--------|---------|
| Pilot + Lap + Time | "Louis Lap 5, 12.34" |
| Pilot + Time | "Louis, 12.34" |
| Lap + Time | "Lap 5, 12.34" |
| Time Only | "12.34" |

### Announcer Types

| Type | Behaviour |
|------|-----------|
| None | Silent — only the firmware-driven start/stop beeps |
| Beep | Short beep on each lap, no speech |
| Lap Time | Speaks every lap |
| 2 Consecutive | Speaks the combined time of the last two laps |
| 3 Consecutive | Speaks the combined time of the last three laps (RaceGOW format) |

### Phonetic Name

If the Phonetic Name field is populated, it is used in place of the Pilot Name for the announcement. This lets you get correct pronunciation of non-phonetic spellings ("Louie" for "Louis", "Ree-shar" for "Richard") without changing the display name shown elsewhere.

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

### Automatic Capture

Races are added to the in-memory log when:
- **Stop Race** is clicked
- **Clear Laps** is clicked (if laps exist)
- The Max-laps auto-stop fires

### Storage

Race history on the current hardware is **session-scoped (RAM-only)**. The XIAO ESP32-C6 build does not reserve a LittleFS slot for race storage — saved laps live in the web UI's state and a parallel in-firmware buffer, both of which clear on reboot.

The web UI surfaces this with a yellow banner reminding the user to **download** the session JSON before unplugging. Once downloaded, the JSON file is the durable record; re-import to view or re-edit later.

A persistent-storage path exists in the firmware code (`RaceHistory::saveRace`, `/races/race_<id>.json` layout) and is exercised by builds that have either an SD card or a dedicated LittleFS race partition. Neither is present on the C6 product configuration.

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

- **Start All** posts to every online client sequentially — calls land within ~350 ms of each other
- **Stop All** posts to every online client. The master's UI updates immediately on each acknowledged stop rather than waiting for the next heartbeat
- Each client decides what to do with the broadcast based on its own *Ignore Race Director Start/Stop if already racing* toggle (default off → honour broadcast)
- The client's local UI flips into race-running state — Start button looks pressed, Stop becomes the active button — same as if the pilot had pressed Start locally
- Race-mode broadcasts are not subject to the 2 s director-state throttle described in [Observability](#observability-serial-log)

### Early Quit Notification

When a client's pilot presses **Stop** locally during an active master-broadcast race, the client posts `/api/multinode/quit` to the master. The master sets `quitEarly = true` on that node and the card flips to **DNF** until the next master-broadcast Start All.

This is what lets a director keep a heat running cleanly when one pilot crashes or batteries out — they tap Stop on their own device, the rest of the field continues racing without interruption, and the director sees exactly who DNF'd.

### Solo-Practice Override

Each client has an **Ignore Race Director Start/Stop if already racing** toggle (`mnSkipMasterStart` in config). When on:

- A master Start All broadcast is silently ignored if the client's timer is already running locally
- The pilot can keep practising while a director runs heats on the rest of the field
- Toggle off to rejoin the directed race format

### Edit Pilot Modal from the Race Tab

Every slot card on the master's Race tab (including the master's own host
card) has a pencil icon that opens a per-pilot editor. The same modal
serves client pilots and the master; controls that don't apply to the host
(Move / Swap, Kick) are hidden on the host card.

The modal can:

- Edit pilot name, color, band/channel, RSSI thresholds, and the skip flag
- Run the **Calibration Wizard** against the selected node — for a client
  the wizard records on that client and pushes thresholds back through
  the master; for the host it runs locally
- Show a **Live RSSI** chart from the selected node at 5 Hz. The firmware
  returns the peak signal from the preceding 200 ms so a brief gate pass
  is caught even at that polling rate. The toggle on the right of the
  title defaults OFF and confirms before enabling during a race
- **Move (Swap) Pilot to Slot** — letter dropdown (A–G) reassigns the
  node to a different slot. If the target slot is occupied, the two
  pilots swap places and both clients persist their new slot through
  reboots
- **Kick from Slot** pauses the client's reconnection attempts for one
  minute so another unit can take its place

Save commits changes and keeps the modal open with a brief "Saved ✓"
feedback. The middle section scrolls; Save/Close stay pinned at the bottom.

### Live RSSI Proxy

Two endpoints feed the Edit Pilot modal's chart:

| Endpoint | What it returns |
|---|---|
| `GET /timer/rssi` | `{"rssi": N}` — the peak signal observed since the previous request, sampled at the firmware's full rate. A 5 Hz poll catches the true peak from the preceding 200 ms rather than a random instantaneous value |
| `GET /api/multinode/rssi?nodeId=N` (master only) | Proxies to the named client's `/timer/rssi` over the AP. A successful round-trip also keeps the master's heartbeat watchdog quiet, so the live view can't false-time-out the client it's watching |

Polling defaults to OFF when the modal opens. Enabling the live feed
during an active race prompts a confirmation dialog explaining the WiFi
bandwidth cost.

### Master Recruit Nearby Units

`POST /api/multinode/recruit?force=0|1` queues a recruit job on the master:

1. Drop the master's AP
2. Scan for every `FPVRaceOne_*` AP in range
3. For each match (skipping units already in Client / Master mode unless
   `force=1` is set), join the target, configure it as a client pointed
   at this master, and reboot it
4. Restart the master's own AP

Total downtime is roughly 60 seconds — a full-screen overlay on the
director's browser holds focus during the pass and confirms completion
when the AP returns.

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
- Multi-node: node mode, master SSID, skip-master-start toggle, preferred slot (client re-requests its last-assigned slot on registration so slots survive reboots), client race-audio opt-in
- OTA: home WiFi credentials, include-prereleases toggle
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

Categories covered include the RX5808 SPI driver, EEPROM, LittleFS, WiFi AP / station, lap timer, race history, web server file presence, and OTA partition health.

---

## Observability (Serial Log)

The firmware emits five structured log streams to the USB-CDC serial port,
all tagged so a postmortem grep is easy:

| Tag | Cadence | Format |
|---|---|---|
| `[HEAP]` | Every 10 s | `free=N min=N maxBlk=N sta=N sse=N` — current free heap, minimum-ever free, largest contiguous block, AP station count, attached SSE clients. Low-heap watchdog auto-reboots when `free < 20 KB` or `maxBlk < 8 KB` for 10 seconds straight |
| `[AP]` | On event | `STA connected/disconnected: <mac>  (now N stations)` — every AP-level association/disassociation. Non-client station disconnects also trigger an SSE eviction line to reclaim heap from orphaned `EventSource` clients |
| `[CORE0]` | Every 10 s + on-event | `window 10s: longest sub-call <name>=<ms>, longest tick gap=<ms>` — names the slowest sub-call seen in the last 10 seconds and the worst gap between iterations. A per-call line fires immediately if any single sub-call takes ≥ 100 ms |
| `[MULTINODE] connected:` | Every 10 s | `A=Sam B=Bob C=Lenny ...` — slot letter + pilot name for every currently-online client. `(none)` if nobody is connected |
| `[MULTINODE]` events | Immediate | `New node X (...)`, `Re-registered node X (...)`, `Updated node X (...)`, `Node X (Pilot) timed out`, `Node X (Pilot) reconnected via heartbeat`, `Node X (Pilot) quit early`, `Node X (Pilot) manually removed`. Slot is shown as a letter (A–G), matching the UI |

Steady-state keep-alive registrations are **silent** — only first-time
joins, recoveries from offline, and changes to user-visible identity
fields produce a line. This keeps the trace usable as a problem
detector instead of a firehose.

### Director-state broadcast throttle

Director-state mirror pushes to clients are rate-limited to **once per
2 seconds** so a burst of state changes coalesces into a single push
instead of stacking up. Race-critical broadcasts (pre-arm, start, stop)
are **not** throttled — their timing matters more than mirror updates.

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
├── OTA/                  — GitHub-Releases-driven over-the-air update flow
├── WEBSERVER/            — ESPAsyncWebServer HTTP + SSE
└── ...
```

Other library directories (`RACEHISTORY`, `TRACKMANAGER`, `USB`, …) compile in but are inert on the current C6 product configuration — they're carried for future hardware revisions that may have an SD card, dedicated race-history partition, or a need for a host-side USB control protocol.

### Web Interface

```
data/
├── index.html            — Single-page application
├── style.css             — Responsive styles, multiple themes, save-button dirty state
├── script.js             — UI logic, staged config, race control, calibration, multi-node
├── audio-announcer.js    — Browser Web Speech API wrapper for lap announcements
└── smoothie.js           — Live RSSI chart library
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
