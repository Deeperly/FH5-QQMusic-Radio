// Persistent bridge options shared by the HTTP UI and runtime audio gate.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace bridge {

struct PlaybackOptions {
    std::string active_source = "spotify"; // "spotify", "airplay", "local", or "radio"
    std::string locale = "en";
    std::string menu_playback = "pause"; // "pause" or "silent"
    std::string race_start = "next";  // "restart", "next", "ignore", or "smart"
    uint32_t race_start_restart_threshold_s = 20;  // "smart" mode cutoff, 5-60
    // Start each song at a fixed offset (seconds) instead of 0. Spotify/Local
    // only; AirPlay can restart via previous-track, but cannot seek to an
    // arbitrary offset. Range 3-240 s.
    bool song_start_offset_enabled = false;
    uint32_t song_start_offset_seconds = 30;
    std::string volume_normalization = "on"; // "on" or "off"
    bool quick_station_skip = false;
    bool equalizer_enabled = false;
    std::array<float, 5> equalizer_bands_db{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::string local_music_dir;
    bool local_recursive = true;
    bool local_shuffle = true;
    uint32_t local_volume_percent = 100;
    std::string local_title_metadata_mode = "metadata"; // "metadata" or "filename"
    std::string local_artist_metadata_mode = "albumArtist"; // "albumArtist", "folder", or "album"
    bool night_runners_mode = false;
    uint32_t night_runners_stopped_volume_decrease_percent = 50;
    uint32_t night_runners_max_speed_mph = 120;
    std::string night_runners_speed_unit = "mph"; // "mph" or "kmh"
    bool night_runners_curve_enabled = false;
    float night_runners_curve_exponent = 2.0f;
    bool night_runners_lazy_volume_enabled = false;
    float night_runners_lazy_hold_seconds = 2.0f;
    bool night_runners_low_cut_enabled = false;
    std::string night_runners_frequency_cut_mode = "low"; // "low" or "high"
    uint32_t night_runners_low_cut_amount_percent = 50;
    uint32_t night_runners_low_cut_frequency_hz = 140;
    bool night_runners_non_driving_volume_enabled = true;
    uint32_t night_runners_non_driving_volume_percent = 50;
    // Dynamic ("speed-relative") night-runners volume model. When enabled it
    // replaces the max-speed/curve/lazy mapping with a threshold + trailing-peak
    // buffer state machine (see fmod_inject.cpp). Reuses
    // night_runners_stopped_volume_decrease_percent as the volume floor.
    bool night_runners_dynamic_mode = false;
    uint32_t night_runners_dynamic_threshold_mph = 40; // min speed for buffered swell
    uint32_t night_runners_dynamic_buffer_percent = 15; // tolerance band as % of peak
    float night_runners_dynamic_buffer_fill_seconds = 3.0f; // time for buffer to fill
    float night_runners_dynamic_increase_seconds = 1.5f; // floor->full ramp-up time
    float night_runners_dynamic_decrease_seconds = 2.5f; // full->floor ramp-down time
    // Rate (mph per second) at which the trailing max speed eases down toward the
    // current speed while above it. Lets a slow deceleration keep the speed
    // inside the buffer (volume stays high). 0 = off (peak holds).
    uint32_t night_runners_dynamic_max_decay_mph_s = 0;
    bool radio_logo_album_art_enabled = true;
    bool radio_logo_custom_graphic_enabled = false;
    std::string radio_logo_spotify_variant = "white"; // "white" or "color"
    bool metadata_truncation_enabled = true;
    uint32_t metadata_truncation_length = 30; // characters, 10-50
};

bool is_valid_active_source(std::string_view value);
bool is_valid_menu_playback(std::string_view value);
bool is_valid_race_start(std::string_view value);
bool is_valid_volume_normalization(std::string_view value);
bool is_valid_night_runners_speed_unit(std::string_view value);
bool is_valid_night_runners_frequency_cut_mode(std::string_view value);
bool is_valid_locale(std::string_view value);
bool is_valid_radio_logo_spotify_variant(std::string_view value);
bool is_valid_local_title_metadata_mode(std::string_view value);
bool is_valid_local_artist_metadata_mode(std::string_view value);
uint32_t sanitize_metadata_truncation_length(uint32_t value);
std::string truncate_metadata_text(std::string_view value, bool enabled,
                                   uint32_t max_chars);
bool parse_options_json(std::string_view json, PlaybackOptions& out);
std::string options_to_json(const PlaybackOptions& options);

class PlaybackOptionsStore {
public:
    explicit PlaybackOptionsStore(std::filesystem::path path);

    void load();
    PlaybackOptions snapshot() const;
    bool update(const PlaybackOptions& options, std::string* error = nullptr);

private:
    bool save_locked(std::string* error);

    std::filesystem::path path_;
    mutable std::mutex mtx_;
    PlaybackOptions options_;
};

} // namespace bridge
