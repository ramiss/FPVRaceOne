# FPVRaceOne Tools

Python utilities for generating the pre-recorded ElevenLabs voice files that ship in the LittleFS image.

These scripts are needed only by maintainers regenerating the voice pack — end users should never have to run them. The Seeed XIAO ESP32-C6 build has no SD card, so audio is bundled into `data/sounds_<voice>/` and flashed as part of the filesystem image.

## Prerequisites

```bash
pip install -r ../requirements.txt
```

You'll also need an active **ElevenLabs API** subscription and an API key.

## Scripts

| Script | Purpose |
|--------|---------|
| `generate_voice_files.py` | Interactive — generates one voice pack, prompting for the API key |
| `generate_voice_files_auto.py` | Non-interactive — reads the API key from environment / config |
| `generate_all_voices.py` | Generates all four voices (Sarah, Rachel, Adam, Antoni) in one run |
| `regenerate_audio.py` | Scans existing folders and only regenerates missing / corrupted files (interactive) |
| `regenerate_audio_auto.py` | Non-interactive variant of the above, useful for CI |
| `upload_sounds_to_sd.py` | Legacy — uploads generated files to an ESP32 SD card. Not used on the current C6 hardware (kept for reference only) |

## Voice File Layout

Each voice produces a folder under `data/`:

```
data/sounds_<voice_name>/
├── gate_1.mp3           # "Gate 1"
├── lap_1.mp3            # "Lap 1"
├── lap_2.mp3            # "Lap 2"
...
├── number_0.mp3         # "zero"
├── number_1.mp3         # "one"
...
├── point.mp3            # "point"
└── seconds.mp3          # "seconds"
```

Approximate sizes:
- ~1–2 MB per voice pack uncompressed
- Full voice generation pass takes a few minutes per voice depending on ElevenLabs latency

## After Regenerating

The voice files are part of the LittleFS image. To get them onto the device:

```bash
# Build the filesystem image including the new audio
pio run --target buildfs --environment seeed_xiao_esp32c6
# Output: .pio/build/seeed_xiao_esp32c6/littlefs.bin
```

Then either flash directly with `pio run -t uploadfs -e seeed_xiao_esp32c6`, or attach `littlefs.bin` to a GitHub Release as `FPVRaceOne-littlefs.bin` so the in-app updater picks it up.

## Troubleshooting

**`ModuleNotFoundError`** — install dependencies: `pip install -r ../requirements.txt`

**Voice generation timeout** — check your ElevenLabs API key and that the account has remaining quota

**Generated files sound robotic / wrong** — the runtime `PiperTTS` engine produces synthesis on-device and is independent of these pre-recorded files; for that voice, see `data/voices/` (Piper models) rather than this tool chain
