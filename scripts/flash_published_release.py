"""
Flash a published GitHub Release onto every connected device.

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
     (FPVRaceOne-merged.bin + FPVRaceOne-littlefs.bin).
  4. Download both into .pio/build/published/<tag>/ (cached per tag).
  5. Auto-detect ALL connected ESP32-C6 USB serial ports.
  6. For every detected device: FULL CHIP ERASE, then flash merged.bin to
     0x0 and littlefs.bin to 0x380000.
     A failure on one device does not stop the others — a per-device
     OK/FAILED summary is printed at the end and the task exits non-zero
     if any device failed.

Non-interactive use:
    set RELEASE_TAG=v0.1.2-beta.5
    pio run -t flash_published_release

WHY merged.bin AND WHY A FULL ERASE
  merged.bin is the CI-built full-flash image: bootloader (0x0), partition
  table (0x8000), boot_app0/otadata (0xE000) and the app (0x10000) in one
  blob, with the NVS gap at 0x9000 left as 0xFF.  It ends well below the
  LittleFS partition at 0x380000, so the two writes never overlap.

  Because merged.bin restores the bootloader and partition table, a full
  `erase_flash` is safe here — unlike a firmware-only flash, which would
  brick the device if the bootloader were erased first.  The erase gives a
  genuinely pristine device: NVS config wiped, both OTA slots cleared,
  coredump cleared, no stale LittleFS remnants.

  Net result is a device byte-identical to a factory unit running this
  release — a stronger guarantee than OTA, which preserves NVS and only
  rewrites the app + filesystem.
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
# partitions_two_ota_XIAO_ESP32_C6.csv, extra_script.py's FS_OFFSET, and
# the merge-bin offsets in .github/workflows/release.yml.  Keep them in
# sync if the partition table ever changes.
MERGED_OFFSET = "0x0"       # bootloader + partitions + boot_app0 + app
FS_OFFSET     = "0x380000"  # LittleFS partition (extra_script.py FS_OFFSET)

MERGED_ASSET = "FPVRaceOne-merged.bin"
FS_ASSET     = "FPVRaceOne-littlefs.bin"

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
    missing = [n for n in (MERGED_ASSET, FS_ASSET) if n not in assets]
    if missing:
        _fail(
            f"Release {tag_name} is missing required asset(s): {', '.join(missing)}.\n"
            f"Assets present: {', '.join(assets) if assets else '(none)'}\n"
            "Was the release built by .github/workflows/release.yml?  Releases\n"
            "published before the merged-image contract was added carry no\n"
            "assets at all and cannot be flashed by this task."
        )

    # Cache downloads per-tag so re-running the task on the same release is
    # near-instant after the first run.
    out_dir = REPO_ROOT / ".pio" / "build" / "published" / tag_name
    out_dir.mkdir(parents=True, exist_ok=True)
    merged_path = out_dir / MERGED_ASSET
    fs_path     = out_dir / FS_ASSET

    print(f"\n[2/4] Downloading to {out_dir.relative_to(REPO_ROOT).as_posix()}/")
    _download(assets[MERGED_ASSET], merged_path, MERGED_ASSET)
    _download(assets[FS_ASSET],     fs_path,     FS_ASSET)

    print("\n[3/4] Locating connected device(s)...")
    ports = _find_esp32_ports()
    if ports is None:
        _fail("pyserial isn't installed in this Python.  "
              "Run via PIO (`pio run -t flash_published_release`), which uses the PIO env that has pyserial.")
    if not ports:
        _fail("No ESP32 USB device detected.\n"
              "Plug the device in (USB-C cable in a USB port — not a charge-only cable) and retry.")
    print(f"      Found {len(ports)} device(s): {', '.join(ports)}")

    print(f"\n[4/4] Erasing + flashing {tag_name} on {len(ports)} device(s)...")
    print(f"      FULL CHIP ERASE (wipes NVS config, OTA slots, coredump, LittleFS)")
    print(f"      merged.bin    → {MERGED_OFFSET}   (bootloader + partitions + boot_app0 + app)")
    print(f"      littlefs.bin  → {FS_OFFSET}")

    # Erase + flash every detected device, continuing past failures so one
    # flaky cable doesn't leave the remaining nodes untouched.  Matches
    # extra_script.py's do_upload_firmware / do_upload_fs behaviour: collect
    # a per-port result, print an OK/FAILED summary, then exit non-zero if
    # any port failed so PIO still marks the task as failed.
    #
    # Erase and write are separate esptool invocations rather than
    # `write_flash -e` so a failure can be attributed to the right phase —
    # an erase that succeeds followed by a write that fails leaves a blank
    # device, which is a materially different situation to report than an
    # erase that never ran.
    def _esptool(port, args):
        return subprocess.run([
            sys.executable, "-m", "esptool",
            "--chip", "esp32c6",
            "--port", port,
            "--baud", "460800",
            "--before", "default_reset",
            "--after", "hard_reset",
        ] + args).returncode

    results = {}
    blanked = []          # erased OK but write failed → device is now empty
    for port in ports:
        print(f"\n   ── {port} ──")

        print(f"   [1/2] Erasing entire flash...")
        rc = _esptool(port, ["erase_flash"])
        if rc != 0:
            results[port] = False
            print(f"   [FAILED] erase_flash exited {rc} on {port} — device untouched, "
                  f"continuing with remaining device(s).", file=sys.stderr)
            continue

        print(f"   [2/2] Writing {MERGED_ASSET} + {FS_ASSET}...")
        rc = _esptool(port, [
            "write_flash",
            MERGED_OFFSET, str(merged_path),
            FS_OFFSET,     str(fs_path),
        ])
        results[port] = (rc == 0)
        if rc != 0:
            blanked.append(port)
            print(f"   [FAILED] write_flash exited {rc} on {port} — flash was already "
                  f"erased, so this device is now BLANK and will not boot until "
                  f"re-flashed.  Continuing with remaining device(s).", file=sys.stderr)

    ok_count   = sum(1 for v in results.values() if v)
    fail_count = len(results) - ok_count

    print()
    print(f"=== Flash Published Release Summary ({tag_name}) ===")
    for port, ok in results.items():
        print(f"  {port}: {'OK' if ok else 'FAILED'}")
    print()

    if fail_count:
        print("=" * 60)
        print(f"  PARTIAL — {ok_count} of {len(results)} device(s) now running {tag_name}")
        print("=" * 60)
        if blanked:
            print(f"  !! {len(blanked)} device(s) were ERASED but not written and are")
            print(f"     now BLANK — they will not boot until re-flashed:")
            for p in blanked:
                print(f"       {p}")
            print()
        print(f"  Re-run the task to retry.  Downloads below are cached, so a")
        print(f"  retry is fast; already-flashed devices are simply redone.")
        print()
        print(f"  Cached downloads (keep / clean as you wish):")
        print(f"    {merged_path}")
        print(f"    {fs_path}")
        print()
        sys.exit(1)

    print("=" * 60)
    print(f"  SUCCESS — {ok_count} device(s) now running {tag_name}")
    print("=" * 60)
    print(f"  Each device was fully erased and re-imaged from the published")
    print(f"  release: bootloader, partition table, app and filesystem are")
    print(f"  byte-for-byte identical to a factory unit on {tag_name}.")
    print(f"  NVS config was wiped, so devices boot with default settings.")
    print()
    print(f"  Cached downloads (keep / clean as you wish):")
    print(f"    {merged_path}")
    print(f"    {fs_path}")
    print()


if __name__ == "__main__":
    main()
