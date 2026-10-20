// =============================================================================
// MusicPanel — MA WebSocket client (v2, Stage 2+3: live updates, WS-only)
//
// Connects, authenticates, and drives the UI from pushed events (no polling).
//   * Reassembles fragmented messages (large events arrive in 4 KB chunks).
//   * On every (re)connect: authenticate, then ask ma to re-sync a full snapshot
//     (so the screen is correct immediately and never stale after a drop).
//   * On each player_updated / queue_updated event: hand it to ma, which reuses
//     the same parsers as the old polling path.
//   * Built-in auto-reconnect + a ping keepalive detect and recover dead sockets.
//
// The `ma` component no longer polls; it owns the state, parsers, and outbound
// commands. This component owns the live connection.
// =============================================================================

#include "maws.h"
#include "ma.h"
#include "network.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <string>

namespace maws {
namespace {

constexpr char kTag[] = "maws";

esp_websocket_client_handle_t s_client = nullptr;
bool s_authed = false;

// Reassembly buffer for fragmented frames (PSRAM — events can be several KB).
char  *s_acc = nullptr;
int    s_acc_cap = 0;
int    s_acc_len = 0;

void send_text(const std::string &s)
{
    if (s_client && esp_websocket_client_is_connected(s_client))
        esp_websocket_client_send_text(s_client, s.c_str(), s.size(), pdMS_TO_TICKS(2000));
}

void send_auth()
{
    std::string token = settings::get("ma_token");
    if (token.empty()) { ESP_LOGW(kTag, "no token; cannot auth"); return; }
    send_text("{\"command\":\"auth\",\"message_id\":\"1\",\"args\":{\"token\":\"" + token + "\"}}");
    ESP_LOGI(kTag, "-> auth sent");
}

// Route one COMPLETE message (after reassembly) to ma.
void dispatch(const char *data, int len)
{
    if (len <= 0) return;
    std::string s(data, (size_t)len);

    // Auth confirmation: on (re)auth, ask ma for a fresh full snapshot so the
    // screen is correct immediately and never stale after a reconnect.
    if (!s_authed && s.find("\"authenticated\":true") != std::string::npos) {
        s_authed = true;
        ESP_LOGI(kTag, "authenticated; syncing snapshot");
        ma::refresh_snapshot();
        return;
    }

    // Pushed events: {"event":"...","object_id":"...","data":{...}}
    // ma filters by the selected player/queue and reuses the poll-path parsers.
    if (s.find("\"event\":\"player_updated\"") != std::string::npos) {
        ma::apply_player_event(s);
    } else if (s.find("\"event\":\"queue_updated\"") != std::string::npos) {
        ma::apply_queue_event(s);
    }
    // other events (media_item_played, other rooms) ignored
}

// Accumulate a (possibly fragmented) frame; dispatch once the whole payload is in.
void accumulate(esp_websocket_event_data_t *e)
{
    int total = e->payload_len;                 // full logical message length
    int off   = e->payload_offset;              // where this fragment sits
    if (total <= 0) return;

    if (s_acc_cap < total) {
        char *nb = (char *)heap_caps_realloc(s_acc, total, MALLOC_CAP_SPIRAM);
        if (!nb) { ESP_LOGW(kTag, "acc realloc failed (%d)", total); return; }
        s_acc = nb; s_acc_cap = total;
    }
    if (off == 0) s_acc_len = 0;                // start of a new message
    if (off + e->data_len <= s_acc_cap) {
        memcpy(s_acc + off, e->data_ptr, e->data_len);
        s_acc_len = off + e->data_len;
    }
    if (s_acc_len >= total) {                    // message complete
        dispatch(s_acc, s_acc_len);
        s_acc_len = 0;
    }
}

void ws_events(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    auto *e = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(kTag, "connected");
            s_authed = false;
            s_acc_len = 0;
            send_auth();
            break;
        case WEBSOCKET_EVENT_DATA:
            // op_code 0x1 text / 0x0 continuation carry payload; 0x8/9/A are control.
            if ((e->op_code == 0x01 || e->op_code == 0x00) && e->payload_len > 0)
                accumulate(e);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(kTag, "disconnected (auto-reconnecting)");
            s_authed = false;
            ma::note_ws_down();
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(kTag, "error");
            break;
        default:
            break;
    }
}

void task(void *)
{
    while (network::state() != network::State::kConnected) vTaskDelay(pdMS_TO_TICKS(500));

    std::string uri = "ws://" + settings::get("ma_host") + ":" +
                      settings::get("ma_port", "8095") + "/ws";
    ESP_LOGI(kTag, "opening %s", uri.c_str());

    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri.c_str();
    cfg.buffer_size = 4096;
    cfg.reconnect_timeout_ms = 5000;      // auto-reconnect on drop
    cfg.network_timeout_ms = 10000;
    cfg.ping_interval_sec = 15;           // heartbeat: detect a silently-dead socket
    cfg.pingpong_timeout_sec = 8;         // no pong within this -> treat dead, reconnect
    cfg.task_stack = 6144;

    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) { ESP_LOGE(kTag, "init failed"); vTaskDelete(nullptr); return; }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_events, nullptr);
    esp_websocket_client_start(s_client);

    // Supervisor: the client auto-reconnects on its own, but can occasionally
    // wedge. If WiFi is up yet the socket has been unhealthy for ~30 s, force a
    // full stop/start so it always claws its way back without a power cycle.
    // (During a WiFi outage we wait — there's nothing to connect to until the
    // network component brings WiFi back.)
    int stuck = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool net   = (network::state() == network::State::kConnected);
        bool ws_ok = s_client && esp_websocket_client_is_connected(s_client) && s_authed;
        if (ws_ok || !net) { stuck = 0; continue; }
        if (++stuck >= 15) {            // ~30 s: WiFi up but no healthy socket
            ESP_LOGW(kTag, "socket stuck with network up -> restarting client");
            esp_websocket_client_stop(s_client);
            esp_websocket_client_start(s_client);
            stuck = 0;
        }
    }
}

} // namespace

void start()
{
    xTaskCreate(task, "maws", 6144, nullptr, 4, nullptr);
}

} // namespace maws
