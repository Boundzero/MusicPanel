// =============================================================================
// MusicPanel — application entry point.
//
// Swipe shell: [Browse] · [Now Playing] · [Settings]. Themed via a widget
// registry (incl. live Album-Match). Settings has speaker, theme, and screen
// orientation pickers.
// =============================================================================

#include "bsp/bsp.h"
#include "ma.h"
#include "network.h"
#include "settings.h"
#include "theme.h"
#include "timesync.h"
#include "weather.h"
#include "mp_log.h"

#include <ctime>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr const char *kTag = "app";

lv_color_t c_bg()      { return lv_color_hex(theme::bg()); }
lv_color_t c_surface() { return lv_color_hex(theme::surface()); }
lv_color_t c_track()   { return lv_color_hex(theme::track()); }
lv_color_t c_accent()  { return lv_color_hex(theme::accent()); }
lv_color_t c_text()    { return lv_color_hex(theme::text()); }
lv_color_t c_text2()   { return lv_color_hex(theme::text_secondary()); }
lv_color_t c_dim()     { return lv_color_hex(theme::text_dim()); }

// Orientation options — labelled by where the power cord exits.
const char *kOrientNames[] = {"Cord: Right", "Cord: Bottom", "Cord: Left", "Cord: Top"};
const int   kOrientDeg[]   = {0, 270, 180, 90};

// ---- themed-widget registry ----
enum ThemeRole {
    ROLE_BG, ROLE_SURFACE, ROLE_TRACK, ROLE_INDICATOR, ROLE_KNOB,
    ROLE_TEXT, ROLE_TEXT2, ROLE_DIM, ROLE_ACCENT_TXT,
};
struct Themed { lv_obj_t *obj; uint8_t role; };
std::vector<Themed> s_themed;
void reg(lv_obj_t *o, uint8_t role) { s_themed.push_back({o, role}); }

void apply_theme()
{
    for (const Themed &t : s_themed) {
        if (!t.obj) continue;
        switch (t.role) {
            case ROLE_BG:      lv_obj_set_style_bg_color(t.obj, c_bg(), LV_PART_MAIN); break;
            case ROLE_SURFACE:
                lv_obj_set_style_bg_color(t.obj, c_surface(), LV_PART_MAIN);
                lv_obj_set_style_bg_color(t.obj, c_accent(), LV_STATE_PRESSED);
                break;
            case ROLE_TRACK:     lv_obj_set_style_bg_color(t.obj, c_track(), LV_PART_MAIN); break;
            case ROLE_INDICATOR: lv_obj_set_style_bg_color(t.obj, c_accent(), LV_PART_INDICATOR); break;
            case ROLE_KNOB:      lv_obj_set_style_bg_color(t.obj, c_text(), LV_PART_KNOB); break;
            case ROLE_TEXT:      lv_obj_set_style_text_color(t.obj, c_text(), 0); break;
            case ROLE_TEXT2:     lv_obj_set_style_text_color(t.obj, c_text2(), 0); break;
            case ROLE_DIM:       lv_obj_set_style_text_color(t.obj, c_dim(), 0); break;
            case ROLE_ACCENT_TXT:lv_obj_set_style_text_color(t.obj, c_accent(), 0); break;
        }
    }
}

lv_obj_t *s_status_grp = nullptr;
lv_obj_t *s_status = nullptr;
lv_obj_t *s_detail = nullptr;

lv_obj_t *s_idle_grp = nullptr;
lv_obj_t *s_idle_clock = nullptr;
lv_obj_t *s_idle_date = nullptr;
lv_obj_t *s_idle_room = nullptr;
lv_obj_t *s_idle_weather = nullptr;
lv_obj_t *s_idle_wx_icon = nullptr;
int s_wx_icon_code = -999;

// Browse page state
lv_obj_t *s_tileview = nullptr;
lv_obj_t *s_browse_list = nullptr;
lv_obj_t *s_browse_title = nullptr;
lv_obj_t *s_browse_back = nullptr;
std::vector<ma::BrowseItem> s_browse_cur;
struct NavLevel { char kind; std::string a; std::string b; std::string name; };
std::vector<NavLevel> s_nav;   // drill stack; empty = root
bool s_browse_started = false;
bool s_alpha_active = false;    // current folder shows an A-Z picker
char s_alpha_letter = 0;        // 0 = showing the letter grid; else that letter's list
std::string s_alpha_letters;    // available letters for the current large folder

lv_obj_t *s_np_grp = nullptr;
lv_obj_t *s_np_room = nullptr;
lv_obj_t *s_np_title = nullptr;
lv_obj_t *s_np_artist = nullptr;
lv_obj_t *s_np_bar = nullptr;
lv_obj_t *s_np_time = nullptr;
lv_obj_t *s_np_pp = nullptr;
lv_obj_t *s_np_shuffle = nullptr;
lv_obj_t *s_np_repeat = nullptr;
lv_obj_t *s_np_repeat_one = nullptr;
lv_obj_t *s_np_vol = nullptr;

lv_obj_t *s_art_img = nullptr;
lv_obj_t *s_art_note = nullptr;
uint8_t  *s_art_buf = nullptr;
lv_image_dsc_t s_art_dsc;

lv_obj_t *s_speaker_btn_label = nullptr;
lv_obj_t *s_theme_btn_label = nullptr;
lv_obj_t *s_orient_btn_label = nullptr;

lv_obj_t *s_picker_overlay = nullptr;
typedef void (*PickerCb)(int);
PickerCb s_picker_cb = nullptr;
int s_pending_idx = -1;
PickerCb s_pending_cb = nullptr;
std::vector<ma::PlayerInfo> s_picker_players;

enum { CMD_PREV, CMD_PP, CMD_NEXT };

void speaker_pick(int idx);
void theme_pick(int idx);
void orient_pick(int idx);
void open_speaker_picker(lv_event_t *e);
void open_theme_picker(lv_event_t *e);
void open_orient_picker(lv_event_t *e);
void set_weather_icon(lv_obj_t *c, int code);
void populate_browse(const std::vector<ma::BrowseItem> &items);
void show_letter_grid(const std::string &letters);
const char *browse_title_text();

void fmt_time(int s, char *o, size_t n) { if (s < 0) s = 0; snprintf(o, n, "%d:%02d", s / 60, s % 60); }
void show(lv_obj_t *o, bool v) { if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }

// ---- transport / control callbacks ----------------------------------------

void transport_cb(lv_event_t *e)
{
    intptr_t which = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    ma::NowPlaying np = ma::now_playing();
    if (!np.playing || np.queue_id.empty()) return;
    const char *cmd = (which == CMD_PREV) ? "player_queues/previous"
                    : (which == CMD_NEXT) ? "player_queues/next"
                                          : "player_queues/play_pause";
    ma::transport(cmd, np.queue_id);
}
void shuffle_cb(lv_event_t *)
{
    ma::NowPlaying np = ma::now_playing();
    if (!np.queue_id.empty()) ma::set_shuffle(np.queue_id, !np.shuffle);
}
void repeat_cb(lv_event_t *)
{
    ma::NowPlaying np = ma::now_playing();
    if (np.queue_id.empty()) return;
    const char *next = (np.repeat == "off") ? "all" : (np.repeat == "all") ? "one" : "off";
    ma::set_repeat(np.queue_id, next);
}
void volume_cb(lv_event_t *e)
{
    lv_obj_t *sl = static_cast<lv_obj_t *>(lv_event_get_target(e));
    ma::NowPlaying np = ma::now_playing();
    if (!np.queue_id.empty()) ma::set_volume(np.queue_id, lv_slider_get_value(sl));
}

// ---- generic picker overlay -----------------------------------------------

void picker_apply_async(void *)
{
    PickerCb cb = s_pending_cb;
    int idx = s_pending_idx;
    s_pending_cb = nullptr;
    s_pending_idx = -1;
    if (s_picker_overlay) { lv_obj_delete(s_picker_overlay); s_picker_overlay = nullptr; }
    if (cb) cb(idx);
}
void picker_close_async(void *)
{
    if (s_picker_overlay) { lv_obj_delete(s_picker_overlay); s_picker_overlay = nullptr; }
}
void picker_row_cb(lv_event_t *e)
{
    s_pending_idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    s_pending_cb = s_picker_cb;
    lv_async_call(picker_apply_async, nullptr);
}
void picker_close_cb(lv_event_t *)
{
    lv_async_call(picker_close_async, nullptr);
}

void open_picker(const char *title, const std::vector<std::string> &items,
                 const std::vector<bool> &enabled, int current, PickerCb cb)
{
    s_picker_cb = cb;

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ov, c_bg(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    s_picker_overlay = ov;

    lv_obj_t *t = lv_label_create(ov);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, c_text(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *list = lv_list_create(ov);
    lv_obj_set_size(list, LV_PCT(92), 356);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(list, c_bg(), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);

    for (size_t i = 0; i < items.size(); ++i) {
        bool en = enabled.empty() || enabled[i];
        const char *icon = (static_cast<int>(i) == current) ? LV_SYMBOL_OK : nullptr;
        lv_obj_t *b = lv_list_add_button(list, icon, items[i].c_str());
        lv_obj_set_style_bg_color(b, c_surface(), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_color_t col = !en ? c_dim() : (static_cast<int>(i) == current ? c_accent() : c_text());
        lv_obj_set_style_text_color(b, col, 0);
        if (en) lv_obj_add_event_cb(b, picker_row_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>((intptr_t)i));
    }

    lv_obj_t *close = lv_button_create(ov);
    lv_obj_set_size(close, 160, 48);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(close, c_surface(), 0);
    lv_obj_add_event_cb(close, picker_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, c_text(), 0);
    lv_obj_center(cl);
}

void speaker_pick(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(s_picker_players.size())) return;
    const ma::PlayerInfo &pl = s_picker_players[idx];
    settings::set("ma_player", pl.id);
    settings::set("ma_player_name", pl.name);
    if (s_speaker_btn_label) lv_label_set_text(s_speaker_btn_label, pl.name.c_str());
}
void open_speaker_picker(lv_event_t *)
{
    ma::get_players(s_picker_players);
    std::string curid = settings::get("ma_player");
    std::vector<std::string> items;
    std::vector<bool> en;
    int cur = -1;
    for (size_t i = 0; i < s_picker_players.size(); ++i) {
        items.push_back(s_picker_players[i].name);
        en.push_back(s_picker_players[i].available);
        if (s_picker_players[i].id == curid) cur = static_cast<int>(i);
    }
    open_picker("Select Speaker", items, en, cur, speaker_pick);
}

void theme_pick(int idx)
{
    theme::set_index(idx);
    theme::update_dynamic(false, 0, 0);
    if (s_theme_btn_label) lv_label_set_text(s_theme_btn_label, theme::name(idx));
    apply_theme();
}
void open_theme_picker(lv_event_t *)
{
    std::vector<std::string> items;
    for (int i = 0; i < theme::count(); ++i) items.push_back(theme::name(i));
    open_picker("Theme", items, {}, theme::current_index(), theme_pick);
}

int orient_current_index()
{
    int deg = atoi(settings::get("orientation", "0").c_str());
    for (int i = 0; i < 4; ++i) if (kOrientDeg[i] == deg) return i;
    return 0;
}
void orient_pick(int idx)
{
    if (idx < 0 || idx >= 4) return;
    settings::set("orientation", std::to_string(kOrientDeg[idx]));
    esp_restart();  // re-init the display at the new rotation
}
void open_orient_picker(lv_event_t *)
{
    std::vector<std::string> items;
    for (int i = 0; i < 4; ++i) items.push_back(kOrientNames[i]);
    open_picker("Orientation", items, {}, orient_current_index(), orient_pick);
}

// ---- album art -------------------------------------------------------------

lv_obj_t *make_btn(lv_obj_t *parent, const char *sym, int size, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    reg(b, ROLE_SURFACE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return l;
}

void update_album_art()
{
    uint8_t *nb = nullptr;
    int nw = 0, nh = 0;
    if (!ma::take_album_art(&nb, &nw, &nh)) return;

    if (nb) {
        uint8_t *old = s_art_buf;
        s_art_buf = nb;
        s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_art_dsc.header.flags = 0;
        s_art_dsc.header.w = nw;
        s_art_dsc.header.h = nh;
        s_art_dsc.header.stride = nw * 2;
        s_art_dsc.data = s_art_buf;
        s_art_dsc.data_size = static_cast<uint32_t>(nw) * nh * 2;
        lv_image_set_src(s_art_img, NULL);
        lv_image_set_src(s_art_img, &s_art_dsc);
        lv_obj_set_size(s_art_img, nw, nh);
        lv_obj_set_size(lv_obj_get_parent(s_art_img), nw, nh);
        lv_obj_center(s_art_img);
        show(s_art_img, true);
        show(s_art_note, false);
        if (old) free(old);
    } else {
        show(s_art_img, false);
        show(s_art_note, true);
        if (s_art_buf) { free(s_art_buf); s_art_buf = nullptr; }
    }
}

// ---- main tick -------------------------------------------------------------

void status_tick(lv_timer_t *)
{
    update_album_art();

    const bool online = (network::state() == network::State::kConnected);
    ma::NowPlaying np = online ? ma::now_playing() : ma::NowPlaying{};

    // Browse: load the root once connected, and apply any ready result.
    if (!s_browse_started && online && ma::state() == ma::State::kConnected) {
        s_browse_started = true;
        ma::browse('B', "", "");
    }
    {
        std::vector<ma::BrowseItem> bi;
        std::string letters;
        if (ma::take_browse_result(bi, letters)) {
            if (!letters.empty()) {
                s_alpha_active = true;
                s_alpha_letter = 0;
                s_alpha_letters = letters;
                show_letter_grid(letters);
            } else {
                populate_browse(bi);
            }
        }
    }

    if (theme::is_album_match()) {
        bool changed = (online && np.playing && np.has_palette)
            ? theme::update_dynamic(true, np.pal_accent, np.pal_bg)
            : theme::update_dynamic(false, 0, 0);
        if (changed) apply_theme();
    }

    const bool ma_ok    = online && (ma::state() == ma::State::kConnected);
    const bool show_np   = online && np.playing;
    const bool show_idle = ma_ok && !np.playing && !np.room.empty();
    const bool show_stat = !show_np && !show_idle;
    show(s_np_grp, show_np);
    show(s_idle_grp, show_idle);
    show(s_status_grp, show_stat);

    if (show_idle) {
        time_t now = time(nullptr);
        if (timesync::synced()) {
            struct tm tmv;
            localtime_r(&now, &tmv);
            char clk[16];
            strftime(clk, sizeof(clk), "%I:%M %p", &tmv);
            const char *cp = (clk[0] == '0') ? clk + 1 : clk;  // strip leading zero
            lv_label_set_text(s_idle_clock, cp);
            char date[40];
            strftime(date, sizeof(date), "%A, %B %e", &tmv);
            lv_label_set_text(s_idle_date, date);
        } else {
            lv_label_set_text(s_idle_clock, "--:--");
            lv_label_set_text(s_idle_date, "syncing time...");
        }
        lv_label_set_text(s_idle_room, np.room.c_str());

        weather::Current wx;
        weather::get(wx);
        if (wx.valid) {
            char wb[48];
            snprintf(wb, sizeof(wb), "%d\xC2\xB0""F  %s", wx.temp_f, wx.text.c_str());
            lv_label_set_text(s_idle_weather, wb);
            if (wx.code != s_wx_icon_code) {
                set_weather_icon(s_idle_wx_icon, wx.code);
                s_wx_icon_code = wx.code;
            }
            show(s_idle_wx_icon, true);
        } else {
            lv_label_set_text(s_idle_weather, "");
            show(s_idle_wx_icon, false);
        }
        return;
    }

    if (show_np) {
        lv_label_set_text(s_np_room, np.room.c_str());
        lv_label_set_text(s_np_title, np.title.empty() ? "(unknown)" : np.title.c_str());
        lv_label_set_text(s_np_artist, np.artist.c_str());
        lv_label_set_text(s_np_pp, np.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
        lv_obj_set_style_text_color(s_np_pp, c_text(), 0);

        lv_obj_set_style_text_color(s_np_shuffle, np.shuffle ? c_accent() : c_dim(), 0);
        lv_color_t rc = (np.repeat == "off") ? c_dim() : c_accent();
        lv_obj_set_style_text_color(s_np_repeat, rc, 0);
        lv_obj_set_style_text_color(s_np_repeat_one, rc, 0);
        show(s_np_repeat_one, np.repeat == "one");

        int dur = np.duration_s > 0 ? np.duration_s : 1;
        lv_bar_set_range(s_np_bar, 0, dur);
        lv_bar_set_value(s_np_bar, np.elapsed_s, LV_ANIM_OFF);
        char eb[8], db[8], line[20];
        fmt_time(np.elapsed_s, eb, sizeof(eb));
        fmt_time(np.duration_s, db, sizeof(db));
        snprintf(line, sizeof(line), "%s / %s", eb, db);
        lv_label_set_text(s_np_time, line);

        if (!lv_obj_has_state(s_np_vol, LV_STATE_PRESSED))
            lv_slider_set_value(s_np_vol, np.volume, LV_ANIM_OFF);
        return;
    }

    if (!online) {
        switch (network::state()) {
            case network::State::kApPortal:
                lv_label_set_text(s_status, "Wi-Fi Setup");
                lv_label_set_text_fmt(s_detail,
                    "On your phone, join Wi-Fi:\n#%06X %s#\nthen open  #%06X 192.168.4.1#",
                    (unsigned)theme::accent(), network::ap_ssid().c_str(), (unsigned)theme::accent());
                break;
            case network::State::kConnecting:
                lv_label_set_text(s_status, "Connecting");
                lv_label_set_text_fmt(s_detail, "Joining %s", network::ssid().c_str());
                break;
            case network::State::kFailed:
                lv_label_set_text(s_status, "Wi-Fi failed");
                lv_label_set_text(s_detail, "Restart to run setup again.");
                break;
            default:
                lv_label_set_text(s_status, "Starting");
                lv_label_set_text(s_detail, "");
                break;
        }
    } else {
        switch (ma::state()) {
            case ma::State::kConnected:
                if (!np.room.empty()) {
                    lv_label_set_text(s_status, np.room.c_str());
                    lv_label_set_text(s_detail, "Nothing playing");
                } else {
                    lv_label_set_text(s_status, "Music Assistant");
                    lv_label_set_text(s_detail, "Choose a speaker in Settings");
                }
                break;
            case ma::State::kError:
                lv_label_set_text(s_status, "Music Assistant");
                lv_label_set_text_fmt(s_detail, "#%06X %s#", (unsigned)theme::error(), ma::error_text().c_str());
                break;
            default:
                lv_label_set_text(s_status, "Connected");
                lv_label_set_text_fmt(s_detail, "%s\nReaching Music Assistant...", network::ip().c_str());
                break;
        }
    }
    lv_obj_center(s_status);
    lv_obj_align_to(s_detail, s_status, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
}

// ---- builders --------------------------------------------------------------

// ---- weather icons (fixed natural colors, composed from LVGL shapes) -------
// Shapes are designed in a 72px space, then scaled up for distance readability.

constexpr int WX_BASE = 72;
constexpr int WX_SIZE = 94;   // ~1.3x
inline int sc(int v) { return v * WX_SIZE / WX_BASE; }

lv_obj_t *wx_dot(lv_obj_t *p, int x, int y, int d, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, sc(d), sc(d));
    lv_obj_set_pos(o, sc(x), sc(y));
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    return o;
}

lv_obj_t *wx_bar(lv_obj_t *p, int x, int y, int w, int h, uint32_t col, int angle = 0)
{
    int W = sc(w), H = sc(h);
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, W, H);
    lv_obj_set_pos(o, sc(x), sc(y));
    lv_obj_set_style_radius(o, (W < H ? W : H) / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    if (angle) {
        lv_obj_set_style_transform_pivot_x(o, W / 2, 0);
        lv_obj_set_style_transform_pivot_y(o, H / 2, 0);
        lv_obj_set_style_transform_rotation(o, angle, 0);
    }
    return o;
}

void wx_cloud(lv_obj_t *c, uint32_t col, int top)
{
    wx_dot(c, 21, top, 30, col);        // top puff (largest)
    wx_dot(c, 9,  top + 12, 22, col);   // left puff
    wx_dot(c, 40, top + 10, 24, col);   // right puff
    wx_bar(c, 12, top + 20, 48, 16, col);  // flat base
}

void wx_sun(lv_obj_t *c, int cx, int cy, uint32_t col)
{
    wx_dot(c, cx - 13, cy - 13, 26, col);
    wx_bar(c, cx - 2, cy - 24, 4, 9, col);   // top
    wx_bar(c, cx - 2, cy + 15, 4, 9, col);   // bottom
    wx_bar(c, cx - 24, cy - 2, 9, 4, col);   // left
    wx_bar(c, cx + 15, cy - 2, 9, 4, col);   // right
    wx_bar(c, cx - 20, cy - 20, 4, 9, col, 450);   // TL diagonal
    wx_bar(c, cx + 16, cy - 20, 4, 9, col, 3150);  // TR diagonal
    wx_bar(c, cx - 20, cy + 16, 4, 9, col, 3150);  // BL diagonal
    wx_bar(c, cx + 16, cy + 16, 4, 9, col, 450);   // BR diagonal
}

void set_weather_icon(lv_obj_t *c, int code)
{
    lv_obj_clean(c);
    const uint32_t SUN = 0xfbbf24, CLOUD = 0xcbd5e1, CLOUD_D = 0x94a3b8;
    const uint32_t RAIN = 0x60a5fa, SNOW = 0xf1f5f9, FOG = 0x94a3b8;

    if (code == 0) {
        wx_sun(c, 36, 36, SUN);
    } else if (code == 1 || code == 2) {                 // partly cloudy
        wx_dot(c, 4, 4, 20, SUN);
        wx_bar(c, 12, 0, 3, 7, SUN);
        wx_bar(c, 0, 12, 7, 3, SUN);
        wx_cloud(c, CLOUD, 24);
    } else if (code == 3) {                              // cloudy
        wx_cloud(c, CLOUD, 16);
    } else if (code == 45 || code == 48) {               // fog
        wx_cloud(c, CLOUD_D, 4);
        wx_bar(c, 14, 44, 44, 4, FOG);
        wx_bar(c, 18, 52, 40, 4, FOG);
        wx_bar(c, 12, 60, 38, 4, FOG);
    } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {  // rain/showers
        wx_cloud(c, CLOUD, 6);
        wx_bar(c, 20, 44, 4, 13, RAIN);
        wx_bar(c, 34, 44, 4, 13, RAIN);
        wx_bar(c, 48, 44, 4, 13, RAIN);
    } else if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {  // snow
        wx_cloud(c, CLOUD, 6);
        wx_dot(c, 20, 46, 7, SNOW);
        wx_dot(c, 33, 50, 7, SNOW);
        wx_dot(c, 46, 46, 7, SNOW);
    } else if (code >= 95) {                             // thunderstorm
        wx_cloud(c, CLOUD_D, 4);
        static lv_point_precise_t bolt[] = {{sc(44), sc(38)}, {sc(31), sc(56)}, {sc(41), sc(56)}, {sc(33), sc(72)}};
        lv_obj_t *l = lv_line_create(c);
        lv_line_set_points(l, bolt, 4);
        lv_obj_set_style_line_color(l, lv_color_hex(SUN), 0);
        lv_obj_set_style_line_width(l, sc(5), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
    } else {
        wx_cloud(c, CLOUD, 16);
    }
}

void build_idle_group(lv_obj_t *parent)
{
    s_idle_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(s_idle_grp);
    lv_obj_set_size(s_idle_grp, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_idle_grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_idle_grp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_idle_grp, 4, 0);
    lv_obj_add_flag(s_idle_grp, LV_OBJ_FLAG_HIDDEN);

    s_idle_clock = lv_label_create(s_idle_grp);
    lv_obj_set_style_text_font(s_idle_clock, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_idle_clock, "--:--");
    reg(s_idle_clock, ROLE_TEXT);

    s_idle_date = lv_label_create(s_idle_grp);
    lv_obj_set_style_text_font(s_idle_date, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_idle_date, "");
    reg(s_idle_date, ROLE_TEXT2);

    s_idle_wx_icon = lv_obj_create(s_idle_grp);
    lv_obj_remove_style_all(s_idle_wx_icon);
    lv_obj_set_size(s_idle_wx_icon, WX_SIZE, WX_SIZE);
    lv_obj_clear_flag(s_idle_wx_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_idle_wx_icon, LV_OBJ_FLAG_HIDDEN);

    s_idle_weather = lv_label_create(s_idle_grp);
    lv_obj_set_style_text_font(s_idle_weather, &lv_font_montserrat_26, 0);
    lv_label_set_text(s_idle_weather, "");
    reg(s_idle_weather, ROLE_ACCENT_TXT);

    lv_obj_t *spacer = lv_obj_create(s_idle_grp);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 24);

    s_idle_room = lv_label_create(s_idle_grp);
    lv_label_set_text(s_idle_room, "");
    reg(s_idle_room, ROLE_DIM);

    lv_obj_t *np_lbl = lv_label_create(s_idle_grp);
    lv_label_set_text(np_lbl, "Nothing playing");
    reg(np_lbl, ROLE_DIM);
}

void build_status_group(lv_obj_t *parent)
{
    s_status_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(s_status_grp);
    lv_obj_set_size(s_status_grp, LV_PCT(100), LV_PCT(100));

    s_status = lv_label_create(s_status_grp);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_status, "Starting");
    lv_obj_center(s_status);
    reg(s_status, ROLE_TEXT);

    s_detail = lv_label_create(s_status_grp);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(s_detail, true);
    lv_obj_set_width(s_detail, LV_PCT(80));
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_detail, "");
    lv_obj_align_to(s_detail, s_status, LV_ALIGN_OUT_BOTTOM_MID, 0, 24);
    reg(s_detail, ROLE_TEXT2);
}

void build_now_playing_group(lv_obj_t *parent)
{
    s_np_grp = lv_obj_create(parent);
    lv_obj_remove_style_all(s_np_grp);
    lv_obj_set_size(s_np_grp, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_np_grp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_np_grp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_np_grp, 6, 0);
    lv_obj_add_flag(s_np_grp, LV_OBJ_FLAG_HIDDEN);

    s_np_room = lv_label_create(s_np_grp);
    lv_label_set_text(s_np_room, "");
    reg(s_np_room, ROLE_DIM);

    lv_obj_t *art = lv_obj_create(s_np_grp);
    lv_obj_set_size(art, 160, 160);
    lv_obj_set_style_radius(art, 16, 0);
    lv_obj_set_style_clip_corner(art, true, 0);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_set_style_pad_all(art, 0, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    reg(art, ROLE_SURFACE);

    s_art_note = lv_label_create(art);
    lv_label_set_text(s_art_note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_art_note, &lv_font_montserrat_28, 0);
    lv_obj_center(s_art_note);
    reg(s_art_note, ROLE_ACCENT_TXT);

    s_art_img = lv_image_create(art);
    lv_obj_center(s_art_img);
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);

    s_np_title = lv_label_create(s_np_grp);
    lv_obj_set_style_text_font(s_np_title, &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_np_title, LV_PCT(90));
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_np_title, "");
    reg(s_np_title, ROLE_TEXT);

    s_np_artist = lv_label_create(s_np_grp);
    lv_obj_set_style_text_font(s_np_artist, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_np_artist, "");
    reg(s_np_artist, ROLE_TEXT2);

    s_np_bar = lv_bar_create(s_np_grp);
    lv_obj_set_size(s_np_bar, 300, 6);
    reg(s_np_bar, ROLE_TRACK);
    reg(s_np_bar, ROLE_INDICATOR);

    s_np_time = lv_label_create(s_np_grp);
    lv_label_set_text(s_np_time, "0:00 / 0:00");
    reg(s_np_time, ROLE_DIM);

    lv_obj_t *btns = lv_obj_create(s_np_grp);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_set_style_pad_top(btns, 6, 0);

    s_np_shuffle = make_btn(btns, LV_SYMBOL_SHUFFLE, 48, shuffle_cb, nullptr);
    lv_obj_t *prev_ic = make_btn(btns, LV_SYMBOL_PREV, 60, transport_cb, reinterpret_cast<void *>((intptr_t)CMD_PREV));
    reg(prev_ic, ROLE_TEXT);
    s_np_pp = make_btn(btns, LV_SYMBOL_PAUSE, 64, transport_cb, reinterpret_cast<void *>((intptr_t)CMD_PP));
    lv_obj_t *next_ic = make_btn(btns, LV_SYMBOL_NEXT, 60, transport_cb, reinterpret_cast<void *>((intptr_t)CMD_NEXT));
    reg(next_ic, ROLE_TEXT);
    s_np_repeat = make_btn(btns, LV_SYMBOL_LOOP, 48, repeat_cb, nullptr);

    lv_obj_t *rbtn = lv_obj_get_parent(s_np_repeat);
    s_np_repeat_one = lv_label_create(rbtn);
    lv_label_set_text(s_np_repeat_one, "1");
    lv_obj_center(s_np_repeat_one);
    lv_obj_add_flag(s_np_repeat_one, LV_OBJ_FLAG_HIDDEN);

    s_np_vol = lv_slider_create(s_np_grp);
    lv_obj_set_width(s_np_vol, 300);
    lv_slider_set_range(s_np_vol, 0, 100);
    reg(s_np_vol, ROLE_TRACK);
    reg(s_np_vol, ROLE_INDICATOR);
    reg(s_np_vol, ROLE_KNOB);
    lv_obj_add_event_cb(s_np_vol, volume_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_clear_flag(s_np_vol, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
}

const char *browse_title_text()
{
    return s_nav.empty() ? "Browse" : s_nav.back().name.c_str();
}

void browse_drill(const ma::BrowseItem &it)
{
    NavLevel lv;
    lv.name = it.name;
    if (it.media_type == "folder")        { lv.kind = 'B'; lv.a = it.path;    lv.b = ""; }
    else if (it.media_type == "artist")   { lv.kind = 'A'; lv.a = it.item_id; lv.b = it.provider; }
    else if (it.media_type == "album")    { lv.kind = 'T'; lv.a = it.item_id; lv.b = it.provider; }
    else if (it.media_type == "playlist") { lv.kind = 'P'; lv.a = it.item_id; lv.b = it.provider; }
    else return;
    s_nav.push_back(lv);
    s_alpha_active = false;
    s_alpha_letter = 0;
    lv_label_set_text(s_browse_title, "Loading...");
    ma::browse(lv.kind, lv.a, lv.b);
}

void browse_row_cb(lv_event_t *e)
{
    int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (i < 0 || i >= static_cast<int>(s_browse_cur.size())) return;
    const ma::BrowseItem &it = s_browse_cur[i];
    if (it.drillable) {
        browse_drill(it);
        return;
    }
    if (!it.playable) return;

    if (it.media_type == "track") {
        // Inside an album/playlist track list: play from here to the end so
        // Next/Prev walk the rest.
        std::vector<std::string> uris;
        for (size_t j = static_cast<size_t>(i); j < s_browse_cur.size(); ++j)
            if (s_browse_cur[j].media_type == "track" && !s_browse_cur[j].uri.empty())
                uris.push_back(s_browse_cur[j].uri);
        if (uris.size() > 1) ma::play_list(uris);
        else                 ma::play_media(it.uri);
    } else {
        // Playlist (play the whole thing), radio/SiriusXM channel, or any other
        // single playable: play just that item. A one-item queue can't shuffle
        // to a random channel.
        ma::play_media(it.uri);
    }
    if (s_tileview) lv_tileview_set_tile_by_index(s_tileview, 1, 0, LV_ANIM_ON);  // jump to Now Playing
}

void letter_cb(lv_event_t *e)
{
    char c = static_cast<char>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    s_alpha_letter = c;
    lv_label_set_text(s_browse_title, "Loading...");
    ma::browse_letter(c);
}

void browse_back_cb(lv_event_t *)
{
    // Inside a letter's list -> back to the letter grid (no fetch).
    if (s_alpha_active && s_alpha_letter != 0) {
        s_alpha_letter = 0;
        show_letter_grid(s_alpha_letters);
        return;
    }
    // Letter grid or a normal list -> up one level.
    if (s_nav.empty()) return;
    s_nav.pop_back();
    s_alpha_active = false;
    s_alpha_letter = 0;
    lv_label_set_text(s_browse_title, "Loading...");
    if (s_nav.empty()) ma::browse('B', "", "");
    else { const NavLevel &lv = s_nav.back(); ma::browse(lv.kind, lv.a, lv.b); }
}

void show_letter_grid(const std::string &letters)
{
    lv_obj_clean(s_browse_list);
    lv_obj_t *grid = lv_obj_create(s_browse_list);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);

    for (char c : letters) {
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 58, 58);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, c_surface(), 0);
        lv_obj_add_event_cb(b, letter_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>((intptr_t)c));
        lv_obj_t *l = lv_label_create(b);
        char s[2] = {c, 0};
        lv_label_set_text(l, s);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(l, c_text(), 0);
        lv_obj_center(l);
    }

    show(s_browse_back, !s_nav.empty());
    lv_label_set_text(s_browse_title, browse_title_text());
}

void populate_browse(const std::vector<ma::BrowseItem> &items)
{
    s_browse_cur = items;
    lv_obj_clean(s_browse_list);

    for (size_t i = 0; i < items.size(); ++i) {
        const char *sym = items[i].drillable ? LV_SYMBOL_DIRECTORY
                        : items[i].playable  ? LV_SYMBOL_PLAY
                                             : LV_SYMBOL_AUDIO;
        lv_obj_t *b = lv_list_add_button(s_browse_list, sym, items[i].name.c_str());
        lv_obj_set_style_bg_color(b, c_surface(), 0);
        lv_obj_set_style_text_color(b, c_text(), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_margin_bottom(b, 4, 0);
        if (items[i].drillable || items[i].playable)
            lv_obj_add_event_cb(b, browse_row_cb, LV_EVENT_CLICKED, reinterpret_cast<void *>((intptr_t)i));
    }
    if (items.empty()) {
        lv_obj_t *b = lv_list_add_text(s_browse_list, "(empty)");
        lv_obj_set_style_text_color(b, c_dim(), 0);
    }

    show(s_browse_back, !s_nav.empty());
    if (s_alpha_active && s_alpha_letter != 0) {
        char t[24];
        snprintf(t, sizeof(t), "%s  (%c)", browse_title_text(), s_alpha_letter);
        lv_label_set_text(s_browse_title, t);
    } else {
        lv_label_set_text(s_browse_title, browse_title_text());
    }
}

void build_browse_page(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g, 10, 0);
    lv_obj_set_style_pad_row(g, 8, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);

    // Header: back button + current-location title.
    lv_obj_t *hdr = lv_obj_create(g);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);

    s_browse_back = lv_button_create(hdr);
    lv_obj_set_size(s_browse_back, 44, 44);
    lv_obj_set_style_radius(s_browse_back, 10, 0);
    lv_obj_set_style_shadow_width(s_browse_back, 0, 0);
    reg(s_browse_back, ROLE_SURFACE);
    lv_obj_add_event_cb(s_browse_back, browse_back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(s_browse_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    reg(bl, ROLE_TEXT);
    lv_obj_add_flag(s_browse_back, LV_OBJ_FLAG_HIDDEN);

    s_browse_title = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_browse_title, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_browse_title, "Browse");
    lv_label_set_long_mode(s_browse_title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(s_browse_title, 1);
    reg(s_browse_title, ROLE_TEXT);

    // Scrolling list of items.
    s_browse_list = lv_list_create(g);
    lv_obj_set_width(s_browse_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_browse_list, 1);
    lv_obj_set_style_bg_opa(s_browse_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_browse_list, 0, 0);
    lv_obj_set_style_pad_all(s_browse_list, 0, 0);
    reg(s_browse_list, ROLE_BG);
}

lv_obj_t *settings_row(lv_obj_t *parent, const char *section, lv_event_cb_t cb, const char *value)
{
    lv_obj_t *sec = lv_label_create(parent);
    lv_label_set_text(sec, section);
    reg(sec, ROLE_DIM);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 360, 52);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    reg(btn, ROLE_SURFACE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, value);
    lv_obj_center(lbl);
    reg(lbl, ROLE_TEXT);
    return lbl;
}

void build_settings_page(lv_obj_t *parent)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(g, 30, 0);
    lv_obj_set_style_pad_row(g, 8, 0);

    lv_obj_t *title = lv_label_create(g);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    reg(title, ROLE_TEXT);

    std::string room = settings::get("ma_player_name");
    s_speaker_btn_label = settings_row(g, "SPEAKER", open_speaker_picker,
                                       room.empty() ? "Choose speaker" : room.c_str());
    s_theme_btn_label = settings_row(g, "THEME", open_theme_picker, theme::name(theme::current_index()));
    s_orient_btn_label = settings_row(g, "ORIENTATION", open_orient_picker, kOrientNames[orient_current_index()]);
}

void build_ui()
{
    lv_obj_t *scr = lv_screen_active();
    reg(scr, ROLE_BG);

    lv_obj_t *tv = lv_tileview_create(scr);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    reg(tv, ROLE_BG);
    s_tileview = tv;

    lv_obj_t *t_browse = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    lv_obj_t *t_now    = lv_tileview_add_tile(tv, 1, 0, LV_DIR_HOR);
    lv_obj_t *t_set    = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
    for (lv_obj_t *t : {t_browse, t_now, t_set}) {
        lv_obj_set_style_pad_all(t, 0, 0);
        reg(t, ROLE_BG);
    }

    build_browse_page(t_browse);
    build_status_group(t_now);
    build_idle_group(t_now);
    build_now_playing_group(t_now);
    build_settings_page(t_set);

    lv_tileview_set_tile_by_index(tv, 1, 0, LV_ANIM_OFF);
    apply_theme();
    lv_timer_create(status_tick, 500, nullptr);
}

} // namespace

extern "C" void app_main()
{
    mp::log::init();
    MP_LOGI(kTag, "MusicPanel starting");

    ESP_ERROR_CHECK(settings::init());
    theme::init();
    int deg = atoi(settings::get("orientation", "0").c_str());

    if (bsp::init(deg) != ESP_OK) {
        MP_LOGE(kTag, "BSP init failed — halting");
        return;
    }
    MP_LOGI(kTag, "BSP up: %ux%u rot %d", bsp::width(), bsp::height(), bsp::rotation());

    ESP_ERROR_CHECK(network::init());
    timesync::init();
    weather::init();
    ma::init();

    bsp::lvgl_lock(-1);
    build_ui();
    bsp::lvgl_unlock();

    bsp::set_backlight(100);

    network::start();
    ma::start();

    vTaskDelete(nullptr);
}
