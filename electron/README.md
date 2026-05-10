# FPVRaceOne Desktop App (Electron)

Native desktop application for the FPVRaceOne Lap Timer. Same UI as the web interface, with the option to connect over USB for zero-latency local control.

The app loads `data/index.html` (the same single-page web interface that runs on the device) and provides a Node.js-side serial transport so the page can talk to the device over USB CDC instead of HTTP.

## Features

- **WiFi or USB** — toggle in-app; auto-detect the FPVRaceOne USB device
- **Same UI** — uses the device's own `data/` folder; what you see in the browser is what you see here
- **Cross-platform** — Windows, macOS, Linux

WiFi mode is HTTP + Server-Sent Events to `http://192.168.4.1`. USB mode is JSON-over-serial through `serialport`. The transport layer (`data/usb-transport.js`) abstracts both behind one API so feature code doesn't care which is active.

## Setup

```bash
cd electron
npm install
```

Installs:
- `electron` — desktop runtime
- `serialport` — native USB serial
- `electron-builder` — installer / packager

## Run

```bash
npm start
```

Loads `../data/index.html`. Edit any file under `data/` and restart with `npm start` to see changes.

## Build

```bash
npm run build:win    # Windows installer (NSIS)
npm run build -- --mac
npm run build -- --linux
```

Output goes to `electron/dist/`. The Windows installer creates a Start menu entry and a desktop shortcut.

## WiFi vs USB

| | WiFi | USB |
|--|------|-----|
| Latency | ~50–100 ms | ~10 ms |
| Setup | Connect to `FPVRaceOne_XXXX` AP | Plug in USB-C |
| Auto-detect | No (fixed IP) | Yes |
| Firmware OTA | ✅ | ❌ (requires WiFi for GitHub fetch) |
| Multi-Node race directing | ✅ (master needs the AP) | ⚠ master role only meaningful over WiFi |
| All other features | ✅ | ✅ |

## Troubleshooting

**"Cannot find FPVRaceOne device"** — confirm the device is plugged in via USB. On Windows it should appear in Device Manager as an Espressif or USB Serial device.

**"Permission denied" on serial port** — close any other program using the COM port (Arduino IDE, PuTTY, screen, …) and reconnect.

**WiFi not connecting** — make sure your computer is on the `FPVRaceOne_XXXX` network. Switch to USB mode if you can't reach the AP.

**Changes don't show up after editing `data/`** — fully quit the app (Ctrl+C in the terminal) and run `npm start` again. Electron caches HTML/JS aggressively.

**Reinstall from scratch:**
```bash
cd electron
rm -rf node_modules
npm install
```

## Keyboard Shortcuts

- **Ctrl+R** — refresh
- **F11** — toggle fullscreen
- **F12** — toggle DevTools

## License

MIT — same as the parent project.
