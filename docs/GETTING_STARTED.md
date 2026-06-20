# Getting Started with FPVRaceOne

Complete guide to setting up and configuring your FPVRaceOne lap timer.

**Navigation:** [Home](../README.md) | [User Guide](USER_GUIDE.md) | [Features](FEATURES.md)

---

## Table of Contents

1. [What You Need](#what-you-need)
2. [Connect to FPVRaceOne](#connect-to-fpvraceone)
3. [Configure Your VTx Frequency](#configure-your-vtx-frequency)
4. [Set Pilot Information](#set-pilot-information)
5. [Run the Calibration Wizard](#run-the-calibration-wizard)
6. [First Race](#first-race)
7. [Multi-Node Racing (Optional)](#multi-node-racing-optional)
8. [Firmware Updates (OTA)](#firmware-updates-ota)
9. [Tips](#tips)
10. [Troubleshooting](#troubleshooting)
11. [Default Settings Reference](#default-settings-reference)

---

## What You Need

1. A pre-flashed FPVRaceOne lap timer and a phone, tablet, or laptop with WiFi
2. Any USB-C compatible battery bank or power supply (not included). If you'd
   rather run off your LiPo / Li-ion packs, search "USB-C Power Supply Cable
   for GoPro" — there are adapters in the ~$10 range

**Battery life estimates:**

| USB power bank | Approximate runtime |
|---|---|
| 1000 mAh | 1.5 – 2.5 hours |
| 2000 mAh | 3 – 5 hours |
| 5000 mAh | 8 – 12 hours |

The USB-C connector is used for **power and manual flashing** only. All
racing, calibration, and configuration happens through the WiFi web UI.

---

## Connect to FPVRaceOne

### WiFi (Default)

1. Power on your FPVRaceOne device and wait 10–15 seconds
2. On your phone or laptop, connect to the WiFi network: **`FPVRaceOne_XXXX`**  
   *(XXXX = last 4 digits of the device MAC address)*
3. Password: **`fpvraceone`**
4. Open a browser and go to: **`http://192.168.4.1`**

> Your device can connect to FPVRaceOne WiFi and maintain cellular internet simultaneously on most platforms — no captive portal is used. (Samsung phones force a choice between local WiFi and cellular data; everything else lets both coexist.)

The USB-C connector on the device is used for **power** and for **manual flashing** if needed. There is no user-facing USB control protocol in the current firmware — all racing, calibration, and configuration happens through the WiFi web UI.

---

## Configure Your VTx Frequency

1. Open the **Settings** tab (gear icon)
2. In the **Pilot Info** section, select your **Band** and **Channel** to match your drone's VTx
3. The frequency (MHz) updates automatically below the selector
4. The band/channel change is **auto-saved** immediately

**Common setups:**

| System | Band | Example |
|--------|------|---------|
| Analog 5.8 GHz | RaceBand (R) | R1 = 5658 MHz |
| DJI O3 / O4 | DJI O3/O4-20 | 5669–5912 MHz |
| HDZero | HDZero-R | 5658–5917 MHz |
| Walksnail | WLKSnail-R | 5658–5917 MHz |

---

## Set Pilot Information

In the **Pilot Info** section:

| Field | Description |
|-------|-------------|
| **Pilot Name** | Full name — used in voice announcements and race records |
| **Callsign** | Short display name (max 10 chars) — shown in the UI |
| **Phonetic Name** | How TTS pronounces your name (e.g. "Louie" for "Louis") |
| **Pilot Color** | Your racing color, used in race history and display |

In the **TTS Settings** section, choose your **Announcer Type** and **Lap Announcement Format**. Lap announcements are spoken by the browser's built-in voice, so the actual voice depends on the device you have the web UI open on — no audio files live on the device itself.

All settings in the Settings panel are **staged** until you click **Save Configuration** (the button highlights orange when there are unsaved changes).

---

## Run the Calibration Wizard

Proper calibration is the most important step for accurate timing.

### Step 1: Launch the Wizard

1. Navigate to the **Calibration** tab
2. Click **"Start Calibration Wizard"**

### Step 2: Record a Fly-Over

1. Power on your drone and let the VTx warm up for 30 seconds
2. Important: set your VTX to a fixed power level (not auto)
3. Click **Record** in the wizard
4. Fly your drone through the gate at race speed — **exactly 3 passes** is ideal.
   Fly through the start gate to the next gate and back through the start gate (x3).
5. Click **Stop Recording**

The wizard auto-detects the three highest peaks and overlays them on the chart. If detection misses one, drag a marker manually.

### Step 3: Review the Peak-Spread Check

If the three peaks differ in height by more than ~15 %, the wizard shows a warning recommending you re-fly. Equal-height peaks calibrate better — re-flying takes 30 seconds and is almost always worth it. If the spread is acceptable, just continue.

### Step 4: Apply

The wizard calculates thresholds with conservative safety margins:

- **Enter RSSI** ≈ 95 % of the *weakest* of the three peaks (~5 % headroom for lap-to-lap variation)
- **Exit RSSI** = Enter − 4 (tight hysteresis tuned for close-pattern tracks; raised above the recording's noise floor if needed)

Review the values, tweak manually if you have a specific reason, then tap **Apply Thresholds**. The values are saved immediately.

### Step 5: Verify

Watch the live RSSI chart on the Calibration tab while flying one or two more passes. You should see a clean peak well above Enter, then drop below Exit on the way out. If detection misses or fires twice, see *Missing Laps* / *False Laps* in the troubleshooting section.

---

## First Race

1. Navigate to the **Race** tab
2. Click **"Start Race"**
3. Listen for the countdown and start beep
4. Fly through the gate — the first pass is recorded as **Gate 1 (hole shot)**
5. Subsequent passes are recorded as **Lap 1, Lap 2**, etc.
6. Click **"Stop Race"** when finished — the race is saved automatically

**[Full racing guide →](USER_GUIDE.md#racing)**

---

## Multi-Node Racing (Optional)

If you have a second device — or up to seven of them — pair them as a single race-directing network. No router, no extra hardware.

### Set Up the Master

1. Pick one device to be the race director
2. Open **Settings → Multi-Node Timing**
3. Set **Node Mode** = `Master` and tap **Apply Multi-Node & Reboot**
4. After reboot, note the master's AP name (e.g. `FPVRaceOne_AB12CD`)

### Set Up Each Client — Option A: Master Recruit (one tap)

1. On the **master**, open **Settings → Multi-Node Timing → Recruit Nearby Units**
2. The master scans for every `FPVRaceOne_*` AP in range, joins each one,
   switches it into Client mode, points it at the master's SSID, and reboots it
3. A full-screen overlay shows progress; the master's own AP drops for
   ~60 seconds during the pass — reconnect when it returns
4. By default, units already in `Client` or `Master` mode are skipped — tick
   "Force recruit nodes already configured as clients" to re-target them too

This is the fastest setup for ad-hoc events where pilots haven't pre-configured
their timers.

### Set Up Each Client — Option B: Manual

1. On the next device, open **Settings → Multi-Node Timing**
2. Set **Node Mode** = `Client`
3. Tap **Scan** to discover masters in range, or paste the master's SSID into **Master SSID**
4. Tap **Apply Multi-Node & Reboot**
5. After reboot, the client will join the master's WiFi automatically

### Race

- On the **master**, every connected client appears as a card on the Race
  tab with the pilot's name, lap count, and live state (● Running, ○
  Stopped, ⚠ DNF). Slot labels are letters **A** through **G**
- Tap **Start All** to start every pilot at once; tap **Stop All** to end the heat
- Each client also keeps its own local Start / Stop — useful for a pilot
  to drop out mid-heat (their card shows **DNF** on the master, the heat
  keeps going)

### Editing pilots from the master

Every slot card (including the **Host** card for the master itself) has a
pencil icon. Tap it to open the Edit Pilot modal where you can:

- Change pilot name, color, band, channel, RSSI thresholds, or skip flag
- Run the **Calibration Wizard** for that client remotely (the wizard
  records on the target client and pushes thresholds back) or locally
  for the host
- View **Live RSSI** from the selected client (toggle on the right of
  the title) — peak-detected at the firmware so even a 5 Hz poll catches
  brief gate passes
- **Move (Swap) Pilot to Slot** — pick a target letter; if occupied, the
  two pilots swap places, both clients persist the new slot to NVS
- **Kick from Slot** (clients only) — pauses the client's reconnect
  attempts for one minute so another unit can take its place

Save commits and keeps the modal open; Close dismisses.

### Tip — Solo Practice During a Heat

If you want to keep practising while a director is running heats on the other pilots, enable **"Ignore Race Director Start/Stop if already racing"** on your client (Settings → Multi-Node). Now a director's Start All won't reset your practice run.

**[Full multi-node guide →](USER_GUIDE.md#multi-node-timing)**

---

## Firmware Updates (OTA)

The device updates itself from GitHub Releases — no PlatformIO or cables required.

1. **Settings → Firmware Update** → enter your home WiFi credentials (one-time)
2. Tap **Check for Updates** — the device briefly joins your home network,
   queries GitHub, then returns to AP mode
3. If a newer release is available, tap **Update Now**. The device flashes
   filesystem + firmware and reboots once

Updates are blocked while a race is running. A failed download keeps the
previous firmware on the device — a flaky network can't brick it.

For manual flashing (new hardware, recovery, or development), see
[Flashing Guide](FLASHING_OPTIONAL.md).

---

## Tips

- **Race History** — Races are not persisted across power cycles on the
  current hardware. Use **Download** in the Race History tab to save the
  session JSON before unplugging
- **Config Backup** — The Settings footer has **Download** / **Import**
  buttons. Export your config before a major firmware update so you can
  restore band/channel/calibration in seconds afterwards
- **Voice on the director's laptop** — Enable the Voice toggle on the race
  director's device (and leave it off on pilot phones) so laps are called
  out without the gate sounding like a chorus
- **Recalibrate when you change frequency** — Calibration is per-frequency.
  If you fly on a different VTx band/channel for a session, run the wizard
  again so the Enter/Exit thresholds suit the new signal

---

## Troubleshooting

### No WiFi Network Appearing

- Wait 15 seconds after power-on for the AP to start
- Check the USB power supply is providing stable 5V
- Try pressing the BOOT/RESET button on the device

### No RSSI Reading / Signal Flat at Zero

- Verify the RX5808 module is seated correctly
- Check that the RSSI pin is connected (A2 on XIAO C6)
- Confirm band and channel match your VTx exactly
- Let the VTx warm up — cold VTx produces lower/unstable RSSI

### Missing Laps

- Lower Enter RSSI by 5 points
- Confirm the VTx has warmed up (30 seconds minimum)
- Check band and channel are set correctly
- In Settings → Signal Processing, lower the **Pipeline Smoothing** slider to reduce signal lag

### False / Extra Laps

- Increase **Minimum Lap Time** in Settings → Race Setup
- Raise Exit RSSI by 5 points
- In Settings → Signal Processing, raise the **Pipeline Smoothing** slider for extra noise rejection
- Move the timer further from the flight path

### Can't Connect to Web Interface

- Make sure you are connected to the `FPVRaceOne_XXXX` WiFi network
- Navigate directly to `http://192.168.4.1`
- Try a different browser or clear the browser cache
- Check that no VPN is active on your device

### Voice Announcements Not Working

- In Settings → TTS, turn the **Voice** toggle on and tap **Test Voice** to hear a sample announcement
- Check that your browser has not blocked autoplay audio (Chrome and Safari often require an initial user interaction on the page before audio plays)
- The announcement is spoken by the **browser's** built-in voice (Web Speech API), so the voice depends on the device the web UI is open on. On phones, system text-to-speech needs to be enabled in the OS settings; on desktops Chrome/Edge/Safari all ship a usable English voice out of the box

---

## Default Settings Reference

| Setting | Value |
|---|---|
| WiFi SSID / Password | `FPVRaceOne_XXXX` / `fpvraceone` |
| Web address (Single / Client) | `http://192.168.4.1` |
| Web address (Master) | `http://192.168.5.1` |
| Min lap time | 5 seconds |
| Max laps | Infinite (0) |
| Pipeline Smoothing | Level 5 |
| Gate-1 Bootstrap | Off |
| Voice | Off |

---

## Next Steps

- **[User Guide](USER_GUIDE.md)** — Master all features
- **[Features Guide](FEATURES.md)** — In-depth technical reference
- **[Flashing Guide](FLASHING_OPTIONAL.md)** — Manual re-flash (advanced / recovery)
