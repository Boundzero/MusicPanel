#include "settings.h"
#include "mp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

namespace settings {
namespace {

constexpr const char *kTag = "settings";
constexpr const char *kNamespace = "musicpanel";

} // namespace

esp_err_t init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        MP_LOGW(kTag, "NVS needs erase (%s), reinitialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

std::string get(const char *key, const std::string &def)
{
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return def;

    size_t len = 0;
    std::string out = def;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0) {
        out.resize(len);
        if (nvs_get_str(h, key, out.data(), &len) == ESP_OK) {
            out.resize(len > 0 ? len - 1 : 0);  // drop trailing NUL
        } else {
            out = def;
        }
    }
    nvs_close(h);
    return out;
}

esp_err_t set(const char *key, const std::string &value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool has(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool present = (nvs_get_str(h, key, nullptr, &len) == ESP_OK);
    nvs_close(h);
    return present;
}

bool is_provisioned()
{
    return has("wifi_ssid") && !get("wifi_ssid").empty();
}

} // namespace settings
