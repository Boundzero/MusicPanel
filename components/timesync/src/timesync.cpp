#include "timesync.h"
#include "settings.h"
#include "mp_log.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include <cstdlib>
#include <ctime>
#include <string>

namespace timesync {
namespace {
constexpr const char *kTag = "time";
constexpr const char *kDefaultTz = "EST5EDT,M3.2.0,M11.1.0";  // US Eastern
bool s_started = false;
} // namespace

void init()
{
    std::string tz = settings::get("tz", kDefaultTz);
    setenv("TZ", tz.c_str(), 1);
    tzset();

    if (!s_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) == ESP_OK) {
            s_started = true;
            MP_LOGI(kTag, "SNTP started, TZ=%s", tz.c_str());
        } else {
            MP_LOGW(kTag, "SNTP init failed");
        }
    }
}

bool synced()
{
    time_t now = time(nullptr);
    return now > 1600000000;  // ~2020; anything above means SNTP has set the clock
}

} // namespace timesync
