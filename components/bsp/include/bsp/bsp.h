// =============================================================================
// Board Support Package — public API.
//
// This is the entire surface the rest of the firmware is allowed to use to
// talk to hardware. It is intentionally small and panel-agnostic: callers ask
// for a display, a touch input, a backlight level, and the active geometry.
// They never learn which controller, bus, or pins are underneath.
//
// To port MusicPanel to a new panel, add a board under boards/<name>/ and a
// Kconfig choice. No file outside components/bsp changes.
// =============================================================================
#pragma once
#include "esp_err.h"
#include "lvgl.h"
#include <cstdint>
namespace bsp {
// Brings up the selected board: display controller, touch controller,
// backlight, and the LVGL runtime (buffers, tick source, and timer task).
//
// `rotation` is degrees clockwise (0/90/180/270). It must be passed here (not
// changed later) because 90/270 need a full-frame buffer + rotation scratch,
// while 0/180 use small partial buffers and a hardware flip. Change orientation
// by persisting the setting and rebooting.
esp_err_t init(int rotation = 0);

int rotation();  // current rotation in degrees

// Active panel geometry, in pixels (the UI coordinate space; always native
// since 90/270 are rotated at flush time, not in LVGL).
std::uint16_t width();
std::uint16_t height();
// Backlight level, 0-100 percent. Values are clamped.
void set_backlight(std::uint8_t percent);
// The LVGL display created during init(), for callers that need it directly.
lv_display_t *display();
// LVGL is single-threaded. Any code touching LVGL objects from outside the
// BSP's own timer task MUST hold this lock. timeout_ms < 0 waits forever.
// Returns true if the lock was acquired.
bool lvgl_lock(int timeout_ms);
void lvgl_unlock();
} // namespace bsp
