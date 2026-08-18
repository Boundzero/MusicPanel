#include "ma.h"
#include "network.h"
#include "settings.h"
#include "mp_log.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace ma {
namespace {

constexpr const char *kTag = "ma";
constexpr const char *kTempToken = "";

constexpr int kArtReq   = 160;
constexpr int kArtAlloc = 176;

struct Cmd { char command[48]; char args[2048]; };
struct BrowseReq { char kind; char a[128]; char b[64]; };

std::atomic<State> s_state{State::kIdle};
SemaphoreHandle_t  s_mux = nullptr;
QueueHandle_t      s_cmdq = nullptr;
std::string        s_status = "Starting";
std::string        s_error;
NowPlaying         s_np;
int64_t            s_np_at_ms = 0;
std::vector<PlayerInfo> s_players;

// Wall-clock position: MA reports elapsed_time as-of a server timestamp
// (elapsed_time_last_updated). With SNTP-synced time we compute the exact live
// position as base + (now - measured_at); no smoothing, no sawtooth.
double s_pos_base  = 0;   // elapsed seconds at s_pos_lu
double s_pos_lu    = 0;   // server unix time when measured
bool   s_pos_valid = false;

uint8_t   *s_art_pending = nullptr;
int        s_art_w = 0, s_art_h = 0;
bool       s_art_new = false;
std::string s_art_url;

// Browse: served by a dedicated task so navigation doesn't block the UI or the
// poll loop. url/token are cached once the poll task has resolved them.
std::string       s_api_url;
std::string       s_token;
QueueHandle_t     s_browse_q = nullptr;
std::vector<BrowseItem> s_browse_items;
std::string       s_browse_letters;      // non-empty => show A-Z picker
cJSON            *s_browse_cache = nullptr;  // PSRAM tree of a large folder (browse_task only)
bool              s_browse_ready = false;
std::atomic<bool> s_browse_inflight{false};

constexpr int kBrowseRowCap = 30;   // max rows rendered at once (UI memory limit)
constexpr int kAlphaThreshold = 30; // folders larger than this switch to A-Z

void lock()   { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }
int64_t now_ms() { return esp_timer_get_time() / 1000; }

std::string jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}
double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}
uint32_t rgb_from_array(cJSON *arr)
{
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 3) return 0;
    int r = static_cast<int>(cJSON_GetNumberValue(cJSON_GetArrayItem(arr, 0)));
    int g = static_cast<int>(cJSON_GetNumberValue(cJSON_GetArrayItem(arr, 1)));
    int b = static_cast<int>(cJSON_GetNumberValue(cJSON_GetArrayItem(arr, 2)));
    auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
    return (cl(r) << 16) | (cl(g) << 8) | cl(b);
}

// Fold a Latin-1 accented letter (second byte of a C3 xx UTF-8 sequence) to ASCII.
void append_latin1(std::string &o, unsigned char t)
{
    int cp = 0xC0 + (t & 0x3F);  // U+00C0 .. U+00FF
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: o += 'A'; break;
        case 0xC6: o += "AE"; break;
        case 0xC7: o += 'C'; break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: o += 'E'; break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: o += 'I'; break;
        case 0xD0: o += 'D'; break;
        case 0xD1: o += 'N'; break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: o += 'O'; break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: o += 'U'; break;
        case 0xDD: o += 'Y'; break;
        case 0xDF: o += "ss"; break;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: o += 'a'; break;
        case 0xE6: o += "ae"; break;
        case 0xE7: o += 'c'; break;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: o += 'e'; break;
        case 0xEC: case 0xED: case 0xEE: case 0xEF: o += 'i'; break;
        case 0xF0: o += 'd'; break;
        case 0xF1: o += 'n'; break;
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: o += 'o'; break;
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: o += 'u'; break;
        case 0xFD: case 0xFF: o += 'y'; break;
        default: break;  // ×, ÷, Þ, þ and anything else: drop
    }
}

// Replace UTF-8 smart punctuation / accents with ASCII so LVGL's Montserrat
// (ASCII-only) font doesn't render them as "tofu" boxes.
std::string normalize(const std::string &s)
{
    std::string o;
    o.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { o += static_cast<char>(c); i++; continue; }

        if (c == 0xE2 && i + 2 < n && static_cast<unsigned char>(s[i + 1]) == 0x80) {
            switch (static_cast<unsigned char>(s[i + 2])) {
                case 0x98: case 0x99: o += '\''; break;   // ' '
                case 0x9C: case 0x9D: o += '"';  break;   // " "
                case 0x93: case 0x94: o += '-';  break;   // – —
                case 0xA6: o += "...";            break;   // …
                default: break;                            // other punctuation: drop
            }
            i += 3; continue;
        }
        if (c == 0xC2 && i + 1 < n) {
            if (static_cast<unsigned char>(s[i + 1]) == 0xA0) o += ' ';  // nbsp
            i += 2; continue;
        }
        if (c == 0xC3 && i + 1 < n) {
            append_latin1(o, static_cast<unsigned char>(s[i + 1]));
            i += 2; continue;
        }
        // Unknown multibyte: skip lead + continuation bytes.
        i++;
        while (i < n && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
    }
    return o;
}

bool api_post(const std::string &url, const std::string &token,
              const std::string &json_body, std::string &body_out, int &status_out)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 6000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    std::string auth = "Bearer " + token;
    esp_http_client_set_header(c, "Authorization", auth.c_str());

    bool ok = false;
    if (esp_http_client_open(c, json_body.size()) == ESP_OK) {
        if (esp_http_client_write(c, json_body.data(), json_body.size()) >= 0) {
            esp_http_client_fetch_headers(c);
            status_out = esp_http_client_get_status_code(c);
            char buf[512];
            int r;
            while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) body_out.append(buf, r);
            ok = true;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

bool http_get(const std::string &url, const std::string &token, std::string &out)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    std::string auth = "Bearer " + token;
    esp_http_client_set_header(c, "Authorization", auth.c_str());

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            char buf[1024];
            int r;
            while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
                out.append(buf, r);
                if (out.size() > 300000) break;
            }
            ok = !out.empty();
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

std::string resize_url(const std::string &u, int px)
{
    auto p = u.find("size=");
    if (p == std::string::npos) return u;
    size_t vstart = p + 5;
    auto e = u.find('&', vstart);
    std::string val = std::to_string(px);
    if (e == std::string::npos) return u.substr(0, vstart) + val;
    return u.substr(0, vstart) + val + u.substr(e);
}

void set_pending_art(uint8_t *buf, int w, int h)
{
    lock();
    if (s_art_pending) heap_caps_free(s_art_pending);
    s_art_pending = buf; s_art_w = w; s_art_h = h; s_art_new = true;
    unlock();
}

void fetch_and_decode_art(const std::string &image_url, const std::string &token)
{
    std::string url = resize_url(image_url, kArtReq);
    std::string jpg;
    if (!http_get(url, token, jpg)) { set_pending_art(nullptr, 0, 0); return; }

    const size_t cap = static_cast<size_t>(kArtAlloc) * kArtAlloc * 2;
    uint8_t *out = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!out) { set_pending_art(nullptr, 0, 0); return; }

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = reinterpret_cast<uint8_t *>(jpg.data());
    cfg.indata_size = jpg.size();
    cfg.outbuf = out;
    cfg.outbuf_size = cap;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;
    cfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t oi;
    if (esp_jpeg_decode(&cfg, &oi) != ESP_OK) {
        heap_caps_free(out);
        set_pending_art(nullptr, 0, 0);
        return;
    }
    set_pending_art(out, oi.width, oi.height);
}

void fill_from_player(cJSON *p, NowPlaying &np)
{
    std::string st = jstr(p, "playback_state");
    np.playing = (st == "playing" || st == "paused");
    np.paused  = (st == "paused");
    np.room    = normalize(jstr(p, "display_name"));
    np.queue_id = jstr(p, "player_id");
    np.volume   = static_cast<int>(jnum(p, "volume_level"));
    if (np.playing) {
        cJSON *cm = cJSON_GetObjectItem(p, "current_media");
        if (cJSON_IsObject(cm)) {
            np.title     = normalize(jstr(cm, "title"));
            np.artist    = normalize(jstr(cm, "artist"));
            np.album     = normalize(jstr(cm, "album"));
            np.image_url = jstr(cm, "image_url");
            np.duration_s = static_cast<int>(jnum(cm, "duration"));
            np.elapsed_s  = static_cast<int>(jnum(cm, "elapsed_time"));
            // players/all can also carry the measurement timestamp; used as a
            // fallback position source (the queue's copy is preferred).
            double lu = jnum(cm, "elapsed_time_last_updated");
            if (lu > 1600000000) {
                s_pos_base = jnum(cm, "elapsed_time");
                s_pos_lu = lu;
                s_pos_valid = true;
            } else {
                s_pos_valid = false;  // may be set by the queue poll
            }
            cJSON *pal = cJSON_GetObjectItem(cm, "palette");
            if (cJSON_IsObject(pal)) {
                np.pal_accent = rgb_from_array(cJSON_GetObjectItem(pal, "accent"));
                np.pal_bg     = rgb_from_array(cJSON_GetObjectItem(pal, "background_dark"));
                np.has_palette = (np.pal_accent != 0 || np.pal_bg != 0);
            }
        }
    }
}

void parse_players(const std::string &json)
{
    cJSON *root = cJSON_Parse(json.c_str());
    NowPlaying np;
    std::vector<PlayerInfo> plist;
    std::string sel = settings::get("ma_player");

    if (cJSON_IsArray(root)) {
        cJSON *selp = nullptr, *first_playing = nullptr, *p = nullptr;
        cJSON_ArrayForEach(p, root) {
            std::string id = jstr(p, "player_id");
            if (id.empty()) continue;
            bool hide = cJSON_IsTrue(cJSON_GetObjectItem(p, "hide_in_ui"));
            if (!hide) {
                PlayerInfo pi;
                pi.id = id;
                pi.name = normalize(jstr(p, "display_name"));
                pi.available = cJSON_IsTrue(cJSON_GetObjectItem(p, "available"));
                plist.push_back(pi);
            }
            std::string st = jstr(p, "playback_state");
            if (id == sel) selp = p;
            if ((st == "playing" || st == "paused") && !first_playing) first_playing = p;
        }

        cJSON *use = sel.empty() ? first_playing : selp;
        if (use) {
            fill_from_player(use, np);
        } else if (!sel.empty()) {
            np.room = normalize(settings::get("ma_player_name"));
        }
    }
    if (root) cJSON_Delete(root);

    lock();
    np.shuffle = s_np.shuffle;
    np.repeat  = s_np.repeat;
    s_np = np;
    s_np_at_ms = now_ms();
    s_players = plist;
    if (!np.playing) s_status = np.room.empty() ? "Choose a speaker in Settings" : "Nothing playing";
    unlock();
}

void parse_queues(const std::string &json)
{
    cJSON *root = cJSON_Parse(json.c_str());
    if (cJSON_IsArray(root)) {
        lock(); std::string want = s_np.queue_id; unlock();
        if (!want.empty()) {
            cJSON *q = nullptr;
            cJSON_ArrayForEach(q, root) {
                if (jstr(q, "queue_id") != want) continue;
                bool sh = cJSON_IsTrue(cJSON_GetObjectItem(q, "shuffle_enabled"));
                std::string rp = jstr(q, "repeat_mode");
                double el = jnum(q, "elapsed_time");
                double lu = jnum(q, "elapsed_time_last_updated");
                lock();
                s_np.shuffle = sh;
                if (!rp.empty()) s_np.repeat = rp;
                if (lu > 1600000000) {  // queue's timestamp is authoritative
                    s_pos_base = el;
                    s_pos_lu = lu;
                    s_pos_valid = true;
                }
                unlock();
                break;
            }
        }
    }
    if (root) cJSON_Delete(root);
}

std::string pretty(const std::string &s)
{
    std::string o = s;
    for (char &c : o) if (c == '_') c = ' ';
    if (!o.empty()) o[0] = static_cast<char>(toupper(static_cast<unsigned char>(o[0])));
    return o;
}

std::string browse_display_name(cJSON *it)
{
    std::string name = jstr(it, "name");
    if (name == "..") return "";  // MA's parent-nav entry; we handle Back ourselves
    std::string tkey = jstr(it, "translation_key");
    return name.empty() ? pretty(tkey) : normalize(name);
}

BrowseItem make_item(cJSON *it, const std::string &disp)
{
    BrowseItem bi;
    bi.name = disp;
    bi.path = jstr(it, "path");
    bi.uri = jstr(it, "uri");
    bi.item_id = jstr(it, "item_id");
    bi.provider = jstr(it, "provider");
    bi.media_type = jstr(it, "media_type");
    bi.is_folder = (bi.media_type == "folder");
    bi.playable = cJSON_IsTrue(cJSON_GetObjectItem(it, "is_playable"));
    // Folders, artists, and albums open deeper (browse). Playlists play whole,
    // tracks and radio play directly — handled in the UI tap.
    bi.drillable = (bi.media_type == "folder" || bi.media_type == "artist" ||
                    bi.media_type == "album");
    return bi;
}

char first_letter(const std::string &disp)
{
    char c = static_cast<char>(toupper(static_cast<unsigned char>(disp[0])));
    return (c >= 'A' && c <= 'Z') ? c : '#';
}

void parse_browse_root(cJSON *root, std::vector<BrowseItem> &out)
{
    if (!cJSON_IsArray(root)) return;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, root) {
        std::string disp = browse_display_name(it);
        if (disp.empty()) continue;
        out.push_back(make_item(it, disp));
        if ((int)out.size() >= kBrowseRowCap) break;
    }
}

void compute_letters(cJSON *root, std::string &out)
{
    bool seen[27] = {false};
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, root) {
        std::string disp = browse_display_name(it);
        if (disp.empty()) continue;
        char c = first_letter(disp);
        if (c == '#') seen[26] = true; else seen[c - 'A'] = true;
    }
    for (int i = 0; i < 26; ++i) if (seen[i]) out += static_cast<char>('A' + i);
    if (seen[26]) out += '#';
}

void filter_cache(char letter, std::vector<BrowseItem> &out)
{
    if (!s_browse_cache) return;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, s_browse_cache) {
        std::string disp = browse_display_name(it);
        if (disp.empty()) continue;
        if (first_letter(disp) != letter) continue;
        out.push_back(make_item(it, disp));
        if ((int)out.size() >= kBrowseRowCap) break;
    }
}

// Fetch a browse response into a PSRAM buffer (grows as needed, hard-capped).
bool browse_fetch(const std::string &body, char **outbuf, size_t *outlen)
{
    esp_http_client_config_t cfg = {};
    cfg.url = s_api_url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    std::string auth = "Bearer " + s_token;
    esp_http_client_set_header(c, "Authorization", auth.c_str());

    size_t cap = 64 * 1024, len = 0;
    char *buf = static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!buf) { esp_http_client_cleanup(c); return false; }

    bool ok = false;
    if (esp_http_client_open(c, body.size()) == ESP_OK &&
        esp_http_client_write(c, body.data(), body.size()) >= 0) {
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st == 200) {
            char tmp[1024];
            int r;
            while ((r = esp_http_client_read(c, tmp, sizeof(tmp))) > 0) {
                if (len + r > cap) {
                    size_t ncap = cap * 2;
                    if (ncap > 2 * 1024 * 1024) { MP_LOGW(kTag, "browse response too large"); break; }
                    char *nb = static_cast<char *>(heap_caps_realloc(buf, ncap, MALLOC_CAP_SPIRAM));
                    if (!nb) break;
                    buf = nb; cap = ncap;
                }
                memcpy(buf + len, tmp, r);
                len += r;
            }
            ok = (len > 0);
        } else {
            MP_LOGW(kTag, "browse HTTP %d", st);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) { heap_caps_free(buf); return false; }
    *outbuf = buf;
    *outlen = len;
    return true;
}

void browse_task(void *)
{
    BrowseReq r;
    for (;;) {
        if (xQueueReceive(s_browse_q, &r, portMAX_DELAY) != pdTRUE) continue;

        std::vector<BrowseItem> items;
        std::string letters;

        if (r.kind == 'L') {
            // Letter filter on the cached large folder (no network).
            filter_cache(r.a[0], items);
            MP_LOGI(kTag, "browse letter '%c' -> %d items", r.a[0], (int)items.size());
        } else {
            std::string a = r.a, b = r.b, cmd, args;
            if (r.kind == 'A') {
                cmd = "music/artists/artist_albums";
                args = "{\"item_id\":\"" + a + "\",\"provider_instance_id_or_domain\":\"" + b + "\"}";
            } else if (r.kind == 'T') {
                cmd = "music/albums/album_tracks";
                args = "{\"item_id\":\"" + a + "\",\"provider_instance_id_or_domain\":\"" + b + "\"}";
            } else if (r.kind == 'P') {
                cmd = "music/playlists/playlist_tracks";
                args = "{\"item_id\":\"" + a + "\",\"provider_instance_id_or_domain\":\"" + b + "\"}";
            } else {  // 'B' folder browse
                cmd = "music/browse";
                args = a.empty() ? "{}" : (std::string("{\"path\":\"") + a + "\"}");
            }
            std::string body = std::string("{\"message_id\":\"b\",\"command\":\"") + cmd + "\",\"args\":" + args + "}";

            // A new fetch invalidates any cached large folder.
            if (s_browse_cache) { cJSON_Delete(s_browse_cache); s_browse_cache = nullptr; }

            char *buf = nullptr;
            size_t len = 0;
            if (browse_fetch(body, &buf, &len)) {
                cJSON *root = cJSON_ParseWithLength(buf, len);
                heap_caps_free(buf);
                if (root) {
                    int n = cJSON_IsArray(root) ? cJSON_GetArraySize(root) : 0;
                    if (n > kAlphaThreshold + 1) {   // +1 for a possible ".." entry
                        compute_letters(root, letters);
                        s_browse_cache = root;       // keep for letter filtering
                    } else {
                        parse_browse_root(root, items);
                        cJSON_Delete(root);
                    }
                }
                MP_LOGI(kTag, "browse [%c] '%s' -> %d items%s (free int=%u)",
                        r.kind, a.empty() ? "root" : a.c_str(), (int)items.size(),
                        letters.empty() ? "" : " [A-Z]",
                        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            } else {
                MP_LOGW(kTag, "browse fetch failed");
            }
        }

        lock();
        s_browse_items = items;
        s_browse_letters = letters;
        s_browse_ready = true;
        unlock();
        s_browse_inflight = false;
    }
}

void run_command(const std::string &url, const std::string &token, const Cmd &c)
{
    std::string body = std::string("{\"message_id\":\"c\",\"command\":\"") + c.command +
                       "\",\"args\":" + c.args + "}";
    std::string resp;
    int status = 0;
    api_post(url, token, body, resp, status);
    MP_LOGI(kTag, "cmd %s -> HTTP %d", c.command, status);
}

void task(void *)
{
    s_state = State::kWaitingNetwork;
    while (network::state() != network::State::kConnected) vTaskDelay(pdMS_TO_TICKS(500));

    std::string token = settings::get("ma_token");
    if (token.empty()) token = kTempToken;
    if (token.empty()) {
        s_state = State::kNoToken;
        for (;;) vTaskDelay(pdMS_TO_TICKS(2000));
    }

    std::string url = "http://" + settings::get("ma_host") + ":" + settings::get("ma_port", "8095") + "/api";
    const std::string players_body = R"({"message_id":"1","command":"players/all","args":{}})";
    const std::string queues_body  = R"({"message_id":"2","command":"player_queues/all","args":{}})";
    MP_LOGI(kTag, "polling %s", url.c_str());
    s_state = State::kConnecting;

    // Hand url/token to the browse task and start it now that creds exist.
    s_api_url = url;
    s_token = token;
    xTaskCreate(browse_task, "ma_browse", 8192, nullptr, 4, nullptr);

    for (;;) {
        Cmd c;
        if (xQueueReceive(s_cmdq, &c, pdMS_TO_TICKS(3000)) == pdTRUE) {
            run_command(url, token, c);
            vTaskDelay(pdMS_TO_TICKS(400));
        }

        std::string resp;
        int status = 0;
        if (!api_post(url, token, players_body, resp, status)) {
            s_state = State::kError;
            lock(); s_error = "no response (check server address)"; unlock();
            continue;
        }
        if (status == 401 || status == 403) {
            s_state = State::kError;
            lock(); s_error = "auth failed (check token)"; unlock();
            continue;
        }
        if (status != 200) {
            s_state = State::kError;
            lock(); s_error = "HTTP " + std::to_string(status); unlock();
            continue;
        }
        parse_players(resp);
        s_state = State::kConnected;

        lock();
        bool playing = s_np.playing;
        std::string img = s_np.image_url;
        unlock();

        if (playing) {
            std::string qresp;
            int qstatus = 0;
            if (api_post(url, token, queues_body, qresp, qstatus) && qstatus == 200)
                parse_queues(qresp);
            if (img != s_art_url) {
                s_art_url = img;
                if (img.empty()) set_pending_art(nullptr, 0, 0);
                else             fetch_and_decode_art(img, token);
            }
        } else if (!s_art_url.empty()) {
            s_art_url.clear();
            set_pending_art(nullptr, 0, 0);
        }
    }
}

void enqueue(const std::string &cmd, const std::string &args)
{
    if (!s_cmdq) return;
    Cmd c{};
    std::strncpy(c.command, cmd.c_str(), sizeof(c.command) - 1);
    std::strncpy(c.args, args.c_str(), sizeof(c.args) - 1);
    xQueueSend(s_cmdq, &c, 0);
}

} // namespace

void *psram_malloc(size_t s) { return heap_caps_malloc(s, MALLOC_CAP_SPIRAM); }
void  psram_free(void *p)    { heap_caps_free(p); }

void init()
{
    cJSON_Hooks hooks;
    hooks.malloc_fn = psram_malloc;
    hooks.free_fn = psram_free;
    cJSON_InitHooks(&hooks);   // keep JSON parse trees off the scarce internal heap

    s_mux = xSemaphoreCreateMutex();
    s_cmdq = xQueueCreate(6, sizeof(Cmd));
    s_browse_q = xQueueCreate(4, sizeof(BrowseReq));
}
void start() { xTaskCreate(task, "ma", 16384, nullptr, 4, nullptr); }

State state() { return s_state.load(); }
std::string status_line() { lock(); std::string v = s_status; unlock(); return v; }
std::string error_text()  { lock(); std::string v = s_error;  unlock(); return v; }

NowPlaying now_playing()
{
    lock();
    NowPlaying np = s_np;
    int64_t at = s_np_at_ms;
    double pos_base = s_pos_base;
    double pos_lu = s_pos_lu;
    bool pos_valid = s_pos_valid;
    unlock();

    if (np.playing) {
        time_t nowu = time(nullptr);
        if (pos_valid && nowu > 1600000000) {
            // Exact: elapsed at measurement + real time since (frozen if paused).
            double delta = np.paused ? 0.0 : (static_cast<double>(nowu) - pos_lu);
            int p = static_cast<int>(pos_base + delta + 0.5);
            if (p < 0) p = 0;
            if (np.duration_s > 0 && p > np.duration_s) p = np.duration_s;
            np.elapsed_s = p;
        } else if (!np.paused && np.duration_s > 0) {
            // Fallback before SNTP sync: advance locally from the last poll.
            np.elapsed_s += static_cast<int>((now_ms() - at) / 1000);
            if (np.elapsed_s > np.duration_s) np.elapsed_s = np.duration_s;
        }
    }
    return np;
}

void get_players(std::vector<PlayerInfo> &out)
{
    lock();
    out = s_players;
    unlock();
}

bool take_album_art(uint8_t **buf, int *w, int *h)
{
    lock();
    if (!s_art_new) { unlock(); return false; }
    *buf = s_art_pending; *w = s_art_w; *h = s_art_h;
    s_art_pending = nullptr; s_art_new = false;
    unlock();
    return true;
}

void transport(const std::string &command, const std::string &queue_id)
{
    if (queue_id.empty()) return;
    enqueue(command, "{\"queue_id\":\"" + queue_id + "\"}");
}
void set_volume(const std::string &player_id, int level)
{
    if (player_id.empty()) return;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    enqueue("players/cmd/volume_set",
            "{\"player_id\":\"" + player_id + "\",\"volume_level\":" + std::to_string(level) + "}");
}
void set_shuffle(const std::string &queue_id, bool on)
{
    if (queue_id.empty()) return;
    enqueue("player_queues/shuffle",
            "{\"queue_id\":\"" + queue_id + "\",\"shuffle_enabled\":" + (on ? "true" : "false") + "}");
}
void set_repeat(const std::string &queue_id, const std::string &mode)
{
    if (queue_id.empty()) return;
    enqueue("player_queues/repeat",
            "{\"queue_id\":\"" + queue_id + "\",\"repeat_mode\":\"" + mode + "\"}");
}

void browse(char kind, const std::string &a, const std::string &b)
{
    if (!s_browse_q) return;
    BrowseReq r{};
    r.kind = kind;
    std::strncpy(r.a, a.c_str(), sizeof(r.a) - 1);
    std::strncpy(r.b, b.c_str(), sizeof(r.b) - 1);
    s_browse_inflight = true;
    xQueueSend(s_browse_q, &r, 0);
}

void browse_letter(char letter)
{
    if (!s_browse_q) return;
    BrowseReq r{};
    r.kind = 'L';
    r.a[0] = letter;
    s_browse_inflight = true;
    xQueueSend(s_browse_q, &r, 0);
}

bool browse_inflight() { return s_browse_inflight.load(); }

bool take_browse_result(std::vector<BrowseItem> &items, std::string &letters)
{
    lock();
    if (!s_browse_ready) { unlock(); return false; }
    items = s_browse_items;
    letters = s_browse_letters;
    s_browse_ready = false;
    unlock();
    return true;
}

void play_media(const std::string &uri)
{
    std::string q = settings::get("ma_player");
    if (q.empty() || uri.empty()) return;
    // "replace" clears the room's queue and starts fresh (no leftovers from a
    // previous selection bleeding into Next).
    enqueue("player_queues/play_media",
            "{\"queue_id\":\"" + q + "\",\"media\":\"" + uri + "\",\"option\":\"replace\"}");
}

void play_list(const std::vector<std::string> &uris)
{
    std::string q = settings::get("ma_player");
    if (q.empty() || uris.empty()) return;
    std::string arr = "[";
    for (size_t i = 0; i < uris.size(); ++i) {
        std::string piece = (i ? "," : "") + ("\"" + uris[i] + "\"");
        if (arr.size() + piece.size() > 1900) break;  // stay within Cmd.args
        arr += piece;
    }
    arr += "]";
    enqueue("player_queues/play_media",
            "{\"queue_id\":\"" + q + "\",\"media\":" + arr + ",\"option\":\"replace\"}");
}

} // namespace ma
