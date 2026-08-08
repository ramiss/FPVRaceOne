"""
Build and upload the RX5808 emulator firmware to the attached WROOM-32.

Why this is a separate task rather than an env in the product platformio.ini:
the emulator is bench equipment for a different board, and it must never be
reachable from a normal `pio run` or end up inside a release merged.bin.

PORT SELECTION
  With both rig boards plugged in there are two ESP32 serial ports, and
  uploading the emulator sketch to the FPVRaceOne unit would be an annoying
  mistake to make.  They are distinguishable by USB vendor ID:

    XIAO ESP32-C6  -> 0x303A  (Espressif native USB CDC, no bridge chip)
    WROOM-32 kit   -> 0x10C4 / 0x1A86 / 0x0403  (CP210x, CH340, FTDI bridge)

  So any non-Espressif-VID ESP32 port is the emulator.  Override with
  EMULATOR_PORT if the heuristic guesses wrong on your hardware.
"""

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT    = Path(__file__).resolve().parent.parent
EMULATOR_DIR = REPO_ROOT / "tools" / "rx5808-emulator"

# USB-serial bridge chips — these indicate a devkit-style board (the emulator).
BRIDGE_VIDS = {
    0x10C4,  # Silicon Labs CP210x
    0x1A86,  # QinHeng CH340/CH341
    0x0403,  # FTDI
}
# Espressif native USB — the XIAO C6 running FPVRaceOne.  Never upload here.
NATIVE_ESP_VID = 0x303A


def _fail(msg, code=1):
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(code)


def _vid_of(device):
    """USB vendor ID for a port name, or None if unknown."""
    try:
        import serial.tools.list_ports
    except ImportError:
        return None
    for p in serial.tools.list_ports.comports():
        if p.device.upper() == device.upper():
            return p.vid
    return None


def find_emulator_port():
    override = os.environ.get("EMULATOR_PORT", "").strip()
    if override:
        # The override used to bypass every safety check.  Pointing it at an
        # FPVRaceOne unit would flash ESP32 (WROOM) firmware at an ESP32-C6 —
        # a different architecture entirely, and the emulator project's
        # platformio.ini has no idea it is talking to the wrong chip.  Every
        # client unit is also a 0x303A device, so this is easy to get wrong
        # with several boards on the bench.
        vid = _vid_of(override)
        if vid == NATIVE_ESP_VID:
            _fail(f"EMULATOR_PORT={override} is an Espressif native-USB device "
                  f"(VID 0x{NATIVE_ESP_VID:04X}).\n"
                  f"That is an ESP32-C6 — an FPVRaceOne master or client, NOT "
                  f"the emulator.\n"
                  f"The emulator is a WROOM-32 behind a CP210x/CH340/FTDI "
                  f"bridge chip.\n"
                  f"Refusing to upload ESP32 firmware to a C6.")
        if vid is None:
            print(f"      WARNING: {override} is not a recognised USB serial "
                  f"port; cannot verify it is not an ESP32-C6.")
        print(f"      Using EMULATOR_PORT override: {override}")
        return override

    try:
        import serial.tools.list_ports
    except ImportError:
        _fail("pyserial not available.  Run this through PlatformIO "
              "(`pio run -t upload_emulator`), which uses the PIO env.")

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        _fail("No serial ports found.  Is the emulator plugged in?")

    bridged = [p for p in ports if p.vid in BRIDGE_VIDS]
    native  = [p for p in ports if p.vid == NATIVE_ESP_VID]

    print("      Ports seen:")
    for p in ports:
        tag = ""
        if p.vid in BRIDGE_VIDS:
            tag = "  <- bridge chip (emulator candidate)"
        elif p.vid == NATIVE_ESP_VID:
            # Master AND every client are ESP32-C6 on this VID, so there are
            # usually several.  Naming the serial number (which is the device
            # MAC) makes it obvious which unit each one is.
            sn = f"  SER={p.serial_number}" if p.serial_number else ""
            tag = f"  <- ESP32-C6, EXCLUDED (FPVRaceOne master/client){sn}"
        vid = f"{p.vid:04X}" if p.vid else "????"
        print(f"        {p.device:10s} VID={vid}  {p.description}{tag}")

    if len(bridged) == 1:
        return bridged[0].device

    if not bridged:
        _fail("No CP210x/CH340/FTDI port found — that is what a WROOM-32 devkit\n"
              "presents.  Only Espressif native-USB ports were seen, which are\n"
              f"FPVRaceOne units.  Set EMULATOR_PORT=COMx to force a port.\n"
              f"({len(native)} native-USB port(s) detected.)")

    _fail("Multiple bridge-chip ports found: "
          + ", ".join(p.device for p in bridged)
          + "\nSet EMULATOR_PORT=COMx to choose one.")


def main():
    print("\n=== Upload RX5808 Emulator ===\n")

    if not (EMULATOR_DIR / "platformio.ini").is_file():
        _fail(f"Emulator project not found at {EMULATOR_DIR}")

    print("[1/2] Locating emulator board...")
    port = find_emulator_port()
    print(f"      Target: {port}\n")

    print("[2/2] Building and uploading...")
    cmd = [
        sys.executable, "-m", "platformio", "run",
        "-t", "upload",
        "-d", str(EMULATOR_DIR),
        "--upload-port", port,
    ]
    result = subprocess.run(cmd)
    if result.returncode != 0:
        _fail(f"platformio exited {result.returncode}", code=result.returncode)

    print()
    print("=" * 60)
    print(f"  SUCCESS — emulator firmware running on {port}")
    print("=" * 60)
    print("  Next: validate the analog path before trusting any timing.")
    print("    cd tools/timing-harness")
    print(f"    python harness.py --emu {port} --dut <FPVRaceOne port> level --value 200")
    print()
    print("  Or use the GUI:  python tools/timing-harness/gui.py")
    print()


if __name__ == "__main__":
    main()
