// =============================================================================
// Network — Wi-Fi connection and first-run provisioning.
//
// On start(), if credentials have been saved it connects as a station. If not,
// it brings up its own access point ("MusicPanel-XXXX") and a captive setup
// page at http://192.168.4.1 where the user enters Wi-Fi + the Music Assistant
// address on their phone. On save it stores everything and reboots to connect.
//
// The UI reads state() / the string getters to show progress on-screen. All
// getters are safe to call from the LVGL task.
// =============================================================================

#pragma once

#include "esp_err.h"
#include <string>

namespace network {

enum class State {
    kIdle,        // not started yet
    kApPortal,    // running SoftAP + setup page, waiting for the user
    kConnecting,  // joining the saved Wi-Fi network
    kConnected,   // online
    kFailed,      // couldn't join after retries
};

// Sets up netif, the event loop, and the Wi-Fi driver. Call after settings::init().
esp_err_t init();

// Connects (if provisioned) or starts the setup portal (if not).
void start();

State state();

std::string ap_ssid();  // SoftAP name shown during setup
std::string ip();       // current IP address, once available
std::string ssid();     // the network being joined (for "Connecting to …")

} // namespace network
