// =============================================================================
// Board profile: Guition / JCZN ESP32-4848S040
//   MCU     : ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB octal PSRAM)
//   Display : ST7701S, 480x480, 16-bit parallel RGB, init via 3-wire SPI
//   Touch   : GT911 capacitive, I2C @ 0x5D
//   Sold under many names (AITRIP, Guition, JCZN, "4.0inch_ESP32-4848S040").
//
// Pins and timings below are cross-checked against the ESPHome device
// database, Tasmota, and the Arduino_GFX board file for this design. They are
// a validated starting point, NOT gospel for every clone:
//
//   * RED/BLUE SWAP: if the splash shows swapped colours, exchange the `red`
//     and `blue` entries in the data[] map. This design is known to be wired
//     either way depending on batch.
//   * TIMINGS: if you see tearing or roll, nudge the porch values / pclk. The
//     values here favour stability over maximum frame rate.
// =============================================================================

#include "bsp/bsp_board.h"

namespace bsp {

const bsp_board_t &bsp_board_get()
{
    static const bsp_board_t board = {
        .name = "ESP32-4848S040",

        .panel = PanelKind::kRgbSt7701,
        .timing = {
            .pclk_hz            = 12'000'000,
            .h_res              = 480,
            .v_res              = 480,
            .hsync_pulse_width  = 8,
            .hsync_back_porch   = 20,
            .hsync_front_porch  = 10,
            .vsync_pulse_width  = 8,
            .vsync_back_porch   = 10,
            .vsync_front_porch  = 10,
            .pclk_active_neg    = false,
        },
        .rgb = {
            .de = 18, .vsync = 17, .hsync = 16, .pclk = 21, .disp_en = -1,
            // esp_lcd RGB565 bit order: data[0..4]=Blue, [5..10]=Green,
            // [11..15]=Red (blue is the LSB group). See RED/BLUE SWAP note.
            .data = {
                /* B0..B4  */ 4, 5, 6, 7, 15,
                /* G0..G5  */ 8, 20, 3, 46, 9, 10,
                /* R0..R4  */ 11, 12, 13, 14, 0,
            },
            .spi_cs = 39, .spi_sck = 48, .spi_mosi = 47,
        },

        .touch = TouchKind::kGt911,
        .touch_bus = {
            .i2c_sda = 19, .i2c_scl = 45, .i2c_int = -1, .i2c_rst = -1,
            .i2c_hz = 400'000,
            .i2c_addr = 0x5D,
        },

        .backlight_gpio = 38,
        .backlight_active_low = false,
    };
    return board;
}

} // namespace bsp
