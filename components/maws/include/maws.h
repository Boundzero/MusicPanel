// =============================================================================
// MusicPanel — Music Assistant WebSocket client (v2, STAGE 1: connect + log)
//
// This runs ALONGSIDE the existing polling client (ma). Stage 1 only connects,
// authenticates, and logs incoming events to serial so we can verify the
// handshake and see real push events on hardware. It does NOT drive the UI yet.
//
// Polling (component `ma`) is untouched and remains the source of truth for the
// screen. Remove the maws::start() call to disable.
// =============================================================================

#pragma once

namespace maws {

// Starts the WebSocket task (waits for network, connects, auths, logs events).
void start();

} // namespace maws
