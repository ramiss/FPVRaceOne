# FPVRaceOne Tools

> **Status: legacy / not used by the current build.**
>
> The Python scripts in this directory pre-generate MP3 voice files for an
> earlier TTS pipeline that fed lap announcements into the device via
> LittleFS. The **current FPVRaceOne firmware does not ship any audio files**
> and the on-device PiperTTS path is not wired up either — lap announcements
> are spoken by the **browser's** built-in Web Speech API (whatever voice
> your phone or laptop's OS provides). These scripts are preserved for
> reference in case a future hardware revision adds enough flash / SD
> capacity to bundle pre-recorded audio again.

If you're a current user looking to change voices, do it in your phone or
laptop's accessibility / TTS settings — there is nothing to configure on
the device side.

## What the scripts do (historical)

| Script | Purpose |
|--------|---------|
| `generate_voice_files.py` | Interactive — generates one voice pack using the ElevenLabs API |
| `generate_voice_files_auto.py` | Non-interactive variant |
| `generate_all_voices.py` | Generates all four bundled voices (Sarah, Rachel, Adam, Antoni) in one run |
| `regenerate_audio.py` | Scans existing folders and re-generates only missing files |
| `regenerate_audio_auto.py` | Non-interactive variant |
| `upload_sounds_to_sd.py` | Uploads a generated pack to an ESP32 SD card. **Not applicable to the C6 build** (no SD card) |

The expected output folder layout was:

```
data/sounds_<voice>/
├── gate_1.mp3           # "Gate 1"
├── lap_1.mp3 … lap_50.mp3
├── number_0.mp3 … number_99.mp3
├── point.mp3            # "point"
└── seconds.mp3          # "seconds"
```

Roughly 1–2 MB per voice pack.

## Prerequisites (if you do run them)

```bash
pip install -r ../requirements.txt
```

Plus an active ElevenLabs API subscription and an API key.

## Resurrecting this path

Should a future hardware revision add the flash or SD space to host audio
files again, the steps would be:

1. Regenerate (or restore) `data/sounds_<voice>/` directories
2. Build the filesystem image (`pio run -t buildfs`) — this would push the
   image well past the current 917 504-byte LittleFS partition
3. Increase the LittleFS partition size in `partitions_two_ota_XIAO_ESP32_C6.csv`
4. Re-enable the ElevenLabs / Piper code paths in `data/audio-announcer.js`
   (the dropdown is currently hidden via `style="display:none"`)
