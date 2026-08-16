// v1 slim HTTP server.
//
// Two routes only:
//   GET /api/state    — one-shot JSON snapshot of BridgeState
//   GET /api/events   — SSE stream that emits the JSON snapshot whenever
//                       the store's revision counter advances
//   GET /api/options  — current persistent playback options
//   POST /api/options — validate + persist playback options
//   GET /*            — static files from the SPA dist directory
// No auth. No Web API proxy.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace bridge {

class BridgeStateStore;
class PlaybackOptionsStore;
class SourceManager;
struct PlaybackOptions;

struct ServerConfig {
    uint16_t port = 8103;
    std::filesystem::path ui_dist_dir;  // absolute path to SPA dist/
    std::filesystem::path locales_dir;  // editable locale JSON files
};

// Blocking — runs on the calling thread. Returns when running goes
// false (the listening loop polls running every ~1 s via httplib's
// shutdown mechanism).
void run_server(const ServerConfig& cfg,
                BridgeStateStore& store,
                PlaybackOptionsStore& options,
                SourceManager& sources,
                const std::function<void(const PlaybackOptions&)>& on_options_changed,
                // Bootstrap Spotify login with an OAuth access token (web-UI
                // "Log in with Spotify" flow). Returns true if accepted.
                const std::function<bool(const std::string& access_token)>& on_spotify_oauth,
                std::atomic<bool>& running);

} // namespace bridge
