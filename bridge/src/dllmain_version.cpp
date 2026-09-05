// Entry point for the version.dll proxy DLL (v1 strip, 2026-05-19).
//
// Sideloaded by forzahorizon6.exe via Windows DLL search order.
// Forwards all 17 real version.dll exports to System32 (see forwarders.cpp).
//
// On DLL_PROCESS_ATTACH we spawn a background thread that:
//   - Initializes the file logger
//   - Constructs InProcessInjector, FmodInject, LibrespotPlayer
//   - Starts a status-sampler thread (polls injector + fmod state at 1 Hz
//     into BridgeStateStore)
//   - Starts a metadata-pump thread (polls active source metadata at 2 Hz
//     and pushes into InProcessInjector::push_metadata)
//   - Starts the slim HTTP server (/api/state + /api/events + SPA static)
//
// No Web API client. No OAuth. No tokens directory. The librespotc cached
// blob is the only persisted credential.

#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>

#include "bridge_state.h"
#include "config.h"
#include "fmod_inject.h"
#include "injector_inproc.h"
#include "airplay_source.h"
#include "local_file_source.h"
#include "librespot.h"
#include "log_file.h"
#include "options.h"
#include "qqmusic_source.h"
#include "radio_source.h"
#include "server.h"
#include "source_manager.h"
#include "spotify_source.h"
#include "stderr_pump.h"
#include "vanilla_source.h"
#include "forza_dataout.h"
#include "game_profile.h"

#pragma comment(lib, "Psapi.lib")  // GetProcessMemoryInfo (DIAG mem telemetry)

namespace {

HMODULE g_module = nullptr;
std::atomic<bool> g_running{true};

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string module_basename_of(uintptr_t /*base*/) {
    char buf[MAX_PATH] = {};
    HMODULE main = ::GetModuleHandleW(nullptr);
    ::GetModuleFileNameA(main, buf, sizeof(buf));
    std::filesystem::path p(buf);
    return to_lower(p.filename().string());
}

bool ui_file_contains(const std::filesystem::path& path, const std::string& needle) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 2 * 1024 * 1024) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    return in && data.find(needle) != std::string::npos;
}

bool verify_ui_credit_integrity(const std::filesystem::path& ui_dist_dir) {
    static constexpr const char* kCreditText = "Made by Big John";
    static constexpr const char* kCreditHref = "https://ko-fi.com/big_john";

    std::error_code ec;
    if (!std::filesystem::exists(ui_dist_dir / "index.html", ec)) return false;

    bool saw_text = false;
    bool saw_href = false;
    for (std::filesystem::recursive_directory_iterator it(ui_dist_dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        auto ext = to_lower(it->path().extension().string());
        if (ext != ".html" && ext != ".js" && ext != ".css") continue;

        if (!saw_text) saw_text = ui_file_contains(it->path(), kCreditText);
        if (!saw_href) saw_href = ui_file_contains(it->path(), kCreditHref);
        if (saw_text && saw_href) return true;
    }
    return false;
}

std::string read_release_version(const std::filesystem::path& ui_dist_dir) {
    auto path = ui_dist_dir / "release.json";
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 16 * 1024) return {};

    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in) return {};

    std::string key = "\"version\"";
    size_t pos = data.find(key);
    if (pos == std::string::npos) return {};
    pos = data.find(':', pos + key.size());
    if (pos == std::string::npos) return {};
    pos = data.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    size_t end = data.find('"', pos + 1);
    if (end == std::string::npos || end <= pos + 1) return {};

    std::string version = data.substr(pos + 1, end - pos - 1);
    if (version.size() > 64) return {};
    return version;
}

void status_sampler(bridge::InProcessInjector& injector,
                    bridge::FmodInject& fmod_inject,
                    bridge::SourceManager& sources,
                    bridge::BridgeStateStore& store) {
    using namespace std::chrono_literals;
    std::string main_module = module_basename_of(0);
    uint64_t mem_tick = 0;
    while (g_running.load()) {
        // Game / injector.
        uintptr_t base = injector.module_base();
        bool attached  = (base != 0);
        bool ready     = injector.is_ready();
        store.set_game(attached, attached ? main_module : "", static_cast<uint64_t>(base), ready);

        // Audio / fmod.
        auto fs = fmod_inject.status();
        store.set_audio(fs.audio_active, fs.r10_active, fs.migrated_radio_bus,
                        fs.channel_handle, fs.channel_group, fs.ring_available,
                        fs.output_gain, fs.prebuffer_audio, fs.local_audio_hold,
                        fs.underruns, fs.native_dsp_calls, fs.native_dsp_len,
                        fs.native_dsp_in_ch, fs.native_dsp_out_ch,
                        fs.native_channel_i, fs.native_channel_vt,
                        fs.native_channel_508_bits, fs.native_channel_530,
                        fs.native_channel_568_bits, fs.native_send_state_1c0,
                        fs.native_send_tail_nonzero, fs.native_send_tail_hash);
        store.set_driving(fs.driving_speed_available, fs.driving_speed_mps,
                          fs.night_runners_volume_factor,
                          fs.night_runners_non_driving_volume_active,
                          fs.night_runners_lazy_cooldown,
                          fs.night_runners_speed_progress,
                          fs.night_runners_dynamic_peak_mps,
                          fs.night_runners_dynamic_buffer_mps,
                          fs.night_runners_dynamic_state);
        sources.publish_source_states();

#if defined(SPOTIFY_RADIO_DIAG)
        // Commit-charge / leak telemetry for the rare "memory maxed out" reports
        // (game telemetry shows >physical RAM = COMMIT charge, not working set).
        // Watch commit + priv_commit climb over time; priv_regions climbing =
        // many small leaked pages (write_game_string); a huge priv_largest = a
        // single oversized allocation (e.g. image decode); ring full + dsp_calls
        // flat = stalled consumer with the producer backing up.
        if ((mem_tick++ % 5) == 0) {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            uint64_t commit_mb = 0, ws_mb = 0;
            if (::GetProcessMemoryInfo(
                    ::GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                    sizeof(pmc))) {
                commit_mb = pmc.PrivateUsage / (1024 * 1024);
                ws_mb = pmc.WorkingSetSize / (1024 * 1024);
            }
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            ::GlobalMemoryStatusEx(&ms);
            uint64_t priv_commit = 0, priv_largest = 0, priv_regions = 0;
            uintptr_t priv_largest_base = 0, priv_largest_alloc = 0;
            uint32_t priv_largest_prot = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            uintptr_t scan_addr = 0;
            const uintptr_t kMaxUser = 0x00007FFFFFFFFFFFull;
            while (scan_addr < kMaxUser &&
                   ::VirtualQuery(reinterpret_cast<LPCVOID>(scan_addr), &mbi,
                                  sizeof(mbi))) {
                uintptr_t next =
                    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (next <= scan_addr) break;
                if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
                    priv_commit += mbi.RegionSize;
                    ++priv_regions;
                    if (mbi.RegionSize > priv_largest) {
                        priv_largest = mbi.RegionSize;
                        priv_largest_base =
                            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                        priv_largest_alloc =
                            reinterpret_cast<uintptr_t>(mbi.AllocationBase);
                        priv_largest_prot = mbi.Protect;
                    }
                }
                scan_addr = next;
            }
            char lbase[40], lalloc[40];
            std::snprintf(lbase, sizeof(lbase), "0x%llX",
                          (unsigned long long)priv_largest_base);
            std::snprintf(lalloc, sizeof(lalloc), "0x%llX",
                          (unsigned long long)priv_largest_alloc);
            uint64_t wgs_n = 0, wgs_b = 0;
            injector.debug_string_allocs(wgs_n, wgs_b);
            bridge::log::info(
                "[mem-diag] commit=" + std::to_string(commit_mb) + "MB"
                + " ws=" + std::to_string(ws_mb) + "MB"
                + " sysload=" + std::to_string(ms.dwMemoryLoad) + "%"
                + " availphys=" + std::to_string(ms.ullAvailPhys / (1024 * 1024)) + "MB"
                + " pagefile_avail=" + std::to_string(ms.ullAvailPageFile / (1024 * 1024)) + "MB"
                + " priv_commit=" + std::to_string(priv_commit / (1024 * 1024)) + "MB"
                + " priv_regions=" + std::to_string(priv_regions)
                + " priv_largest=" + std::to_string(priv_largest / (1024 * 1024)) + "MB"
                + " priv_largest_base=" + lbase
                + " priv_largest_alloc=" + lalloc
                + " priv_largest_prot=0x" + std::to_string(priv_largest_prot)
                + " ring_avail=" + std::to_string(fs.ring_available)
                + " dsp_calls=" + std::to_string(fs.native_dsp_calls)
                + " underruns=" + std::to_string(fs.underruns)
                + " wgs_allocs=" + std::to_string(wgs_n)
                + " wgs_kb=" + std::to_string(wgs_b / 1024));
        }
#endif

        std::this_thread::sleep_for(1s);
    }
}

void driving_sampler(bridge::FmodInject& fmod_inject,
                     bridge::BridgeStateStore& store) {
    using namespace std::chrono_literals;
    constexpr auto kDrivingPublishInterval = 50ms;
    while (g_running.load()) {
        auto fs = fmod_inject.status();
        store.set_driving(fs.driving_speed_available, fs.driving_speed_mps,
                          fs.night_runners_volume_factor,
                          fs.night_runners_non_driving_volume_active,
                          fs.night_runners_lazy_cooldown,
                          fs.night_runners_speed_progress,
                          fs.night_runners_dynamic_peak_mps,
                          fs.night_runners_dynamic_buffer_mps,
                          fs.night_runners_dynamic_state);
        std::this_thread::sleep_for(kDrivingPublishInterval);
    }
}

void metadata_pump(bridge::InProcessInjector& injector,
                   bridge::SourceManager& sources,
                   bridge::PlaybackOptionsStore& options,
                   const std::filesystem::path& logo_dir) {
    using namespace std::chrono_literals;
    constexpr auto kMetadataPumpInterval = 50ms;
    constexpr auto kMetadataWriteInterval = 500ms;
    auto next_metadata_write = std::chrono::steady_clock::now();
    while (g_running.load()) {
        auto opts = options.snapshot();
        auto lt = sources.last_track();

        // Apply a pending race-start song offset once the (skipped-to) track is
        // up and playing. Armed only by the race-start next/restart actions, so
        // normal song changes are unaffected.
        sources.service_race_start_offset();

        injector.configure_runtime_radio_logo(
            logo_dir,
            opts.radio_logo_album_art_enabled,
            opts.radio_logo_custom_graphic_enabled,
            opts.radio_logo_spotify_variant,
            sources.active_source_id(),
            lt.artwork_key,
            lt.artwork_bytes,
            lt.artwork_loading);
        auto now = std::chrono::steady_clock::now();
        if (now >= next_metadata_write && injector.is_ready()) {
            next_metadata_write = now + kMetadataWriteInterval;
            if (!lt.title.empty()) {
                injector.push_metadata(
                    bridge::truncate_metadata_text(
                        lt.title,
                        opts.metadata_truncation_enabled,
                        opts.metadata_truncation_length),
                    bridge::truncate_metadata_text(
                        lt.artist,
                        opts.metadata_truncation_enabled,
                        opts.metadata_truncation_length));
            }
        }
        injector.fh5_logo_diag();  // DIAG/FH5-only read-only logo-resource inspection
        std::this_thread::sleep_for(kMetadataPumpInterval);
    }
}

DWORD WINAPI init_thread_proc(LPVOID) {
    // DLL directory — for logs, cache, packaged UI.
    wchar_t dll_path_buf[MAX_PATH];
    ::GetModuleFileNameW(g_module, dll_path_buf, MAX_PATH);
    auto dll_dir = std::filesystem::path(dll_path_buf).parent_path();
    bridge::AppConfig cfg = bridge::resolve_config(dll_dir);
    std::string package_version = read_release_version(cfg.ui_dist_dir);

#if defined(SPOTIFY_RADIO_PUBLIC) && !defined(SPOTIFY_RADIO_DIAG)
    bridge::log::init({}, package_version);  // public builds skip on-disk logging entirely
#else
    bridge::log::init(dll_dir / "spotify-radio.log", package_version);
#endif
    bridge::log::info(std::string("=== Spotify Radio Bridge (version.dll v1) starting, build ")
                      + __DATE__ + " " + __TIME__
                      + (package_version.empty() ? "" : " package " + package_version)
                      + " ===");
#ifdef SPOTIFY_RADIO_DIAG
    bridge::log::info("=== DIAGNOSTIC BUILD — stderr capture + file log enabled ===");
    bridge::stderr_pump::start();
#endif

    bridge::BridgeStateStore store;
    bool ui_credit_verified = verify_ui_credit_integrity(cfg.ui_dist_dir);
    store.set_ui_integrity(ui_credit_verified);
    if (!ui_credit_verified) {
        bridge::log::warn("[ui] Credit integrity check failed for " + cfg.ui_dist_dir.string());
    }

    bridge::PlaybackOptionsStore options(dll_dir / "spotify-radio" / "options.json");
    options.load();
    {
        auto initial_options = options.snapshot();
        store.set_options(initial_options.locale,
                          initial_options.menu_playback,
                          initial_options.race_start,
                          initial_options.race_start_restart_threshold_s,
                          initial_options.song_start_offset_enabled,
                          initial_options.song_start_offset_seconds,
                          initial_options.volume_normalization,
                          initial_options.quick_station_skip,
                          initial_options.equalizer_enabled,
                          initial_options.equalizer_bands_db,
                          initial_options.local_volume_percent,
                          initial_options.local_title_metadata_mode,
                          initial_options.local_artist_metadata_mode,
                          initial_options.night_runners_mode,
                          initial_options.night_runners_stopped_volume_decrease_percent,
                          initial_options.night_runners_max_speed_mph,
                          initial_options.night_runners_speed_unit,
                          initial_options.night_runners_curve_enabled,
                          initial_options.night_runners_curve_exponent,
                          initial_options.night_runners_lazy_volume_enabled,
                          initial_options.night_runners_lazy_hold_seconds,
                          initial_options.night_runners_low_cut_enabled,
                          initial_options.night_runners_frequency_cut_mode,
                          initial_options.night_runners_low_cut_amount_percent,
                          initial_options.night_runners_low_cut_frequency_hz,
                          initial_options.night_runners_non_driving_volume_enabled,
                          initial_options.night_runners_non_driving_volume_percent,
                          initial_options.night_runners_dynamic_mode,
                          initial_options.night_runners_dynamic_threshold_mph,
                          initial_options.night_runners_dynamic_buffer_percent,
                          initial_options.night_runners_dynamic_buffer_fill_seconds,
                          initial_options.night_runners_dynamic_increase_seconds,
                          initial_options.night_runners_dynamic_decrease_seconds,
                          initial_options.night_runners_dynamic_max_decay_mph_s,
                          initial_options.radio_logo_album_art_enabled,
                          initial_options.radio_logo_custom_graphic_enabled,
                          initial_options.radio_logo_spotify_variant,
                          initial_options.metadata_truncation_enabled,
                          initial_options.metadata_truncation_length);
    }

    bridge::InProcessInjector injector;
    bool injector_attached = injector.attach();
    if (!injector_attached) {
        bridge::log::error("[inject] attach failed; metadata/audio discovery will stay offline");
    }
    auto logo_dir = dll_dir / "spotify-radio" / "logos";
    {
        auto initial_options = options.snapshot();
        injector.configure_runtime_radio_logo(
            logo_dir,
            initial_options.radio_logo_album_art_enabled,
            initial_options.radio_logo_custom_graphic_enabled,
            initial_options.radio_logo_spotify_variant,
            initial_options.active_source);
    }
    {
        // Runtime radio logo. Production path = in-place BC7 upload into the
        // engine-owned stock Streamer logo resource (start_runtime_radio_logo_override).
        // runtime-logo-disabled.flag turns this optional override off.
        std::error_code ec;
        auto disabled_flag =
            dll_dir / "spotify-radio" / "runtime-logo-disabled.flag";
        const bool runtime_logo_disabled =
            std::filesystem::exists(disabled_flag, ec);
        if (runtime_logo_disabled) {
            bridge::log::warn("[logo] runtime-logo-disabled.flag present; "
                              "runtime logo override disabled for this process");
        } else if (injector_attached) {
            injector.start_runtime_radio_logo_override();
        }
    }

    bridge::FmodInject fmod_inject(injector, dll_dir);
    fmod_inject.start();

    bridge::LibrespotConfig lscfg;
    lscfg.device_name = "FH6 Radio";
    lscfg.cache_dir   = dll_dir / "spotify-radio" / "librespot-cache";
    lscfg.volume_normalization = options.snapshot().volume_normalization == "on";
    lscfg.equalizer_enabled = options.snapshot().equalizer_enabled;
    lscfg.equalizer_bands_db = options.snapshot().equalizer_bands_db;
    std::error_code ec;
    std::filesystem::create_directories(lscfg.cache_dir, ec);
    bridge::LibrespotPlayer librespot(std::move(lscfg));
    librespot.set_state_store(&store);
    bridge::SpotifySource spotify_source(librespot);
    bridge::AirPlayPlayer airplay(
        "FH6 Radio",
        dll_dir / "spotify-radio" / "airplay-cache",
        [&fmod_inject](const int16_t* samples, size_t frames, float gain) {
            return fmod_inject.feed_pcm_s16(
                samples, frames, bridge::FmodInject::kPcmChannels, gain);
        });
    bridge::AirPlaySource airplay_source(airplay);
    constexpr size_t kLocalFilePrebufferBytes =
        (bridge::FmodInject::kPcmSampleRate * bridge::FmodInject::kPcmChannels *
         sizeof(int16_t)) / 2; // Keep local decode within ~500 ms of output.
    bridge::LocalFileSource local_source(
        [&fmod_inject](const int16_t* samples, size_t frames, float gain) {
            return fmod_inject.feed_pcm_s16(
                samples, frames, bridge::FmodInject::kPcmChannels, gain);
        },
        [&fmod_inject]() {
            if (!fmod_inject.pcm_accepting()) return size_t{0};
            size_t buffered = fmod_inject.pcm_buffered_bytes();
            if (buffered >= kLocalFilePrebufferBytes) return size_t{0};
            return std::min(fmod_inject.pcm_free_bytes(),
                            kLocalFilePrebufferBytes - buffered);
        },
        [&fmod_inject]() {
            fmod_inject.clear_pcm();
        });
    bridge::RadioSource radio_source(
        dll_dir / "spotify-radio" / "radio-stations.json",
        [&fmod_inject](const int16_t* samples, size_t frames, float gain) {
            return fmod_inject.feed_pcm_s16(
                samples, frames, bridge::FmodInject::kPcmChannels, gain);
        },
        [&fmod_inject]() {
            if (!fmod_inject.pcm_accepting()) return size_t{0};
            return fmod_inject.pcm_free_bytes();
        },
        [&fmod_inject]() {
            fmod_inject.clear_pcm();
        });
    bridge::QQMusicSource qqmusic_source(
        [&fmod_inject](const float* samples, size_t frames) {
            return fmod_inject.feed_pcm_float(samples, frames);
        },
        [&fmod_inject]() {
            fmod_inject.clear_pcm();
        },
        options.snapshot().qqmusic_process_name,
        options.snapshot().qqmusic_executable);
    {
        auto initial_options = options.snapshot();
        std::string ignored;
        local_source.configure(initial_options.local_music_dir,
                               initial_options.local_recursive,
                               initial_options.local_shuffle,
                               &ignored);
        local_source.set_volume_normalization(
            initial_options.volume_normalization == "on");
        local_source.set_equalizer(initial_options.equalizer_enabled,
                                  initial_options.equalizer_bands_db);
        local_source.set_volume_percent(initial_options.local_volume_percent);
        local_source.set_metadata_display_modes(
            initial_options.local_title_metadata_mode,
            initial_options.local_artist_metadata_mode);
        radio_source.set_volume_percent(initial_options.local_volume_percent);
    }
    local_source.start();
    radio_source.start();
    bridge::VanillaSource vanilla_source;
    bridge::SourceManager sources;
    sources.set_state_store(&store);
    {
        auto initial_options = options.snapshot();
        sources.set_metadata_truncation(
            initial_options.metadata_truncation_enabled,
            initial_options.metadata_truncation_length);
    }
    sources.set_clear_pcm_callback([&fmod_inject]() {
        fmod_inject.clear_pcm();
    });
    sources.set_spotify_source(spotify_source);
    sources.register_source(airplay_source);
    sources.set_local_source(local_source);
    sources.register_source(radio_source);
    sources.register_source(qqmusic_source);
    sources.register_source(vanilla_source);
    {
        auto initial_options = options.snapshot();
        if (initial_options.active_source == "spotify") {
            bridge::log::info("[source] Starting initial source spotify");
            sources.activate_current_source();
        } else if (!sources.switch_to(initial_options.active_source)) {
            bridge::log::warn("[source] Unknown initial source " +
                              initial_options.active_source +
                              "; falling back to spotify");
            sources.activate_current_source();
        } else {
            bridge::log::info("[source] Starting initial source " +
                              initial_options.active_source);
        }
    }
    if (sources.active_source_id() == "radio") {
        // Resume the last station if the game closed while online radio was
        // playing. Runs async: play_station does network resolution.
        radio_source.resume_last_station_async();
    }
    sources.publish_source_states();
    fmod_inject.set_menu_playback_controls(
        [&options, &sources]() {
            if (sources.active_source_id() == "radio") return false;
            return options.snapshot().menu_playback == "pause";
        },
        [&options, &sources]() {
            if (sources.active_source_id() == "radio") return std::string("ignore");
            auto race_start = options.snapshot().race_start;
            if (sources.active_source_id() == "airplay" && race_start == "smart") {
                return std::string("restart_if_past_threshold");
            }
            return race_start;
        },
        [&options, &sources]() {
            if (sources.active_source_id() == "radio") return false;
            return options.snapshot().quick_station_skip;
        },
        [&sources]() { return sources.is_playing(); },
        [&sources]() { sources.pause_at_audio_boundary(); },
        [&sources]() { sources.resume_rewound(1000); },
        [&sources, &options]() {
            // Race-start "restart": if the song-start offset is enabled (and the
            // source can seek), restart at the offset instead of 0.
            auto o = options.snapshot();
            std::string source_id = sources.active_source_id();
            if (o.song_start_offset_enabled &&
                source_id != "airplay" && source_id != "radio") {
                uint32_t offset_ms = o.song_start_offset_seconds * 1000u;
                uint32_t dur = sources.last_track().duration_ms;
                if (dur == 0 || offset_ms < dur) return sources.seek(offset_ms);
            }
            if (source_id == "radio") return false;
            return sources.restart_current_track();
        },
        [&sources, &options]() {
            // Race-start "next": skip, then arm the offset for the track that
            // loads (applied by service_race_start_offset in the metadata pump).
            std::string before = sources.last_track().uri;
            std::string source_id = sources.active_source_id();
            if (source_id == "radio") return false;
            bool ok = sources.next_track();
            auto o = options.snapshot();
            if (ok && o.song_start_offset_enabled &&
                source_id != "airplay" && source_id != "radio") {
                sources.request_race_start_offset(
                    o.song_start_offset_seconds * 1000u, before);
            }
            return ok;
        },
        [&sources]() { return sources.current_position_ms(); },
        [&options]() { return options.snapshot().race_start_restart_threshold_s; });
    fmod_inject.set_station_change_track([&sources]() {
        return std::string(sources.active_source_id()) == "qqmusic" &&
               sources.next_track();
    });
    fmod_inject.set_injection_gate([&sources]() {
        return std::string(sources.active_source_id()) == "vanilla";
    });
    fmod_inject.set_night_runners_controls(
        [&options]() { return options.snapshot().night_runners_mode; },
        [&options]() {
            return options.snapshot().night_runners_stopped_volume_decrease_percent;
        },
        [&options]() { return options.snapshot().night_runners_max_speed_mph; },
        [&options]() { return options.snapshot().night_runners_curve_enabled; },
        [&options]() { return options.snapshot().night_runners_curve_exponent; },
        [&options]() {
            return options.snapshot().night_runners_lazy_volume_enabled;
        },
        [&options]() {
            return options.snapshot().night_runners_lazy_hold_seconds;
        },
        [&options]() { return options.snapshot().night_runners_low_cut_enabled; },
        [&options]() {
            return options.snapshot().night_runners_frequency_cut_mode;
        },
        [&options]() {
            return options.snapshot().night_runners_low_cut_amount_percent;
        },
        [&options]() {
            return options.snapshot().night_runners_low_cut_frequency_hz;
        },
        [&options]() {
            return options.snapshot().night_runners_non_driving_volume_enabled;
        },
        [&options]() {
            return options.snapshot().night_runners_non_driving_volume_percent;
        },
        [&injector]() { return injector.vehicle_speed_mps(); },
        [&options]() { return options.snapshot().night_runners_dynamic_mode; },
        [&options]() {
            return options.snapshot().night_runners_dynamic_threshold_mph;
        },
        [&options]() {
            return options.snapshot().night_runners_dynamic_buffer_percent;
        },
        [&options]() {
            return options.snapshot().night_runners_dynamic_buffer_fill_seconds;
        },
        [&options]() {
            return options.snapshot().night_runners_dynamic_increase_seconds;
        },
        [&options]() {
            return options.snapshot().night_runners_dynamic_decrease_seconds;
        },
        [&options]() {
            return options.snapshot().night_runners_dynamic_max_decay_mph_s;
        });

    // FH5 has no portable in-memory speed source, so feed Night Runners speed
    // from the game's own Data Out UDP telemetry. Inert until the user enables
    // Settings>HUD>Data Out (IP 127.0.0.1, this port). Default 5300 (the common
    // Forza Data Out port); override via spotify-radio/dataout-port.txt.
    bridge::ForzaDataOut dataout;
    if (bridge::active_profile().id == bridge::GameId::FH5) {
        uint16_t dataout_port = 5300;
        {
            std::ifstream pf(dll_dir / "spotify-radio" / "dataout-port.txt");
            int p = 0;
            if (pf && (pf >> p) && p > 0 && p <= 65535) {
                dataout_port = static_cast<uint16_t>(p);
            }
        }
        injector.set_external_speed_only(true);
        dataout.start(dataout_port, [&injector](float mps) {
            injector.push_external_speed_mps(mps);
        });
    }

    std::thread sampler(status_sampler, std::ref(injector), std::ref(fmod_inject),
                        std::ref(sources), std::ref(store));
    std::thread driving(driving_sampler, std::ref(fmod_inject), std::ref(store));
    std::thread pump(metadata_pump, std::ref(injector), std::ref(sources),
                     std::ref(options), logo_dir);

    bridge::ServerConfig scfg;
    scfg.port         = cfg.port;
    scfg.ui_dist_dir  = cfg.ui_dist_dir;
    scfg.locales_dir  = cfg.locales_dir;
    bridge::log::info("HTTP server starting on port " + std::to_string(scfg.port));
    bridge::run_server(
        scfg,
        store,
        options,
        sources,
        [&sources, &local_source, &radio_source, &injector, logo_dir](const bridge::PlaybackOptions& next) {
            sources.set_volume_normalization(next.volume_normalization == "on");
            sources.set_equalizer(next.equalizer_enabled, next.equalizer_bands_db);
            sources.set_metadata_truncation(next.metadata_truncation_enabled,
                                            next.metadata_truncation_length);
            local_source.set_volume_percent(next.local_volume_percent);
            local_source.set_metadata_display_modes(
                next.local_title_metadata_mode,
                next.local_artist_metadata_mode);
            radio_source.set_volume_percent(next.local_volume_percent);
            sources.publish_source_states();
            auto lt = sources.last_track();
            injector.configure_runtime_radio_logo(
                logo_dir,
                next.radio_logo_album_art_enabled,
                next.radio_logo_custom_graphic_enabled,
                next.radio_logo_spotify_variant,
                sources.active_source_id(),
                lt.artwork_key,
                lt.artwork_bytes,
                lt.artwork_loading);
        },
        [&librespot](const std::string& access_token) {
            return librespot.authenticate_with_oauth(access_token);
        },
        g_running);

    g_running = false;
    dataout.stop();
    if (sampler.joinable()) sampler.join();
    if (driving.joinable()) driving.join();
    if (pump.joinable())    pump.join();

    airplay.stop();
    fmod_inject.stop();
    librespot.stop();
    local_source.shutdown();
    radio_source.shutdown();

    bridge::log::info("Bridge shutdown.");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        ::CreateThread(nullptr, 0, init_thread_proc, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        bridge::stderr_pump::stop();
        bridge::log::shutdown();
        break;
    }
    return TRUE;
}
