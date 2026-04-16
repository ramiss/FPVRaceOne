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
6. [Troubleshooting](#troubleshooting)

---

## Connect to FPVRaceOne

### WiFi (Default)

1. Power on your FPVRaceOne device and wait 10–15 seconds
2. On your phone or laptop, connect to the WiFi network: **`FPVRaceOne_XXXX`**  
   *(XXXX = last 4 digits of the device MAC address)*
3. Password: **`fpvraceone`**
4. Open a browser and go to: **`http://192.168.4.1`**

> Your device can connect to FPVRaceOne WiFi and maintain cellular internet simultaneously — no captive portal is used.

### USB (Optional)

1. Download the [Electron desktop app](https://github.com/LouisHitchcock/FPVGate/releases)
2. Connect the FPVRaceOne device via USB-C
3. Launch the app and select your COM port
4. All features work identically to WiFi mode with lower latency

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

In the **TTS Settings** section, choose your **Announcer Type**, **Voice**, and **Lap Announcement Format**.

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
3. Fly your drone through the gate at race speed — at least 2–3 passes
4. Click **Stop Recording**

The wizard displays the recorded RSSI signal on a chart and automatically calculates recommended **Enter** and **Exit** thresholds.

### Step 3: Review and Apply

- Review the chart — you should see clean peaks for each gate pass
- Adjust the calculated thresholds manually if needed:
  - **Enter RSSI:** 3–5 points below the observed peak
  - **Exit RSSI:** 8–10 points below Enter RSSI
- Click **"Apply Thresholds"** — values are saved immediately

**Good calibration example:**
```
Observed peak:  150
Enter RSSI:     145  (5 below peak)
Exit RSSI:      135  (10 below Enter)
```

### Step 4: Verify

Fly through the gate once more and check that the Calibration tab shows a single clean peak crossing both thresholds. Then head to the Race tab.

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
- Try switching to **V2 (RotorHazard Bessel IIR)** mode in Settings → Signal Processing for lower-latency detection

### False / Extra Laps

- Increase **Minimum Lap Time** in Settings → Race Setup
- Raise Exit RSSI by 5 points
- In V1 mode, increase **Enter Hold Samples** (Settings → Signal Processing → Detection) to require more consecutive samples before registering an entry
- Move the timer further from the flight path

### Can't Connect to Web Interface

- Make sure you are connected to the `FPVRaceOne_XXXX` WiFi network
- Navigate directly to `http://192.168.4.1`
- Try a different browser or clear the browser cache
- Check that no VPN is active on your device

### Voice Announcements Not Working

- In Settings → TTS, click **Enable Audio** and test with **Generate Audio**
- Check that your browser has not blocked autoplay audio
- Try switching to **PiperTTS** which does not require pre-recorded files

---

## Next Steps

- **[User Guide](USER_GUIDE.md)** — Master all features
- **[Features Guide](FEATURES.md)** — In-depth technical reference
- **[Flashing Guide](FLASHING_OPTIONAL.md)** — Manual re-flash (advanced / recovery)
