#include "config.h"

namespace bridge {

AppConfig resolve_config(const std::filesystem::path& dll_dir) {
    AppConfig cfg;
    std::error_code ec;

    // Shipped layout: <game>/spotify-radio/ui/dist next to the DLL.
    auto packaged = dll_dir / "spotify-radio" / "ui" / "dist";
    if (std::filesystem::exists(packaged, ec)) {
        cfg.ui_dist_dir = packaged;
        cfg.locales_dir = dll_dir / "spotify-radio" / "locales";
        return cfg;
    }

    // Dev layout: project build output normally places the DLL in bridge/bin.
    auto dev = dll_dir.parent_path() / "ui" / "dist";
    ec.clear();
    if (std::filesystem::exists(dev, ec)) {
        cfg.ui_dist_dir = dev;
        cfg.locales_dir = dll_dir.parent_path() / "locales";
        return cfg;
    }

    // Last fallback is relative to the process working directory for manual runs.
    ec.clear();
    auto cwd = std::filesystem::current_path(ec);
    auto cwd_dev = cwd / "bridge" / "ui" / "dist";
    if (!ec && std::filesystem::exists(cwd_dev, ec)) {
        cfg.ui_dist_dir = cwd_dev;
        cfg.locales_dir = cwd / "bridge" / "locales";
        return cfg;
    }

    cfg.ui_dist_dir = dev;
    cfg.locales_dir = dll_dir.parent_path() / "locales";
    return cfg;
}

} // namespace bridge
