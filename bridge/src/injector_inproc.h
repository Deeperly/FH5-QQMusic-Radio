// In-process metadata injector for version.dll (phase 2.5).
//
// Runs INSIDE forzahorizon6.exe — direct pointer dereference with SEH
// instead of ReadProcessMemory / WriteProcessMemory.  Discovery is
// identical to the cross-process injector (RTTI chain walk + heap scan)
// but much faster because there's no RPM overhead.

#pragma once

#include "injector_iface.h"
#include "sigscan.h"

#include <Windows.h>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

namespace bridge {

class InProcessInjector : public IMetadataInjector {
public:
    struct GameStateDebugSnapshot {
        uintptr_t radio_state = 0;
        uintptr_t radio_player = 0;
        uint8_t menu_open = 0;
        uint8_t race_active_a = 0;
        uint8_t race_active_b = 0;
        uint32_t race_restart_marker = 0;
        uint32_t stinger_a = 0;
        uint32_t stinger_b = 0;
        uint32_t stinger_c = 0;
        uint32_t stinger_d = 0;
        uint32_t stinger_e = 0;
        bool radio_state_read = false;
        bool radio_player_read = false;
    };

    struct RadioStreamDebugEntry {
        uintptr_t wrapper = 0;
        uintptr_t fmod_sound = 0;
        uintptr_t sample_properties = 0;
        uint32_t handle = 0;
        std::string sound_name;
        std::string display_name;
        std::string artist;
        bool active = false;
    };

    struct RadioStreamDebugSnapshot {
        uintptr_t radio_state = 0;
        uintptr_t radio_player = 0;
        uintptr_t selected_station = 0;
        std::string selected_station_name;
        bool radio_state_read = false;
        bool radio_player_read = false;
        bool selected_station_read = false;
        std::vector<RadioStreamDebugEntry> streams;
    };

    InProcessInjector() = default;
    ~InProcessInjector() override;

    bool attach() override;
    bool is_attached() const override;
    bool is_ready() const override {
        return discovery_done_.load() &&
               (!radio_logo_probe_required_.load() ||
                radio_logo_probe_finished_.load());
    }
    bool discovery_ready() const {
        return discovery_done_.load();
    }
    void detach() override;

    // Accessors for native radio/DSP integration (set once during discovery,
    // immutable after)
    uintptr_t module_base() const { return module_base_; }
    size_t    module_size() const { return module_size_; }
    const std::vector<uintptr_t>& radio_instances() const { return addrs_.radio_stream_instances; }

    // Sigscan-resolved RVAs. Populated once during attach(); empty if the
    // resolver failed (FmodInject treats missing RVAs as a graceful no-op).
    const ResolvedRvas& rvas() const { return rvas_; }

    // Re-run sigscan for any RVA still unresolved. Cheap: each helper
    // skips already-filled slots, so this only re-scans the misses. Used
    // by FmodInject's init loop to recover from lazy-decrypted .text
    // blocks (FH6 encrypts critical FMOD wrappers at rest; the bytes
    // become scannable only after the game has called into them once).
    // The initial attach pass is quiet on failure; the first retry logs
    // detailed FAIL lines if anything is still missing, then later retries
    // are quiet again to avoid warning spam.
    void retry_signatures();
    bool push_metadata(const std::string& track_name, const std::string& artist) override;
    RadioState get_state() const override;

    // MSVC std::string read (direct in-process access with SEH).
    std::optional<std::string> read_game_string(uintptr_t addr) const;

    // Phase 3h refinement: returns true iff the game's currently active
    // radio station is R10 (Streamer-Mode Spotify slot).
    bool is_spotify_station_active() const;

    // Returns the RadioStreamFmod wrapper/channel handle for the Spotify stream.
    // media_free=false: anchor only on the R10 wired sample (patched-media path)
    // so it does not migrate onto a stale vanilla radio stream. media_free=true:
    // the patched single-sample SampleList is absent, so anchor on the active
    // Streamer Mode stream regardless of which curated track is loaded (only one
    // stream is live at a time; the caller gates on Streamer Mode being the
    // selected station). Lets the DSP attach with vanilla radio files.
    bool spotify_radio_stream_handle(uintptr_t& wrapper,
                                     uint32_t& handle,
                                     bool& has_sound,
                                     bool media_free = false) const;

    // Re-scan live heap for RadioStreamFmod refcount wrappers using the
    // already-discovered vtable. This is cheaper and quieter than the full
    // RTTI discovery pass and helps recover route-created wrapper churn.
    bool refresh_radio_stream_instances();

    // Runtime radio logo: in-place BC7 upload into the engine-owned stock
    // Streamer logo resource.
    void configure_runtime_radio_logo(std::filesystem::path logo_dir,
                                      bool album_art_enabled,
                                      bool custom_graphic_enabled,
                                      std::string spotify_variant,
                                      std::string active_source,
                                      std::string artwork_key = {},
                                      std::vector<uint8_t> artwork_bytes = {},
                                      bool artwork_loading = false);
    void start_runtime_radio_logo_override();

    // Read-only menu-state signal. The field name is intentionally narrow
    // because it is not a complete public game-state model.
    bool is_game_menu_open() const;

    // Read-only race-activity gate used by the audio transition logic.
    bool is_race_active() const;

    // Read-only restart-menu edge gate used by the audio transition logic.
    bool is_race_restart_menu_active() const;

    // Narrow race-menu audio-state marker, not a general race activity flag.
    bool is_race_stinger_menu_active() const;

    // Diagnostic raw game-state fields used by SPOTIFY_RADIO_DIAG builds to
    // debug menu/garage/race false positives without adding extra live probes.
    GameStateDebugSnapshot game_state_debug_snapshot() const;

    // Diagnostic radio-stream snapshot for long vanilla/DJ observation runs.
    // This is read-only and intentionally compact: callers should log only on
    // changes so waiting for natural song endings does not flood the log.
    RadioStreamDebugSnapshot radio_stream_debug_snapshot() const;

    // Diagnostic counters for the write_game_string VirtualAlloc path (the only
    // MEM_COMMIT allocation in the injector). SPOTIFY_RADIO_DIAG memory telemetry
    // uses these to attribute commit-charge changes to game-string allocations.
    void debug_string_allocs(uint64_t& count, uint64_t& bytes) const {
        count = wgs_alloc_count_.load(std::memory_order_relaxed);
        bytes = wgs_alloc_bytes_.load(std::memory_order_relaxed);
    }

    // Read-only car-speed source for Night Runners mode. Returns empty until
    // the version-resolved local Data Out source mirror validates a sample.
    std::optional<float> vehicle_speed_mps() const;

    // Push a speed sample (m/s) from an external source (the Forza Data Out UDP
    // listener on games without a usable in-memory speed mirror, e.g. FH5).
    // Feeds the same fresh-sample fast path vehicle_speed_mps() reads.
    void push_external_speed_mps(float mps);

    // When set, vehicle_speed_mps() returns ONLY externally-pushed samples and
    // never runs the in-memory speed-telemetry resolver (used on FH5, whose
    // Data Out graph has no portable in-memory mirror — speed comes via UDP).
    void set_external_speed_only(bool on);

    // DIAG-only, FH5-only: read-only inspection of the engine default texture
    // vector and handle-table decode. This is not the radio-logo container; it
    // keeps the FH5 handle model visible while the real logo resource is found.
    void fh5_logo_diag();

    // Native-DSP state handoff used by runtime logo replacement. The logo path
    // avoids holding bridge-created UI textures through garage/non-driving UI
    // transitions because those states use a different logo binding/lifetime.
    void update_runtime_logo_game_state(bool non_driving_candidate,
                                        bool garage_candidate,
                                        bool race_stinger);

private:
    mutable std::mutex mtx_;
    uintptr_t module_base_ = 0;
    size_t    module_size_ = 0;
    size_t    text_size_ = 0;
    uint32_t  rdata_offset_ = 0;
    size_t    rdata_size_ = 0;
    GameAddresses addrs_;
    ResolvedRvas  rvas_;
    int signature_retry_attempts_ = 0;
    std::atomic<bool> discovery_done_{false};
    std::atomic<bool> discovery_running_{false};
    std::thread discovery_thread_;
    std::string last_injected_track_;

    std::atomic<bool> speed_sampler_running_{false};
    std::thread speed_sampler_thread_;
    std::atomic<float> speed_last_mps_{0.0f};
    std::atomic<uint64_t> speed_last_sample_ms_{0};
    std::atomic<uint32_t> speed_capture_count_{0};
    std::atomic<bool> external_speed_only_{false};
    mutable std::atomic<uint64_t> wgs_alloc_count_{0};
    mutable std::atomic<uint64_t> wgs_alloc_bytes_{0};
    std::atomic<bool> runtime_logo_game_state_known_{false};
    std::atomic<bool> runtime_logo_non_driving_candidate_{false};
    std::atomic<bool> runtime_logo_garage_candidate_{false};
    std::atomic<bool> runtime_logo_race_stinger_{false};
    std::atomic<uint64_t> runtime_logo_state_change_ms_{0};
    std::atomic<uint64_t> runtime_logo_last_non_driving_ms_{0};

    std::atomic<bool> radio_logo_probe_running_{false};
    std::atomic<bool> ui_stock_upload_test_{false};
    std::atomic<bool> radio_logo_probe_required_{false};
    std::atomic<bool> radio_logo_probe_finished_{true};
    std::thread radio_logo_probe_thread_;
    std::mutex runtime_radio_logo_mtx_;
    std::mutex runtime_radio_logo_cv_mtx_;
    std::condition_variable runtime_radio_logo_cv_;
    std::filesystem::path runtime_radio_logo_dir_;
    bool runtime_radio_logo_album_art_enabled_ = true;
    bool runtime_radio_logo_custom_graphic_enabled_ = false;
    std::string runtime_radio_logo_spotify_variant_ = "white";
    std::string runtime_radio_logo_active_source_ = "spotify";
    std::string runtime_radio_logo_artwork_key_;
    std::vector<uint8_t> runtime_radio_logo_artwork_bytes_;
    bool runtime_radio_logo_artwork_loading_ = false;
    uint64_t runtime_radio_logo_revision_ = 1;
    std::atomic<uint64_t> runtime_radio_logo_wake_seq_{0};

    // PE parsing
    void parse_pe_sections();

    // Discovery thread — retries until radio system is live
    void start_discovery();
    bool discover_addresses();
    bool refresh_sample_properties();
    void start_speed_sampler();
    void speed_sampler_loop();
    void radio_logo_cache_probe_loop(bool apply_vanilla_swap,
                                     bool apply_custom_art,
                                     bool stock_resource_validate,
                                     bool inplace_update);
    void fh5_radio_logo_probe_loop();

    // Heap scan — walk MEM_PRIVATE regions for vtable pointer
    std::vector<uintptr_t> scan_heap(uintptr_t target_vtable) const;

    // MSVC std::string write (direct in-process access with SEH)
    bool write_game_string(uintptr_t addr, const std::string& text);
    bool is_spotify_station_active_locked() const;
    bool is_game_menu_open_locked() const;
    bool is_race_active_locked() const;
    bool is_race_restart_menu_active_locked() const;
    bool any_radio_stream_sound_active_locked() const;
};

} // namespace bridge
