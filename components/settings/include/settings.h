// =============================================================================
// Settings — the single source of truth for all persistent configuration.
//
// Thin, typed wrapper over NVS. Everything the device needs to remember across
// reboots (Wi-Fi credentials, Music Assistant address, theme, brightness, ...)
// goes through here, so no other component talks to NVS directly.
//
// Well-known keys used so far:
//   "wifi_ssid", "wifi_pass"   — Wi-Fi credentials
//   "ma_host",   "ma_port"     — Music Assistant server address
// =============================================================================

#pragma once

#include "esp_err.h"
#include <string>

namespace settings {

// Initialises NVS. Call once, early, before any other settings/network call.
esp_err_t init();

// String get/set. get() returns `def` if the key is absent.
std::string get(const char *key, const std::string &def = "");
esp_err_t   set(const char *key, const std::string &value);
esp_err_t   erase(const char *key);
bool        has(const char *key);

// True once Wi-Fi credentials have been stored (i.e. setup has been completed).
bool is_provisioned();

} // namespace settings
