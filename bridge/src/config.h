// v1 slim config. No more .secrets.local, no client_id, no tokens dir.
// The only knobs left are the SSE port and where to find the SPA dist.

#pragma once

#include <cstdint>
#include <filesystem>

namespace bridge {

struct AppConfig {
    uint16_t port = 8103;
    std::filesystem::path ui_dist_dir;
    std::filesystem::path locales_dir;
};

// Resolve config relative to the version.dll location. If a packaged
// `spotify-radio/ui/dist` is alongside, that's preferred. Otherwise the dev
// fallback under the repo bridge/ui/dist is used.
AppConfig resolve_config(const std::filesystem::path& dll_dir);

} // namespace bridge
