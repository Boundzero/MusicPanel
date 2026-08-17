#include "weather.h"
#include "network.h"
#include "settings.h"
#include "mp_log.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace weather {
namespace {

constexpr const char *kTag = "wx";

SemaphoreHandle_t s_mux = nullptr;
bool s_valid = false;
int  s_temp = 0;
int  s_code = 0;

void lock()   { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

const char *code_text(int c)
{
    if (c == 0) return "Clear";
    if (c == 1) return "Mostly clear";
    if (c == 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c >= 85 && c <= 86) return "Snow showers";
    if (c >= 95) return "Thunderstorm";
    return "";
}

std::string urlencode(const std::string &s)
{
    std::string o;
    char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else if (c == ' ') o += "%20";
        else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
    }
    return o;
}

bool https_get(const std::string &url, std::string &out)
{
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;      // default 512 is too small for these servers' headers
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            char buf[512];
            int r;
            while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
                out.append(buf, r);
                if (out.size() > 20000) break;
            }
            ok = !out.empty();
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

bool geocode(const std::string &loc, double &lat, double &lon)
{
    bool numeric = !loc.empty();
    for (char c : loc) if (!isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }

    std::string body;
    if (numeric) {
        if (!https_get("https://api.zippopotam.us/us/" + loc, body)) return false;
        cJSON *root = cJSON_Parse(body.c_str());
        bool ok = false;
        if (root) {
            cJSON *places = cJSON_GetObjectItem(root, "places");
            if (cJSON_IsArray(places) && cJSON_GetArraySize(places) > 0) {
                cJSON *p0 = cJSON_GetArrayItem(places, 0);
                cJSON *la = cJSON_GetObjectItem(p0, "latitude");
                cJSON *lo = cJSON_GetObjectItem(p0, "longitude");
                if (cJSON_IsString(la) && cJSON_IsString(lo)) {
                    lat = atof(la->valuestring);
                    lon = atof(lo->valuestring);
                    ok = true;
                }
            }
            cJSON_Delete(root);
        }
        return ok;
    }

    if (!https_get("https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + urlencode(loc), body))
        return false;
    cJSON *root = cJSON_Parse(body.c_str());
    bool ok = false;
    if (root) {
        cJSON *res = cJSON_GetObjectItem(root, "results");
        if (cJSON_IsArray(res) && cJSON_GetArraySize(res) > 0) {
            cJSON *r0 = cJSON_GetArrayItem(res, 0);
            cJSON *la = cJSON_GetObjectItem(r0, "latitude");
            cJSON *lo = cJSON_GetObjectItem(r0, "longitude");
            if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
                lat = la->valuedouble;
                lon = lo->valuedouble;
                ok = true;
            }
        }
        cJSON_Delete(root);
    }
    return ok;
}

bool fetch_forecast(double lat, double lon, int &temp_f, int &code)
{
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code&temperature_unit=fahrenheit&timezone=auto",
        lat, lon);

    std::string body;
    if (!https_get(url, body)) return false;

    cJSON *root = cJSON_Parse(body.c_str());
    bool ok = false;
    if (root) {
        cJSON *cur = cJSON_GetObjectItem(root, "current");
        if (cJSON_IsObject(cur)) {
            cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
            cJSON *w = cJSON_GetObjectItem(cur, "weather_code");
            if (cJSON_IsNumber(t)) { temp_f = static_cast<int>(lround(t->valuedouble)); ok = true; }
            if (cJSON_IsNumber(w)) code = static_cast<int>(w->valuedouble);
        }
        cJSON_Delete(root);
    }
    return ok;
}

void task(void *)
{
    // Wait for network AND a synced clock (TLS validates cert dates against it).
    while (network::state() != network::State::kConnected || time(nullptr) < 1600000000)
        vTaskDelay(pdMS_TO_TICKS(1000));

    std::string loc = settings::get("location");
    if (loc.empty()) {
        MP_LOGW(kTag, "no location set — weather disabled");
        for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
    }

    double lat = 0, lon = 0;
    while (!geocode(loc, lat, lon)) {
        MP_LOGW(kTag, "geocode failed for '%s' — retrying", loc.c_str());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    MP_LOGI(kTag, "geocoded '%s' -> %.3f, %.3f", loc.c_str(), lat, lon);

    for (;;) {
        int t = 0, c = 0;
        if (fetch_forecast(lat, lon, t, c)) {
            lock(); s_valid = true; s_temp = t; s_code = c; unlock();
            MP_LOGI(kTag, "%d F, code %d (%s)", t, c, code_text(c));
            vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));  // refresh every 15 min
        } else {
            MP_LOGW(kTag, "forecast fetch failed — retrying soon");
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));       // transient failure: retry in 1 min
        }
    }
}

} // namespace

void init()
{
    s_mux = xSemaphoreCreateMutex();
    xTaskCreate(task, "weather", 16384, nullptr, 3, nullptr);
}

void get(Current &out)
{
    lock();
    out.valid = s_valid;
    out.temp_f = s_temp;
    out.code = s_code;
    unlock();
    out.text = code_text(out.code);
}

} // namespace weather
