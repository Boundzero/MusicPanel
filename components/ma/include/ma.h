// =============================================================================
// Music Assistant client.
//
// Tracks a selected player and reports its now-playing state (including the
// album's extracted colour palette for the Album-Match theme). Exposes the
// player list; decodes album art off the UI thread; queues commands.
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ma {

enum class State {
    kIdle, kWaitingNetwork, kNoToken, kConnecting, kConnected, kError,
};

struct NowPlaying {
    bool playing = false;
    bool paused  = false;
    std::string queue_id;
    std::string room;
    std::string title;
    std::string artist;
    std::string album;
    std::string image_url;
    std::string repeat = "off";
    bool shuffle = false;
    int elapsed_s  = 0;
    int duration_s = 0;
    int volume     = 0;

    // Album-extracted colours (0xRRGGBB) for the Album-Match theme.
    bool     has_palette = false;
    uint32_t pal_accent = 0;
    uint32_t pal_bg = 0;
};

struct PlayerInfo {
    std::string id;
    std::string name;
    bool available = true;
};

struct BrowseItem {
    std::string name;
    std::string path;        // browse handle for folders
    std::string uri;         // for playing
    std::string item_id;     // for artist_albums / album_tracks / playlist_tracks
    std::string provider;
    std::string media_type;
    bool is_folder = false;
    bool drillable = false;  // folder/artist/album/playlist open deeper; track plays
    bool playable = false;
};

void init();
void start();

State       state();
std::string status_line();
std::string error_text();
NowPlaying  now_playing();
void        get_players(std::vector<PlayerInfo> &out);

bool take_album_art(uint8_t **buf, int *w, int *h);

void transport(const std::string &command, const std::string &queue_id);
void set_volume(const std::string &player_id, int level);
void set_shuffle(const std::string &queue_id, bool on);
void set_repeat(const std::string &queue_id, const std::string &mode);

// Music browsing. Requests are async; results via take_browse_result().
// kind: 'B' folder (a=path), 'A' artist albums, 'T' album tracks,
// 'P' playlist tracks (a=item_id, b=provider). A folder with many items
// returns a set of first-letters instead (browse by letter for big libraries).
void browse(char kind, const std::string &a, const std::string &b);
void browse_letter(char letter);     // filter the last large folder to one letter
bool browse_inflight();
// Returns true when a new result is ready. If 'letters' is non-empty, show an
// A-Z picker (items will be empty); otherwise show 'items'.
bool take_browse_result(std::vector<BrowseItem> &items, std::string &letters);

// Play an item (by uri) on the currently selected room/queue (replaces queue).
void play_media(const std::string &uri);
// Play a list of track uris as a fresh queue (e.g. an album from a tapped track).
void play_list(const std::vector<std::string> &uris);

} // namespace ma
