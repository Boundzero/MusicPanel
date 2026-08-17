// =============================================================================
// Logging facade.
//
// A thin wrapper over ESP-IDF's esp_log so the rest of the codebase depends on
// one project-owned logging surface rather than esp_log directly. That keeps
// the option open to add file logging, a log ring buffer for an on-screen
// diagnostics view, or per-subsystem verbosity later — without touching call
// sites.
// =============================================================================

#pragma once

#include "esp_log.h"

#define MP_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define MP_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define MP_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define MP_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define MP_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)

namespace mp::log {

// Sets baseline log levels. Call once at startup. Later this is where a
// stored "log verbosity" setting would be applied.
void init();

} // namespace mp::log
