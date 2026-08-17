# Hardware

## Target MCU

ESP32-S3 — specifically the **ESP32-S3-WROOM-1-N16R8** module:

- Xtensa dual-core LX7 @ 240 MHz
- 16 MB Flash, 8 MB octal PSRAM
- Wi-Fi 2.4 GHz + Bluetooth 5 (LE)

### Minimum bar

- 16 MB Flash, 8 MB PSRAM. Boards below this (4 MB flash, no PSRAM, or first-gen ESP32) are not supported.

## Reference Hardware (validated — brought up and confirmed working)

**Guition / JCZN ESP32-4848S040** (resold as AITRIP; the unit used here is laser-etched "Sparkle IoT XH-S3E"). These are a clone family — batches differ, so the values below are what was verified on this specific unit.

| Item              | Spec                                   |
|-------------------|----------------------------------------|
| Module            | ESP32-S3-WROOM-1-N16R8                  |
| Display           | 4.0", 480×480 IPS, ST7701S             |
| Display interface | 16-bit parallel RGB + 3-wire SPI init  |
| Touch             | GT911 capacitive, I²C @ 0x5D           |
| Backlight         | GPIO38 (LEDC PWM)                       |
| USB-serial        | CH340 (appears as /dev/ttyUSB0)         |

### Pin map (verified)

- RGB control: DE 18, VSYNC 17, HSYNC 16, PCLK 21
- RGB data (RGB565): R [11,12,13,14,0] · G [8,20,3,46,9,10] · B [4,5,6,7,15]
- Init SPI (3-wire): CS 39, SCK 48, MOSI 47
- Touch I²C: SDA 19, SCL 45
- Backlight: GPIO38

### Verified working display config (the hard-won part)

These are the values that actually drive this panel. Getting them wrong produces a
lit-but-black screen with no error in the log.

- **3-wire SPI must be SPI mode 3.** This was the key fix — with mode 1 the ST7701
  never receives a valid init sequence, so every init variant produced a black screen.
- 3-wire SPI clock: `PANEL_IO_3WIRE_SPI_CLK_MAX` (an explicit Hz value like 2 MHz is
  rejected by the driver with "Invalid Clock frequency").
- RGB timing: pclk **12 MHz**, non-inverted (`pclk_active_neg = false`),
  hsync pulse 8 / back 20 / front 10, vsync pulse 8 / back 10 / front 10.
  (Matches the ESPHome/espcontrol config confirmed working on this exact unit.)
- Framebuffer: **`num_fbs = 1`** in PSRAM. With `num_fbs = 2` and LVGL partial mode
  the panel scans an undrawn buffer → black/flicker.
- ST7701 init: board-specific sequence (Arduino_GFX "type1", cross-checked against the
  Tasmota config for this board), sent as `vendor_config.init_cmds`.
- Data-pin order in `esp_lcd`: index [0..4]=Blue, [5..10]=Green, [11..15]=Red.

### Known-good reference firmware (for sanity-checking the panel)

espcontrol (https://jtenniswood.github.io/espcontrol/screens/4848s040) drives this
board correctly and was used to prove the panel physically works. Flashing it requires
the BOOT button (small SMD button on the board) and a Chromium browser (WebSerial).

## Flashing notes

- Toolchain: ESP-IDF 5.3.x. Flash with `idf.py -p /dev/ttyUSB0 flash monitor`.
- CH340 driver is built into the Linux kernel; user must be in the `dialout` group.
- First flash: `idf.py -p /dev/ttyUSB0 erase-flash` once to clear vendor demo + stale NVS.
- If flashing stalls at "Connecting…", hold BOOT, tap RST, release BOOT.

## Responsive support (future panels)

The layout system must adapt to: 320×480, 480×272, 480×480, 800×480, and later 1024×600.
Anything board- or panel-specific lives only in `components/bsp`. No display-specific UI code.
