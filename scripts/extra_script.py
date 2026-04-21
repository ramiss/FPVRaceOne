Import("env")
import subprocess
import os
from pathlib import Path

# ── Shared helpers ────────────────────────────────────────────────────────────

ESP32_VIDS = {
    0x10C4,  # Silicon Labs CP210x
    0x1A86,  # QinHeng CH340/CH341
    0x0403,  # FTDI
    0x303A,  # Espressif native USB
    0x239A,  # Adafruit
    0x2341,  # Arduino
}

def find_esp32_ports():
    try:
        import serial.tools.list_ports
        ports = [p.device for p in serial.tools.list_ports.comports() if p.vid in ESP32_VIDS]
        if not ports:
            all_ports = [p.device for p in serial.tools.list_ports.comports()]
            print(f"  No known ESP32 USB devices found. All ports: {all_ports or 'none'}")
        return ports
    except ImportError:
        print("  ERROR: pyserial not available - cannot auto-detect ports")
        return []

def run_esptool(env, port, args):
    python = env.subst("$PYTHONEXE")
    cmd = [
        python, "-m", "esptool",
        "--chip", "esp32c6",
        "--port", port,
        "--baud", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
    ] + args
    result = subprocess.run(cmd)
    return result.returncode == 0

def print_summary(label, results):
    print(f"\n=== {label} Summary ===")
    for port, ok in results.items():
        print(f"  {port}: {'OK' if ok else 'FAILED'}")
    print()

# ── Core logic (return bool) ──────────────────────────────────────────────────

def do_connect_wifi(env):
    script = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "connect_wifi.ps1")
    result = subprocess.run(
        ["powershell", "-ExecutionPolicy", "Bypass", "-File", script],
        check=False
    )
    return result.returncode == 0


FS_OFFSET    = "0x320000"  # spiffs partition in partitions_two_ota_XIAO_ESP32_C6.csv
FS_SIZE      = 0xE0000     # 917504 bytes
FS_BLOCK     = 4096
DISK_VERSION = (2 << 16) | 1  # LittleFS 2.1

def build_littlefs_image(env, fs_image):
    try:
        from littlefs import LittleFS
    except ImportError:
        print("  ERROR: littlefs-python not available in PlatformIO environment")
        return False

    data_dir = Path(env.subst("$PROJECT_DATA_DIR"))
    if not data_dir.is_dir():
        print(f"  ERROR: data directory not found at {data_dir}")
        return False

    print(f"  Building LittleFS image from {data_dir}...")
    block_count = FS_SIZE // FS_BLOCK
    fs = LittleFS(
        block_size=FS_BLOCK,
        block_count=block_count,
        read_size=1,
        prog_size=1,
        cache_size=FS_BLOCK,
        lookahead_size=32,
        block_cycles=500,
        name_max=64,
        disk_version=DISK_VERSION,
        mount=True,
    )

    for item in sorted(data_dir.rglob("*")):
        rel = item.relative_to(data_dir).as_posix()
        if item.is_dir():
            fs.makedirs(rel, exist_ok=True)
        else:
            if item.parent != data_dir:
                fs.makedirs(item.relative_to(data_dir).parent.as_posix(), exist_ok=True)
            with fs.open(rel, "wb") as dest:
                dest.write(item.read_bytes())

    Path(fs_image).write_bytes(fs.context.buffer)
    print(f"  Filesystem image written to {fs_image}")
    return True

def do_upload_fs(env):
    fs_image = os.path.join(env.subst("$BUILD_DIR"), "littlefs.bin")
    if not build_littlefs_image(env, fs_image):
        return False

    ports = find_esp32_ports()
    if not ports:
        return False

    print(f"\nFound {len(ports)} device(s): {', '.join(ports)}")
    print("Uploading filesystem to all...\n")

    results = {}
    for port in ports:
        print(f"── {port} ──────────────────────────────")
        results[port] = run_esptool(env, port, ["write_flash", FS_OFFSET, fs_image])

    print_summary("Filesystem Upload", results)
    return all(results.values())

def do_upload_firmware(env):
    build_dir = env.subst("$BUILD_DIR")
    fw_dir    = env.subst("$FRAMEWORK_DIR")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware   = os.path.join(build_dir, "firmware.bin")
    boot_app0  = os.path.join(fw_dir, "tools", "partitions", "boot_app0.bin")

    for f, label in [(bootloader, "bootloader"), (partitions, "partitions"), (firmware, "firmware")]:
        if not os.path.exists(f):
            print(f"  ERROR: {label} not found at {f} — run 'Build' first.")
            return False

    ports = find_esp32_ports()
    if not ports:
        return False

    print(f"\nFound {len(ports)} device(s): {', '.join(ports)}")
    print("Uploading firmware to all...\n")

    flash_args = [
        "write_flash", "-z",
        "--flash_mode", "dio",
        "--flash_freq", "80m",
        "--flash_size", "detect",
        "0x0000", bootloader,
        "0x8000", partitions,
    ]
    if os.path.exists(boot_app0):
        flash_args += ["0xE000", boot_app0]
    else:
        print(f"  WARNING: boot_app0.bin not found at {boot_app0}, skipping")
    flash_args += ["0x10000", firmware]

    results = {}
    for port in ports:
        print(f"── {port} ──────────────────────────────")
        results[port] = run_esptool(env, port, flash_args)

    print_summary("Firmware Upload", results)
    return all(results.values())

# ── Individual targets ────────────────────────────────────────────────────────

def connect_wifi(source, target, env):
    do_connect_wifi(env)

env.AddCustomTarget(
    name="connect_wifi",
    dependencies=None,
    actions=connect_wifi,
    title="Connect WiFi Adapters",
    description="Connect Windows WiFi adapters to ESP32 nodes"
)

def upload_fs_all(source, target, env):
    do_upload_fs(env)

env.AddCustomTarget(
    name="upload_fs_all",
    dependencies=None,
    actions=upload_fs_all,
    title="Upload Filesystem (All Devices)",
    description="Build littlefs.bin (if needed) then flash to every connected ESP32"
)

def upload_firmware_all(source, target, env):
    do_upload_firmware(env)

env.AddCustomTarget(
    name="upload_firmware_all",
    dependencies=None,
    actions=upload_firmware_all,
    title="Upload Firmware (All Devices)",
    description="Flash firmware.bin to every connected ESP32"
)

# ── Deploy all (FS → Firmware → WiFi) ────────────────────────────────────────

def deploy_all(source, target, env):
    print("\n" + "="*50)
    print("STEP 1/3: Upload Filesystem")
    print("="*50)
    if not do_upload_fs(env):
        print("\n[STOPPED] Filesystem upload failed.")
        return

    print("\n" + "="*50)
    print("STEP 2/3: Upload Firmware")
    print("="*50)
    if not do_upload_firmware(env):
        print("\n[STOPPED] Firmware upload failed.")
        return

    print("\n" + "="*50)
    print("STEP 3/3: Connect WiFi Adapters")
    print("="*50)
    if not do_connect_wifi(env):
        print("\n[STOPPED] WiFi connect failed.")
        return

    print("\n" + "="*50)
    print("Deploy complete.")
    print("="*50 + "\n")

env.AddCustomTarget(
    name="deploy_all",
    dependencies=["$BUILD_DIR/firmware.bin"],
    actions=deploy_all,
    title="Deploy All (FS + Firmware + WiFi)",
    description="Upload filesystem, upload firmware, connect WiFi — stops on any error"
)
