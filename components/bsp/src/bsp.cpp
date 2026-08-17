// =============================================================================
// BSP core - panel-agnostic bring-up.
//
// Handles the "RGB + ST7701S init over 3-wire SPI + GT911 I2C touch" family.
// Orientation: 0/180 use the panel's hardware scan flip with efficient partial
// buffers; 90/270 can't be scanned sideways on an RGB panel, so LVGL renders
// upright to a full-frame buffer and flush_cb rotates the pixels by hand.
// =============================================================================

#include "bsp/bsp.h"
#include "bsp/bsp_board.h"
#include "mp_log.h"

#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bsp {
namespace {

constexpr const char *kTag = "bsp";

constexpr ledc_timer_t   kBlTimer   = LEDC_TIMER_0;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;
constexpr ledc_mode_t    kBlMode    = LEDC_LOW_SPEED_MODE;
constexpr int            kBlDutyRes = LEDC_TIMER_10_BIT;
constexpr int            kBlMaxDuty = 1023;

constexpr int kBufRows = 40;

esp_lcd_panel_handle_t s_panel   = nullptr;
esp_lcd_touch_handle_t s_touch   = nullptr;
lv_display_t          *s_display = nullptr;
lv_indev_t            *s_indev   = nullptr;
SemaphoreHandle_t      s_lvgl_mux = nullptr;
const bsp_board_t     *s_board   = nullptr;
int                    s_rotation = 0;          // 0/90/180/270, clockwise
uint8_t               *s_rot_scratch = nullptr; // full-frame scratch for 90/270

bool rotated() { return s_rotation == 90 || s_rotation == 270; }

// ST7701S init for the Guition/JCZN ESP32-4848S040.
static const st7701_lcd_init_cmd_t st7701_4848s040_init[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x00,0x11,0x18,0x0E,0x11,0x06,0x07,0x08,0x07,0x22,0x04,0x12,0x0F,0xAA,0x31,0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00,0x11,0x19,0x0E,0x12,0x07,0x08,0x08,0x08,0x22,0x04,0x11,0x11,0xA9,0x32,0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x32}, 1, 0},
    {0xB2, (uint8_t[]){0x07}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08,0xA0,0x00,0x00,0x07,0xA0,0x00,0x00,0x00,0x44,0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11,0x11,0x44,0x44,0xED,0xA0,0x00,0x00,0xEC,0xA0,0x00,0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44,0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A,0xE9,0xD8,0xA0,0x0C,0xEB,0xD8,0xA0,0x0E,0xED,0xD8,0xA0,0x10,0xEF,0xD8,0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44,0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09,0xE8,0xD8,0xA0,0x0B,0xEA,0xD8,0xA0,0x0D,0xEC,0xD8,0xA0,0x0F,0xEE,0xD8,0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02,0x00,0xE4,0xE4,0x88,0x00,0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C,0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB,0x89,0x76,0x54,0x02,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x20,0x45,0x67,0x98,0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE5, (uint8_t[]){0xE4}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x60}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 20},
};

// ---- LVGL callbacks --------------------------------------------------------

void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (rotated()) {
        // Full-frame buffer in; rotate the whole frame into scratch, push it.
        const int N = s_board->timing.h_res;  // square panel
        const uint16_t *src = reinterpret_cast<const uint16_t *>(px_map);
        uint16_t *dst = reinterpret_cast<uint16_t *>(s_rot_scratch);
        if (s_rotation == 90) {          // cord bottom
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                    dst[y * N + x] = src[(N - 1 - x) * N + y];
        } else {                          // 270, cord top
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                    dst[y * N + x] = src[x * N + (N - 1 - y)];
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, N, N, dst);
    } else {
        const int x1 = area->x1, y1 = area->y1;
        const int x2 = area->x2 + 1, y2 = area->y2 + 1;
        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, px_map);
    }
    lv_display_flush_ready(disp);
}

// Map raw GT911 coordinates (native panel orientation) into the rotated UI
// space. Each branch is isolated: if one orientation's touch is mirrored or
// swapped on-device, it's a one-line fix in exactly one block.
void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t rx = 0, ry = 0;
    uint8_t  cnt = 0;
    esp_lcd_touch_read_data(s_touch);
    const bool pressed = esp_lcd_touch_get_coordinates(s_touch, &rx, &ry, nullptr, &cnt, 1);
    if (!pressed || cnt == 0) { data->state = LV_INDEV_STATE_RELEASED; return; }

    const int W = s_board->timing.h_res;
    const int H = s_board->timing.v_res;
    int x = rx, y = ry;
    switch (s_rotation) {
        case 90:   x = ry;         y = W - 1 - rx; break;  // cord bottom
        case 180:  x = W - 1 - rx; y = H - 1 - ry; break;  // cord left (also hw-mirrored)
        case 270:  x = H - 1 - ry; y = rx;         break;  // cord top
        default:   x = rx;         y = ry;         break;  // cord right
    }
    data->point.x = x;
    data->point.y = y;
    data->state   = LV_INDEV_STATE_PRESSED;
}

void lvgl_tick_cb(void *) { lv_tick_inc(2); }

void lvgl_task(void *)
{
    const esp_timer_create_args_t targs = {
        .callback = lvgl_tick_cb, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "lv_tick", .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick = nullptr;
    esp_timer_create(&targs, &tick);
    esp_timer_start_periodic(tick, 2 * 1000);

    for (;;) {
        uint32_t delay_ms = 5;
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms > 20) delay_ms = 20;
        if (delay_ms < 2)  delay_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---- Hardware bring-up -----------------------------------------------------

esp_err_t backlight_init()
{
    if (s_board->backlight_gpio < 0) return ESP_OK;

    const ledc_timer_config_t tcfg = {
        .speed_mode = kBlMode, .duty_resolution = (ledc_timer_bit_t)kBlDutyRes,
        .timer_num = kBlTimer, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), kTag, "bl timer");

    const ledc_channel_config_t ccfg = {
        .gpio_num = s_board->backlight_gpio, .speed_mode = kBlMode,
        .channel = kBlChannel, .timer_sel = kBlTimer,
        .duty = 0, .hpoint = 0,
    };
    return ledc_channel_config(&ccfg);
}

esp_err_t display_init_st7701()
{
    const auto &rgb = s_board->rgb;
    const auto &t   = s_board->timing;

    esp_lcd_panel_io_handle_t io = nullptr;
    const esp_lcd_panel_io_3wire_spi_config_t io_cfg = {
        .line_config = {
            .cs_io_type = IO_TYPE_GPIO, .cs_gpio_num = rgb.spi_cs,
            .scl_io_type = IO_TYPE_GPIO, .scl_gpio_num = rgb.spi_sck,
            .sda_io_type = IO_TYPE_GPIO, .sda_gpio_num = rgb.spi_mosi,
            .io_expander = nullptr,
        },
        .expect_clk_speed = PANEL_IO_3WIRE_SPI_CLK_MAX,
        .spi_mode = 3, .lcd_cmd_bytes = 1, .lcd_param_bytes = 1,
        .flags = {
            .use_dc_bit = 1, .dc_zero_on_data = 0, .lsb_first = 0,
            .cs_high_active = 0, .del_keep_cs_inactive = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io), kTag, "3wire spi");

    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 12 * 1000 * 1000,
            .h_res = t.h_res,
            .v_res = t.v_res,
            .hsync_pulse_width = 8,
            .hsync_back_porch  = 20,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .vsync_back_porch  = 10,
            .vsync_front_porch = 10,
            .flags = { .pclk_active_neg = false },
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = static_cast<size_t>(t.h_res) * 10,
        .psram_trans_align = 64,
        .hsync_gpio_num = rgb.hsync,
        .vsync_gpio_num = rgb.vsync,
        .de_gpio_num = rgb.de,
        .pclk_gpio_num = rgb.pclk,
        .disp_gpio_num = rgb.disp_en,
        .flags = { .fb_in_psram = 1 },
    };
    for (int i = 0; i < 16; ++i) rgb_cfg.data_gpio_nums[i] = rgb.data[i];

    st7701_vendor_config_t vendor = {};
    vendor.rgb_config = &rgb_cfg;
    vendor.init_cmds = st7701_4848s040_init;
    vendor.init_cmds_size = sizeof(st7701_4848s040_init) / sizeof(st7701_lcd_init_cmd_t);

    const esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &dev, &s_panel), kTag, "st7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), kTag, "init");

    // 0/180 use the panel's hardware scan flip; 90/270 are rotated in flush.
    if (s_rotation == 180) esp_lcd_panel_mirror(s_panel, true, true);
    else                   esp_lcd_panel_mirror(s_panel, false, false);
    return ESP_OK;
}

esp_err_t touch_init_gt911()
{
    const auto &tb = s_board->touch_bus;

    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = tb.i2c_sda, .scl_io_num = tb.i2c_scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = { .clk_speed = tb.i2c_hz },
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &i2c_cfg), kTag, "i2c cfg");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), kTag, "i2c drv");

    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = tb.i2c_addr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .lcd_param_bits = 0,
        .flags = { .disable_control_phase = 1 },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_NUM_0, &tp_io_cfg, &tp_io),
        kTag, "tp io");

    // Always native orientation; rotation handled by touch_read_cb.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = s_board->timing.h_res, .y_max = s_board->timing.v_res,
        .rst_gpio_num = (gpio_num_t)tb.i2c_rst,
        .int_gpio_num = (gpio_num_t)tb.i2c_int,
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    return esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
}

esp_err_t lvgl_init()
{
    lv_init();

    const auto &t = s_board->timing;
    s_display = lv_display_create(t.h_res, t.v_res);
    lv_display_set_flush_cb(s_display, flush_cb);

    if (rotated()) {
        // Full-frame buffer + rotation scratch (both PSRAM). LVGL renders
        // upright; flush_cb rotates into scratch and pushes the whole panel.
        const size_t full_bytes = static_cast<size_t>(t.h_res) * t.v_res * sizeof(uint16_t);
        void *b1 = heap_caps_malloc(full_bytes, MALLOC_CAP_SPIRAM);
        s_rot_scratch = static_cast<uint8_t *>(heap_caps_malloc(full_bytes, MALLOC_CAP_SPIRAM));
        if (!b1 || !s_rot_scratch) {
            MP_LOGE(kTag, "rotated buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
        lv_display_set_buffers(s_display, b1, nullptr, full_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    } else {
        const size_t buf_bytes = static_cast<size_t>(t.h_res) * kBufRows * sizeof(uint16_t);
        void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
        if (!b1 || !b2) {
            MP_LOGE(kTag, "LVGL buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
        lv_display_set_buffers(s_display, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);

    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mux) return ESP_ERR_NO_MEM;

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, nullptr, 4, nullptr, 1);
    return ESP_OK;
}

} // namespace

// ---- Public API ------------------------------------------------------------

esp_err_t init(int rotation)
{
    s_board = &bsp_board_get();
    MP_LOGI(kTag, "board: %s", s_board->name);

    rotation = ((rotation % 360) + 360) % 360;
    s_rotation = (rotation / 90) * 90;
    MP_LOGI(kTag, "rotation: %d", s_rotation);

    ESP_RETURN_ON_ERROR(backlight_init(), kTag, "backlight");

    switch (s_board->panel) {
        case PanelKind::kRgbSt7701:
            ESP_RETURN_ON_ERROR(display_init_st7701(), kTag, "display");
            break;
    }
    switch (s_board->touch) {
        case TouchKind::kGt911:
            ESP_RETURN_ON_ERROR(touch_init_gt911(), kTag, "touch");
            break;
    }

    ESP_RETURN_ON_ERROR(lvgl_init(), kTag, "lvgl");
    return ESP_OK;
}

int rotation() { return s_rotation; }

std::uint16_t width()  { return s_board ? s_board->timing.h_res : 0; }
std::uint16_t height() { return s_board ? s_board->timing.v_res : 0; }

void set_backlight(std::uint8_t percent)
{
    if (!s_board || s_board->backlight_gpio < 0) return;
    if (percent > 100) percent = 100;
    int duty = kBlMaxDuty * percent / 100;
    if (s_board->backlight_active_low) duty = kBlMaxDuty - duty;
    ledc_set_duty(kBlMode, kBlChannel, duty);
    ledc_update_duty(kBlMode, kBlChannel);
}

lv_display_t *display() { return s_display; }

bool lvgl_lock(int timeout_ms)
{
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, ticks) == pdTRUE;
}

void lvgl_unlock() { xSemaphoreGiveRecursive(s_lvgl_mux); }

} // namespace bsp
