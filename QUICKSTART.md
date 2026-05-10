# FPVRaceOne Quick Start Guide

Get your FPVRaceOne lap timer up and running in minutes!

## What You Need

A pre-flashed FPVRaceOne lap timer (ETSY link coming soon) and a phone, tablet, or laptop with WiFi.

## First Connection

1. **Power on** the device — wait ~10 seconds for the AP to come up
2. **Connect to WiFi**:
   - Network: `FPVRaceOne_XXXX` (XXXX = last 4 digits of MAC address)
   - Password: `fpvraceone`
3. **Open** `http://192.168.4.1` in any browser 
4. **Configure** (Settings → Pilot Info):
   - Set **Band & Channel** to match your VTx
   - Set **Pilot Name**, **Callsign**, **Phonetic Name**
   - Pick a **Pilot Color** and **Theme**
   - Tap **Save Configuration** — the orange button confirms there are unsaved changes

## Calibrate (Calibration Tab)

The wizard does the math for you. Manual threshold tweaking is rarely necessary.

1. Power on your drone and let the VTx warm up for 30 seconds
2. Tap **Start Calibration Wizard → Record**
3. Fly **3 passes** through the gate at race speed
4. Tap **Stop Recording**
5. The wizard auto-detects the three peaks and calculates Enter / Exit thresholds
6. If a peak-spread warning appears, re-fly — equal-height peaks calibrate better
7. Tap **Apply Thresholds** — saved instantly

Manual override: drag any peak marker, or use the Enter / Exit sliders on the Calibration tab and **Save RSSI Thresholds**.

## Race! (Race Tab)

1. Tap **Start Race** — listen for the countdown and start beep
2. Fly through the gate — first pass is **Gate 1** (hole shot), subsequent passes are **Lap 1**, **Lap 2**, …
3. Live stats update on every pass (fastest lap, median, best 3, fastest 3 consecutive)
4. Tap **Stop Race** — the race is saved automatically

## Default Settings

| Setting | Value |
|---------|-------|
| WiFi SSID / Password | `FPVRaceOne_XXXX` / `fpvraceone` |
| Web address | `http://192.168.4.1` |
| Min lap time | 5 seconds |
| Max laps | Infinite (0) |
| Pipeline Smoothing | Level 5 (upstream FPVGate default) |
| Gate-1 Bootstrap | Off |

## Multi-Node Racing (Up to 8 Devices)

Network multiple FPVRaceOne timers together for head-to-head racing — no router, no extra hardware. One device runs in **Master** mode (race director) and up to **7 Clients** join its WiFi.

### Setup

**Master**
1. **On the master device** — Settings → Multi-Node Timing → Node Mode = `Master` → **Apply & Reboot**. Note the AP name shown on screen (e.g. `FPVRaceOne_AB12CD`)
2. Login to the master device at `http://192.168.5.1`
3. Monitor the connection of other nodes on the Race screen.
4. Start Race to syncronize all nodes and receive race data.

**Client**
1. **On each client device** — Settings → Multi-Node Timing → Node Mode = `Client` → tap **Scan** to discover masters in range (or paste the master SSID manually) → **Apply & Reboot**
2. Make sure your frequency and channel are selected and that you have calibrated your RSSI signal detection.
3. The master will start and stop the race.  
4. View master race data by logging into the master node or stay on your own node to watch your stats.

### Race

- The **master's** Race tab shows a card per pilot with live state (● Running, ○ Stopped, ⚠ DNF), lap count, and last lap
- Tap **Start All** — every client starts simultaneously
- Tap **Stop All** — every client stops cleanly
- A pilot who taps **Stop** locally during a master race shows up as **DNF** on the director's screen; the rest of the heat continues

### Solo Practice Override

Each client has an **Ignore Race Director Start/Stop if already racing** toggle. Turn it on to keep practising while a director runs heats on the rest of the field — Start All from the master won't reset your local race. Note the master can kick nodes to allow others to connect.

## Firmware Updates (OTA)

The device updates itself from GitHub Releases — no PlatformIO needed.

1. **Settings → Firmware Update** → enter your home WiFi credentials (one-time)
2. Tap **Check for Updates** — the device joins your home network briefly, queries GitHub, returns to AP mode
3. If a newer release is available, tap **Update Now**. The device flashes filesystem + firmware and reboots once

Updates are blocked during a race; failed downloads keep the previous firmware.

## Tips

- **Race History** — Races are not saved on the device. Download from memory as JSON for import later.
- **Marshalling Mode** — Add or edit laps after a race finishes (Race History → expand a race → edit)
- **Config Backup** — Settings footer has Download / Import buttons (good idea before major firmware updates)
- **USB / Electron app** — Available from the [Releases page](https://github.com/ramiss/FPVRaceOne/releases/latest) for zero-latency local control

## Troubleshooting

### Can't find WiFi network
- Check the device is powered (USB-C) and wait 10–15 s after power-on
- Look for `FPVRaceOne_` followed by 4 hex characters

### Missing laps
- Lower **Enter RSSI** by 5 points, or rerun the wizard with a cleaner 3-pass flight
- Make sure the VTx has warmed up at least 30 seconds
- Drop **Pipeline Smoothing** by one notch (Settings → Signal Processing) to reduce signal lag

### Too many false laps
- Raise **Exit RSSI** by 3–5 points
- Increase **Min Lap Time** to 8–10 seconds
- Raise **Pipeline Smoothing** by one notch for extra noise rejection
- Move the timer further from the flight path

### First lap missed when drone is parked in the gate at race start
- Enable **Gate-1 Bootstrap** in Settings → Signal Processing

## Need Help?

- Full documentation: [User Guide](docs/USER_GUIDE.md), [Features](docs/FEATURES.md)
- Issues: [GitHub Issues](https://github.com/ramiss/FPVRaceOne/issues)

---

**Happy Flying!**
