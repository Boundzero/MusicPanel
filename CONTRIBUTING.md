# Contributing to MusicPanel

Thanks for your interest! A few notes to make contributions smooth.

## Development setup

- **ESP-IDF v5.3.5** (install per Espressif's docs).
- Target: `esp32s3`. Board: Guition/JCZN ESP32-4848S040.
- Build: `idf.py set-target esp32s3 && idf.py build`
- Flash + logs: `idf.py -p <PORT> flash monitor`

## Guidelines

- Keep the **BSP layer hardware-agnostic** — board-specific values belong in a board profile, not in app code. New panels should be a new board, not edits to `main/`.
- The UI is single-threaded (LVGL). Anything touching LVGL objects from another task must hold `bsp::lvgl_lock()`.
- Network calls (Music Assistant, weather) run on background tasks, never on the UI thread.
- Match the existing style: descriptive names, comments that explain *why*, minimal cleverness.

## Reporting issues

Please include:
- Board variant and where you bought it
- ESP-IDF version
- Music Assistant version and which providers are connected
- Relevant serial log lines (redact your token!)

## Firmware releases

Maintainers: after building, produce the merged image and update the installer:

```bash
idf.py merge-bin -o docs/firmware/musicpanel-merged.bin
# bump "version" in docs/manifest.json, commit both
```
