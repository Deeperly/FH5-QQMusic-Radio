#include "bridge_state.h"
#include "log_file.h"
#include "game_profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace bridge {

namespace {

// Minimal JSON-string escaper. UI only displays ASCII track names anyway —
// log lines may contain quotes / backslashes, so we cover the common cases.
void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void append_bool (std::string& out, bool v)    { out += v ? "true" : "false"; }
void append_u64  (std::string& out, uint64_t v){ out += std::to_string(v); }
void append_u32  (std::string& out, uint32_t v){ out += std::to_string(v); }
void append_i64  (std::string& out, int64_t v) { out += std::to_string(v); }

void append_float(std::string& out, float v) {
    if (!std::isfinite(v)) v = 0.0f;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    out += buf;
}

void append_float_bits(std::string& out, uint32_t bits) {
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    append_float(out, v);
}

void append_hex_u64(std::string& out, uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "\"0x%llX\"", (unsigned long long)v);
    out += buf;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace

void BridgeStateStore::set_game(bool attached, const std::string& module,
                                uint64_t base, bool ready) {
    {
        std::lock_guard lock(mtx_);
        state_.game_attached  = attached;
        state_.game_module    = module;
        state_.game_base      = base;
        state_.injector_ready = ready;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_audio(bool active, bool r10_active, bool migrated,
                                 uint64_t channel, uint64_t cg, uint64_t ring_avail,
                                 float output_gain, bool prebuffer, bool local_hold,
                                 uint64_t underruns, uint64_t native_dsp_calls,
                                 uint32_t native_dsp_len, int native_dsp_in_ch,
                                 int native_dsp_out_ch,
                                 uint64_t native_channel_i,
                                 uint64_t native_channel_vt,
                                 uint32_t native_channel_508_bits,
                                 uint64_t native_channel_530,
                                 uint32_t native_channel_568_bits,
                                 uint64_t native_send_state_1c0,
                                 uint32_t native_send_tail_nonzero,
                                 uint64_t native_send_tail_hash) {
    {
        std::lock_guard lock(mtx_);
        state_.audio_active        = active;
        state_.r10_active          = r10_active;
        state_.migrated_radio_bus  = migrated;
        state_.audio_channel       = channel;
        state_.audio_cg            = cg;
        state_.ring_available      = ring_avail;
        state_.audio_output_gain   = output_gain;
        state_.audio_prebuffer     = prebuffer;
        state_.audio_local_hold    = local_hold;
        state_.audio_underruns     = underruns;
        state_.native_dsp_calls    = native_dsp_calls;
        state_.native_dsp_len      = native_dsp_len;
        state_.native_dsp_in_ch    = native_dsp_in_ch;
        state_.native_dsp_out_ch   = native_dsp_out_ch;
        state_.native_channel_i    = native_channel_i;
        state_.native_channel_vt   = native_channel_vt;
        state_.native_channel_508_bits = native_channel_508_bits;
        state_.native_channel_530  = native_channel_530;
        state_.native_channel_568_bits = native_channel_568_bits;
        state_.native_send_state_1c0 = native_send_state_1c0;
        state_.native_send_tail_nonzero = native_send_tail_nonzero;
        state_.native_send_tail_hash = native_send_tail_hash;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_spotify_connection(bool connected, bool blob_cached,
                                              const std::string& device_id) {
    {
        std::lock_guard lock(mtx_);
        state_.spotify_connected    = connected;
        state_.spotify_blob_cached  = blob_cached;
        state_.spotify_device_id    = device_id;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_spotify_playing(bool playing) {
    {
        std::lock_guard lock(mtx_);
        state_.spotify_playing = playing;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_spotify_transferred_away(bool away) {
    {
        std::lock_guard lock(mtx_);
        state_.spotify_transferred_away = away;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_active_source(const std::string& source_id) {
    {
        std::lock_guard lock(mtx_);
        state_.active_source = source_id.empty() ? "spotify" : source_id;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_sources(
    const std::vector<BridgeState::SourceState>& sources) {
    {
        std::lock_guard lock(mtx_);
        state_.sources = sources;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_airplay_source(bool available, bool running,
                                          bool connected, bool playing,
                                          const std::string& device_name,
                                          float volume_db,
                                          uint32_t volume_percent,
                                          const std::string& error) {
    {
        std::lock_guard lock(mtx_);
        state_.airplay_available = available;
        state_.airplay_running = running;
        state_.airplay_connected = connected;
        state_.airplay_playing = playing;
        state_.airplay_device_name = device_name;
        state_.airplay_volume_db = volume_db;
        state_.airplay_volume_percent = volume_percent;
        state_.airplay_error = error;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_local_source(bool available, bool ready, bool playing,
                                        const std::string& music_dir,
                                        const std::string& default_music_dir,
                                        bool recursive, bool shuffle,
                                        uint32_t track_count,
                                        uint32_t unsupported_count,
                                        uint32_t position_ms,
                                        const std::string& error) {
    {
        std::lock_guard lock(mtx_);
        state_.local_available = available;
        state_.local_ready = ready;
        state_.local_playing = playing;
        state_.local_music_dir = music_dir;
        state_.local_default_music_dir = default_music_dir;
        state_.local_recursive = recursive;
        state_.local_shuffle = shuffle;
        state_.local_track_count = track_count;
        state_.local_unsupported_count = unsupported_count;
        state_.local_position_ms = position_ms;
        state_.local_error = error;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_radio_source(bool available, bool connected,
                                        bool playing,
                                        const std::string& station_name,
                                        const std::string& station_id,
                                        const std::string& codec,
                                        uint32_t bitrate,
                                        const std::string& error) {
    {
        std::lock_guard lock(mtx_);
        state_.radio_available = available;
        state_.radio_connected = connected;
        state_.radio_playing = playing;
        state_.radio_station_name = station_name;
        state_.radio_station_id = station_id;
        state_.radio_codec = codec;
        state_.radio_bitrate = bitrate;
        state_.radio_error = error;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_driving(bool speed_available,
                                   float speed_mps,
                                   float night_runners_volume_factor,
                                   bool night_runners_non_driving_volume_active,
                                   float night_runners_lazy_cooldown,
                                   float night_runners_speed_progress,
                                   float night_runners_dynamic_peak_mps,
                                   float night_runners_dynamic_buffer_mps,
                                   uint32_t night_runners_dynamic_state) {
    if (!std::isfinite(speed_mps) || speed_mps < 0.0f) speed_mps = 0.0f;
    if (!std::isfinite(night_runners_volume_factor) ||
        night_runners_volume_factor < 0.0f) {
        night_runners_volume_factor = 1.0f;
    }
    if (!std::isfinite(night_runners_lazy_cooldown)) {
        night_runners_lazy_cooldown = 0.0f;
    }
    night_runners_lazy_cooldown =
        std::clamp(night_runners_lazy_cooldown, 0.0f, 1.0f);
    if (!std::isfinite(night_runners_speed_progress)) {
        night_runners_speed_progress = 0.0f;
    }
    night_runners_speed_progress =
        std::clamp(night_runners_speed_progress, 0.0f, 1.0f);
    if (!std::isfinite(night_runners_dynamic_peak_mps) ||
        night_runners_dynamic_peak_mps < 0.0f) {
        night_runners_dynamic_peak_mps = 0.0f;
    }
    if (!std::isfinite(night_runners_dynamic_buffer_mps) ||
        night_runners_dynamic_buffer_mps < 0.0f) {
        night_runners_dynamic_buffer_mps = 0.0f;
    }
    if (night_runners_dynamic_state > 3) night_runners_dynamic_state = 0;

    bool changed = false;
    {
        std::lock_guard lock(mtx_);
        changed =
            state_.driving_speed_available != speed_available ||
            std::fabs(state_.driving_speed_mps - speed_mps) > 0.05f ||
            std::fabs(state_.night_runners_volume_factor -
                      night_runners_volume_factor) > 0.005f ||
            state_.night_runners_non_driving_volume_active !=
                night_runners_non_driving_volume_active ||
            std::fabs(state_.night_runners_lazy_cooldown -
                      night_runners_lazy_cooldown) > 0.005f ||
            std::fabs(state_.night_runners_speed_progress -
                      night_runners_speed_progress) > 0.005f ||
            std::fabs(state_.night_runners_dynamic_peak_mps -
                      night_runners_dynamic_peak_mps) > 0.05f ||
            std::fabs(state_.night_runners_dynamic_buffer_mps -
                      night_runners_dynamic_buffer_mps) > 0.05f ||
            state_.night_runners_dynamic_state != night_runners_dynamic_state;
        if (!changed) return;

        state_.driving_speed_available = speed_available;
        state_.driving_speed_mps = speed_mps;
        state_.night_runners_volume_factor = night_runners_volume_factor;
        state_.night_runners_non_driving_volume_active =
            night_runners_non_driving_volume_active;
        state_.night_runners_lazy_cooldown = night_runners_lazy_cooldown;
        state_.night_runners_speed_progress = night_runners_speed_progress;
        state_.night_runners_dynamic_peak_mps = night_runners_dynamic_peak_mps;
        state_.night_runners_dynamic_buffer_mps =
            night_runners_dynamic_buffer_mps;
        state_.night_runners_dynamic_state = night_runners_dynamic_state;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_options(const std::string& locale,
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
                                   uint32_t metadata_truncation_length) {
    {
        std::lock_guard lock(mtx_);
        state_.locale = locale.empty() ? "en" : locale;
        state_.menu_playback = menu_playback;
        state_.race_start = race_start;
        state_.race_start_restart_threshold_s = race_start_restart_threshold_s;
        state_.song_start_offset_enabled = song_start_offset_enabled;
        state_.song_start_offset_seconds =
            std::clamp<uint32_t>(song_start_offset_seconds, 3u, 240u);
        state_.volume_normalization = volume_normalization;
        state_.quick_station_skip = quick_station_skip;
        state_.equalizer_enabled = equalizer_enabled;
        state_.equalizer_bands_db = equalizer_bands_db;
        state_.local_volume_percent = local_volume_percent;
        state_.local_title_metadata_mode =
            local_title_metadata_mode == "filename" ? "filename" : "metadata";
        state_.local_artist_metadata_mode =
            local_artist_metadata_mode == "folder" ||
            local_artist_metadata_mode == "album"
                ? local_artist_metadata_mode
                : "albumArtist";
        state_.night_runners_mode = night_runners_mode;
        state_.night_runners_stopped_volume_decrease_percent =
            night_runners_stopped_volume_decrease_percent;
        state_.night_runners_max_speed_mph = night_runners_max_speed_mph;
        state_.night_runners_speed_unit = night_runners_speed_unit;
        state_.night_runners_curve_enabled = night_runners_curve_enabled;
        state_.night_runners_curve_exponent =
            std::isfinite(night_runners_curve_exponent)
                ? night_runners_curve_exponent
                : 2.0f;
        state_.night_runners_lazy_volume_enabled =
            night_runners_lazy_volume_enabled;
        state_.night_runners_lazy_hold_seconds =
            std::isfinite(night_runners_lazy_hold_seconds)
                ? std::clamp(night_runners_lazy_hold_seconds, 0.5f, 10.0f)
                : 2.0f;
        state_.night_runners_low_cut_enabled =
            night_runners_low_cut_enabled;
        state_.night_runners_frequency_cut_mode =
            night_runners_frequency_cut_mode == "high" ? "high" : "low";
        state_.night_runners_low_cut_amount_percent =
            night_runners_low_cut_amount_percent > 100
                ? 100
                : night_runners_low_cut_amount_percent;
        state_.night_runners_low_cut_frequency_hz =
            night_runners_low_cut_frequency_hz;
        state_.night_runners_non_driving_volume_enabled =
            night_runners_non_driving_volume_enabled;
        state_.night_runners_non_driving_volume_percent =
            night_runners_non_driving_volume_percent > 100
                ? 100
                : night_runners_non_driving_volume_percent;
        state_.night_runners_dynamic_mode = night_runners_dynamic_mode;
        state_.night_runners_dynamic_threshold_mph =
            std::clamp<uint32_t>(night_runners_dynamic_threshold_mph, 5u, 250u);
        state_.night_runners_dynamic_buffer_percent =
            night_runners_dynamic_buffer_percent > 50
                ? 50
                : night_runners_dynamic_buffer_percent;
        state_.night_runners_dynamic_buffer_fill_seconds =
            std::isfinite(night_runners_dynamic_buffer_fill_seconds)
                ? std::clamp(night_runners_dynamic_buffer_fill_seconds,
                             0.5f, 15.0f)
                : 3.0f;
        state_.night_runners_dynamic_increase_seconds =
            std::isfinite(night_runners_dynamic_increase_seconds)
                ? std::clamp(night_runners_dynamic_increase_seconds,
                             0.2f, 15.0f)
                : 1.5f;
        state_.night_runners_dynamic_decrease_seconds =
            std::isfinite(night_runners_dynamic_decrease_seconds)
                ? std::clamp(night_runners_dynamic_decrease_seconds,
                             0.2f, 15.0f)
                : 2.5f;
        state_.night_runners_dynamic_max_decay_mph_s =
            std::min<uint32_t>(night_runners_dynamic_max_decay_mph_s, 30u);
        state_.radio_logo_album_art_enabled = radio_logo_album_art_enabled;
        state_.radio_logo_custom_graphic_enabled =
            radio_logo_custom_graphic_enabled;
        state_.radio_logo_spotify_variant =
            radio_logo_spotify_variant == "color" ? "color" : "white";
        state_.metadata_truncation_enabled = metadata_truncation_enabled;
        state_.metadata_truncation_length =
            std::clamp<uint32_t>(metadata_truncation_length, 10u, 50u);
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_ui_integrity(bool credit_verified) {
    {
        std::lock_guard lock(mtx_);
        state_.ui_credit_verified = credit_verified;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::set_track(const std::string& uri, const std::string& title,
                                 const std::string& artist, const std::string& album,
                                 uint32_t duration_ms) {
    {
        std::lock_guard lock(mtx_);
        // Push previous track onto recent if it changed and was non-empty.
        if (!state_.track_uri.empty() && state_.track_uri != uri) {
            state_.recent.push_front({ state_.track_title, state_.track_artist, now_ms() });
            while (state_.recent.size() > 5) state_.recent.pop_back();
        }
        state_.track_uri         = uri;
        state_.track_title       = title;
        state_.track_artist      = artist;
        state_.track_album       = album;
        state_.track_duration_ms = duration_ms;
    }
    rev_.fetch_add(1, std::memory_order_release);
}

void BridgeStateStore::push_recent(const std::string& title, const std::string& artist) {
    {
        std::lock_guard lock(mtx_);
        state_.recent.push_front({ title, artist, now_ms() });
        while (state_.recent.size() > 5) state_.recent.pop_back();
    }
    rev_.fetch_add(1, std::memory_order_release);
}

BridgeState BridgeStateStore::snapshot() const {
    std::lock_guard lock(mtx_);
    return state_;
}

uint64_t BridgeStateStore::revision() const {
    return rev_.load(std::memory_order_acquire);
}

std::string BridgeStateStore::snapshot_json() const {
    BridgeState s;
    {
        std::lock_guard lock(mtx_);
        s = state_;
    }
    auto errors = bridge::log::recent_errors(5);
    auto tail   = bridge::log::tail(50);

    std::string out;
    out.reserve(2048);
    out += "{";

    // game
    // `id` is the detected host title ("fh6"/"fh5"/"unknown"). The bridge runs
    // inside the game process, so active_profile() always reflects the running
    // game whenever the UI can reach us (i.e. whenever `online`). The web UI uses
    // it to pick the matching Forza Horizon logo.
    const char* game_id = "unknown";
    switch (active_profile().id) {
        case GameId::FH6: game_id = "fh6"; break;
        case GameId::FH5: game_id = "fh5"; break;
        default:          game_id = "unknown"; break;
    }
    out += "\"game\":{";
    out += "\"id\":";          append_json_string(out, game_id); out += ",";
    out += "\"attached\":";    append_bool(out, s.game_attached); out += ",";
    out += "\"module\":";      append_json_string(out, s.game_module); out += ",";
    out += "\"base\":";        append_hex_u64(out, s.game_base); out += ",";
    out += "\"injector_ready\":"; append_bool(out, s.injector_ready);
    out += "},";

    // audio
    out += "\"audio\":{";
    out += "\"active\":";      append_bool(out, s.audio_active); out += ",";
    out += "\"r10_active\":";  append_bool(out, s.r10_active); out += ",";
    out += "\"migrated\":";    append_bool(out, s.migrated_radio_bus); out += ",";
    out += "\"channel\":";     append_hex_u64(out, s.audio_channel); out += ",";
    out += "\"cg\":";          append_hex_u64(out, s.audio_cg); out += ",";
    out += "\"ring_avail\":";  append_u64(out, s.ring_available); out += ",";
    out += "\"output_gain\":";
    append_float(out, s.audio_output_gain);
    out += ",";
    out += "\"prebuffer\":";   append_bool(out, s.audio_prebuffer); out += ",";
    out += "\"local_hold\":";  append_bool(out, s.audio_local_hold); out += ",";
    out += "\"underruns\":";   append_u64(out, s.audio_underruns); out += ",";
    out += "\"native_dsp_calls\":"; append_u64(out, s.native_dsp_calls); out += ",";
    out += "\"native_dsp_len\":"; append_u32(out, s.native_dsp_len); out += ",";
    out += "\"native_dsp_in_ch\":"; append_i64(out, s.native_dsp_in_ch); out += ",";
    out += "\"native_dsp_out_ch\":"; append_i64(out, s.native_dsp_out_ch); out += ",";
    out += "\"native_channel_i\":"; append_hex_u64(out, s.native_channel_i); out += ",";
    out += "\"native_channel_vt\":"; append_hex_u64(out, s.native_channel_vt); out += ",";
    out += "\"native_channel_508_bits\":";
    append_hex_u64(out, s.native_channel_508_bits); out += ",";
    out += "\"native_channel_508\":";
    append_float_bits(out, s.native_channel_508_bits); out += ",";
    out += "\"native_channel_530\":";
    append_hex_u64(out, s.native_channel_530); out += ",";
    out += "\"native_channel_568_bits\":";
    append_hex_u64(out, s.native_channel_568_bits); out += ",";
    out += "\"native_channel_568\":";
    append_float_bits(out, s.native_channel_568_bits); out += ",";
    out += "\"native_send_state_1c0\":";
    append_hex_u64(out, s.native_send_state_1c0); out += ",";
    out += "\"native_send_tail_nonzero\":";
    append_u32(out, s.native_send_tail_nonzero); out += ",";
    out += "\"native_send_tail_hash\":";
    append_hex_u64(out, s.native_send_tail_hash);
    out += "},";

    // spotify
    out += "\"spotify\":{";
    out += "\"connected\":";   append_bool(out, s.spotify_connected); out += ",";
    out += "\"blob_cached\":"; append_bool(out, s.spotify_blob_cached); out += ",";
    out += "\"playing\":";     append_bool(out, s.spotify_playing); out += ",";
    out += "\"transferred_away\":"; append_bool(out, s.spotify_transferred_away); out += ",";
    out += "\"device_id\":";   append_json_string(out, s.spotify_device_id);
    out += "},";

    // AirPlay
    out += "\"airplay\":{";
    out += "\"available\":"; append_bool(out, s.airplay_available); out += ",";
    out += "\"running\":"; append_bool(out, s.airplay_running); out += ",";
    out += "\"connected\":"; append_bool(out, s.airplay_connected); out += ",";
    out += "\"playing\":"; append_bool(out, s.airplay_playing); out += ",";
    out += "\"deviceName\":"; append_json_string(out, s.airplay_device_name); out += ",";
    out += "\"volumeDb\":"; append_float(out, s.airplay_volume_db); out += ",";
    out += "\"volumePercent\":"; append_u32(out, s.airplay_volume_percent); out += ",";
    out += "\"error\":"; append_json_string(out, s.airplay_error);
    out += "},";

    // source manager
    out += "\"activeSource\":";
    append_json_string(out, s.active_source);
    out += ",";
    out += "\"sources\":{";
    if (s.sources.empty()) {
        out += "\"spotify\":{";
        out += "\"id\":\"spotify\",";
        out += "\"displayName\":\"Spotify\",";
        out += "\"available\":true,";
        out += "\"connected\":"; append_bool(out, s.spotify_connected); out += ",";
        out += "\"playing\":"; append_bool(out, s.spotify_playing); out += ",";
        out += "\"capabilities\":{";
        out += "\"pause\":true,\"resume\":true,\"restart\":true,\"next\":true";
        out += "}";
        out += "}";
        out += ",";
        out += "\"local\":{";
        out += "\"id\":\"local\",";
        out += "\"displayName\":\"Local Files\",";
        out += "\"available\":"; append_bool(out, s.local_available); out += ",";
        out += "\"connected\":"; append_bool(out, s.local_ready); out += ",";
        out += "\"playing\":"; append_bool(out, s.local_playing); out += ",";
        out += "\"capabilities\":{";
        out += "\"pause\":true,\"resume\":true,\"restart\":true,\"next\":true,";
        out += "\"previous\":true,\"seek\":true";
        out += "}";
        out += "}";
    } else {
        for (size_t i = 0; i < s.sources.size(); ++i) {
            const auto& src = s.sources[i];
            if (i) out += ",";
            append_json_string(out, src.id);
            out += ":{";
            out += "\"id\":"; append_json_string(out, src.id); out += ",";
            out += "\"displayName\":"; append_json_string(out, src.display_name); out += ",";
            out += "\"available\":"; append_bool(out, src.available); out += ",";
            out += "\"connected\":"; append_bool(out, src.connected); out += ",";
            out += "\"playing\":"; append_bool(out, src.playing); out += ",";
            out += "\"capabilities\":{";
            out += "\"pause\":"; append_bool(out, src.capabilities.pause); out += ",";
            out += "\"resume\":"; append_bool(out, src.capabilities.resume); out += ",";
            out += "\"restart\":"; append_bool(out, src.capabilities.restart); out += ",";
            out += "\"next\":"; append_bool(out, src.capabilities.next); out += ",";
            out += "\"previous\":"; append_bool(out, src.capabilities.previous); out += ",";
            out += "\"seek\":"; append_bool(out, src.capabilities.seek);
            out += "}";
            out += "}";
        }
    }
    out += "},";

    // local files
    out += "\"local\":{";
    out += "\"available\":"; append_bool(out, s.local_available); out += ",";
    out += "\"ready\":"; append_bool(out, s.local_ready); out += ",";
    out += "\"playing\":"; append_bool(out, s.local_playing); out += ",";
    out += "\"musicDir\":"; append_json_string(out, s.local_music_dir); out += ",";
    out += "\"defaultMusicDir\":"; append_json_string(out, s.local_default_music_dir); out += ",";
    out += "\"recursive\":"; append_bool(out, s.local_recursive); out += ",";
    out += "\"shuffle\":"; append_bool(out, s.local_shuffle); out += ",";
    out += "\"trackCount\":"; append_u32(out, s.local_track_count); out += ",";
    out += "\"unsupportedCount\":"; append_u32(out, s.local_unsupported_count); out += ",";
    out += "\"position_ms\":"; append_u32(out, s.local_position_ms); out += ",";
    out += "\"supportedFormats\":[\"mp3\",\"wav\",\"flac\"],";
    out += "\"error\":"; append_json_string(out, s.local_error);
    out += "},";

    // online radio
    out += "\"radio\":{";
    out += "\"available\":"; append_bool(out, s.radio_available); out += ",";
    out += "\"connected\":"; append_bool(out, s.radio_connected); out += ",";
    out += "\"playing\":"; append_bool(out, s.radio_playing); out += ",";
    out += "\"stationName\":"; append_json_string(out, s.radio_station_name); out += ",";
    out += "\"stationId\":"; append_json_string(out, s.radio_station_id); out += ",";
    out += "\"codec\":"; append_json_string(out, s.radio_codec); out += ",";
    out += "\"bitrate\":"; append_u32(out, s.radio_bitrate); out += ",";
    out += "\"error\":"; append_json_string(out, s.radio_error);
    out += "},";

    // driving
    out += "\"driving\":{";
    out += "\"speedAvailable\":"; append_bool(out, s.driving_speed_available); out += ",";
    out += "\"speedMps\":"; append_float(out, s.driving_speed_mps); out += ",";
    out += "\"nightRunnersVolumeFactor\":";
    append_float(out, s.night_runners_volume_factor); out += ",";
    out += "\"nightRunnersLazyCooldown\":";
    append_float(out, s.night_runners_lazy_cooldown); out += ",";
    out += "\"nightRunnersSpeedProgress\":";
    append_float(out, s.night_runners_speed_progress); out += ",";
    out += "\"nightRunnersPeakMps\":";
    append_float(out, s.night_runners_dynamic_peak_mps); out += ",";
    out += "\"nightRunnersBufferMps\":";
    append_float(out, s.night_runners_dynamic_buffer_mps); out += ",";
    out += "\"nightRunnersDriveState\":";
    append_u32(out, s.night_runners_dynamic_state); out += ",";
    out += "\"nonFreeroamVolumeActive\":";
    append_bool(out, s.night_runners_non_driving_volume_active);
    out += "},";

    // options
    out += "\"options\":{";
    out += "\"activeSource\":"; append_json_string(out, s.active_source); out += ",";
    out += "\"locale\":"; append_json_string(out, s.locale); out += ",";
    out += "\"menuPlayback\":"; append_json_string(out, s.menu_playback); out += ",";
    out += "\"raceStartPlayback\":"; append_json_string(out, s.race_start); out += ",";
    out += "\"raceStartRestartThresholdSeconds\":"; append_u32(out, s.race_start_restart_threshold_s); out += ",";
    out += "\"songStartOffsetEnabled\":"; append_bool(out, s.song_start_offset_enabled); out += ",";
    out += "\"songStartOffsetSeconds\":"; append_u32(out, s.song_start_offset_seconds); out += ",";
    out += "\"volumeNormalization\":"; append_json_string(out, s.volume_normalization); out += ",";
    out += "\"quickStationSkip\":"; append_bool(out, s.quick_station_skip); out += ",";
    out += "\"equalizerEnabled\":"; append_bool(out, s.equalizer_enabled); out += ",";
    out += "\"equalizerBands\":[";
    for (size_t i = 0; i < s.equalizer_bands_db.size(); ++i) {
        if (i) out += ",";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", s.equalizer_bands_db[i]);
        out += buf;
    }
    out += "],";
    out += "\"localMusicDir\":"; append_json_string(out, s.local_music_dir); out += ",";
    out += "\"localRecursive\":"; append_bool(out, s.local_recursive); out += ",";
    out += "\"localShuffle\":"; append_bool(out, s.local_shuffle); out += ",";
    out += "\"localVolume\":"; append_u32(out, s.local_volume_percent); out += ",";
    out += "\"localTitleMetadataMode\":";
    append_json_string(out, s.local_title_metadata_mode); out += ",";
    out += "\"localArtistMetadataMode\":";
    append_json_string(out, s.local_artist_metadata_mode); out += ",";
    out += "\"nightRunnersMode\":"; append_bool(out, s.night_runners_mode); out += ",";
    out += "\"nightRunnersStoppedVolumeDecrease\":";
    append_u32(out, s.night_runners_stopped_volume_decrease_percent); out += ",";
    out += "\"nightRunnersMaxSpeedMph\":";
    append_u32(out, s.night_runners_max_speed_mph); out += ",";
    out += "\"nightRunnersSpeedUnit\":";
    append_json_string(out, s.night_runners_speed_unit); out += ",";
    out += "\"nightRunnersCurveEnabled\":";
    append_bool(out, s.night_runners_curve_enabled); out += ",";
    out += "\"nightRunnersCurveExponent\":";
    append_float(out, s.night_runners_curve_exponent); out += ",";
    out += "\"nightRunnersLazyVolumeEnabled\":";
    append_bool(out, s.night_runners_lazy_volume_enabled); out += ",";
    out += "\"nightRunnersLazyHoldSeconds\":";
    append_float(out, s.night_runners_lazy_hold_seconds); out += ",";
    out += "\"nightRunnersLowCutEnabled\":";
    append_bool(out, s.night_runners_low_cut_enabled); out += ",";
    out += "\"nightRunnersFrequencyCutMode\":";
    append_json_string(out, s.night_runners_frequency_cut_mode); out += ",";
    out += "\"nightRunnersLowCutAmount\":";
    append_u32(out, s.night_runners_low_cut_amount_percent); out += ",";
    out += "\"nightRunnersLowCutFrequencyHz\":";
    append_u32(out, s.night_runners_low_cut_frequency_hz); out += ",";
    out += "\"nightRunnersNonDrivingVolumeEnabled\":";
    append_bool(out, s.night_runners_non_driving_volume_enabled); out += ",";
    out += "\"nightRunnersNonDrivingVolume\":";
    append_u32(out, s.night_runners_non_driving_volume_percent); out += ",";
    out += "\"nightRunnersDynamicMode\":";
    append_bool(out, s.night_runners_dynamic_mode); out += ",";
    out += "\"nightRunnersDynamicThresholdMph\":";
    append_u32(out, s.night_runners_dynamic_threshold_mph); out += ",";
    out += "\"nightRunnersDynamicBufferPercent\":";
    append_u32(out, s.night_runners_dynamic_buffer_percent); out += ",";
    out += "\"nightRunnersDynamicBufferFillSeconds\":";
    append_float(out, s.night_runners_dynamic_buffer_fill_seconds); out += ",";
    out += "\"nightRunnersDynamicIncreaseSeconds\":";
    append_float(out, s.night_runners_dynamic_increase_seconds); out += ",";
    out += "\"nightRunnersDynamicDecreaseSeconds\":";
    append_float(out, s.night_runners_dynamic_decrease_seconds); out += ",";
    out += "\"nightRunnersDynamicMaxDecayMphS\":";
    append_u32(out, s.night_runners_dynamic_max_decay_mph_s); out += ",";
    out += "\"radioLogoAlbumArtEnabled\":";
    append_bool(out, s.radio_logo_album_art_enabled); out += ",";
    out += "\"radioLogoCustomGraphicEnabled\":";
    append_bool(out, s.radio_logo_custom_graphic_enabled); out += ",";
    out += "\"radioLogoSpotifyVariant\":";
    append_json_string(out, s.radio_logo_spotify_variant); out += ",";
    out += "\"metadataTruncationEnabled\":";
    append_bool(out, s.metadata_truncation_enabled); out += ",";
    out += "\"metadataTruncationLength\":";
    append_u32(out, s.metadata_truncation_length);
    out += "},";

    // packaged UI integrity
    out += "\"ui\":{";
    out += "\"credit_verified\":"; append_bool(out, s.ui_credit_verified);
    out += "},";

    // track
    out += "\"track\":{";
    out += "\"uri\":";         append_json_string(out, s.track_uri); out += ",";
    out += "\"title\":";       append_json_string(out, s.track_title); out += ",";
    out += "\"artist\":";      append_json_string(out, s.track_artist); out += ",";
    out += "\"album\":";       append_json_string(out, s.track_album); out += ",";
    out += "\"duration_ms\":"; append_u32(out, s.track_duration_ms);
    out += "},";

    // recent
    out += "\"recent\":[";
    for (size_t i = 0; i < s.recent.size(); ++i) {
        if (i) out += ",";
        out += "{\"title\":";   append_json_string(out, s.recent[i].title);
        out += ",\"artist\":";  append_json_string(out, s.recent[i].artist);
        out += ",\"at_ms\":";   append_i64(out, s.recent[i].at_ms);
        out += "}";
    }
    out += "],";

    // errors
    out += "\"errors\":[";
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) out += ",";
        append_json_string(out, errors[i]);
    }
    out += "],";

    // log_tail
    out += "\"log_tail\":[";
    for (size_t i = 0; i < tail.size(); ++i) {
        if (i) out += ",";
        append_json_string(out, tail[i]);
    }
    out += "]";

    out += "}";
    return out;
}

} // namespace bridge
