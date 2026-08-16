// v1 — read-only status snapshot for the slim status web UI.
//
// BridgeState aggregates the things the UI displays. It's updated by:
//   - InProcessInjector (game.* fields)
//   - FmodInject        (audio.* fields)
//   - LibrespotPlayer   (spotify.*, track, recent)
//   - log_file          (log_tail, recent_errors)
//
// Producers call `update_*` setters under a single mutex. The web UI
// pulls the JSON snapshot on demand via `snapshot_json()`. No write
// endpoints — UI is purely visual.

#pragma once

#include "audio_source.h"

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

struct BridgeState {
    // ---- game ----
    bool        game_attached  = false;
    std::string game_module    = "";
    uint64_t    game_base      = 0;
    bool        injector_ready = false;

    // ---- audio (FmodInject) ----
    bool     audio_active      = false;          // Sound + Channel created
    bool     r10_active        = false;          // R10 station currently selected
    bool     migrated_radio_bus = false;         // Channel routed off master
    uint64_t audio_channel     = 0;
    uint64_t audio_cg          = 0;
    uint64_t ring_available    = 0;
    float    audio_output_gain = 0.0f;
    bool     audio_prebuffer   = false;
    bool     audio_local_hold  = false;
    uint64_t audio_underruns   = 0;
    uint64_t native_dsp_calls  = 0;
    uint32_t native_dsp_len    = 0;
    int      native_dsp_in_ch  = 0;
    int      native_dsp_out_ch = 0;
    uint64_t native_channel_i  = 0;
    uint64_t native_channel_vt = 0;
    uint32_t native_channel_508_bits = 0;
    uint64_t native_channel_530 = 0;
    uint32_t native_channel_568_bits = 0;
    uint64_t native_send_state_1c0 = 0;
    uint32_t native_send_tail_nonzero = 0;
    uint64_t native_send_tail_hash = 0;

    // ---- spotify (librespotc) ----
    bool        spotify_connected = false;
    bool        spotify_blob_cached = false;
    bool        spotify_playing   = false;
    bool        spotify_transferred_away = false;
    std::string spotify_device_id;

    // ---- source manager ----
    std::string active_source = "spotify";
    struct SourceState {
        std::string id;
        std::string display_name;
        bool available = true;
        bool connected = false;
        bool playing = false;
        SourceCapabilities capabilities;
    };
    std::vector<SourceState> sources;

    // ---- AirPlay (airplayc) ----
    bool        airplay_available = true;
    bool        airplay_running = false;
    bool        airplay_connected = false;
    bool        airplay_playing = false;
    std::string airplay_device_name = "FH6 Radio";
    float       airplay_volume_db = -15.0f;
    uint32_t    airplay_volume_percent = 50;
    std::string airplay_error;

    // ---- local files ----
    bool        local_available = true;
    bool        local_ready = false;
    bool        local_playing = false;
    std::string local_music_dir;
    std::string local_default_music_dir;
    bool        local_recursive = true;
    bool        local_shuffle = true;
    uint32_t    local_track_count = 0;
    uint32_t    local_unsupported_count = 0;
    uint32_t    local_position_ms = 0;
    std::string local_error;

    // ---- online radio ----
    bool        radio_available = true;
    bool        radio_connected = false;
    bool        radio_playing = false;
    std::string radio_station_name;
    std::string radio_station_id;
    std::string radio_codec;
    uint32_t    radio_bitrate = 0;
    std::string radio_error;

    // ---- driving / speed-dependent audio ----
    bool  driving_speed_available = false;
    float driving_speed_mps = 0.0f;
    float night_runners_volume_factor = 1.0f;
    float night_runners_lazy_cooldown = 0.0f;
    float night_runners_speed_progress = 0.0f;
    // Dynamic-mode runtime telemetry for the speed-state bar.
    float night_runners_dynamic_peak_mps = 0.0f;
    float night_runners_dynamic_buffer_mps = 0.0f;
    uint32_t night_runners_dynamic_state = 0; // 0 below,1 accel,2 hold,3 fade

    // ---- mod options ----
    std::string locale = "en";
    std::string menu_playback = "pause";
    std::string race_start = "next";
    uint32_t race_start_restart_threshold_s = 20;
    bool song_start_offset_enabled = false;
    uint32_t song_start_offset_seconds = 30;
    std::string volume_normalization = "on";
    bool quick_station_skip = false;
    bool equalizer_enabled = false;
    std::array<float, 5> equalizer_bands_db{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t local_volume_percent = 100;
    std::string local_title_metadata_mode = "metadata";
    std::string local_artist_metadata_mode = "albumArtist";
    bool night_runners_mode = false;
    uint32_t night_runners_stopped_volume_decrease_percent = 50;
    uint32_t night_runners_max_speed_mph = 120;
    std::string night_runners_speed_unit = "mph";
    bool night_runners_curve_enabled = false;
    float night_runners_curve_exponent = 2.0f;
    bool night_runners_lazy_volume_enabled = false;
    float night_runners_lazy_hold_seconds = 2.0f;
    bool night_runners_low_cut_enabled = false;
    std::string night_runners_frequency_cut_mode = "low";
    uint32_t night_runners_low_cut_amount_percent = 50;
    uint32_t night_runners_low_cut_frequency_hz = 140;
    bool night_runners_non_driving_volume_enabled = true;
    uint32_t night_runners_non_driving_volume_percent = 50;
    bool night_runners_non_driving_volume_active = false;
    bool night_runners_dynamic_mode = false;
    uint32_t night_runners_dynamic_threshold_mph = 40;
    uint32_t night_runners_dynamic_buffer_percent = 15;
    float night_runners_dynamic_buffer_fill_seconds = 3.0f;
    float night_runners_dynamic_increase_seconds = 1.5f;
    float night_runners_dynamic_decrease_seconds = 2.5f;
    uint32_t night_runners_dynamic_max_decay_mph_s = 0;
    bool radio_logo_album_art_enabled = true;
    bool radio_logo_custom_graphic_enabled = false;
    std::string radio_logo_spotify_variant = "white";
    bool metadata_truncation_enabled = true;
    uint32_t metadata_truncation_length = 30;

    // ---- packaged UI integrity ----
    bool ui_credit_verified = true;

    // ---- current track ----
    std::string track_uri;
    std::string track_title;
    std::string track_artist;
    std::string track_album;
    uint32_t    track_duration_ms = 0;

    // ---- recent (last N played) ----
    struct RecentTrack {
        std::string title;
        std::string artist;
        int64_t     at_ms = 0;  // epoch ms
    };
    std::deque<RecentTrack> recent;
};

class BridgeStateStore {
public:
    BridgeStateStore() = default;

    // Producer setters — each one takes the mutex, updates the named
    // sub-record, and bumps the revision counter so the SSE loop knows
    // when to push a new frame.
    void set_game(bool attached, const std::string& module, uint64_t base, bool ready);
    void set_audio(bool active, bool r10_active, bool migrated,
                   uint64_t channel, uint64_t cg, uint64_t ring_avail,
                   float output_gain, bool prebuffer, bool local_hold,
                   uint64_t underruns, uint64_t native_dsp_calls,
                   uint32_t native_dsp_len, int native_dsp_in_ch,
                   int native_dsp_out_ch,
                   uint64_t native_channel_i, uint64_t native_channel_vt,
                   uint32_t native_channel_508_bits,
                   uint64_t native_channel_530,
                   uint32_t native_channel_568_bits,
                   uint64_t native_send_state_1c0,
                   uint32_t native_send_tail_nonzero,
                   uint64_t native_send_tail_hash);
    void set_spotify_connection(bool connected, bool blob_cached,
                                const std::string& device_id);
    void set_spotify_playing(bool playing);
    void set_spotify_transferred_away(bool away);
    void set_active_source(const std::string& source_id);
    void set_sources(const std::vector<BridgeState::SourceState>& sources);
    void set_airplay_source(bool available, bool running, bool connected,
                            bool playing, const std::string& device_name,
                            float volume_db, uint32_t volume_percent,
                            const std::string& error);
    void set_local_source(bool available, bool ready, bool playing,
                          const std::string& music_dir,
                          const std::string& default_music_dir,
                          bool recursive, bool shuffle,
                          uint32_t track_count,
                          uint32_t unsupported_count,
                          uint32_t position_ms,
                          const std::string& error);
    void set_radio_source(bool available, bool connected, bool playing,
                          const std::string& station_name,
                          const std::string& station_id,
                          const std::string& codec,
                          uint32_t bitrate,
                          const std::string& error);
    void set_driving(bool speed_available,
                     float speed_mps,
                     float night_runners_volume_factor,
                     bool night_runners_non_driving_volume_active,
                     float night_runners_lazy_cooldown,
                     float night_runners_speed_progress,
                     float night_runners_dynamic_peak_mps,
                     float night_runners_dynamic_buffer_mps,
                     uint32_t night_runners_dynamic_state);
    void set_options(const std::string& locale,
                     const std::string& menu_playback,
                     const std::string& race_start,
                     uint32_t race_start_restart_threshold_s,
                     bool song_start_offset_enabled,
                     uint32_t song_start_offset_seconds,
                     const std::string& volume_normalization,
                     bool quick_station_skip,
                     bool equalizer_enabled,
                     const std::array<float, 5>& equalizer_bands_db,
                     uint32_t local_volume_percent,
                     const std::string& local_title_metadata_mode,
                     const std::string& local_artist_metadata_mode,
                     bool night_runners_mode,
                     uint32_t night_runners_stopped_volume_decrease_percent,
                     uint32_t night_runners_max_speed_mph,
                     const std::string& night_runners_speed_unit,
                     bool night_runners_curve_enabled,
                     float night_runners_curve_exponent,
                     bool night_runners_lazy_volume_enabled,
                     float night_runners_lazy_hold_seconds,
                     bool night_runners_low_cut_enabled,
                     const std::string& night_runners_frequency_cut_mode,
                     uint32_t night_runners_low_cut_amount_percent,
                     uint32_t night_runners_low_cut_frequency_hz,
                     bool night_runners_non_driving_volume_enabled,
                     uint32_t night_runners_non_driving_volume_percent,
                     bool night_runners_dynamic_mode,
                     uint32_t night_runners_dynamic_threshold_mph,
                     uint32_t night_runners_dynamic_buffer_percent,
                     float night_runners_dynamic_buffer_fill_seconds,
                     float night_runners_dynamic_increase_seconds,
                     float night_runners_dynamic_decrease_seconds,
                     uint32_t night_runners_dynamic_max_decay_mph_s,
                     bool radio_logo_album_art_enabled,
                     bool radio_logo_custom_graphic_enabled,
                     const std::string& radio_logo_spotify_variant,
                     bool metadata_truncation_enabled,
                     uint32_t metadata_truncation_length);
    void set_ui_integrity(bool credit_verified);
    void set_track(const std::string& uri, const std::string& title,
                   const std::string& artist, const std::string& album,
                   uint32_t duration_ms);
    void push_recent(const std::string& title, const std::string& artist);

    // Consumer: copy snapshot (cheap, no locking by caller).
    BridgeState snapshot() const;

    // Revision bumps on any setter call. SSE loop polls this to
    // decide whether to re-serialize + emit.
    uint64_t revision() const;

    // JSON snapshot — used by SSE + /api/state. Self-contained, no
    // nlohmann::json dependency (drops huge vendored header).
    std::string snapshot_json() const;

private:
    mutable std::mutex      mtx_;
    BridgeState             state_;
    std::atomic<uint64_t>   rev_{0};
};

} // namespace bridge
