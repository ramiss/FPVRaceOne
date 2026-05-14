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
3. Password: **`fpvraceone`**
4. Open a browser and go to **`http://192.168.4.1`**

> No captive portal — your phone or laptop can stay connected to FPVRaceOne WiFi while maintaining a cellular internet connection. (Note: Samsung devices do not allow wifi to a non-internet AP and cellular at the same time).


### Status LEDs

Two small LEDs are visible through the holes on the underside of the case, just to one side of the USB-C connector — one yellow, one red.

| LED | What it means |
|-----|---------------|
| **Yellow** | Web server status. **Off** during boot. **Slow blink (~1 Hz)** once the WiFi AP is up and the web UI is ready for clients. Off again only if the firmware has stopped serving — almost always a sign to check the serial monitor. |
| **Red** | Not used. **Briefly on** at every power-up, then **off**. |

So the quick health check after power-up: red flickers once, yellow comes on blinking after a couple of seconds → device is ready to accept WiFi connections at `http://192.168.4.1` (or `http://192.168.5.1` in Master mode).

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

Session-scoped log of races completed since the last power-up.

- Race list with timestamps, pilot info, and summary stats
- Expand any race for full lap-by-lap analysis + interactive timeline / playback
- Edit race names and tags
- Marshalling mode — add, remove, or edit laps after the race ended
- Download individual races or the whole session as JSON
- Import races from a previously-downloaded JSON

> **Races are not persisted across power cycles** on the current hardware (no SD card, no flash slot reserved for race storage). Once you're done racing, **download** the session JSON to keep the data — the next reboot starts with an empty list.

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

### TTS Settings

Lap announcements are spoken by your **browser's** built-in voice (Web Speech API) — the device itself doesn't carry any audio files, so the voice you hear is whichever one your phone or laptop's OS provides for English.

| Setting | Options |
|---------|---------|
| **Announcer Type** | None / Beep / Lap Time / 2 Consecutive Laps / 3 Consecutive Laps |
| **Lap Announcement Format** | Pilot + Lap + Time / Pilot + Time / Lap + Time / Time Only |
| **Announcer Rate** | 0.1–2.0× playback speed |

**Enable Voice** / **Disable Voice** buttons toggle announcements.  
**Test Voice** plays a sample announcement with your current pilot name.

Because the audio comes from the browser, **the device that's logged in to the web UI is the one that speaks** — handy if you want the race director's laptop to call out laps while pilot phones stay silent (just don't enable voice on the pilot pages).

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

A single RSSI processing pipeline based verbatim on the upstream FPVGate algorithm.

Applies a chain of filters in sequence:

1. **Kalman filter** — Tracks signal dynamics while rejecting ADC noise
2. **Median-of-3** — Removes isolated spike samples
3. **Moving average (7 samples)** — Smooths the signal
4. **EMA** — User-tunable low-pass via the **Pipeline Smoothing** slider (default level 5 = α 0.15 = upstream behaviour; lower = less lag, higher = more smoothing)
5. **Step limiter (±12/sample)** — Prevents single-sample teleport jumps

**Detection parameters (built in, not exposed):**

| Parameter | Default | Notes |
|-----------|---------|-------|
| Enter hold samples | 4 | Consecutive samples at/above Enter RSSI before peak tracking starts |
| Exit confirm samples | 2 | Consecutive raw samples below Exit RSSI to confirm exit |
| Peak min above exit | 5 | Peak must exceed Exit by at least this many counts to count as a lap |
| Ceiling-drift watchdog | 3 s | If "in gate" longer than this without exit, state is force-reset |

**Gate-1 Bootstrap (toggle):** When on, the first lap of a race is special-cased so a drone already inside the gate at race start still produces a clean first lap (relaxed enter, lower 2-sample debounce, 3-count peak margin). When off, Gate 1 behaves like any other gate.

### LED Setup

Configures the on-device status LED (preset, brightness, colour). Used to indicate WiFi state, race state, and lap events.

### Multi-Node Timing

Network up to **8 FPVRaceOne devices** together — no router, no hub, no extra hardware. One device acts as the race-director **Master** and up to **7 Clients** join its WiFi. Lap events stream back to the master in real time so the director sees every pilot's race on a single screen.

#### Roles

| Mode | What it does |
|------|--------------|
| **Single** (default) | Standalone — no inter-device traffic |
| **Master** | Hosts the AP that clients join. Renders the multi-node Race tab with a card per pilot. Broadcasts Start All / Stop All |
| **Client** | Connects to a master AP. Forwards lap counts and times via 1 Hz heartbeats. Honours master Start / Stop unless overridden |

#### Settings (Settings → Multi-Node)

| Setting | Visible when | Description |
|---------|--------------|-------------|
| **Node Mode** | Always | Single / Master / Client. Changing this requires reboot — the **Apply Multi-Node & Reboot** button only enables when the mode changes |
| **Master SSID** | Client | The master device's AP name (e.g. `FPVRaceOne_AB12CD`). Type it manually or use **Scan** to discover masters in range |
| **Scan for Masters** | Client | Lists every `FPVRaceOne_*` AP within range — tap one to autofill |
| **Ignore Race Director Start/Stop if already racing** | Client | When on, a master Start/Stop broadcast is ignored if the client is already mid-race locally. Lets a pilot keep practising while a heat is being run on the others |

#### Race-Directing Flow

1. **Master** Race tab shows a card per connected client: pilot name, running indicator, lap count, last lap time
2. Tap **Start All** — every client starts simultaneously
3. As pilots fly, each client streams its laps back. The master cards update live (Server-Sent Events, sub-second latency)
4. Tap **Stop All** — every client stops cleanly
5. If a pilot crashes out, they tap **Stop** on their own client. The master sees a **DNF** badge on that pilot's card; the rest of the heat continues uninterrupted

#### State Indicators on Master Cards

| Indicator | Meaning |
|-----------|---------|
| ● **Running** | Client's lap timer is running |
| ○ **Stopped** | Client is idle |
| ⚠ **DNF** | Client pressed Stop locally during an active master-broadcast race |

### Firmware Update

The device updates itself from GitHub Releases.

1. Enter your **Home WiFi** credentials (one-time, saved automatically)
2. Tap **Check for Updates** — the device briefly joins your home network, queries GitHub, returns to AP mode
3. If a newer release exists, you'll see the version + release notes — tap **Update Now**
4. The device flashes the filesystem image, then the firmware image, then reboots once. Total time ~1–3 minutes
5. Your device may briefly disconnect from the device's AP during the update — reconnect when prompted

**Safety:**
- Updates are blocked while a race is running
- Failed downloads keep the previous firmware — there is no risk of bricking
- Manual flashing via PlatformIO / esptool is still available for first-time setup or recovery (see [FLASHING_OPTIONAL.md](FLASHING_OPTIONAL.md))

### WiFi & Connection

| Setting | Description |
|---------|-------------|
| **Antenna** | Switch between internal PCB antenna and external connector (takes effect on next boot) |
| **TX Power** | WiFi transmit power in dBm, 2–21 (takes effect on next boot). Lower values reduce range and interference; raise it back to 21 for maximum AP range |

The Home WiFi fields used by the firmware updater (Settings → Firmware Update) double as station-mode credentials — once entered, the device can join your home WiFi for OTA pulls.

### System Settings

| Section | Options |
|---------|---------|
| **Appearance** | Theme — pick from 23 colour schemes |
| **Device** | Reboot button (required for antenna and TX power changes to take effect) |
| **Race Tab** | "Always hide download reminder banner" — permanently dismiss the *races are lost on flash* banner once you've started downloading races regularly |
| **Developer** | **Dev Mode (Simulate Laps)** — when on, tap a pilot's name on the Race tab to inject a random simulated lap. Useful for testing multi-node UI without real quads |

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
2. Click **Record**, fly **3 gate passes** at race speed, click **Stop**
3. The wizard auto-detects the three highest peaks and overlays markers — drag any marker if a peak was misidentified
4. Click **Calculate** — the wizard shows the recommended **Enter** and **Exit** thresholds
5. If the three peaks differ in height by >15 %, the wizard recommends a re-fly. Equal-height peaks calibrate better; re-flying takes 30 seconds and is almost always worth it
6. Tap **Apply Thresholds** — values are saved immediately

**How the thresholds are picked:**

- **Enter** ≈ 95 % of the *weakest* of the three peaks — leaves about 5 % headroom for lap-to-lap variation. Tight enough that adjacent gates in close-pattern layouts typically don't trigger
- **Exit** = Enter − 7 — tight enough for tiny-whoop tracks where gates are close together, but raised above the recording's 35th-percentile noise floor if needed for hysteresis

### Manual Threshold Adjustment

Use the **Enter** and **Exit** sliders in the Calibration tab for fine-tuning. The live RSSI chart shows the signal and threshold lines in real time. Click **"Save RSSI Thresholds"** when satisfied.

You can also **pause** the live chart to examine a specific moment, then **resume** the live feed.

### Calibration Tips

| Problem | Fix |
|---------|-----|
| Missing laps | Lower Enter RSSI by 5 points; lower **Pipeline Smoothing** in Settings → Signal Processing for less lag |
| False / extra laps | Raise Exit RSSI by 5, increase Min Lap Time, or raise **Pipeline Smoothing** to reject more noise |
| Inconsistent detection | Ensure VTx has warmed up 30+ seconds; rerun the wizard with cleaner peaks |
| Drone already in gate at start triggers spurious Lap 1 | Enable **Gate-1 Bootstrap** in Settings → Signal Processing |

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

Each race you finish is logged to the **Race History** tab automatically. The log lives in RAM only — power-cycling the device clears it, so **download what you want to keep** before unplugging.

### Viewing and Editing

- Click any race card to expand the full lap table, statistics, charts, and the interactive timeline
- Click **Edit** to add a name or tag to the race
- Click **Download** to save the race as a JSON file

### Marshalling Mode

Edit laps after a race is complete:

- **Add a lap** — Click between two existing laps, enter the lap time in seconds
- **Remove a lap** — Click the remove button next to any lap and confirm

Statistics and charts update immediately. Changes are saved into the same in-memory record (so they survive subsequent edits, but still won't survive a reboot — download first).

### Export / Import

- **Download All Races** — Saves the current session's history as a single JSON file
- **Import Races** — Loads a previously-downloaded JSON. On hardware without persistent storage (i.e. this one), importing **replaces** the in-memory history rather than merging

---

## Advanced Features

### Pipeline Smoothing

The single Pipeline Smoothing slider in **Settings → Signal Processing** tunes the EMA stage of the detection pipeline:

- **Level 5 (default)** = identical to upstream FPVGate behaviour
- **Lower** = less smoothing, faster response, more noise passes through
- **Higher** = more smoothing, slower response, more noise rejection

Most pilots never need to touch this. Adjust it only if calibration consistently misses laps (lower it) or constantly fires twice (raise it).

### Multi-Node Race Directing

Pair two or more devices for head-to-head racing. See [Multi-Node](#multi-node) above. Common patterns:

- **Solo practice while a master is broadcasting** — turn on *Skip Master Start* on the client so a director's Start All doesn't reset your local race
- **Pilot drops out mid-heat** — the pilot presses Stop on their client; the master sees a **DNF** badge on that pilot's card and the rest of the race continues uninterrupted

### Config Backup & Restore

In **Settings → footer**:
- **Download Config** — Saves all settings as a JSON file
- **Import Config** — Restores settings from a previously downloaded JSON

Export your config before any firmware update.

### Theme Selection

Multiple colour themes are available in **Settings → Device → Theme**. The logo automatically switches between black and white versions for light and dark themes.

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
- Enable voice announcements on the race director's device so laps are called out without anyone watching the screen
- Download your race session JSON before unplugging the device — race history is not retained across power cycles
- Export your config too if you've spent time fine-tuning, especially before a firmware update

---

**Questions or issues? [Open a GitHub issue](https://github.com/ramiss/FPVRaceOne/issues)**
