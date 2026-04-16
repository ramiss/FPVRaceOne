# FPVRaceOne

**Personal FPV Lap Timer — Hardware coming soon!**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A compact, self-contained RSSI-based lap timing solution for 5.8 GHz FPV drones. Perfect for personal practice sessions, small indoor tracks, and training. No transponders, no complex infrastructure — just plug in, calibrate, and fly.

---

## Screenshots

| Race Screen | Configuration |
|:-----------:|:-------------:|
| ![Race Screen](screenshots/12-12-2025/Race%20-%2012-12-2025.png) | ![Config Menu](screenshots/12-12-2025/Config%20Screen%20-%20Pilot%20Info%2012-12-2025.png) |

| Calibration Wizard — Recording | Calibration Wizard — Complete |
|:------------------------------:|:-----------------------------:|
| ![Calibration Recording](screenshots/12-12-2025/Calibration%20Wizard%20Recording%20-%2012-12-2025.png) | ![Calibration Complete](screenshots/12-12-2025/Calibration%20Wizard%20Completed%20-%2012-12-2025.png) |

---

## How It Works

FPVRaceOne uses an RX5808 video receiver module to monitor your drone's RSSI (signal strength). As you fly through the gate:

1. **Approach** — RSSI rises above the Enter threshold → crossing begins
2. **Peak** — RSSI peaks when you're closest to the gate
3. **Exit** — RSSI falls below the Exit threshold → lap time recorded

```
RSSI  │     /\
      │    /  \
      │   /    \     ← Single clean peak
Enter ├──/──────\───
      │ /        \
Exit  ├/──────────\─
      └─────────────── Time
```

The time between consecutive peaks is your lap time. Two configurable signal processing pipelines let you tune the balance between responsiveness and noise rejection for your specific environment.

---

## Key Features

### Dual Connectivity
- **WiFi Access Point** — works with any browser, no app required
- Simultaneous WiFi AP + cellular internet on mobile devices (no captive DNS)
- **USB Serial CDC** — zero-latency wired connection
- Electron desktop app for Windows / Mac / Linux

### Signal Processing — V1 and V2 Modes

Two runtime-switchable RSSI processing pipelines, selectable from the Settings page:

**V1 — FPVGate Multi-Stage Pipeline**
- Kalman filter → Median-of-3 → 7-sample moving average → EMA → step limiter
- Tunable Enter Hold Samples and Exit Confirm Samples for debounce control
- Best for noisy environments where extra smoothing reduces false laps

**V2 — RotorHazard Bessel IIR Filter**
- 2nd-order Bessel low-pass filter at selectable cutoff: **100 Hz / 50 Hz / 20 Hz**
- Single-sample gate entry and exit (trusts the filter rather than hold counters)
- Lap timestamp placed at the midpoint of the signal peak plateau for improved accuracy
- Best for clean environments where low latency is the priority

### RSSI Calibration Wizard
- Guided fly-over recording with real-time RSSI chart
- Automatic Enter / Exit threshold calculation from the recorded peak
- Visual calibration overview overlaid on the live scanner
- Pause / resume live signal view for threshold fine-tuning
- Works identically in both V1 and V2 modes

### Voice Announcements
- Pre-recorded ElevenLabs voices (4 voices included)
- PiperTTS for low-latency on-device synthesis
- Phonetic name support for accurate TTS pronunciation
- Configurable announcement formats (full, lap time only, time only)

### Race Analysis
- Real-time lap tracking with gap-to-best analysis
- Fastest lap highlighting
- Fastest 3 consecutive laps (RaceGOW format)
- Download race history to your device as JSON; re-import later
- Marshalling mode — add, remove, or edit laps post-race
- Detailed race analysis view

### Configuration & UX
- **Unsaved-changes indicator** — Save Configuration button highlights orange (with pulse animation) when there are pending changes; returns to inactive when saved
- Config backup and restore (download / import JSON)
- OTA updates for both **firmware** and **filesystem** via ElegantOTA
- Theme selector (multiple colour themes)
- Mobile-responsive web interface

### Webhooks & Integration
- HTTP webhook support for external LED controllers and integrations
- Configurable per-event triggers: race start, race stop, lap
- Gate LED control with granular enable/disable per event
- Up to 10 webhook endpoints

### Developer Tools
- Comprehensive self-test diagnostics (19 tests)
- Serial monitor built into the web UI
- USB transport abstraction layer
- Open source — MIT License

---

## Supported Bands & Frequencies

| Band | Channels (MHz) |
|------|----------------|
| **A (Boscam A)** | 5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725 |
| **B (Boscam B)** | 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866 |
| **E (Boscam E)** | 5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945 |
| **F (Fatshark)** | 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880 |
| **R (RaceBand)** | 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 |
| **L (LowBand)** | 5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621 |
| **DJI v1 25 MHz** | 5660, 5695, 5735, 5770, 5805, 5878, 5914, 5839 |
| **DJI v1 25 CE** | 5735, 5770, 5805, 5839 |
| **DJI v1 50** | 5695, 5770, 5878, 5839 |
| **DJI O3/O4 10/20** | 5669, 5705, 5768, 5804, 5839, 5876, 5912 |
| **DJI O3/O4 20 CE** | 5768, 5804, 5839 |
| **DJI O3/O4 40** | 5677, 5794, 5902 |
| **DJI O3/O4 40 CE** | 5794 |
| **DJI O4 RaceBand** | 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 |
| **HDZero RaceBand** | 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 |
| **HDZero E** | 5707 |
| **HDZero F** | 5740, 5760, 5800 |
| **HDZero CE** | 5732, 5769, 5806, 5843 |
| **Walksnail RaceBand** | 5658, 5659, 5732, 5769, 5806, 5843, 5880, 5917 |
| **Walksnail 25** | 5660, 5695, 5735, 5770, 5805, 5878, 5914, 5839 |
| **Walksnail 25 CE** | 5735, 5770, 5805, 5839 |
| **Walksnail 50** | 5695, 5770, 5878, 5839 |

---

## Quick Start

### Hardware

Pre-made and flashed hardware — link coming soon!

**[Detailed hardware setup →](docs/GETTING_STARTED.md)**

### Connect via WiFi

1. Power on the device
2. Connect your phone or laptop to the `FPVRaceOne_XXXX` network (password: `fpvraceone`)
3. Open `http://192.168.4.1` in your browser
4. Go to **Settings → Set your VTx band and channel**
5. Go to **Calibration → Run the wizard** to set RSSI thresholds
6. Press **Start** and fly!

### Connect via USB

1. Download the [Electron app from releases](https://github.com/LouisHitchcock/FPVGate/releases)
2. Connect the device via USB
3. Launch the app and select your COM port
4. All features work identically to WiFi mode

**[Complete user guide →](docs/USER_GUIDE.md)**

---

## OTA Updates

FPVRaceOne supports over-the-air updates for both firmware and filesystem via **ElegantOTA**.

1. Open `http://192.168.4.1/update` in your browser
2. **Firmware update** — upload the `.bin` from `.pio/build/seeed_xiao_esp32c6/firmware.bin`
3. **Filesystem update** — build the filesystem image first, then upload:

```bash
pio run --target buildfs --environment seeed_xiao_esp32c6
# Output: .pio/build/seeed_xiao_esp32c6/littlefs.bin
```

Select **Filesystem** in the ElegantOTA UI, upload `littlefs.bin`, and the device will reboot with updated web files.

> Both firmware and filesystem must be updated after a release that changes either. They are independent uploads.

---

## Project Status

**Product:** FPVRaceOne  
**Platform:** Seeed XIAO ESP32-C6  
**License:** MIT  
**Status:** Stable Beta — actively maintained

### Changelog

**v1.1.0 (Current)**
- Renamed product to **FPVRaceOne**
- Added **V1 / V2 switchable signal processing** — runtime toggle between FPVGate multi-stage pipeline and RotorHazard Bessel IIR filter
- Added **Bessel filter cutoff selector** — 100 Hz / 50 Hz / 20 Hz for V2 mode
- Added **configurable detection parameters** — Enter Hold Samples and Exit Confirm Samples (V1), now persisted to config
- Added **V2 midpoint timestamping** — lap time recorded at the centre of the signal peak plateau
- Improved **ADC scaling** — corrected denominator for 6 dB attenuation range (avoids RSSI inflation)
- Fixed **Kalman filter Q/R inversion** — process noise and measurement noise were swapped; corrected for 29× improvement in filter responsiveness
- Fixed **RSSI display off-by-one** — `getRssi()` was returning 100-sample-old data
- Removed `vTaskDelay(50)` from debug logger — was stalling the RSSI loop by 50 ms on every threshold crossing event
- Increased **SSE update rate** from 5 Hz to 20 Hz for more responsive live RSSI display
- Moved webhook HTTP calls to Core 0 — prevents synchronous POST (up to 300 ms) from blocking the RSSI loop on Core 1
- Added **Save Configuration dirty-state indicator** — button highlights orange with pulse animation when there are unsaved changes; returns to inactive after successful save
- Added `enterHoldSamples` and `exitConfirmSamples` to firmware config and settings UI (previously UI-only, not persisted)

**v1.0.0**
- Forked and ported from [FPVGate](https://github.com/LouisHitchcock/FPVGate) v1.4.1 by LouisHitchcock
- Ported to Seeed XIAO ESP32-C6 hardware
- Added all popular analog and digital VTx bands
- Added USB CDC transport with Electron desktop app support
- Removed SD card track management (no SD card on this hardware)
- Removed captive DNS — allows simultaneous WiFi AP + cellular internet on mobile
- Fixed frequency stuck on RaceBand Ch 1
- Hides hardware features (LED, VBat, SD) when pins are not defined for the target board

---

## Credits

FPVRaceOne is derived from [FPVGate](https://github.com/LouisHitchcock/FPVGate) v1.2.0 by LouisHitchcock, which is itself a heavily modified fork of [PhobosLT](https://github.com/phobos-/PhobosLT) by phobos-. The original project provided the foundation for RSSI-based lap timing on ESP32.

Signal processing concepts and Bessel IIR filter coefficients for V2 mode are derived from [RotorHazard](https://github.com/RotorHazard/RotorHazard).

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

*Built for pilots, by pilots.*
