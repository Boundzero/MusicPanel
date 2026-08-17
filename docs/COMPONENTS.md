# Component Architecture

MusicPanel is built as independent ESP-IDF components under `components/`, one
per responsibility from `SOFTWARE_ARCHITECTURE.md`. Each component exposes a
narrow public header in its `include/` directory and hides everything else. The
`main` component wires them together and owns no logic of its own.

The golden rule: **anything hardware-, panel-, or resolution-specific lives in
`bsp`.** Everything above it is written against pixels and abstractions, never
against a particular board.

## Status

| Component        | Responsibility                                              | Status        | Phase |
|------------------|-------------------------------------------------------------|---------------|-------|
| `bsp`            | Board bring-up: display, touch, backlight, LVGL runtime     | **Implemented** | 1 |
| `logging`        | Project logging facade over esp_log                         | **Implemented** | 1 |
| `network`        | Wi-Fi provisioning + connection management                  | Planned       | 1 |
| `ota`            | A/B OTA update flow                                         | Planned       | 1 |
| `settings`       | NVS-backed persistent settings (single source of truth)     | Planned       | 1–5 |
| `display_manager`| Owns screen lifecycle, brightness, screensaver              | Planned       | 4 |
| `layout_engine`  | Breakpoint/aspect-ratio layout profiles                     | Planned       | 4 |
| `theme_engine`   | Runtime colors, fonts, icon sets                            | Planned       | 4 |
| `navigation`     | Three-mode navigation: Idle / Music / Settings              | Planned       | 2–5 |
| `ui_widgets`     | Reusable themed LVGL widgets                                | Planned       | 2–5 |
| `ma_client`      | Music Assistant connection + commands                       | Planned       | 2 |
| `player_manager` | Player/room selection, temporary switch, auto-return        | Planned       | 2–3 |
| `albumart_cache` | Fetch, downscale, LRU-cache album art in PSRAM              | Planned       | 2 |
| `clock`          | Time source, NTP                                            | Planned       | 4 |
| `weather`        | Weather provider (pluggable)                                | Planned       | 4 |

## Adding a new board

1. Create `components/bsp/boards/<name>/board.cpp` returning a `bsp_board_t`.
2. Add a `choice` entry in `components/bsp/Kconfig`.
3. Add the source to `components/bsp/CMakeLists.txt` under its `CONFIG_` guard.

No file outside `components/bsp` should change.
