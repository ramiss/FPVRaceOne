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
8. [Webhooks & Integration](#webhooks--integration)
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
| Default password | `fpvgate1` |
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

Two complete RSSI processing pipelines are available, switchable at runtime via **Settings → Signal Processing**. Both read the same raw ADC value and write to the same RSSI history buffer, so the calibration wizard and all gate detection logic work identically regardless of which mode is selected.

### ADC Input

- **Hardware:** Seeed XIAO ESP32-C6 12-bit ADC
- **Attenuation:** 6 dB (input range 0–1.75 V)
- **Scale:** 1 V ≈ 2340 counts; full-scale divisor = 2400
- **Output:** 0–255 (8-bit, clamped)

### V1 — FPVGate Multi-Stage Pipeline

A sequential chain of five filter stages:

| Stage | Filter | Parameters |
|-------|--------|------------|
| 1 | Kalman | Q = 5.0 (process noise), R = 8.0 (measurement noise) |
| 2 | Median-of-3 | Rolling 3-sample window |
| 3 | Moving average | 7-sample window |
| 4 | EMA | α = 0.15 |
| 5 | Step limiter | Max ±12 counts per sample |

**Gate detection (V1):**
- **Enter Hold Samples** (configurable, default 4) — consecutive samples at or above Enter RSSI before gate entry is registered
- **Exit Confirm Samples** (configurable, default 2) — consecutive samples below Exit RSSI before gate exit is confirmed

All filter state is reset at race start so stale data from a previous race cannot affect the first lap.

### V2 — RotorHazard Bessel IIR

A single 2nd-order Bessel infinite impulse response filter applied directly to raw RSSI. Coefficients are ported from the RotorHazard open-source project.

| Cutoff | b₀ | a₁ | a₂ | Character |
|--------|----|----|-----|-----------|
| 100 Hz | 9.054e-2 | −0.24114 | 0.87898 | Fastest / least smoothing |
| 50 Hz | 2.921e-2 | −0.49774 | 1.38090 | Balanced |
| 20 Hz | 5.593e-3 | −0.75788 | 1.73551 | Smoothest / most lag |

Output formula: `out = v[0] + v[2] + 2·v[1]` (DC gain = 1.0 — verified analytically).

**Gate detection (V2):**
- Single-sample entry and exit (trusts the filter rather than hold counters)
- Lap timestamp placed at the **midpoint of the signal peak plateau**: `peakTime + peakDuration / 2`

### Kalman Filter (V1)

Standard Kalman predictor-corrector:

- **Q (process noise)** — How fast the true signal can change. Higher Q = more responsive, more noise passes through.
- **R (measurement noise)** — How noisy the ADC is. Higher R = more smoothing, more lag.
- Steady-state gain ≈ 43% at Q=5, R=8 (29× more responsive than the original misconfigured defaults).

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
| V2 midpoint correction | Reduces systematic error | Timestamp shifted to peak centre |

### Minimum Lap Time

Laps triggered within the minimum lap time are silently ignored. This prevents double-counting when the drone lingers near the gate or crashes close to it.

---

## Calibration Wizard

### Recording Phase

- Firmware stores up to 5000 RSSI samples with millisecond timestamps in a dedicated calibration buffer
- Samples are captured at the same rate as the live RSSI loop
- Multiple fly-over passes can be recorded in a single session

### Analysis Phase

- The full recorded dataset is streamed to the browser in pages (500 samples/page) after recording stops
- A chart renders all samples with the calculated Enter and Exit thresholds overlaid as horizontal lines
- Thresholds are calculated from the observed peak and valley values

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

Races are stored in LittleFS (`/races.json`) and persist across reboots and firmware updates (as long as the filesystem is not erased).

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

## Webhooks & Integration

HTTP POST webhooks are sent to configured IP addresses on race events.

| Endpoint | Trigger |
|----------|---------|
| `/RaceStart` | Race starts |
| `/RaceStop` | Race stops |
| `/Lap` | Lap detected (or manually added) |

- Up to 10 target IP addresses
- Each event type can be independently enabled or disabled
- A master **Gate LEDs Enabled** toggle controls whether any webhooks fire
- Webhook HTTP calls run on Core 0 (WiFi/network core) and do not block the RSSI sampling loop on Core 1

---

## Configuration Management

### Storage

All device configuration is stored in EEPROM with a version stamp. When the version doesn't match (e.g. after a firmware update that adds new fields), the device loads defaults.

A backup is also written to LittleFS which can be restored if EEPROM is invalidated.

### Versioning

`CONFIG_VERSION` is incremented whenever the config struct changes. The current version is **11**.

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
- Min lap time, max laps, alarm threshold
- Announcer type, rate, voice, lap format, phonetic name
- Pilot name, callsign, color
- LED preset, brightness, speed, colours, manual override
- Webhooks enabled, IPs, per-event toggles, gate LEDs enabled
- WiFi SSID, password, external antenna, TX power
- RSSI sensitivity
- Filter mode (V1/V2), Bessel Hz cutoff
- Enter Hold Samples, Exit Confirm Samples
- Theme, voice enabled, lap format

---

## OTA Updates

Firmware and filesystem are updated independently via ElegantOTA at `http://192.168.4.1/update`.

### Firmware

Build target: `pio run --environment seeed_xiao_esp32c6`  
Output: `.pio/build/seeed_xiao_esp32c6/firmware.bin`

Select **Firmware** in ElegantOTA and upload.

### Filesystem

Build target: `pio run --target buildfs --environment seeed_xiao_esp32c6`  
Output: `.pio/build/seeed_xiao_esp32c6/littlefs.bin`

Select **Filesystem** in ElegantOTA and upload.

> Both should be updated after any release that changes either firmware logic or web UI files. They are completely independent uploads.

---

## Self-Test System

Access via **Settings → System Diagnostics → Run All Tests**.

19 tests covering hardware, storage, connectivity, and software:

| Category | Tests |
|----------|-------|
| Hardware | RX5808 RSSI module, buzzer |
| Storage | EEPROM, LittleFS |
| Connectivity | WiFi AP, USB CDC, transport layer |
| Software | Configuration validity, lap timer, race history, webhooks, web server files, OTA partitions |

Results show pass/fail per test with detail text. A diagnostic log can be downloaded for support.

---

## Technical Architecture

### Firmware Structure

```
src/
└── main.cpp              — Entry point; Core 1 = RSSI loop, Core 0 = WiFi/webhooks

lib/
├── CALIBRATION/          — Wizard recording and buffer management
├── CONFIG/               — Configuration struct, EEPROM, serialisation
├── DEBUG/                — Debug logger (no vTaskDelay in hot path)
├── KALMAN/               — Kalman filter (standard Q/R convention)
├── LAPTIMER/             — V1/V2 pipeline, gate state machine, lap events
├── RX5808/               — RX5808 SPI driver and ADC read
├── WEBSERVER/            — ESPAsyncWebServer HTTP + SSE at 20 Hz
├── WEBHOOKMANAGER/       — HTTP POST webhooks (Core 0)
└── ...
```

### Web Interface

```
data/
├── index.html            — Single-page application
├── style.css             — Responsive styles, 23 themes, save-button dirty state
├── script.js             — UI logic, staged config, race control, calibration
└── usb-transport.js      — USB Serial CDC communication layer
```

### Two-Core Architecture

| Core | Responsibilities |
|------|-----------------|
| Core 1 (main loop) | RSSI sampling at 100 Hz, Kalman/Bessel filtering, gate detection, lap events |
| Core 0 (parallel task) | WiFi stack, HTTP server, SSE streaming, webhook HTTP POST calls |

Webhook calls (up to 300 ms synchronous HTTP) were moved to Core 0 specifically to prevent them from stalling RSSI reads during lap events.

### SSE Streaming

Live RSSI data is pushed to the browser via Server-Sent Events at **20 Hz** (50 ms interval). SSE is one-way (server → client) and avoids WebSocket overhead for the high-frequency RSSI feed.

### Memory Layout (XIAO ESP32-C6)

| Partition | Size | Contents |
|-----------|------|----------|
| Bootloader | 32 KB | ESP32 bootloader |
| Partition table | 4 KB | Partition layout |
| OTA_0 (App0) | 2 MB | Primary firmware |
| OTA_1 (App1) | 2 MB | OTA update staging |
| LittleFS | ~1.5 MB | Web files, race history, audio |

---

**Questions or issues? [Open a GitHub issue](https://github.com/LouisHitchcock/FPVGate/issues)**
