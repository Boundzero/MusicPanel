// =============================================================================
// Time sync — SNTP + timezone for the idle clock.
//
// init() applies the persisted timezone and starts SNTP (which syncs once the
// network is up). After that, standard localtime_r() returns correct local
// time with DST. synced() reports whether the clock is trustworthy yet.
// =============================================================================

#pragma once

namespace timesync {

void init();     // apply TZ, start SNTP (call after network::init)
bool synced();   // true once SNTP has set a real time

} // namespace timesync
