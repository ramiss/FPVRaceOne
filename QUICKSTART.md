# FPVRaceOne Quick Start

The 5-minute path from unboxing to first lap. For detail on any step —
hardware, multi-node racing, firmware updates, troubleshooting — see
**[Getting Started](docs/GETTING_STARTED.md)**.

## 1. Power and connect

- Plug a USB-C power source into the FPVRaceOne; wait ~10 seconds for the AP to come up
- Place the Lap Timer at the timing gate (typically the first gate), with the stripes (front of the unit) facing towards the center of the gate (facing the drone as it passes)
  Note: make sure the timer has a clear line of sight to your browsing device.  You may need to attach it higher on the gate if there is grass or other obstacles in the way.  
- Connect your phone / laptop to WiFi **`FPVRaceOne_XXXX`** (password **`fpvraceone`**)
- Open **`http://192.168.4.1`** in any browser

## 2. Set your VTx band and channel

**Settings → Pilot Info → Band** and **Channel** to match your drone's VTx.
Auto-saves on selection.

## 3. Calibrate

1. Power on your drone with **VTx on a fixed power level** (not auto); let it warm up ~30 s
2. **Calibration** tab → **Start Calibration Wizard → Record**
3. Fly **3 passes** through the start gate (through, around, back — repeat)
4. **Stop Recording** → review the three peaks → **Apply Thresholds**

## 4. Race

**Race** tab → **Start Race** → fly! → **Stop Race** when done. The session is
saved automatically. **Download** the session JSON before unplugging if you
want to keep it — race history isn't persisted across reboots.

---

## Default Settings (Quick Reference)

| Setting | Value |
|---|---|
| WiFi | `FPVRaceOne_XXXX` / `fpvraceone` |
| Web address | `http://192.168.4.1` |
| Min lap time | 5 seconds |
| Max laps | Infinite (0) |
| Pipeline Smoothing | Level 5 |
| Gate-1 Bootstrap | Off |

---

**Next:** [Getting Started](docs/GETTING_STARTED.md) covers multi-node race
directing, OTA firmware updates, the Edit Pilot modal, the full
troubleshooting tables, and tips for race day.
