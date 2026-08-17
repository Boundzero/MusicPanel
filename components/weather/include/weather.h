// =============================================================================
// Weather — current conditions for the idle dashboard.
//
// Geocodes the configured location (city name or US ZIP) and polls Open-Meteo
// over HTTPS for the current temperature (°F) and a condition code. All work
// happens on a background task; the UI reads the latest snapshot via get().
// =============================================================================

#pragma once

#include <string>

namespace weather {

struct Current {
    bool        valid = false;
    int         temp_f = 0;
    int         code = 0;    // WMO weather code
    std::string text;        // human-readable condition
};

void init();               // start the background task (after network + time)
void get(Current &out);    // latest snapshot (thread-safe)

} // namespace weather
