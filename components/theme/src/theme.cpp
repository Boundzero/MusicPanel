#include "theme.h"
#include "settings.h"

#include <cstdlib>
#include <string>

namespace theme {
namespace {

struct Palette {
    const char *name;
    uint32_t bg, surface, track, accent, text, secondary, dim, error;
};

const Palette kProfiles[] = {
    {"Midnight", 0x101014, 0x1b1b20, 0x33333a, 0x3b82f6, 0xf2f2f5, 0x9a9aa2, 0x6b6b73, 0xe06666},
    {"Graphite", 0x0d0d0f, 0x1c1c22, 0x35353d, 0x8b5cf6, 0xf5f5f7, 0x9a9aa2, 0x6b6b73, 0xe06666},
    {"Forest",   0x0c1410, 0x16221c, 0x2c3a32, 0x22c55e, 0xf2f5f2, 0x9aa29a, 0x6b736b, 0xe06666},
    {"Sunset",   0x151013, 0x231a1e, 0x3a2c33, 0xf59e0b, 0xf5f2ee, 0xa29a97, 0x736b68, 0xe06666},
    {"Ocean",    0x0b1218, 0x152029, 0x2a3a45, 0x38bdf8, 0xeef5f9, 0x93a3ad, 0x64727b, 0xe06666},
    {"Light",    0xf2f2f5, 0xffffff, 0xd0d0d8, 0x2563eb, 0x101014, 0x555560, 0x8a8a92, 0xcc3333},
};
const int kCount = sizeof(kProfiles) / sizeof(kProfiles[0]);
const int kAlbum = kCount;  // virtual "Album Match" index

int s_idx = 0;
Palette s_dynamic = kProfiles[0];
uint32_t s_dyn_key = 0;

int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

uint32_t scale(uint32_t c, int num, int den)
{
    int r = ((c >> 16) & 0xff) * num / den;
    int g = ((c >> 8) & 0xff) * num / den;
    int b = (c & 0xff) * num / den;
    return (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
}
uint32_t lighten(uint32_t c, int d)
{
    int r = clamp8(((c >> 16) & 0xff) + d);
    int g = clamp8(((c >> 8) & 0xff) + d);
    int b = clamp8((c & 0xff) + d);
    return (r << 16) | (g << 8) | b;
}
uint32_t ensure_bright(uint32_t c, int minmax)
{
    int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
    if (mx == 0) return (minmax << 16) | (minmax << 8) | minmax;
    if (mx < minmax) { r = r * minmax / mx; g = g * minmax / mx; b = b * minmax / mx; }
    return (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
}

// Contrast-safe: dark tinted background, progressively lighter surfaces, a
// visible album accent, near-white text. Borrows the album's mood, never its
// legibility problems.
void derive(uint32_t accent, uint32_t bg_in, Palette &p)
{
    uint32_t base = scale(bg_in, 1, 4);  // ~25% brightness tint
    p.name = "Album Match";
    p.bg = base;
    p.surface = lighten(base, 20);
    p.track = lighten(base, 40);
    p.accent = ensure_bright(accent, 170);
    p.text = 0xf2f2f5;
    p.secondary = 0x9a9aa2;
    p.dim = 0x6b6b73;
    p.error = 0xe06666;
}

const Palette &active()
{
    return (s_idx == kAlbum) ? s_dynamic : kProfiles[s_idx];
}

} // namespace

void init()
{
    int i = atoi(settings::get("theme", "0").c_str());
    s_idx = (i >= 0 && i <= kAlbum) ? i : 0;
    s_dynamic = kProfiles[0];
}

int         count()         { return kCount + 1; }  // + Album Match
const char *name(int i)
{
    if (i == kAlbum) return "Album Match";
    return (i >= 0 && i < kCount) ? kProfiles[i].name : "";
}
int  current_index() { return s_idx; }
bool is_album_match() { return s_idx == kAlbum; }

void set_index(int i)
{
    if (i < 0 || i > kAlbum) return;
    s_idx = i;
    settings::set("theme", std::to_string(i));
}

bool update_dynamic(bool playing, uint32_t accent, uint32_t bg_in)
{
    uint32_t key = playing ? (accent ^ (bg_in * 3) ^ 0x9e3779b9u) : 0;
    if (key == s_dyn_key) return false;
    s_dyn_key = key;
    if (!playing) s_dynamic = kProfiles[0];   // Midnight fallback when idle
    else          derive(accent, bg_in, s_dynamic);
    return true;
}

uint32_t bg()             { return active().bg; }
uint32_t surface()        { return active().surface; }
uint32_t track()          { return active().track; }
uint32_t accent()         { return active().accent; }
uint32_t text()           { return active().text; }
uint32_t text_secondary() { return active().secondary; }
uint32_t text_dim()       { return active().dim; }
uint32_t error()          { return active().error; }

} // namespace theme
