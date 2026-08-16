#include "server.h"
#include "bridge_state.h"
#include "local_file_source.h"
#include "log_file.h"
#include "options.h"
#include "radio_source.h"
#include "source_manager.h"
#include "spotify_oauth.h"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include "../vendor/httplib.h"

#include <Windows.h>
#include <ShObjIdl.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

namespace bridge {

namespace {

void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    unsigned char u = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out.push_back(hex[(u >> 4) & 0xf]);
                    out.push_back(hex[u & 0xf]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

std::string json_ok(bool ok = true, std::string_view error = {}) {
    std::string out = "{\"ok\":";
    out += ok ? "true" : "false";
    if (!error.empty()) {
        out += ",\"error\":";
        append_json_string(out, error);
    }
    out += "}";
    return out;
}

bool extract_json_string(std::string_view json,
                         std::string_view key,
                         std::string& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t quote = json.find('"', colon + 1);
    if (quote == std::string_view::npos) return false;

    out.clear();
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return true;
        if (c == '\\') {
            if (++i >= json.size()) return false;
            char e = json[i];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return false;
            }
            continue;
        }
        out.push_back(c);
    }
    return false;
}

bool has_json_key(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    return json.find(std::string_view(needle.data(), needle.size())) != std::string_view::npos;
}

bool extract_json_bool(std::string_view json, std::string_view key, bool& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    if (json.substr(pos, 4) == "true") {
        out = true;
        return true;
    }
    if (json.substr(pos, 5) == "false") {
        out = false;
        return true;
    }
    return false;
}

bool extract_json_u32(std::string_view json, std::string_view key, uint32_t& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    char* end = nullptr;
    std::string rest(json.substr(pos));
    unsigned long parsed = std::strtoul(rest.c_str(), &end, 10);
    if (!end || end == rest.c_str() || parsed > UINT32_MAX) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool extract_json_u64(std::string_view json, std::string_view key, uint64_t& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    char* end = nullptr;
    std::string rest(json.substr(pos));
    unsigned long long parsed = std::strtoull(rest.c_str(), &end, 10);
    if (!end || end == rest.c_str()) return false;
    out = static_cast<uint64_t>(parsed);
    return true;
}

LocalFileSource* local_source(SourceManager& sources) {
    return dynamic_cast<LocalFileSource*>(sources.find_source("local"));
}

RadioSource* radio_source(SourceManager& sources) {
    return dynamic_cast<RadioSource*>(sources.find_source("radio"));
}

bool active_source_is(SourceManager& sources, std::string_view id) {
    return std::string_view(sources.active_source_id()) == id;
}

bool is_safe_locale_id(std::string_view id) {
    return is_valid_locale(id);
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string locale_display_name(std::string_view id, std::string_view raw) {
    std::string name;
    if (extract_json_string(raw, "name", name) && !name.empty()) return name;
    return std::string(id);
}

std::string locales_manifest_json(const std::filesystem::path& locales_dir) {
    std::vector<std::pair<std::string, std::string>> locales;
    std::error_code ec;
    if (std::filesystem::exists(locales_dir, ec)) {
        for (std::filesystem::directory_iterator it(locales_dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;
            auto path = entry.path();
            if (path.extension() != ".json") continue;
            std::string id = path.stem().string();
            if (!is_safe_locale_id(id)) continue;
            std::string raw = read_text_file(path);
            locales.emplace_back(id, locale_display_name(id, raw));
        }
    }
    std::sort(locales.begin(), locales.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string body = "{\"locales\":[";
    for (size_t i = 0; i < locales.size(); ++i) {
        if (i) body += ",";
        body += "{\"id\":";
        append_json_string(body, locales[i].first);
        body += ",\"name\":";
        append_json_string(body, locales[i].second);
        body += "}";
    }
    body += "]}";
    return body;
}

void append_local_track_json(std::string& body,
                             const LocalFileSource::TrackInfo& track) {
    body += "{\"index\":" + std::to_string(track.index);
    body += ",\"path\":";
    append_json_string(body, track.path);
    body += ",\"title\":";
    append_json_string(body, track.title);
    body += ",\"folder\":";
    append_json_string(body, track.folder);
    body += "}";
}

std::string local_library_json(LocalFileSource& local) {
    auto tracks = local.library_snapshot();
    std::string body = "{\"tracks\":[";
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) body += ",";
        append_local_track_json(body, tracks[i]);
    }
    body += "]}";
    return body;
}

std::string local_queue_json(LocalFileSource& local) {
    auto manual = local.manual_queue_snapshot();
    auto autoplay = local.autoplay_queue_snapshot();
    std::string body = "{\"manual\":[";
    for (size_t i = 0; i < manual.size(); ++i) {
        if (i) body += ",";
        body += "{\"id\":" + std::to_string(manual[i].id) + ",\"track\":";
        append_local_track_json(body, manual[i].track);
        body += "}";
    }
    body += "],\"autoplay\":[";
    for (size_t i = 0; i < autoplay.size(); ++i) {
        if (i) body += ",";
        append_local_track_json(body, autoplay[i]);
    }
    body += "]}";
    return body;
}

void publish_options(BridgeStateStore& store, const PlaybackOptions& options) {
    store.set_options(options.locale,
                      options.menu_playback, options.race_start,
                      options.race_start_restart_threshold_s,
                      options.song_start_offset_enabled,
                      options.song_start_offset_seconds,
                      options.volume_normalization, options.quick_station_skip,
                      options.equalizer_enabled, options.equalizer_bands_db,
                      options.local_volume_percent,
                      options.local_title_metadata_mode,
                      options.local_artist_metadata_mode,
                      options.night_runners_mode,
                      options.night_runners_stopped_volume_decrease_percent,
                      options.night_runners_max_speed_mph,
                      options.night_runners_speed_unit,
                      options.night_runners_curve_enabled,
                      options.night_runners_curve_exponent,
                      options.night_runners_lazy_volume_enabled,
                      options.night_runners_lazy_hold_seconds,
                      options.night_runners_low_cut_enabled,
                      options.night_runners_frequency_cut_mode,
                      options.night_runners_low_cut_amount_percent,
                      options.night_runners_low_cut_frequency_hz,
                      options.night_runners_non_driving_volume_enabled,
                      options.night_runners_non_driving_volume_percent,
                      options.night_runners_dynamic_mode,
                      options.night_runners_dynamic_threshold_mph,
                      options.night_runners_dynamic_buffer_percent,
                      options.night_runners_dynamic_buffer_fill_seconds,
                      options.night_runners_dynamic_increase_seconds,
                      options.night_runners_dynamic_decrease_seconds,
                      options.night_runners_dynamic_max_decay_mph_s,
                      options.radio_logo_album_art_enabled,
                      options.radio_logo_custom_graphic_enabled,
                      options.radio_logo_spotify_variant,
                      options.metadata_truncation_enabled,
                      options.metadata_truncation_length);
}

void persist_active_source(BridgeStateStore& store,
                           PlaybackOptionsStore& options,
                           std::string_view source) {
    PlaybackOptions next = options.snapshot();
    next.active_source = std::string(source);
    std::string error;
    if (!options.update(next, &error)) {
        log::warn("[server] active source persist failed: " + error);
        return;
    }
    publish_options(store, next);
}

std::string utf8_from_wide(const std::wstring& value) {
    if (value.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()),
                                     nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed);
    return out;
}

bool browse_for_folder(const std::string& initial_dir,
                       std::string& out_dir,
                       std::string& error) {
    HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE);
    bool should_uninit = SUCCEEDED(init_hr);
    if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
        error = "Could not initialize Windows folder picker";
        return false;
    }

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        if (should_uninit) CoUninitialize();
        error = "Could not create Windows folder picker";
        return false;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"Select music folder for FH6 Radio");

    std::wstring initial = wide_from_utf8(initial_dir);
    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        if (should_uninit) CoUninitialize();
        error.clear();
        return false;
    }
    if (FAILED(hr)) {
        dialog->Release();
        if (should_uninit) CoUninitialize();
        error = "Folder picker failed";
        return false;
    }

    IShellItem* item = nullptr;
    hr = dialog->GetResult(&item);
    if (FAILED(hr) || !item) {
        dialog->Release();
        if (should_uninit) CoUninitialize();
        error = "No folder selected";
        return false;
    }

    PWSTR path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (SUCCEEDED(hr) && path) {
        out_dir = utf8_from_wide(path);
        CoTaskMemFree(path);
    }
    item->Release();
    dialog->Release();
    if (should_uninit) CoUninitialize();

    if (out_dir.empty()) {
        error = "No folder selected";
        return false;
    }
    return true;
}

} // namespace

void run_server(const ServerConfig& cfg,
                BridgeStateStore& store,
                PlaybackOptionsStore& options,
                SourceManager& sources,
                const std::function<void(const PlaybackOptions&)>& on_options_changed,
                const std::function<bool(const std::string&)>& on_spotify_oauth,
                std::atomic<bool>& running) {
    httplib::Server svr;

    svr.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(store.snapshot_json(), "application/json");
    });

    // ---- Spotify OAuth (Authorization Code + PKCE) ----------------------------
    // Alternative to Zeroconf pairing for sandboxed/GamePass installs where the
    // controller can't reach the in-process pairing server. The browser does the
    // user login; the bridge exchanges the code (outbound) and logs in.
    auto oauth_redirect_uri = [&cfg]() {
        return "http://127.0.0.1:" + std::to_string(cfg.port) + "/login";
    };

    svr.Post("/api/spotify/oauth/start",
             [&](const httplib::Request&, httplib::Response& res) {
        std::string url = spotify_oauth::build_authorize_url(oauth_redirect_uri());
        std::string body = "{\"authUrl\":";
        append_json_string(body, url);
        body += "}";
        res.set_header("Cache-Control", "no-store");
        res.set_content(body, "application/json");
    });

    // Loopback redirect target (auto-capture when the browser is on this PC).
    svr.Get("/login", [&](const httplib::Request& req, httplib::Response& res) {
        auto esc = [](const std::string& s) {
            std::string o; o.reserve(s.size());
            for (char c : s) {
                if (c == '&') o += "&amp;";
                else if (c == '<') o += "&lt;";
                else if (c == '>') o += "&gt;";
                else if (c == '"') o += "&quot;";
                else o += c;
            }
            return o;
        };
        auto page = [&res, &esc](const std::string& title, const std::string& sub, bool ok) {
            (void)ok;
            std::string b =
                "<!doctype html><html><head><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>FH6 Spotify Radio</title><style>:root{color-scheme:dark}"
                "@import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap');"
                "*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;"
                "place-items:center;padding:24px;color:#fff;background:#0a0a0a;"
                "font-family:'Inter',-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
                "-webkit-font-smoothing:antialiased;-moz-osx-font-smoothing:grayscale;"
                "-webkit-text-size-adjust:100%;user-select:none}.message{max-width:min(92vw,420px);text-align:center}"
                "h1{margin:0;color:#fff;font-size:1.05rem;line-height:1.2;font-weight:600}"
                "p{margin:.15rem 0 0;color:#a0a0a0;font-size:.85rem;line-height:1.2}"
                "</style></head><body><main class=message><h1>";
            b += esc(title);
            b += "</h1><p>";
            b += esc(sub);
            b += "</p></main></body></html>";
            res.set_content(b, "text/html");
        };
        std::string err = req.has_param("error") ? req.get_param_value("error") : "";
        if (!err.empty()) {
            res.status = 400; page("Login cancelled", err, false); return;
        }
        std::string code  = req.has_param("code")  ? req.get_param_value("code")  : "";
        std::string state = req.has_param("state") ? req.get_param_value("state") : "";
        if (code.empty()) {
            res.status = 400;
            page("Couldn't complete login", "No authorization code was returned.", false);
            return;
        }
        if (!spotify_oauth::state_matches(state)) {
            res.status = 400;
            page("Couldn't complete login", "Login state mismatch — please try again.", false);
            return;
        }
        std::string token, exch_err;
        if (!spotify_oauth::exchange_code(code, token, exch_err)) {
            res.status = 400; page("Couldn't complete login", exch_err, false); return;
        }
        bool ok = on_spotify_oauth && on_spotify_oauth(token);
        if (ok) {
            page("Spotify connected", "You can close this tab and return to the game.", true);
        } else {
            page("Almost there", "Logged in, but the radio couldn't start — return to "
                                 "the game and try again.", false);
        }
    });

    // Paste-code fallback (phone / non-loopback origins where the redirect can't
    // reach this PC). Accepts the raw code or the full redirected URL.
    svr.Post("/api/spotify/oauth/code",
             [&](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        std::string code = req.body;
        // Trim whitespace; if a full URL was pasted, pull out the code param.
        auto cpos = code.find("code=");
        if (cpos != std::string::npos) {
            code = code.substr(cpos + 5);
            auto amp = code.find_first_of("&\r\n \t");
            if (amp != std::string::npos) code = code.substr(0, amp);
        }
        while (!code.empty() && (code.back() == '\r' || code.back() == '\n' ||
                                 code.back() == ' ' || code.back() == '\t')) {
            code.pop_back();
        }
        if (code.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"empty code\"}", "application/json");
            return;
        }
        std::string token, exch_err;
        if (!spotify_oauth::exchange_code(code, token, exch_err)) {
            res.status = 400;
            std::string body = "{\"error\":";
            append_json_string(body, exch_err);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        bool ok = on_spotify_oauth && on_spotify_oauth(token);
        res.set_content(ok ? "{\"ok\":true}" : "{\"error\":\"login failed\"}",
                        "application/json");
    });

    svr.Get("/api/options", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(options_to_json(options.snapshot()), "application/json");
    });

    svr.Get("/api/locales", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(locales_manifest_json(cfg.locales_dir), "application/json");
    });

    svr.Get(R"(/api/locales/([A-Za-z0-9_-]+))",
            [&](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        if (!is_safe_locale_id(id)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid locale\"}", "application/json");
            return;
        }
        auto path = cfg.locales_dir / (id + ".json");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) ||
            !std::filesystem::is_regular_file(path, ec)) {
            res.status = 404;
            res.set_content("{\"error\":\"locale not found\"}", "application/json");
            return;
        }
        std::string body = read_text_file(path);
        if (body.empty()) {
            res.status = 500;
            res.set_content("{\"error\":\"could not read locale\"}", "application/json");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(body, "application/json");
    });

    svr.Post("/api/options", [&](const httplib::Request& req, httplib::Response& res) {
        PlaybackOptions next;
        if (!parse_options_json(req.body, next)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid options\"}", "application/json");
            return;
        }
        auto current = options.snapshot();
        if (!has_json_key(req.body, "activeSource")) {
            next.active_source = current.active_source;
        }
        if (!has_json_key(req.body, "locale")) {
            next.locale = current.locale;
        }
        if (!has_json_key(req.body, "localMusicDir")) {
            next.local_music_dir = current.local_music_dir;
        }
        if (!has_json_key(req.body, "localRecursive")) {
            next.local_recursive = current.local_recursive;
        }
        if (!has_json_key(req.body, "localShuffle")) {
            next.local_shuffle = current.local_shuffle;
        }
        if (!has_json_key(req.body, "localVolume")) {
            next.local_volume_percent = current.local_volume_percent;
        }
        if (!has_json_key(req.body, "localTitleMetadataMode")) {
            next.local_title_metadata_mode = current.local_title_metadata_mode;
        }
        if (!has_json_key(req.body, "localArtistMetadataMode")) {
            next.local_artist_metadata_mode = current.local_artist_metadata_mode;
        }
        if (!has_json_key(req.body, "nightRunnersMode")) {
            next.night_runners_mode = current.night_runners_mode;
        }
        if (!has_json_key(req.body, "nightRunnersStoppedVolumeDecrease")) {
            next.night_runners_stopped_volume_decrease_percent =
                current.night_runners_stopped_volume_decrease_percent;
        }
        if (!has_json_key(req.body, "nightRunnersMaxSpeedMph")) {
            next.night_runners_max_speed_mph = current.night_runners_max_speed_mph;
        }
        if (!has_json_key(req.body, "nightRunnersSpeedUnit")) {
            next.night_runners_speed_unit = current.night_runners_speed_unit;
        }
        if (!has_json_key(req.body, "nightRunnersCurveEnabled")) {
            next.night_runners_curve_enabled = current.night_runners_curve_enabled;
        }
        if (!has_json_key(req.body, "nightRunnersCurveExponent")) {
            next.night_runners_curve_exponent = current.night_runners_curve_exponent;
        }
        if (!has_json_key(req.body, "nightRunnersLazyVolumeEnabled")) {
            next.night_runners_lazy_volume_enabled =
                current.night_runners_lazy_volume_enabled;
        }
        if (!has_json_key(req.body, "nightRunnersLazyHoldSeconds")) {
            next.night_runners_lazy_hold_seconds =
                current.night_runners_lazy_hold_seconds;
        }
        if (!has_json_key(req.body, "nightRunnersLowCutEnabled")) {
            next.night_runners_low_cut_enabled =
                current.night_runners_low_cut_enabled;
        }
        if (!has_json_key(req.body, "nightRunnersFrequencyCutMode")) {
            next.night_runners_frequency_cut_mode =
                current.night_runners_frequency_cut_mode;
        }
        if (!has_json_key(req.body, "nightRunnersLowCutAmount")) {
            next.night_runners_low_cut_amount_percent =
                current.night_runners_low_cut_amount_percent;
        }
        if (!has_json_key(req.body, "nightRunnersLowCutFrequencyHz")) {
            next.night_runners_low_cut_frequency_hz =
                current.night_runners_low_cut_frequency_hz;
        }
        if (!has_json_key(req.body, "nightRunnersNonDrivingVolumeEnabled")) {
            next.night_runners_non_driving_volume_enabled =
                current.night_runners_non_driving_volume_enabled;
        }
        if (!has_json_key(req.body, "nightRunnersNonDrivingVolume")) {
            next.night_runners_non_driving_volume_percent =
                current.night_runners_non_driving_volume_percent;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicMode")) {
            next.night_runners_dynamic_mode = current.night_runners_dynamic_mode;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicThresholdMph")) {
            next.night_runners_dynamic_threshold_mph =
                current.night_runners_dynamic_threshold_mph;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicBufferPercent")) {
            next.night_runners_dynamic_buffer_percent =
                current.night_runners_dynamic_buffer_percent;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicBufferFillSeconds")) {
            next.night_runners_dynamic_buffer_fill_seconds =
                current.night_runners_dynamic_buffer_fill_seconds;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicIncreaseSeconds")) {
            next.night_runners_dynamic_increase_seconds =
                current.night_runners_dynamic_increase_seconds;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicDecreaseSeconds")) {
            next.night_runners_dynamic_decrease_seconds =
                current.night_runners_dynamic_decrease_seconds;
        }
        if (!has_json_key(req.body, "nightRunnersDynamicMaxDecayMphS")) {
            next.night_runners_dynamic_max_decay_mph_s =
                current.night_runners_dynamic_max_decay_mph_s;
        }
        if (!has_json_key(req.body, "radioLogoAlbumArtEnabled")) {
            next.radio_logo_album_art_enabled =
                current.radio_logo_album_art_enabled;
        }
        if (!has_json_key(req.body, "radioLogoCustomGraphicEnabled")) {
            next.radio_logo_custom_graphic_enabled =
                current.radio_logo_custom_graphic_enabled;
        }
        if (!has_json_key(req.body, "radioLogoSpotifyVariant")) {
            next.radio_logo_spotify_variant =
                current.radio_logo_spotify_variant;
        }
        if (!has_json_key(req.body, "metadataTruncationEnabled")) {
            next.metadata_truncation_enabled =
                current.metadata_truncation_enabled;
        }
        if (!has_json_key(req.body, "metadataTruncationLength")) {
            next.metadata_truncation_length =
                current.metadata_truncation_length;
        }

        std::string error;
        if (!options.update(next, &error)) {
            res.status = 500;
            res.set_content("{\"error\":\"could not persist options\"}", "application/json");
            log::warn("[server] options update failed: " + error);
            return;
        }

        bool local_changed =
            next.local_music_dir != current.local_music_dir ||
            next.local_recursive != current.local_recursive ||
            next.local_shuffle != current.local_shuffle;
        if (local_changed) {
            auto* local = local_source(sources);
            std::string ignored;
            if (local) {
                local->configure(next.local_music_dir, next.local_recursive,
                                 next.local_shuffle, &ignored);
                local->publish_state(store);
            }
        }
        publish_options(store, next);
        if (on_options_changed) on_options_changed(next);
        res.set_content(options_to_json(next), "application/json");
    });

    svr.Post("/api/source/switch", [&](const httplib::Request& req, httplib::Response& res) {
        std::string source;
        if (!extract_json_string(req.body, "source", source)) {
            res.status = 400;
            res.set_content("{\"error\":\"source required\"}", "application/json");
            return;
        }
        if (!sources.find_source(source)) {
            res.status = 404;
            res.set_content("{\"error\":\"unknown source\"}", "application/json");
            return;
        }
        persist_active_source(store, options, source);
        if (!sources.switch_to(source)) {
            res.status = 404;
            res.set_content("{\"error\":\"unknown source\"}", "application/json");
            return;
        }
        sources.publish_source_states();
        if (on_options_changed) on_options_changed(options.snapshot());
        res.set_content(json_ok(), "application/json");
    });

    svr.Get("/api/source/radio/search", [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        uint32_t offset = 0;
        uint32_t limit = 24;
        if (req.has_param("offset")) {
            offset = static_cast<uint32_t>(
                std::strtoul(req.get_param_value("offset").c_str(), nullptr, 10));
        }
        if (req.has_param("limit")) {
            limit = static_cast<uint32_t>(
                std::strtoul(req.get_param_value("limit").c_str(), nullptr, 10));
        }
        std::string error;
        std::string body = radio->search_json(
            req.has_param("q") ? req.get_param_value("q") : "",
            req.has_param("tag") ? req.get_param_value("tag") : "",
            req.has_param("countryCode") ? req.get_param_value("countryCode") : "",
            req.has_param("language") ? req.get_param_value("language") : "",
            offset, limit, &error);
        if (!error.empty()) {
            body.pop_back();
            body += ",\"error\":";
            append_json_string(body, error);
            body += "}";
        }
        res.set_content(body, "application/json");
    });

    svr.Get("/api/source/radio/suggestions", [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        uint32_t limit = 24;
        if (req.has_param("limit")) {
            limit = static_cast<uint32_t>(
                std::strtoul(req.get_param_value("limit").c_str(), nullptr, 10));
        }
        std::string error;
        std::string body = radio->suggestions_json(
            req.has_param("kind") ? req.get_param_value("kind") : "top",
            limit, &error);
        if (!error.empty()) {
            body.pop_back();
            body += ",\"error\":";
            append_json_string(body, error);
            body += "}";
        }
        res.set_content(body, "application/json");
    });

    svr.Get("/api/source/radio/favorites", [&](const httplib::Request&, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        res.set_content(radio->favorites_json(), "application/json");
    });

    svr.Post("/api/source/radio/favorites", [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        std::string error;
        if (!radio->add_favorite_json(req.body, &error)) {
            res.status = 400;
            std::string body = "{\"error\":";
            append_json_string(body, error.empty() ? "invalid station" : error);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        res.set_content(radio->favorites_json(), "application/json");
    });

    svr.Delete(R"(/api/source/radio/favorites/(.+))",
               [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        std::string error;
        if (!radio->remove_favorite(std::string(req.matches[1]), &error)) {
            res.status = 404;
            std::string body = "{\"error\":";
            append_json_string(body, error.empty() ? "favorite not found" : error);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        res.set_content(radio->favorites_json(), "application/json");
    });

    svr.Post("/api/source/radio/play", [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        persist_active_source(store, options, "radio");
        if (!sources.switch_to("radio")) {
            res.status = 404;
            res.set_content("{\"error\":\"could not switch source\"}", "application/json");
            return;
        }
        std::string error;
        bool ok = radio->play_station_json(req.body, &error);
        sources.publish_source_states();
        if (!ok) {
            res.status = 400;
            std::string body = "{\"error\":";
            append_json_string(body, error.empty() ? "could not play station" : error);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        res.set_content(json_ok(), "application/json");
    });

    svr.Get("/api/source/radio/manual", [&](const httplib::Request&, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        res.set_content(radio->manual_json(), "application/json");
    });

    svr.Delete(R"(/api/source/radio/manual/(.+))",
               [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        std::string error;
        if (!radio->remove_manual(std::string(req.matches[1]), &error)) {
            res.status = 404;
            std::string body = "{\"error\":";
            append_json_string(body, error.empty() ? "saved station not found" : error);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        res.set_content(radio->manual_json(), "application/json");
    });

    svr.Post("/api/source/radio/stop", [&](const httplib::Request&, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        radio->stop_playback();
        sources.publish_source_states();
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/radio/play-url", [&](const httplib::Request& req, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        std::string url;
        if (!extract_json_string(req.body, "url", url)) {
            res.status = 400;
            res.set_content("{\"error\":\"url required\"}", "application/json");
            return;
        }
        std::string name;
        extract_json_string(req.body, "name", name);
        persist_active_source(store, options, "radio");
        if (!sources.switch_to("radio")) {
            res.status = 404;
            res.set_content("{\"error\":\"could not switch source\"}", "application/json");
            return;
        }
        std::string error;
        bool ok = radio->play_url(url, name, &error);
        sources.publish_source_states();
        if (!ok) {
            res.status = 400;
            std::string body = "{\"error\":";
            append_json_string(body, error.empty() ? "could not play URL" : error);
            body += "}";
            res.set_content(body, "application/json");
            return;
        }
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/radio/pause", [&](const httplib::Request&, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        radio->pause_at_audio_boundary();
        sources.publish_source_states();
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/radio/resume", [&](const httplib::Request&, httplib::Response& res) {
        auto* radio = radio_source(sources);
        if (!radio) {
            res.status = 404;
            res.set_content("{\"error\":\"radio source unavailable\"}", "application/json");
            return;
        }
        persist_active_source(store, options, "radio");
        if (!sources.switch_to("radio")) {
            res.status = 404;
            res.set_content("{\"error\":\"could not switch source\"}", "application/json");
            return;
        }
        radio->resume_rewound(0);
        sources.publish_source_states();
        res.set_content(json_ok(radio->is_playing(),
                                radio->is_playing() ? "" : radio->last_error()),
                        "application/json");
    });

    svr.Post("/api/source/local/rescan", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }

        PlaybackOptions next = options.snapshot();
        std::string path;
        if (extract_json_string(req.body, "musicDir", path) ||
            extract_json_string(req.body, "path", path)) {
            next.local_music_dir = path;
        } else if (has_json_key(req.body, "musicDir") || has_json_key(req.body, "path")) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid musicDir\"}", "application/json");
            return;
        }
        bool b = false;
        if (extract_json_bool(req.body, "recursive", b)) {
            next.local_recursive = b;
        }
        if (extract_json_bool(req.body, "shuffle", b)) {
            next.local_shuffle = b;
        }

        std::string scan_error;
        bool scan_ok = local->configure(next.local_music_dir, next.local_recursive,
                                        next.local_shuffle, &scan_error);

        std::string persist_error;
        if (!options.update(next, &persist_error)) {
            res.status = 500;
            res.set_content("{\"error\":\"could not persist options\"}", "application/json");
            log::warn("[server] local options update failed: " + persist_error);
            return;
        }

        publish_options(store, next);
        local->publish_state(store);
        if (on_options_changed) on_options_changed(next);

        std::string body = "{\"ok\":";
        body += scan_ok ? "true" : "false";
        body += ",\"trackCount\":" + std::to_string(local->track_count());
        body += ",\"unsupportedCount\":" + std::to_string(local->unsupported_count());
        if (!scan_error.empty()) {
            body += ",\"error\":";
            append_json_string(body, scan_error);
        }
        body += "}";
        res.set_content(body, "application/json");
    });

    svr.Post("/api/source/local/browse", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }

        PlaybackOptions next = options.snapshot();
        std::string initial = next.local_music_dir;
        std::string body_initial;
        if (extract_json_string(req.body, "initialDir", body_initial) && !body_initial.empty()) {
            initial = body_initial;
        }

        std::string selected;
        std::string browse_error;
        if (!browse_for_folder(initial, selected, browse_error)) {
            if (browse_error.empty()) {
                res.set_content("{\"ok\":false,\"cancelled\":true}", "application/json");
            } else {
                res.status = 500;
                std::string body = "{\"error\":";
                append_json_string(body, browse_error);
                body += "}";
                res.set_content(body, "application/json");
            }
            return;
        }

        next.local_music_dir = selected;
        std::string scan_error;
        bool scan_ok = local->configure(next.local_music_dir, next.local_recursive,
                                        next.local_shuffle, &scan_error);

        std::string persist_error;
        if (!options.update(next, &persist_error)) {
            res.status = 500;
            res.set_content("{\"error\":\"could not persist options\"}", "application/json");
            log::warn("[server] local browse options update failed: " + persist_error);
            return;
        }

        publish_options(store, next);
        local->publish_state(store);
        if (on_options_changed) on_options_changed(next);

        std::string body = "{\"ok\":";
        body += scan_ok ? "true" : "false";
        body += ",\"musicDir\":";
        append_json_string(body, selected);
        body += ",\"trackCount\":" + std::to_string(local->track_count());
        body += ",\"unsupportedCount\":" + std::to_string(local->unsupported_count());
        if (!scan_error.empty()) {
            body += ",\"error\":";
            append_json_string(body, scan_error);
        }
        body += "}";
        res.set_content(body, "application/json");
    });

    svr.Get("/api/source/local/library", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        res.set_content(local_library_json(*local), "application/json");
    });

    svr.Get("/api/source/local/queue", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        res.set_content(local_queue_json(*local), "application/json");
    });

    svr.Post("/api/source/local/play-index", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        uint32_t index = 0;
        if (!extract_json_u32(req.body, "index", index)) {
            res.status = 400;
            res.set_content("{\"error\":\"index required\"}", "application/json");
            return;
        }
        persist_active_source(store, options, "local");
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local play-index index="
                  + std::to_string(index));
#endif
        if (!sources.switch_to("local") || !local->play_library_index(index)) {
            res.status = 404;
            res.set_content("{\"error\":\"track unavailable\"}", "application/json");
            return;
        }
        sources.publish_source_states();
        res.set_content(local_queue_json(*local), "application/json");
    });

    svr.Post("/api/source/local/queue-add", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        uint32_t index = 0;
        if (!extract_json_u32(req.body, "index", index)) {
            res.status = 400;
            res.set_content("{\"error\":\"index required\"}", "application/json");
            return;
        }
        if (!local->queue_library_index(index)) {
            res.status = 404;
            res.set_content("{\"error\":\"track unavailable\"}", "application/json");
            return;
        }
        sources.publish_source_states();
        res.set_content(local_queue_json(*local), "application/json");
    });

    svr.Post("/api/source/local/queue-folder", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        std::string folder;
        if (!extract_json_string(req.body, "folder", folder)) {
            res.status = 400;
            res.set_content("{\"error\":\"folder required\"}", "application/json");
            return;
        }
        uint32_t added = local->queue_folder(folder);
        std::string body = local_queue_json(*local);
        body.pop_back();
        body += ",\"added\":" + std::to_string(added) + "}";
        sources.publish_source_states();
        res.set_content(body, "application/json");
    });

    svr.Post("/api/source/local/queue-remove", [&](const httplib::Request& req, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        uint64_t id = 0;
        if (!extract_json_u64(req.body, "id", id)) {
            res.status = 400;
            res.set_content("{\"error\":\"id required\"}", "application/json");
            return;
        }
        if (!local->remove_manual_queue_entry(id)) {
            res.status = 404;
            res.set_content("{\"error\":\"queue entry unavailable\"}", "application/json");
            return;
        }
        sources.publish_source_states();
        res.set_content(local_queue_json(*local), "application/json");
    });

    svr.Post("/api/source/local/play", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        if (!sources.find_source("local")) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
        persist_active_source(store, options, "local");
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local play");
#endif
        if (!sources.switch_to("local")) {
            res.status = 404;
            res.set_content("{\"error\":\"could not switch source\"}", "application/json");
            return;
        }
        local->resume_rewound(0);
        sources.publish_source_states();
        res.set_content(json_ok(local->is_playing(),
                                local->is_playing() ? "" : local->last_error()),
                        "application/json");
    });

    svr.Post("/api/source/local/pause", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
        if (!local) {
            res.status = 404;
            res.set_content("{\"error\":\"local source unavailable\"}", "application/json");
            return;
        }
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local pause");
#endif
        local->pause_at_audio_boundary();
        sources.publish_source_states();
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/airplay/pause", [&](const httplib::Request&, httplib::Response& res) {
        if (!active_source_is(sources, "airplay")) {
            res.status = 409;
            res.set_content("{\"error\":\"airplay source inactive\"}", "application/json");
            return;
        }
        sources.pause_at_audio_boundary();
        sources.publish_source_states();
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/airplay/resume", [&](const httplib::Request&, httplib::Response& res) {
        if (!active_source_is(sources, "airplay")) {
            res.status = 409;
            res.set_content("{\"error\":\"airplay source inactive\"}", "application/json");
            return;
        }
        sources.resume_rewound(0);
        sources.publish_source_states();
        res.set_content(json_ok(), "application/json");
    });

    svr.Post("/api/source/airplay/next", [&](const httplib::Request&, httplib::Response& res) {
        if (!active_source_is(sources, "airplay")) {
            res.status = 409;
            res.set_content("{\"error\":\"airplay source inactive\"}", "application/json");
            return;
        }
        bool ok = sources.next_track();
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : "could not skip"), "application/json");
    });

    svr.Post("/api/source/airplay/previous", [&](const httplib::Request&, httplib::Response& res) {
        if (!active_source_is(sources, "airplay")) {
            res.status = 409;
            res.set_content("{\"error\":\"airplay source inactive\"}", "application/json");
            return;
        }
        bool ok = sources.previous_track();
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : "could not skip"), "application/json");
    });

    svr.Post("/api/source/local/next", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local next");
#endif
        bool ok = sources.next_track();
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : (local ? local->last_error() : "no next track")),
                        "application/json");
    });

    svr.Post("/api/source/local/previous", [&](const httplib::Request&, httplib::Response& res) {
        auto* local = local_source(sources);
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local previous");
#endif
        bool ok = sources.previous_track();
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : (local ? local->last_error() : "no previous track")),
                        "application/json");
    });

    svr.Post("/api/source/local/restart", [&](const httplib::Request&, httplib::Response& res) {
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local restart");
#endif
        bool ok = sources.restart_current_track();
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : "could not restart"), "application/json");
    });

    svr.Post("/api/source/local/seek", [&](const httplib::Request& req, httplib::Response& res) {
        uint32_t position_ms = 0;
        if (!extract_json_u32(req.body, "positionMs", position_ms) &&
            !extract_json_u32(req.body, "position_ms", position_ms)) {
            res.status = 400;
            res.set_content("{\"error\":\"positionMs required\"}", "application/json");
            return;
        }
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] HTTP local seek target_ms="
                  + std::to_string(position_ms));
#endif
        bool ok = sources.seek(position_ms);
        sources.publish_source_states();
        res.set_content(json_ok(ok, ok ? "" : "could not seek"), "application/json");
    });

    svr.Get("/api/events", [&](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");

        res.set_content_provider(
            "text/event-stream",
            [&store, &running](size_t /*offset*/, httplib::DataSink& sink) {
                // Initial snapshot.
                {
                    std::string j = "data: " + store.snapshot_json() + "\n\n";
                    if (!sink.write(j.data(), j.size())) return false;
                }
                uint64_t last_rev = store.revision();
                auto last_beat   = std::chrono::steady_clock::now();
                while (running.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    uint64_t rev = store.revision();
                    if (rev != last_rev) {
                        last_rev = rev;
                        std::string j = "data: " + store.snapshot_json() + "\n\n";
                        if (!sink.write(j.data(), j.size())) return false;
                        last_beat = std::chrono::steady_clock::now();
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        if (now - last_beat >= std::chrono::seconds(15)) {
                            if (!sink.write(": heartbeat\n\n", 14)) return false;
                            last_beat = now;
                        }
                    }
                }
                return false;
            });
    });

    // Static SPA — single mount, catch-all GET serves index.html for unknown
    // routes so client-side routing (if any) just works.
    if (std::filesystem::exists(cfg.ui_dist_dir / "index.html")) {
        svr.set_mount_point("/", cfg.ui_dist_dir.string());
        log::info("[server] Serving UI from " + cfg.ui_dist_dir.string());
    } else {
        svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                "<title>Spotify Radio - Install incomplete</title></head>"
                "<body style=\"background:#121212;color:#fff;font-family:Segoe UI,Arial,sans-serif;"
                "margin:0;padding:40px 20px\">"
                "<main style=\"max-width:680px;margin:0 auto;line-height:1.5\">"
                "<h1 style=\"margin:0 0 12px\">Spotify Radio - Install incomplete</h1>"
                "<p style=\"font-size:18px\">The bridge is running, but the dashboard files were not found.</p>"
                "<p>Close the game and extract the full release ZIP directly into your Forza Horizon 6 game "
                "folder, then allow Windows to merge and overwrite files.</p>"
                "<p>Make sure these folders from the ZIP are present in the game folder:</p>"
                "<ul>"
                "<li><code>spotify-radio/ui/dist</code> - dashboard files</li>"
                "<li><code>media</code> - patched radio/audio assets</li>"
                "</ul>"
                "<p>Do not copy only <code>version.dll</code>. This page is reachable because "
                "<code>version.dll</code> is installed, but the rest of the package is missing or incomplete.</p>"
                "<p><a href=\"/api/state\" style=\"color:#1DB954\">Open technical status JSON</a></p>"
                "</main>"
                "</body></html>",
                "text/html");
        });
        log::warn("[server] UI dist missing index.html at " + cfg.ui_dist_dir.string() + " — serving fallback page.");
    }

    // CORS for dev (when SPA hot-reloads from a separate vite port).
    svr.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Background watchdog: poll running flag and gracefully stop httplib.
    std::thread watchdog([&]() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        svr.stop();
    });

    log::info("[server] Listening on http://0.0.0.0:" + std::to_string(cfg.port));
    if (!svr.listen("0.0.0.0", cfg.port)) {
        log::error("[server] Failed to bind port " + std::to_string(cfg.port));
    }
    if (watchdog.joinable()) watchdog.join();
}

} // namespace bridge
