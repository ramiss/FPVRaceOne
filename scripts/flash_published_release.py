"""
Flash a published GitHub Release onto the connected device.

Why this exists:
  A local `deploy_all` builds the firmware/filesystem on your machine — same
  version string as the published release (assuming HEAD is at the tag) but
  *not* the same bytes, since the build timestamp and toolchain version
  almost always differ between local builds and CI.  When you want the
  device to carry *exactly* what an OTA upgrade would deliver (e.g. for
  pre-release acceptance testing or for resetting a test device back to
  the published baseline after some local experimentation), use this task.

Flow:
  1. Determine the GitHub repo from the `origin` remote.
  2. Query the Releases API for the latest release, or the tag in
     $RELEASE_TAG if set.
  3. Verify the release carries the two required assets
     (FPVRaceOne-firmware.bin + FPVRaceOne-littlefs.bin).
  4. Download both into .pio/build/published/<tag>/.
  5. Auto-detect the ESP32-C6 USB serial port.
  6. Flash firmware.bin to the OTA_0 entry (0x10000) and littlefs.bin to
     the LittleFS partition (0x320000).

Non-interactive use:
    set RELEASE_TAG=v0.1.2-beta.5
    pio run -t flash_published_release

Bootloader and partition table are NOT included — they live on the device
already (the OTA flow doesn't touch them either).  If the device is in a
totally bricked state, do a manual `pio run -t upload` first, then this.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

# Force line buffering so PIO's task runner flushes output as it happens.
try:
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)
except (AttributeError, ValueError):
    pass

REPO_ROOT = Path(__file__).resolve().parent.parent

# Flash layout for the seeed_xiao_esp32c6 target.  These MUST match
# extra_script.py's FS_OFFSET and platformio.ini's
# board_upload.offset_address.  Keep all three in sync if the partition
# table ever changes.
FW_OFFSET = "0x10000"   # OTA_0 entry (board_upload.offset_address)
FS_OFFSET = "0x320000"  # LittleFS partition (extra_script.py FS_OFFSET)

FW_ASSET = "FPVRaceOne-firmware.bin"
FS_ASSET = "FPVRaceOne-littlefs.bin"

# Same USB vendor IDs extra_script.py uses to identify an attached XIAO.
ESP32_VIDS = {
    0x10C4,  # Silicon Labs CP210x
    0x1A86,  # QinHeng CH340/CH341
    0x0403,  # FTDI
    0x303A,  # Espressif native USB
    0x239A,  # Adafruit
    0x2341,  # Arduino
}


def _git(cmd):
    return subprocess.run(
        ["git"] + cmd,
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
    ).stdout.strip()


def _fail(msg, code=1):
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(code)


def _find_esp32_ports():
    """Mirror of extra_script.py's port detector, kept local to avoid
    importing the PIO build script (which calls `Import('env')` at module
    load and would fail outside a SCons context)."""
    try:
        import serial.tools.list_ports
    except ImportError:
        return None  # signal "pyserial not installed"
    return [p.device for p in serial.tools.list_ports.comports() if p.vid in ESP32_VIDS]


def _get_owner_repo():
    """Parse owner/repo from the `origin` git remote URL."""
    url = _git(["remote", "get-url", "origin"])
    if not url:
        _fail("No `origin` remote configured. `git remote add origin <url>` first.")
    m = re.search(r"github\.com[:/]([^/]+)/([^/.]+?)(?:\.git)?/?$", url)
    if not m:
        _fail(f"`origin` ({url}) doesn't look like a GitHub URL.")
    return m.group(1), m.group(2)


def _fetch_release(owner, repo, tag=None):
    """Hit the Releases API for either the named tag or `latest`."""
    if tag:
        url = f"https://api.github.com/repos/{owner}/{repo}/releases/tags/{tag}"
    else:
        url = f"https://api.github.com/repos/{owner}/{repo}/releases/latest"
    req = Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "FPVRaceOne-flasher",
    })
    try:
        with urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except HTTPError as e:
        if e.code == 404:
            if tag:
                _fail(f"No release found with tag '{tag}' on {owner}/{repo}.")
            else:
                _fail(
                    f"No published releases found on {owner}/{repo}.\n"
                    "Tag and push a release first (e.g. `pio run -t publish_prerelease`)."
                )
        _fail(f"GitHub API returned HTTP {e.code} for {url}.")
    except URLError as e:
        _fail(f"Could not reach GitHub: {e}.\nCheck your internet connection.")


def _download(url, dest, label):
    print(f"  {label}: downloading...")
    req = Request(url, headers={"User-Agent": "FPVRaceOne-flasher"})
    with urlopen(req, timeout=120) as resp:
        total = int(resp.headers.get("Content-Length", 0))
        bytes_read = 0
        with open(dest, "wb") as f:
            while True:
                chunk = resp.read(64 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                bytes_read += len(chunk)
                if total:
                    pct = (bytes_read * 100) // total
                    print(f"\r    {pct}% ({bytes_read:,} / {total:,} bytes)",
                          end="", flush=True)
    if total:
        print()  # newline after the in-place progress
    print(f"  {label}: saved {bytes_read:,} bytes to {dest.name}")


def main():
    print("\n=== Flash Published Release ===\n")

    owner, repo = _get_owner_repo()
    tag = os.environ.get("RELEASE_TAG", "").strip() or None
    print(f"Repository: {owner}/{repo}")
    print(f"Target:     {tag or '(latest release)'}")
    print()

    print("[1/4] Querying GitHub...")
    release = _fetch_release(owner, repo, tag)
    tag_name = release.get("tag_name", "?")
    is_pre   = bool(release.get("prerelease"))
    print(f"      Found {tag_name}{' (pre-release)' if is_pre else ''}, "
          f"published {release.get('published_at', '?')}")

    assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}
    missing = [n for n in (FW_ASSET, FS_ASSET) if n not in assets]
    if missing:
        _fail(
            f"Release {tag_name} is missing required asset(s): {', '.join(missing)}.\n"
            f"Assets present: {', '.join(assets) if assets else '(none)'}\n"
            "Was the release built by .github/workflows/release.yml?"
        )

    # Cache downloads per-tag so re-running the task on the same release is
    # near-instant after the first run.
    out_dir = REPO_ROOT / ".pio" / "build" / "published" / tag_name
    out_dir.mkdir(parents=True, exist_ok=True)
    fw_path = out_dir / FW_ASSET
    fs_path = out_dir / FS_ASSET

    print(f"\n[2/4] Downloading to {out_dir.relative_to(REPO_ROOT).as_posix()}/")
    _download(assets[FW_ASSET], fw_path, FW_ASSET)
    _download(assets[FS_ASSET], fs_path, FS_ASSET)

    print("\n[3/4] Locating connected device...")
    ports = _find_esp32_ports()
    if ports is None:
        _fail("pyserial isn't installed in this Python.  "
              "Run via PIO (`pio run -t flash_published_release`), which uses the PIO env that has pyserial.")
    if not ports:
        _fail("No ESP32 USB device detected.\n"
              "Plug the device in (USB-C cable in a USB port — not a charge-only cable) and retry.")
    print(f"      Found: {', '.join(ports)}")

    print(f"\n[4/4] Flashing {tag_name}...")
    print(f"      firmware.bin  → {FW_OFFSET}")
    print(f"      littlefs.bin  → {FS_OFFSET}")

    for port in ports:
        print(f"\n   ── {port} ──")
        cmd = [
            sys.executable, "-m", "esptool",
            "--chip", "esp32c6",
            "--port", port,
            "--baud", "460800",
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
            FW_OFFSET, str(fw_path),
            FS_OFFSET, str(fs_path),
        ]
        result = subprocess.run(cmd)
        if result.returncode != 0:
            _fail(f"esptool exited {result.returncode} on {port}.", code=result.returncode)

    print()
    print("=" * 60)
    print(f"  SUCCESS — device(s) now running {tag_name}")
    print("=" * 60)
    print(f"  The flashed binaries are byte-for-byte identical to what an")
    print(f"  OTA `Check for Updates` would deliver for this release.")
    print()
    print(f"  Cached downloads (keep / clean as you wish):")
    print(f"    {fw_path}")
    print(f"    {fs_path}")
    print()


if __name__ == "__main__":
    main()
