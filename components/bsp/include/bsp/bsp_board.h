// =============================================================================
// Board profile — the contract every supported panel implements.
//
// The generic BSP core (src/bsp.cpp) knows how to drive "an RGB panel with a
// 3-wire-SPI-initialised controller and an I2C capacitive touch". A board file
// supplies the concrete numbers (pins, timings, hooks) by returning a filled
// bsp_board_t from bsp_board_get(). Adding a panel is a data exercise, not a
// rewrite of the bring-up logic.
// =============================================================================

#pragma once

#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#include <cstdint>

namespace bsp {

// Panel controller families the core knows how to initialise. Extend as new
// board types are added (e.g. QSPI panels).
enum class PanelKind {
    kRgbSt7701,   // 16-bit parallel RGB, ST7701S init over 3-wire SPI
};

enum class TouchKind {
    kGt911,       // I2C capacitive, address configured below
};

struct RgbTiming {
    std::uint32_t pclk_hz;
    std::uint16_t h_res, v_res;
    std::uint16_t hsync_pulse_width, hsync_back_porch, hsync_front_porch;
    std::uint16_t vsync_pulse_width, vsync_back_porch, vsync_front_porch;
    bool pclk_active_neg;
};

struct RgbPins {
    int de, vsync, hsync, pclk, disp_en;   // disp_en < 0 if unused
    int data[16];                          // RGB565: 5 red, 6 green, 5 blue
    // 3-wire SPI used only for the controller init sequence.
    int spi_cs, spi_sck, spi_mosi;
};

struct TouchBus {
    int i2c_sda, i2c_scl, i2c_int, i2c_rst; // int/rst < 0 if unused
    std::uint32_t i2c_hz;
    std::uint8_t  i2c_addr;
};

struct bsp_board_t {
    const char *name;

    PanelKind panel;
    RgbTiming timing;
    RgbPins   rgb;

    TouchKind touch;
    TouchBus  touch_bus;

    int  backlight_gpio;     // LEDC PWM; < 0 if not controllable
    bool backlight_active_low;
};

// Provided by exactly one boards/<name>/board.cpp (the Kconfig-selected one).
const bsp_board_t &bsp_board_get();

} // namespace bsp
