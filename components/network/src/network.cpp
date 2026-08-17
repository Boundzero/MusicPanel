// =============================================================================
// Network implementation.
//
// Provisioned  -> connect as a station using saved credentials.
// Unprovisioned-> APSTA: run SoftAP + the setup page.
//
// The config web page runs in both modes. It carries Wi-Fi, Music Assistant
// host/token, time zone, and location (for weather). Secrets (password, token)
// are left blank and only overwritten when a new value is entered.
// =============================================================================

#include "network.h"
#include "settings.h"
#include "mp_log.h"

#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <string>

namespace network {
namespace {

constexpr const char *kTag = "net";
constexpr int kMaxRetry = 5;

struct TzOption { const char *val; const char *label; };
const TzOption kZones[] = {
    {"EST5EDT,M3.2.0,M11.1.0",     "Eastern"},
    {"CST6CDT,M3.2.0,M11.1.0",     "Central"},
    {"MST7MDT,M3.2.0,M11.1.0",     "Mountain"},
    {"MST7",                       "Arizona (no DST)"},
    {"PST8PDT,M3.2.0,M11.1.0",     "Pacific"},
    {"AKST9AKDT,M3.2.0,M11.1.0",   "Alaska"},
    {"HST10",                      "Hawaii"},
    {"GMT0BST,M3.5.0/1,M10.5.0",   "UK"},
    {"CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe"},
    {"UTC0",                       "UTC"},
};

std::atomic<State> s_state{State::kIdle};
std::atomic<int>   s_retry{0};
httpd_handle_t     s_httpd = nullptr;

SemaphoreHandle_t s_str_mux = nullptr;
std::string s_ip;
std::string s_ssid;
std::string s_ap_ssid;

void set_str(std::string &dst, const std::string &v)
{
    if (s_str_mux) xSemaphoreTake(s_str_mux, portMAX_DELAY);
    dst = v;
    if (s_str_mux) xSemaphoreGive(s_str_mux);
}
std::string get_str(const std::string &src)
{
    std::string v;
    if (s_str_mux) xSemaphoreTake(s_str_mux, portMAX_DELAY);
    v = src;
    if (s_str_mux) xSemaphoreGive(s_str_mux);
    return v;
}

std::string mac_suffix()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return buf;
}

std::string url_decode(const char *in)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    std::string out;
    for (size_t i = 0; in[i]; ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            out += static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2]));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string html_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string json_escape(const char *s)
{
    std::string out;
    for (size_t i = 0; s[i]; ++i) {
        if (s[i] == '"' || s[i] == '\\') out += '\\';
        out += s[i];
    }
    return out;
}

// ---- HTTP handlers ---------------------------------------------------------

std::string build_page()
{
    const std::string ssid = html_escape(settings::get("wifi_ssid"));
    const std::string mah  = html_escape(settings::get("ma_host"));
    const std::string map  = html_escape(settings::get("ma_port", "8095"));
    const std::string loc  = html_escape(settings::get("location"));
    const std::string tz   = settings::get("tz", "EST5EDT,M3.2.0,M11.1.0");
    const bool have_pass = !settings::get("wifi_pass").empty();
    const bool have_tok  = !settings::get("ma_token").empty();

    std::string p;
    p += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>MusicPanel Setup</title><style>"
         "body{font-family:system-ui,sans-serif;background:#101014;color:#f2f2f5;margin:0;padding:24px;}"
         "h1{font-size:1.3rem;font-weight:600;}label{display:block;margin:16px 0 4px;color:#9a9aa2;font-size:.9rem;}"
         "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:10px;border:1px solid #33333a;"
         "background:#1b1b20;color:#f2f2f5;font-size:1rem;}button{margin-top:24px;width:100%;padding:14px;border:0;"
         "border-radius:10px;background:#3b82f6;color:#fff;font-size:1rem;font-weight:600;}"
         ".row{display:flex;gap:12px;}.row>div{flex:1;}small{color:#6b6b73;display:block;margin-top:6px;}</style></head><body>"
         "<h1>MusicPanel Setup</h1><form method='POST' action='/save'>";

    p += "<label>Wi-Fi network</label>"
         "<input list='nets' name='ssid' autocomplete='off' value='" + ssid + "'>"
         "<datalist id='nets'></datalist>";

    p += "<label>Wi-Fi password</label><input type='password' name='pass' placeholder='";
    p += have_pass ? "leave blank to keep current" : "Password";
    p += "'>";

    p += "<label>Music Assistant server</label><div class='row'>"
         "<div><input name='ma_host' placeholder='192.168.1.x' value='" + mah + "'></div>"
         "<div><input name='ma_port' value='" + map + "'></div></div>";

    p += "<label>Music Assistant token</label><input name='ma_token' autocomplete='off' placeholder='";
    p += have_tok ? "leave blank to keep current" : "paste long-lived token";
    p += "'>";
    p += "<small>Music Assistant: Settings &rarr; Profile &rarr; create a Long-Lived Token.</small>";

    p += "<label>Time zone</label><select name='tz'>";
    for (const TzOption &z : kZones) {
        p += "<option value='";
        p += z.val;
        p += "'";
        if (tz == z.val) p += " selected";
        p += ">";
        p += z.label;
        p += "</option>";
    }
    p += "</select>";

    p += "<label>Location (city or ZIP, for weather)</label>"
         "<input name='location' value='" + loc + "' placeholder='e.g. Stony Brook or 11790'>";

    p += "<button type='submit'>Save &amp; Connect</button></form>"
         "<script>fetch('/scan').then(r=>r.json()).then(l=>{let d=document.getElementById('nets');"
         "l.forEach(s=>{let o=document.createElement('option');o.value=s;d.appendChild(o);});}).catch(()=>{});</script>"
         "</body></html>";
    return p;
}

esp_err_t root_handler(httpd_req_t *req)
{
    std::string page = build_page();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t sc = {};
    esp_wifi_scan_start(&sc, true);

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    static wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);

    std::string json = "[";
    for (int i = 0; i < n; ++i) {
        if (recs[i].ssid[0] == '\0') continue;
        if (json.size() > 1) json += ",";
        json += "\"" + json_escape(reinterpret_cast<char *>(recs[i].ssid)) + "\"";
    }
    json += "]";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t save_handler(httpd_req_t *req)
{
    std::string body;
    body.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }

    char val[512];
    auto field = [&](const char *k) -> std::string {
        if (httpd_query_key_value(body.c_str(), k, val, sizeof(val)) == ESP_OK)
            return url_decode(val);
        return "";
    };

    std::string ssid = field("ssid");
    if (ssid.empty()) {
        httpd_resp_send(req, "Missing Wi-Fi network name.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    settings::set("wifi_ssid", ssid);

    std::string pass = field("pass");
    if (!pass.empty()) settings::set("wifi_pass", pass);
    std::string tok = field("ma_token");
    if (!tok.empty()) settings::set("ma_token", tok);

    std::string mh = field("ma_host"), mp = field("ma_port");
    if (!mh.empty()) settings::set("ma_host", mh);
    if (!mp.empty()) settings::set("ma_port", mp);

    std::string tz = field("tz");
    if (!tz.empty()) settings::set("tz", tz);
    settings::set("location", field("location"));  // may be blank to clear

    MP_LOGI(kTag, "settings saved — restarting");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<html><body style='font-family:system-ui,sans-serif;background:#101014;color:#f2f2f5;"
        "text-align:center;padding-top:60px'><h2>Saved</h2>"
        "<p>MusicPanel is restarting to apply the changes.</p></body></html>",
        HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

void start_http()
{
    if (s_httpd) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        MP_LOGE(kTag, "failed to start config web server");
        return;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = nullptr};
    const httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = nullptr};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler, .user_ctx = nullptr};
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save);
}

// ---- Wi-Fi events ----------------------------------------------------------

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state.load() == State::kApPortal) return;
        if (s_retry.fetch_add(1) < kMaxRetry) {
            esp_wifi_connect();
            s_state = State::kConnecting;
        } else {
            MP_LOGW(kTag, "connect failed after %d tries", kMaxRetry);
            s_state = State::kFailed;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = static_cast<ip_event_got_ip_t *>(data);
        char buf[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, buf, sizeof(buf));
        set_str(s_ip, buf);
        s_retry = 0;
        s_state = State::kConnected;
        MP_LOGI(kTag, "connected, IP %s  (config page: http://%s )", buf, buf);
    }
}

void start_sta()
{
    std::string ssid = settings::get("wifi_ssid");
    std::string pass = settings::get("wifi_pass");
    set_str(s_ssid, ssid);

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char *>(wc.sta.ssid), ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wc.sta.password), pass.c_str(), sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    s_state = State::kConnecting;
    ESP_ERROR_CHECK(esp_wifi_start());
}

void start_ap_portal()
{
    std::string apssid = "MusicPanel-" + mac_suffix();
    set_str(s_ap_ssid, apssid);

    wifi_config_t ap = {};
    std::strncpy(reinterpret_cast<char *>(ap.ap.ssid), apssid.c_str(), sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = apssid.size();
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_state = State::kApPortal;
    ESP_ERROR_CHECK(esp_wifi_start());
    MP_LOGI(kTag, "setup portal: SSID '%s' -> http://192.168.4.1", apssid.c_str());
}

} // namespace

// ---- public API ------------------------------------------------------------

esp_err_t init()
{
    s_str_mux = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr, nullptr));

    return ESP_OK;
}

void start()
{
    if (settings::is_provisioned()) {
        MP_LOGI(kTag, "credentials found — connecting");
        start_sta();
    } else {
        MP_LOGI(kTag, "no credentials — starting setup portal");
        start_ap_portal();
    }
    start_http();
}

State       state()    { return s_state.load(); }
std::string ap_ssid()  { return get_str(s_ap_ssid); }
std::string ip()       { return get_str(s_ip); }
std::string ssid()     { return get_str(s_ssid); }

} // namespace network
