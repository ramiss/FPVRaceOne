# FPVRaceOne Quick Start Guide

Get your FPVRaceOne lap timer up and running in minutes!

## What You Need

1. Purchase a pre-flashed FPVRaceOne Personal Lap Timer (link coming soon)

2. **Power on** your FPVRaceOne device

3. **Connect to WiFi**:
   - Network: `FPVRaceOne_XXXX` (XXXX = last 4 digits of MAC address)
   - Password: `fpvraceone`

4. **Open web interface**:
   - Go to: `http://192.168.4.1`

4. **Configure your settings** (Configuration tab):
   - Set your **Band & Channel** to match your VTx (default: RaceBand 8)
   - Set **Pilot Name** (for backend)
   - Set **Pilot Callsign** (short name for UI)
   - Set **Phonetic Name** (for TTS pronunciation)
   - Choose a **Pilot Color**
   - Select your **Theme** (23 options available!)

5. **Calibrate** (Calibration tab):
   - Power on your drone and wait 30 seconds
   - Place drone ~3-6 feet away
   - Watch the RSSI graph
   - Set **Enter RSSI** 2-5 points below peak
   - Set **Exit RSSI** 8-10 points below Enter
   - Click **Save RSSI Thresholds**

6. **Start Racing!** (Race tab):
   - Click **Start Race**
   - Fly through the gate
   - Laps recorded automatically
   - View analysis with bar charts
   - Click **Stop Race** when done

## Default Settings

- **WiFi**: `FPVRaceOne_XXXX` / `fpvraceone`
- **Web Address**: `http://192.168.4.1`
- **Minimum Lap Time**: 5 seconds
- **Lap Count**: Infinite (0)
- **Theme**: Material Oceanic

## Tips

- **Test Voice** - Use "Test Voice" button to hear TTS pronunciation
- **Download Config** - Backup your settings as JSON
- **Race History** - Races are not saved on this hardware.  Go to the Race History tab and download the race after each session.  You can import any race for analysis later.
- **Visual Analysis** - Lap history and fastest round charts show after each race

## Troubleshooting

### Can't find WiFi network
- Check device is powered (USB-C power)
- Wait 10 seconds after power-on
- Look for `FPVRaceOne_` followed by 4 characters

### Missing laps
- Lower **Enter RSSI** threshold by 5 points
- Ensure VTx warmed up (wait 30 seconds)
- Verify Band/Channel matches your drone

### Too many false laps
- Increase **Minimum Lap Time** to 8-10 seconds
- Raise **Exit RSSI** threshold by 3-5 points
- Move timer further from flight path

## Need Help?

- 📖 Full documentation: [README.md](README.md)

---

**Happy Flying! 🚁**
