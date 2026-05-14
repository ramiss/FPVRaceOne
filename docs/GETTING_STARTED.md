# Getting Started with FPVRaceOne

Complete guide to setting up and configuring your FPVRaceOne lap timer.

**Navigation:** [Home](../README.md) | [User Guide](USER_GUIDE.md) | [Features](FEATURES.md)

---

## Table of Contents

1. [Connect to FPVRaceOne](#connect-to-fpvraceone)
2. [Configure Your VTx Frequency](#configure-your-vtx-frequency)
3. [Set Pilot Information](#set-pilot-information)
4. [Run the Calibration Wizard](#run-the-calibration-wizard)
5. [First Race](#first-race)
6. [Multi-Node Racing (Optional)](#multi-node-racing-optional)
7. [Troubleshooting](#troubleshooting)

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
2. Click **Record** in the wizard
3. Fly your drone through the gate at race speed — **exactly 3 passes** is ideal
4. Click **Stop Recording**

The wizard auto-detects the three highest peaks and overlays them on the chart. If detection misses one, drag a marker manually.

### Step 3: Review the Peak-Spread Check

If the three peaks differ in height by more than ~15 %, the wizard shows a warning recommending you re-fly. Equal-height peaks calibrate better — re-flying takes 30 seconds and is almost always worth it. If the spread is acceptable, just continue.

### Step 4: Apply

The wizard calculates thresholds with conservative safety margins:

- **Enter RSSI** ≈ 95 % of the *weakest* of the three peaks (~5 % headroom for lap-to-lap variation)
- **Exit RSSI** = Enter − 7 (tight enough for tiny-whoop tracks where gates are close together; raised above the recording's noise floor if needed)

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

### Set Up Each Client

1. On the next device, open **Settings → Multi-Node Timing**
2. Set **Node Mode** = `Client`
3. Tap **Scan** to discover masters in range, or paste the master's SSID into **Master SSID**
4. Tap **Apply Multi-Node & Reboot**
5. After reboot, the client will join the master's WiFi automatically

### Race

- On the **master**, every connected client appears as a card on the Race tab with the pilot's name, lap count, and live state (● Running, ○ Stopped, ⚠ DNF)
- Tap **Start All** to start every pilot at once; tap **Stop All** to end the heat
- Each client also keeps its own local Start / Stop — useful for a pilot to drop out mid-heat (their card shows **DNF** on the master, the heat keeps going)

### Tip — Solo Practice During a Heat

If you want to keep practising while a director is running heats on the other pilots, enable **"Ignore Race Director Start/Stop if already racing"** on your client (Settings → Multi-Node). Now a director's Start All won't reset your practice run.

**[Full multi-node guide →](USER_GUIDE.md#multi-node-timing)**

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

- In Settings → TTS, click **Enable Voice** and then **Test Voice** to hear a sample announcement
- Check that your browser has not blocked autoplay audio (Chrome and Safari often require an initial user interaction on the page before audio plays)
- The announcement is spoken by the **browser's** built-in voice (Web Speech API), so the voice depends on the device the web UI is open on. On phones, system text-to-speech needs to be enabled in the OS settings; on desktops Chrome/Edge/Safari all ship a usable English voice out of the box

---

## Next Steps

- **[User Guide](USER_GUIDE.md)** — Master all features
- **[Features Guide](FEATURES.md)** — In-depth technical reference
- **[Flashing Guide](FLASHING_OPTIONAL.md)** — Manual re-flash (advanced / recovery)
