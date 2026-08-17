// =============================================================================
// Theme — the single source of truth for UI colors.
//
// Named color roles (0xRRGGBB). Several built-in profiles plus an "Album Match"
// mode that derives a contrast-safe palette from the playing album's colours.
// Independent of LVGL (returns uint32_t).
// =============================================================================

#pragma once

#include <cstdint>

namespace theme {

void init();  // load saved profile; call after settings::init()

int         count();          // includes the Album-Match entry
const char *name(int i);
int         current_index();
void        set_index(int i);  // select + persist

bool is_album_match();  // is the current selection Album Match?

// Feed the current album colours (call each tick in Album-Match mode).
// playing=false falls back to Midnight. Returns true if the palette changed
// (i.e. the UI should re-apply the theme).
bool update_dynamic(bool playing, uint32_t accent, uint32_t bg);

// Color roles (0xRRGGBB) — return dynamic palette when Album Match is active.
uint32_t bg();
uint32_t surface();
uint32_t track();
uint32_t accent();
uint32_t text();
uint32_t text_secondary();
uint32_t text_dim();
uint32_t error();

} // namespace theme
