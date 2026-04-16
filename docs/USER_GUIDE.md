# FPVRaceOne User Guide

Complete guide to using all features of your FPVRaceOne lap timer.

**Navigation:** [Home](../README.md) | [Getting Started](GETTING_STARTED.md) | [Features](FEATURES.md)

---

## Table of Contents

1. [Connecting](#connecting)
2. [Web Interface Overview](#web-interface-overview)
3. [Settings](#settings)
4. [Calibration](#calibration)
5. [Racing](#racing)
6. [Race Analysis](#race-analysis)
7. [Race History](#race-history)
8. [Advanced Features](#advanced-features)
9. [Tips & Best Practices](#tips--best-practices)

---

## Connecting

### WiFi

FPVRaceOne broadcasts its own WiFi Access Point on startup.

1. Power on the device and wait 10–15 seconds
2. Connect to **`FPVRaceOne_XXXX`** (last 4 digits of MAC address)
3. Password: **`fpvgate1`**
4. Open a browser and go to **`http://192.168.4.1`**

> No captive portal — your phone or laptop can stay connected to FPVRaceOne WiFi while maintaining a cellular internet connection.

### USB

Direct USB connection provides lower latency than WiFi.

1. Download the [Electron desktop app](https://github.com/LouisHitchcock/FPVGate/releases)
2. Connect FPVRaceOne via USB-C
3. Launch the app and select your COM port
4. All features work identically to WiFi mode

**USB advantages:** Zero WiFi latency, works completely offline, WiFi AP remains available for additional viewers.

---

## Web Interface Overview

The interface is organised into four main tabs:

### Race Tab

The primary racing interface.

- **Timer** — Current race elapsed time
- **Lap Counter** — Current lap / max laps (if configured)
- **Control Buttons** — Start Race, Stop Race, Add Lap (manual), Clear Laps
- **Lap Table** — All recorded laps with times and gap-to-previous
- **Statistics** — Fastest lap, median, best 3 laps (sum and consecutive)
- **Charts** — Lap history bar chart and fastest 3-consecutive round chart

### Calibration Tab

RSSI threshold setup and the calibration wizard.

- Live RSSI signal chart
- Calibration Wizard (guided fly-over recording + auto threshold calculation)
- Enter / Exit threshold sliders with save button
- Pause / resume live signal view for threshold fine-tuning

### Race History Tab

Archive of all saved races.

- Race list with dates, pilot info, and summary stats
- Expand any race for full lap-by-lap analysis and charts
- Edit race names and tags
- Marshalling mode — add, remove, or edit laps post-race
- Download individual races or full history as JSON
- Import races from a backup file

### Settings Tab

All configuration, organised into sections with a floating footer.

> **Save Configuration button:** The button highlights **orange** (with a pulse animation) whenever you have unsaved changes. Click it to commit everything to the device. It returns to its inactive state once saved successfully.

---

## Settings

### Race Setup

| Setting | Description |
|---------|-------------|
| **Max Laps** | 0 = infinite; 1–50 = auto-stop when reached |
| **Min Lap Time** | Reject gate triggers closer together than this (seconds) |
| **Alarm Threshold** | Battery voltage alarm level (if battery monitoring enabled) |

### TTS Settings

| Setting | Options |
|---------|---------|
| **Announcer Type** | None / Beep / Lap Time / 2 Consecutive / 3 Consecutive |
| **Voice** | Sarah (ElevenLabs) / Rachel / Adam / Antoni / PiperTTS |
| **Lap Announcement Format** | Full (name + lap + time) / Lap + Time / Time Only |
| **Announcer Rate** | 0.1–2.0× playback speed |

**Enable Audio** / **Disable Audio** buttons toggle voice announcements.  
**Generate Audio** plays a test announcement with your current pilot name.

### Pilot Info

| Setting | Description |
|---------|-------------|
| **Band & Channel** | Must match your drone's VTx exactly |
| **Pilot Name** | Full name for voice announcements and race records |
| **Callsign** | Short display name (max 10 chars) |
| **Phonetic Name** | How TTS pronounces your name (e.g. "Louie" for "Louis") |
| **Pilot Color** | Used in race history and display |

Band and channel changes are **auto-saved** immediately on selection.

### Signal Processing

Choose between two RSSI processing pipelines — switchable at any time without restarting.

#### V1 — FPVGate Multi-Stage Pipeline (Default)

Applies a chain of filters in sequence:

1. **Kalman filter** — Tracks signal dynamics while rejecting ADC noise
2. **Median-of-3** — Removes isolated spike samples
3. **Moving average (7 samples)** — Smooths the signal
4. **EMA (α = 0.15)** — Additional low-pass smoothing
5. **Step limiter (±12/sample)** — Prevents single-sample teleport jumps

**Detection parameters (V1 only):**

| Parameter | Description | Default |
|-----------|-------------|---------|
| **Enter Hold Samples** | Consecutive samples at/above Enter RSSI required before registering gate entry | 4 |
| **Exit Confirm Samples** | Consecutive samples below Exit RSSI required to confirm exit | 2 |

Higher values = more debounce, fewer false triggers; lower values = faster response.

#### V2 — RotorHazard Bessel IIR

A single 2nd-order Bessel low-pass filter applied directly to raw RSSI. Cutoff frequency is configurable:

| Setting | Cutoff | Character |
|---------|--------|-----------|
| 100 Hz | Fastest | Minimal lag, some noise passes through |
| 50 Hz | Balanced | Good compromise for most environments |
| 20 Hz | Smoothest | Maximum noise rejection, most lag |

V2 uses single-sample gate entry/exit (no hold counters) and places the lap timestamp at the **midpoint of the signal peak plateau** for improved accuracy.

**When to use V2:** Clean RF environments where low lag matters more than noise rejection.

### LED Setup

Configures the optional WS2812 gate LED strip (if connected via webhooks).

### Webhooks

Send HTTP POST events to external devices (e.g. gate LED controllers) on race start, race stop, and each lap. Up to 10 target IP addresses. Each event type can be enabled or disabled independently.

### Network / WiFi

Configure the device to connect to an existing WiFi network (station mode) in addition to broadcasting its own AP. Enter SSID and password, then click **"Apply WiFi & Reboot"**.

### Device

| Option | Description |
|--------|-------------|
| **External Antenna** | Switch between internal and external WiFi antenna (requires reboot) |
| **WiFi TX Power** | Transmit power in dBm, 2–21 (requires reboot) |
| **RSSI Sensitivity** | Normal or High (1.5× boost) |
| **Theme** | UI colour scheme (23 options) |
| **Reboot** | Restart the device immediately |

### OTA Updates

See **Settings → OTA** to upload new firmware or filesystem via ElegantOTA.

- Firmware: upload `.pio/build/seeed_xiao_esp32c6/firmware.bin`
- Filesystem: build with `pio run --target buildfs`, then upload `littlefs.bin`

Both are independent uploads and both should be updated after each release.

---

## Calibration

### Understanding RSSI Thresholds

- **Enter RSSI** — Gate crossing begins when signal rises *above* this value
- **Exit RSSI** — Lap is recorded when signal falls *below* this value after peaking

```
RSSI  │     /\
      │    /  \
Enter ├───/────\────
      │  /      \
Exit  ├─/────────\──
      └────────────── Time
```

Enter must always be higher than Exit. The lap timestamp is recorded at the peak.

### Using the Calibration Wizard

1. Go to the **Calibration** tab and click **"Start Calibration Wizard"**
2. Click **Record**, fly 2–3 gate passes at race speed, click **Stop**
3. Review the chart — you should see a distinct peak per pass
4. The wizard auto-calculates Enter and Exit thresholds
5. Adjust manually if needed, then click **"Apply Thresholds"**

### Manual Threshold Adjustment

Use the **Enter** and **Exit** sliders in the Calibration tab for fine-tuning. The live RSSI chart shows the signal and threshold lines in real time. Click **"Save RSSI Thresholds"** when satisfied.

You can also **pause** the live chart to examine a specific moment, then **resume** the live feed.

### Calibration Tips

| Problem | Fix |
|---------|-----|
| Missing laps | Lower Enter RSSI by 5 points |
| False / extra laps | Raise Exit RSSI by 5, or increase Min Lap Time |
| Inconsistent detection | Ensure VTx has warmed up 30+ seconds |
| Noisy signal (V1) | Try V2 with 50 Hz or 20 Hz cutoff |
| Too much lag (V2) | Try V1, or use V2 at 100 Hz |

---

## Racing

### Starting a Race

1. Go to the **Race** tab
2. Click **"Start Race"**
3. Listen for the countdown — "Arm your quad… starting on the tone…"
4. A random 1–5 second delay ends with a start beep at 880 Hz
5. Fly! The first gate pass is **Gate 1** (hole shot)

### During a Race

- The timer runs from the start beep
- Each gate pass registers a lap; voice announces each one (if enabled)
- Click **"Add Lap"** to manually register a lap at the current time if the gate missed a pass
- The lap table and statistics update in real time

### Stopping a Race

Click **"Stop Race"** — the race is saved automatically to history.

If Max Laps is set, the race stops automatically when the count is reached.

### Clearing Laps

Click **"Clear Laps"** — saves the current race to history first, then resets the timer and table.

---

## Race Analysis

### Statistics

| Metric | Description |
|--------|-------------|
| **Fastest Lap** | Quickest single lap and which lap number it was |
| **Median Lap** | Middle value of all laps — best consistency indicator |
| **Best 3 Laps** | Sum of the 3 fastest individual laps (MultiGP scoring) |
| **Fastest 3 Consecutive** | Best back-to-back 3-lap sequence (RaceGOW format) |

Gate 1 (hole shot) is excluded from all best-lap calculations.

### Charts

- **Lap History** — Colour-coded horizontal bars for the last 10 laps; width proportional to time
- **Fastest Round** — Vertical bars for the 3 consecutive laps with the best combined time

The fastest lap row is highlighted in **gold** in the lap table.

---

## Race History

All races are saved automatically. Navigate to the **Race History** tab to review them.

### Viewing and Editing

- Click any race card to expand the full lap table, statistics, and charts
- Click **Edit** to add a name or tag to the race
- Click **Download** to save the race as a JSON file

### Marshalling Mode

Edit laps after a race is complete:

- **Add a lap** — Click between two existing laps, enter the lap time in seconds
- **Remove a lap** — Click the remove button next to any lap and confirm

Statistics and charts update immediately. Changes are saved automatically.

> Export a race before making significant edits — changes are permanent.

### Export / Import

- **Download All Races** — Saves your full history as a single JSON file
- **Import Races** — Merge races from a backup; duplicates are skipped automatically

---

## Advanced Features

### Signal Processing Mode

Switch between V1 and V2 in **Settings → Signal Processing**. See the [Settings](#signal-processing) section for a full comparison.

### OSD Overlay

Click **"Open OSD"** in the Race tab to open a transparent overlay for use in OBS or similar streaming software.

**OBS setup:**
1. Add a **Browser Source**
2. Paste the OSD URL
3. Set width: 1920, height: 1080
4. Position over your stream

### Config Backup & Restore

In **Settings → footer**:
- **Download Config** — Saves all settings as a JSON file
- **Import Config** — Restores settings from a previously downloaded JSON

Export your config before any firmware update.

### Theme Selection

23 colour themes available in **Settings → Device → Theme**. The logo automatically switches between black and white versions for light and dark themes.

---

## Tips & Best Practices

**For accurate timing:**
- Let the VTx warm up for 30 seconds before calibrating
- Recalibrate if you change frequency or fly with other pilots in the same band
- Use Minimum Lap Time to prevent false triggers from crashes near the gate

**For signal quality:**
- Position the RX5808 antenna to face the gate opening
- Keep the timer module away from other strong RF sources (video transmitters on the bench, etc.)

**For racing:**
- Use USB connection for the lowest possible control latency
- Enable voice announcements for faster lap awareness without looking at the screen
- Export your race history before flashing a firmware update

---

**Questions or issues? [Open a GitHub issue](https://github.com/LouisHitchcock/FPVGate/issues)**
