#include "mp_log.h"

namespace mp::log {

void init()
{
    // Project-wide baseline. Individual tags can be tuned here or, later,
    // driven from persistent settings.
    esp_log_level_set("*", ESP_LOG_INFO);
}

} // namespace mp::log
