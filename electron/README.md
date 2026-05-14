# FPVRaceOne Desktop App (Electron)

> **Status: experimental / not actively maintained.**
>
> This Electron wrapper was scaffolded against an earlier USB-CDC protocol
> that is **not currently active in the firmware**. The C6 product
> configuration uses the WiFi web UI for all racing, calibration, and
> configuration — USB-C is used for power and manual flashing only.
>
> The code in this folder is preserved so the USB transport path can be
> re-enabled in a future hardware revision without rebuilding the project
> structure from scratch, but it should **not** be presented to end users
> as a supported way to use FPVRaceOne today.
>
> Use the web UI at `http://192.168.4.1` (or `http://192.168.5.1` in
> Master mode) instead.

## What it was intended to do

Load `data/index.html` (the same single-page web interface that runs on the
device) inside Electron, and have it talk to the device over USB Serial CDC
via a Node.js-side `serialport` bridge. The transport abstraction in
`data/usb-transport.js` was meant to make WiFi and USB indistinguishable to
the rest of the front-end code.

## Reviving this path (developer notes)

1. Re-enable the USB CDC command handler in `lib/USB/usb.cpp` end-to-end
   testing and confirm the JSON command set in `processCommand()` still
   matches what `data/usb-transport.js` emits.
2. Sanity-check that the firmware's WiFi-driven flows (multi-node,
   OTA, SSE keepalives) still operate when the same device is being
   driven over USB simultaneously.
3. Update the install/run steps below as needed.

## Install / run (legacy)

```bash
cd electron
npm install      # electron, serialport, electron-builder
npm start
```

## License

MIT — same as the parent project.
