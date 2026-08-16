#include "options.h"

#include "log_file.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace bridge {

namespace {

constexpr const char* kDefaultMenuPlayback = "pause";
constexpr const char* kDefaultRaceStart = "next";
constexpr uint32_t kDefaultRaceStartRestartThresholdSeconds = 20;
constexpr uint32_t kMinRaceStartRestartThresholdSeconds = 5;
constexpr uint32_t kMaxRaceStartRestartThresholdSeconds = 60;
constexpr uint32_t kDefaultSongStartOffsetSeconds = 30;
constexpr uint32_t kMinSongStartOffsetSeconds = 3;
constexpr uint32_t kMaxSongStartOffsetSeconds = 240;
constexpr const char* kDefaultVolumeNormalization = "on";
constexpr const char* kDefaultActiveSource = "spotify";
constexpr const char* kDefaultLocale = "en";
constexpr const char* kDefaultRadioLogoSpotifyVariant = "white";
constexpr float kMinEqualizerDb = -6.0f;
constexpr float kMaxEqualizerDb = 6.0f;
constexpr uint32_t kDefaultLocalVolumePercent = 100;
constexpr uint32_t kMaxLocalVolumePercent = 300;
constexpr const char* kDefaultLocalTitleMetadataMode = "metadata";
constexpr const char* kDefaultLocalArtistMetadataMode = "albumArtist";
constexpr uint32_t kDefaultNightRunnersStoppedVolumeDecreasePercent = 50;
constexpr uint32_t kMaxNightRunnersStoppedVolumeDecreasePercent = 100;
constexpr uint32_t kDefaultNightRunnersMaxSpeedMph = 120;
constexpr uint32_t kMinNightRunnersMaxSpeedMph = 10;
constexpr uint32_t kMaxNightRunnersMaxSpeedMph = 250;
constexpr const char* kDefaultNightRunnersSpeedUnit = "mph";
constexpr float kDefaultNightRunnersCurveExponent = 2.0f;
constexpr float kMinNightRunnersCurveExponent = 1.0f;
constexpr float kMaxNightRunnersCurveExponent = 4.0f;
constexpr float kDefaultNightRunnersLazyHoldSeconds = 2.0f;
constexpr float kMinNightRunnersLazyHoldSeconds = 0.5f;
constexpr float kMaxNightRunnersLazyHoldSeconds = 10.0f;
constexpr const char* kDefaultNightRunnersFrequencyCutMode = "low";
constexpr uint32_t kDefaultNightRunnersLowCutAmountPercent = 50;
constexpr uint32_t kMaxNightRunnersLowCutAmountPercent = 100;
constexpr uint32_t kDefaultNightRunnersLowCutFrequencyHz = 140;
constexpr uint32_t kMinNightRunnersLowCutFrequencyHz = 40;
constexpr uint32_t kMaxNightRunnersLowEndCutFrequencyHz = 320;
constexpr uint32_t kDefaultNightRunnersHighCutFrequencyHz = 5000;
constexpr uint32_t kMinNightRunnersHighCutFrequencyHz = 1200;
constexpr uint32_t kMaxNightRunnersHighCutFrequencyHz = 12000;
constexpr uint32_t kMaxNightRunnersLowCutFrequencyHz =
    kMaxNightRunnersHighCutFrequencyHz;
constexpr uint32_t kDefaultNightRunnersNonDrivingVolumePercent = 50;
constexpr uint32_t kMaxNightRunnersNonDrivingVolumePercent = 100;
constexpr uint32_t kDefaultNightRunnersDynamicThresholdMph = 40;
constexpr uint32_t kMinNightRunnersDynamicThresholdMph = 5;
constexpr uint32_t kMaxNightRunnersDynamicThresholdMph = 250;
constexpr uint32_t kDefaultNightRunnersDynamicBufferPercent = 15;
constexpr uint32_t kMaxNightRunnersDynamicBufferPercent = 50;
constexpr float kDefaultNightRunnersDynamicBufferFillSeconds = 3.0f;
constexpr float kMinNightRunnersDynamicBufferFillSeconds = 0.5f;
constexpr float kMaxNightRunnersDynamicBufferFillSeconds = 15.0f;
constexpr float kDefaultNightRunnersDynamicIncreaseSeconds = 1.5f;
constexpr float kMinNightRunnersDynamicRampSeconds = 0.2f;
constexpr float kMaxNightRunnersDynamicRampSeconds = 15.0f;
constexpr float kDefaultNightRunnersDynamicDecreaseSeconds = 2.5f;
constexpr uint32_t kDefaultNightRunnersDynamicMaxDecayMphS = 0;
constexpr uint32_t kMaxNightRunnersDynamicMaxDecayMphS = 30;
constexpr uint32_t kDefaultMetadataTruncationLength = 30;
constexpr uint32_t kMinMetadataTruncationLength = 10;
constexpr uint32_t kMaxMetadataTruncationLength = 50;

float sanitize_eq_band(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return std::clamp(value, kMinEqualizerDb, kMaxEqualizerDb);
}

float sanitize_night_runners_curve_exponent(float value) {
    if (!std::isfinite(value)) return kDefaultNightRunnersCurveExponent;
    return std::clamp(value, kMinNightRunnersCurveExponent,
                      kMaxNightRunnersCurveExponent);
}

float sanitize_night_runners_lazy_hold_seconds(float value) {
    if (!std::isfinite(value)) return kDefaultNightRunnersLazyHoldSeconds;
    return std::clamp(value, kMinNightRunnersLazyHoldSeconds,
                      kMaxNightRunnersLazyHoldSeconds);
}

uint32_t default_night_runners_frequency_cut_hz(std::string_view mode) {
    return mode == "high"
        ? kDefaultNightRunnersHighCutFrequencyHz
        : kDefaultNightRunnersLowCutFrequencyHz;
}

uint32_t sanitize_night_runners_frequency_cut_hz(uint32_t value,
                                                 std::string_view mode) {
    if (mode == "high") {
        return std::clamp(value,
                          kMinNightRunnersHighCutFrequencyHz,
                          kMaxNightRunnersHighCutFrequencyHz);
    }
    return std::clamp(value,
                      kMinNightRunnersLowCutFrequencyHz,
                      kMaxNightRunnersLowEndCutFrequencyHz);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool atomic_replace_file(const std::filesystem::path& target,
                         const std::string& content,
                         std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create options directory";
        return false;
    }

    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "could not open temporary options file";
            return false;
        }
        out << content;
        if (!out) {
            if (error) *error = "could not write temporary options file";
            return false;
        }
    }

    if (!::MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not replace options file";
        return false;
    }
    return true;
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

void append_json_string(std::ostringstream& out, std::string_view value) {
    out << '"';
    for (char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u00";
                    static constexpr char hex[] = "0123456789abcdef";
                    unsigned char u = static_cast<unsigned char>(c);
                    out << hex[(u >> 4) & 0xf] << hex[u & 0xf];
                } else {
                    out << c;
                }
        }
    }
    out << '"';
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

bool extract_json_float(std::string_view json, std::string_view key, float& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    char* end = nullptr;
    std::string rest(json.substr(pos));
    float parsed = std::strtof(rest.c_str(), &end);
    if (!end || end == rest.c_str()) return false;
    out = parsed;
    return true;
}

bool extract_json_float_array(std::string_view json,
                              std::string_view key,
                              std::array<float, 5>& out) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t key_pos = json.find(std::string_view(needle.data(), needle.size()));
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find('[', colon + 1);
    if (pos == std::string_view::npos) return false;
    ++pos;

    std::array<float, 5> parsed{};
    for (size_t i = 0; i < parsed.size(); ++i) {
        pos = json.find_first_not_of(" \t\r\n", pos);
        if (pos == std::string_view::npos) return false;
        std::string rest(json.substr(pos));
        char* end = nullptr;
        float value = std::strtof(rest.c_str(), &end);
        if (!end || end == rest.c_str()) return false;
        parsed[i] = sanitize_eq_band(value);
        pos += static_cast<size_t>(end - rest.c_str());
        pos = json.find_first_not_of(" \t\r\n", pos);
        if (i + 1 < parsed.size()) {
            if (pos == std::string_view::npos || json[pos] != ',') return false;
            ++pos;
        }
    }
    pos = json.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string_view::npos || json[pos] != ']') return false;
    out = parsed;
    return true;
}

} // namespace

bool is_valid_menu_playback(std::string_view value) {
    return value == "pause" || value == "silent";
}

bool is_valid_race_start(std::string_view value) {
    return value == "restart" || value == "next" || value == "ignore" ||
           value == "smart";
}

bool is_valid_volume_normalization(std::string_view value) {
    return value == "on" || value == "off";
}

bool is_valid_active_source(std::string_view value) {
    return value == "spotify" || value == "airplay" || value == "local" ||
           value == "radio" || value == "vanilla";
}

bool is_valid_night_runners_speed_unit(std::string_view value) {
    return value == "mph" || value == "kmh";
}

bool is_valid_night_runners_frequency_cut_mode(std::string_view value) {
    return value == "low" || value == "high";
}

bool is_valid_locale(std::string_view value) {
    if (value.empty() || value.size() > 48) return false;
    for (char c : value) {
        bool ok = (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_valid_radio_logo_spotify_variant(std::string_view value) {
    return value == "white" || value == "color";
}

bool is_valid_local_title_metadata_mode(std::string_view value) {
    return value == "metadata" || value == "filename";
}

bool is_valid_local_artist_metadata_mode(std::string_view value) {
    return value == "albumArtist" || value == "folder" || value == "album";
}

uint32_t sanitize_metadata_truncation_length(uint32_t value) {
    return std::clamp(value,
                      kMinMetadataTruncationLength,
                      kMaxMetadataTruncationLength);
}

std::string truncate_metadata_text(std::string_view value, bool enabled,
                                   uint32_t max_chars) {
    if (!enabled) return std::string(value);
    const uint32_t limit = sanitize_metadata_truncation_length(max_chars);
    uint32_t chars = 0;
    size_t cut = value.size();
    bool needs_truncation = false;
    for (size_t i = 0; i < value.size();) {
        if (chars >= limit) {
            cut = i;
            needs_truncation = true;
            break;
        }
        const auto c = static_cast<unsigned char>(value[i]);
        size_t advance = 1;
        if ((c & 0x80) == 0) {
            advance = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < value.size()) {
            advance = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < value.size()) {
            advance = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < value.size()) {
            advance = 4;
        }
        i += advance;
        ++chars;
    }
    if (!needs_truncation) return std::string(value);
    std::string out(value.substr(0, cut));
    out += "...";
    return out;
}

bool parse_options_json(std::string_view json, PlaybackOptions& out) {
    std::string active_source;
    if (extract_json_string(json, "activeSource", active_source)) {
        if (!is_valid_active_source(active_source)) return false;
        out.active_source = active_source;
    } else {
        out.active_source = kDefaultActiveSource;
    }

    std::string locale;
    if (extract_json_string(json, "locale", locale)) {
        if (!is_valid_locale(locale)) return false;
        out.locale = locale;
    } else if (has_json_key(json, "locale")) {
        return false;
    } else {
        out.locale = kDefaultLocale;
    }

    std::string menu_playback;
    if (!extract_json_string(json, "menuPlayback", menu_playback)) return false;
    if (!is_valid_menu_playback(menu_playback)) return false;
    out.menu_playback = menu_playback;

    std::string race_start;
    if (extract_json_string(json, "raceStartPlayback", race_start)) {
        if (!is_valid_race_start(race_start)) return false;
        out.race_start = race_start;
    } else {
        out.race_start = kDefaultRaceStart;
    }

    uint32_t race_restart_threshold = kDefaultRaceStartRestartThresholdSeconds;
    if (extract_json_u32(json, "raceStartRestartThresholdSeconds",
                         race_restart_threshold)) {
        if (race_restart_threshold < kMinRaceStartRestartThresholdSeconds ||
            race_restart_threshold > kMaxRaceStartRestartThresholdSeconds) {
            return false;
        }
        out.race_start_restart_threshold_s = race_restart_threshold;
    } else if (has_json_key(json, "raceStartRestartThresholdSeconds")) {
        return false;
    } else {
        out.race_start_restart_threshold_s =
            kDefaultRaceStartRestartThresholdSeconds;
    }

    bool song_start_offset_enabled = false;
    if (extract_json_bool(json, "songStartOffsetEnabled",
                          song_start_offset_enabled)) {
        out.song_start_offset_enabled = song_start_offset_enabled;
    } else if (has_json_key(json, "songStartOffsetEnabled")) {
        return false;
    } else {
        out.song_start_offset_enabled = false;
    }

    uint32_t song_start_offset = kDefaultSongStartOffsetSeconds;
    if (extract_json_u32(json, "songStartOffsetSeconds", song_start_offset)) {
        if (song_start_offset < kMinSongStartOffsetSeconds ||
            song_start_offset > kMaxSongStartOffsetSeconds) {
            return false;
        }
        out.song_start_offset_seconds = song_start_offset;
    } else if (has_json_key(json, "songStartOffsetSeconds")) {
        return false;
    } else {
        out.song_start_offset_seconds = kDefaultSongStartOffsetSeconds;
    }

    std::string volume_normalization;
    if (extract_json_string(json, "volumeNormalization", volume_normalization)) {
        if (!is_valid_volume_normalization(volume_normalization)) return false;
        out.volume_normalization = volume_normalization;
    } else {
        out.volume_normalization = kDefaultVolumeNormalization;
    }

    bool equalizer_enabled = false;
    if (extract_json_bool(json, "equalizerEnabled", equalizer_enabled)) {
        out.equalizer_enabled = equalizer_enabled;
    } else if (has_json_key(json, "equalizerEnabled")) {
        return false;
    } else {
        out.equalizer_enabled = false;
    }

    bool quick_station_skip = false;
    if (extract_json_bool(json, "quickStationSkip", quick_station_skip)) {
        out.quick_station_skip = quick_station_skip;
    } else if (has_json_key(json, "quickStationSkip")) {
        return false;
    } else {
        out.quick_station_skip = false;
    }

    std::array<float, 5> equalizer_bands{};
    if (extract_json_float_array(json, "equalizerBands", equalizer_bands)) {
        out.equalizer_bands_db = equalizer_bands;
    } else if (has_json_key(json, "equalizerBands")) {
        return false;
    } else {
        out.equalizer_bands_db = {};
    }

    std::string local_music_dir;
    if (extract_json_string(json, "localMusicDir", local_music_dir)) {
        out.local_music_dir = local_music_dir;
    } else if (has_json_key(json, "localMusicDir")) {
        return false;
    } else {
        out.local_music_dir = {};
    }

    bool local_recursive = true;
    if (extract_json_bool(json, "localRecursive", local_recursive)) {
        out.local_recursive = local_recursive;
    } else if (has_json_key(json, "localRecursive")) {
        return false;
    } else {
        out.local_recursive = true;
    }

    bool local_shuffle = true;
    if (extract_json_bool(json, "localShuffle", local_shuffle)) {
        out.local_shuffle = local_shuffle;
    } else if (has_json_key(json, "localShuffle")) {
        return false;
    } else {
        out.local_shuffle = true;
    }

    uint32_t local_volume = kDefaultLocalVolumePercent;
    if (extract_json_u32(json, "localVolume", local_volume)) {
        if (local_volume > kMaxLocalVolumePercent) return false;
        out.local_volume_percent = local_volume;
    } else if (has_json_key(json, "localVolume")) {
        return false;
    } else {
        out.local_volume_percent = kDefaultLocalVolumePercent;
    }

    std::string local_title_metadata_mode;
    if (extract_json_string(json, "localTitleMetadataMode",
                            local_title_metadata_mode)) {
        if (!is_valid_local_title_metadata_mode(local_title_metadata_mode)) {
            return false;
        }
        out.local_title_metadata_mode = local_title_metadata_mode;
    } else if (has_json_key(json, "localTitleMetadataMode")) {
        return false;
    } else {
        out.local_title_metadata_mode = kDefaultLocalTitleMetadataMode;
    }

    std::string local_artist_metadata_mode;
    if (extract_json_string(json, "localArtistMetadataMode",
                            local_artist_metadata_mode)) {
        if (!is_valid_local_artist_metadata_mode(local_artist_metadata_mode)) {
            return false;
        }
        out.local_artist_metadata_mode = local_artist_metadata_mode;
    } else if (has_json_key(json, "localArtistMetadataMode")) {
        return false;
    } else {
        out.local_artist_metadata_mode = kDefaultLocalArtistMetadataMode;
    }

    bool night_runners_mode = false;
    if (extract_json_bool(json, "nightRunnersMode", night_runners_mode)) {
        out.night_runners_mode = night_runners_mode;
    } else if (has_json_key(json, "nightRunnersMode")) {
        return false;
    } else {
        out.night_runners_mode = false;
    }

    uint32_t stopped_decrease = kDefaultNightRunnersStoppedVolumeDecreasePercent;
    if (extract_json_u32(json, "nightRunnersStoppedVolumeDecrease", stopped_decrease)) {
        if (stopped_decrease > kMaxNightRunnersStoppedVolumeDecreasePercent) return false;
        out.night_runners_stopped_volume_decrease_percent = stopped_decrease;
    } else if (has_json_key(json, "nightRunnersStoppedVolumeDecrease")) {
        return false;
    } else {
        out.night_runners_stopped_volume_decrease_percent =
            kDefaultNightRunnersStoppedVolumeDecreasePercent;
    }

    uint32_t max_speed_mph = kDefaultNightRunnersMaxSpeedMph;
    if (extract_json_u32(json, "nightRunnersMaxSpeedMph", max_speed_mph)) {
        if (max_speed_mph < kMinNightRunnersMaxSpeedMph ||
            max_speed_mph > kMaxNightRunnersMaxSpeedMph) {
            return false;
        }
        out.night_runners_max_speed_mph = max_speed_mph;
    } else if (has_json_key(json, "nightRunnersMaxSpeedMph")) {
        return false;
    } else {
        out.night_runners_max_speed_mph = kDefaultNightRunnersMaxSpeedMph;
    }

    std::string speed_unit;
    if (extract_json_string(json, "nightRunnersSpeedUnit", speed_unit)) {
        if (!is_valid_night_runners_speed_unit(speed_unit)) return false;
        out.night_runners_speed_unit = speed_unit;
    } else if (has_json_key(json, "nightRunnersSpeedUnit")) {
        return false;
    } else {
        out.night_runners_speed_unit = kDefaultNightRunnersSpeedUnit;
    }

    bool curve_enabled = false;
    if (extract_json_bool(json, "nightRunnersCurveEnabled", curve_enabled)) {
        out.night_runners_curve_enabled = curve_enabled;
    } else if (has_json_key(json, "nightRunnersCurveEnabled")) {
        return false;
    } else {
        out.night_runners_curve_enabled = false;
    }

    float curve_exponent = kDefaultNightRunnersCurveExponent;
    if (extract_json_float(json, "nightRunnersCurveExponent", curve_exponent)) {
        if (!std::isfinite(curve_exponent) ||
            curve_exponent < kMinNightRunnersCurveExponent ||
            curve_exponent > kMaxNightRunnersCurveExponent) {
            return false;
        }
        out.night_runners_curve_exponent = curve_exponent;
    } else if (has_json_key(json, "nightRunnersCurveExponent")) {
        return false;
    } else {
        out.night_runners_curve_exponent = kDefaultNightRunnersCurveExponent;
    }

    bool lazy_volume_enabled = false;
    if (extract_json_bool(json, "nightRunnersLazyVolumeEnabled",
                          lazy_volume_enabled)) {
        out.night_runners_lazy_volume_enabled = lazy_volume_enabled;
    } else if (has_json_key(json, "nightRunnersLazyVolumeEnabled")) {
        return false;
    } else {
        out.night_runners_lazy_volume_enabled = false;
    }

    float lazy_hold_seconds = kDefaultNightRunnersLazyHoldSeconds;
    if (extract_json_float(json, "nightRunnersLazyHoldSeconds",
                           lazy_hold_seconds)) {
        if (!std::isfinite(lazy_hold_seconds) ||
            lazy_hold_seconds < kMinNightRunnersLazyHoldSeconds ||
            lazy_hold_seconds > kMaxNightRunnersLazyHoldSeconds) {
            return false;
        }
        out.night_runners_lazy_hold_seconds = lazy_hold_seconds;
    } else if (has_json_key(json, "nightRunnersLazyHoldSeconds")) {
        return false;
    } else {
        out.night_runners_lazy_hold_seconds =
            kDefaultNightRunnersLazyHoldSeconds;
    }

    bool low_cut_enabled = false;
    if (extract_json_bool(json, "nightRunnersLowCutEnabled", low_cut_enabled)) {
        out.night_runners_low_cut_enabled = low_cut_enabled;
    } else if (has_json_key(json, "nightRunnersLowCutEnabled")) {
        return false;
    } else {
        out.night_runners_low_cut_enabled = false;
    }

    std::string frequency_cut_mode;
    if (extract_json_string(json, "nightRunnersFrequencyCutMode",
                            frequency_cut_mode)) {
        if (!is_valid_night_runners_frequency_cut_mode(frequency_cut_mode)) {
            return false;
        }
        out.night_runners_frequency_cut_mode = frequency_cut_mode;
    } else if (has_json_key(json, "nightRunnersFrequencyCutMode")) {
        return false;
    } else {
        out.night_runners_frequency_cut_mode =
            kDefaultNightRunnersFrequencyCutMode;
    }

    uint32_t low_cut_amount = kDefaultNightRunnersLowCutAmountPercent;
    if (extract_json_u32(json, "nightRunnersLowCutAmount", low_cut_amount)) {
        if (low_cut_amount > kMaxNightRunnersLowCutAmountPercent) return false;
        out.night_runners_low_cut_amount_percent = low_cut_amount;
    } else if (has_json_key(json, "nightRunnersLowCutAmount")) {
        return false;
    } else {
        out.night_runners_low_cut_amount_percent =
            kDefaultNightRunnersLowCutAmountPercent;
    }

    uint32_t low_cut_frequency =
        default_night_runners_frequency_cut_hz(
            out.night_runners_frequency_cut_mode);
    if (extract_json_u32(json, "nightRunnersLowCutFrequencyHz",
                         low_cut_frequency)) {
        if (low_cut_frequency < kMinNightRunnersLowCutFrequencyHz ||
            low_cut_frequency > kMaxNightRunnersLowCutFrequencyHz) {
            return false;
        }
        out.night_runners_low_cut_frequency_hz =
            sanitize_night_runners_frequency_cut_hz(
                low_cut_frequency,
                out.night_runners_frequency_cut_mode);
    } else if (has_json_key(json, "nightRunnersLowCutFrequencyHz")) {
        return false;
    } else {
        out.night_runners_low_cut_frequency_hz =
            low_cut_frequency;
    }

    bool non_driving_enabled = true;
    if (extract_json_bool(json, "nightRunnersNonDrivingVolumeEnabled",
                          non_driving_enabled)) {
        out.night_runners_non_driving_volume_enabled = non_driving_enabled;
    } else if (has_json_key(json, "nightRunnersNonDrivingVolumeEnabled")) {
        return false;
    } else {
        out.night_runners_non_driving_volume_enabled = true;
    }

    uint32_t non_driving_volume = kDefaultNightRunnersNonDrivingVolumePercent;
    if (extract_json_u32(json, "nightRunnersNonDrivingVolume",
                         non_driving_volume)) {
        if (non_driving_volume > kMaxNightRunnersNonDrivingVolumePercent) {
            return false;
        }
        out.night_runners_non_driving_volume_percent = non_driving_volume;
    } else if (has_json_key(json, "nightRunnersNonDrivingVolume")) {
        return false;
    } else {
        out.night_runners_non_driving_volume_percent =
            kDefaultNightRunnersNonDrivingVolumePercent;
    }

    bool dynamic_mode = false;
    if (extract_json_bool(json, "nightRunnersDynamicMode", dynamic_mode)) {
        out.night_runners_dynamic_mode = dynamic_mode;
    } else if (has_json_key(json, "nightRunnersDynamicMode")) {
        return false;
    } else {
        out.night_runners_dynamic_mode = false;
    }

    uint32_t dynamic_threshold = kDefaultNightRunnersDynamicThresholdMph;
    if (extract_json_u32(json, "nightRunnersDynamicThresholdMph",
                         dynamic_threshold)) {
        if (dynamic_threshold < kMinNightRunnersDynamicThresholdMph ||
            dynamic_threshold > kMaxNightRunnersDynamicThresholdMph) {
            return false;
        }
        out.night_runners_dynamic_threshold_mph = dynamic_threshold;
    } else if (has_json_key(json, "nightRunnersDynamicThresholdMph")) {
        return false;
    } else {
        out.night_runners_dynamic_threshold_mph =
            kDefaultNightRunnersDynamicThresholdMph;
    }

    uint32_t dynamic_buffer = kDefaultNightRunnersDynamicBufferPercent;
    if (extract_json_u32(json, "nightRunnersDynamicBufferPercent",
                         dynamic_buffer)) {
        if (dynamic_buffer > kMaxNightRunnersDynamicBufferPercent) return false;
        out.night_runners_dynamic_buffer_percent = dynamic_buffer;
    } else if (has_json_key(json, "nightRunnersDynamicBufferPercent")) {
        return false;
    } else {
        out.night_runners_dynamic_buffer_percent =
            kDefaultNightRunnersDynamicBufferPercent;
    }

    float dynamic_buffer_fill = kDefaultNightRunnersDynamicBufferFillSeconds;
    if (extract_json_float(json, "nightRunnersDynamicBufferFillSeconds",
                           dynamic_buffer_fill)) {
        if (!std::isfinite(dynamic_buffer_fill) ||
            dynamic_buffer_fill < kMinNightRunnersDynamicBufferFillSeconds ||
            dynamic_buffer_fill > kMaxNightRunnersDynamicBufferFillSeconds) {
            return false;
        }
        out.night_runners_dynamic_buffer_fill_seconds = dynamic_buffer_fill;
    } else if (has_json_key(json, "nightRunnersDynamicBufferFillSeconds")) {
        return false;
    } else {
        out.night_runners_dynamic_buffer_fill_seconds =
            kDefaultNightRunnersDynamicBufferFillSeconds;
    }

    float dynamic_increase = kDefaultNightRunnersDynamicIncreaseSeconds;
    if (extract_json_float(json, "nightRunnersDynamicIncreaseSeconds",
                           dynamic_increase)) {
        if (!std::isfinite(dynamic_increase) ||
            dynamic_increase < kMinNightRunnersDynamicRampSeconds ||
            dynamic_increase > kMaxNightRunnersDynamicRampSeconds) {
            return false;
        }
        out.night_runners_dynamic_increase_seconds = dynamic_increase;
    } else if (has_json_key(json, "nightRunnersDynamicIncreaseSeconds")) {
        return false;
    } else {
        out.night_runners_dynamic_increase_seconds =
            kDefaultNightRunnersDynamicIncreaseSeconds;
    }

    float dynamic_decrease = kDefaultNightRunnersDynamicDecreaseSeconds;
    if (extract_json_float(json, "nightRunnersDynamicDecreaseSeconds",
                           dynamic_decrease)) {
        if (!std::isfinite(dynamic_decrease) ||
            dynamic_decrease < kMinNightRunnersDynamicRampSeconds ||
            dynamic_decrease > kMaxNightRunnersDynamicRampSeconds) {
            return false;
        }
        out.night_runners_dynamic_decrease_seconds = dynamic_decrease;
    } else if (has_json_key(json, "nightRunnersDynamicDecreaseSeconds")) {
        return false;
    } else {
        out.night_runners_dynamic_decrease_seconds =
            kDefaultNightRunnersDynamicDecreaseSeconds;
    }

    uint32_t dynamic_max_decay = kDefaultNightRunnersDynamicMaxDecayMphS;
    if (extract_json_u32(json, "nightRunnersDynamicMaxDecayMphS",
                         dynamic_max_decay)) {
        if (dynamic_max_decay > kMaxNightRunnersDynamicMaxDecayMphS) return false;
        out.night_runners_dynamic_max_decay_mph_s = dynamic_max_decay;
    } else if (has_json_key(json, "nightRunnersDynamicMaxDecayMphS")) {
        return false;
    } else {
        out.night_runners_dynamic_max_decay_mph_s =
            kDefaultNightRunnersDynamicMaxDecayMphS;
    }

    bool radio_logo_album = true;
    if (extract_json_bool(json, "radioLogoAlbumArtEnabled", radio_logo_album)) {
        out.radio_logo_album_art_enabled = radio_logo_album;
    } else if (has_json_key(json, "radioLogoAlbumArtEnabled")) {
        return false;
    } else {
        out.radio_logo_album_art_enabled = true;
    }

    bool radio_logo_custom = false;
    if (extract_json_bool(json, "radioLogoCustomGraphicEnabled",
                          radio_logo_custom)) {
        out.radio_logo_custom_graphic_enabled = radio_logo_custom;
    } else if (has_json_key(json, "radioLogoCustomGraphicEnabled")) {
        return false;
    } else {
        out.radio_logo_custom_graphic_enabled = false;
    }

    std::string radio_logo_variant;
    if (extract_json_string(json, "radioLogoSpotifyVariant",
                            radio_logo_variant)) {
        if (!is_valid_radio_logo_spotify_variant(radio_logo_variant)) {
            return false;
        }
        out.radio_logo_spotify_variant = radio_logo_variant;
    } else if (has_json_key(json, "radioLogoSpotifyVariant")) {
        return false;
    } else {
        out.radio_logo_spotify_variant = kDefaultRadioLogoSpotifyVariant;
    }

    bool metadata_truncation_enabled = true;
    if (extract_json_bool(json, "metadataTruncationEnabled",
                          metadata_truncation_enabled)) {
        out.metadata_truncation_enabled = metadata_truncation_enabled;
    } else if (has_json_key(json, "metadataTruncationEnabled")) {
        return false;
    } else {
        out.metadata_truncation_enabled = true;
    }

    uint32_t metadata_truncation_length = kDefaultMetadataTruncationLength;
    if (extract_json_u32(json, "metadataTruncationLength",
                         metadata_truncation_length)) {
        if (metadata_truncation_length < kMinMetadataTruncationLength ||
            metadata_truncation_length > kMaxMetadataTruncationLength) {
            return false;
        }
        out.metadata_truncation_length = metadata_truncation_length;
    } else if (has_json_key(json, "metadataTruncationLength")) {
        return false;
    } else {
        out.metadata_truncation_length = kDefaultMetadataTruncationLength;
    }
    return true;
}

std::string options_to_json(const PlaybackOptions& options) {
    std::string menu = is_valid_menu_playback(options.menu_playback)
        ? options.menu_playback
        : kDefaultMenuPlayback;
    std::string race = is_valid_race_start(options.race_start)
        ? options.race_start
        : kDefaultRaceStart;
    std::string normalization = is_valid_volume_normalization(options.volume_normalization)
        ? options.volume_normalization
        : kDefaultVolumeNormalization;
    std::string source = is_valid_active_source(options.active_source)
        ? options.active_source
        : kDefaultActiveSource;
    std::string locale = is_valid_locale(options.locale)
        ? options.locale
        : kDefaultLocale;
    std::string speed_unit = is_valid_night_runners_speed_unit(options.night_runners_speed_unit)
        ? options.night_runners_speed_unit
        : kDefaultNightRunnersSpeedUnit;
    std::string frequency_cut_mode =
        is_valid_night_runners_frequency_cut_mode(
            options.night_runners_frequency_cut_mode)
            ? options.night_runners_frequency_cut_mode
            : kDefaultNightRunnersFrequencyCutMode;
    std::string radio_logo_variant =
        is_valid_radio_logo_spotify_variant(options.radio_logo_spotify_variant)
            ? options.radio_logo_spotify_variant
            : kDefaultRadioLogoSpotifyVariant;
    std::string local_title_metadata_mode =
        is_valid_local_title_metadata_mode(options.local_title_metadata_mode)
            ? options.local_title_metadata_mode
            : kDefaultLocalTitleMetadataMode;
    std::string local_artist_metadata_mode =
        is_valid_local_artist_metadata_mode(options.local_artist_metadata_mode)
            ? options.local_artist_metadata_mode
            : kDefaultLocalArtistMetadataMode;
    std::ostringstream out;
    out << "{\"activeSource\":";
    append_json_string(out, source);
    out << ",\"locale\":";
    append_json_string(out, locale);
    out << ",\"menuPlayback\":";
    append_json_string(out, menu);
    out << ",\"raceStartPlayback\":";
    append_json_string(out, race);
    out << ",\"raceStartRestartThresholdSeconds\":"
        << std::clamp(options.race_start_restart_threshold_s,
                      kMinRaceStartRestartThresholdSeconds,
                      kMaxRaceStartRestartThresholdSeconds);
    out << ",\"songStartOffsetEnabled\":"
        << (options.song_start_offset_enabled ? "true" : "false")
        << ",\"songStartOffsetSeconds\":"
        << std::clamp(options.song_start_offset_seconds,
                      kMinSongStartOffsetSeconds,
                      kMaxSongStartOffsetSeconds);
    out << ",\"volumeNormalization\":";
    append_json_string(out, normalization);
    out << ",\"quickStationSkip\":"
        << (options.quick_station_skip ? "true" : "false")
        << ",\"equalizerEnabled\":"
        << (options.equalizer_enabled ? "true" : "false")
        << ",\"equalizerBands\":[";
    out << std::fixed << std::setprecision(1);
    for (size_t i = 0; i < options.equalizer_bands_db.size(); ++i) {
        if (i) out << ",";
        out << sanitize_eq_band(options.equalizer_bands_db[i]);
    }
    out << "],\"localMusicDir\":";
    append_json_string(out, options.local_music_dir);
    out << ",\"localRecursive\":"
        << (options.local_recursive ? "true" : "false")
        << ",\"localShuffle\":"
        << (options.local_shuffle ? "true" : "false")
        << ",\"localVolume\":"
        << std::min(options.local_volume_percent, kMaxLocalVolumePercent)
        << ",\"localTitleMetadataMode\":";
    append_json_string(out, local_title_metadata_mode);
    out << ",\"localArtistMetadataMode\":";
    append_json_string(out, local_artist_metadata_mode);
    out << ",\"nightRunnersMode\":"
        << (options.night_runners_mode ? "true" : "false")
        << ",\"nightRunnersStoppedVolumeDecrease\":"
        << std::min(options.night_runners_stopped_volume_decrease_percent,
                    kMaxNightRunnersStoppedVolumeDecreasePercent)
        << ",\"nightRunnersMaxSpeedMph\":"
        << std::clamp(options.night_runners_max_speed_mph,
                      kMinNightRunnersMaxSpeedMph,
                      kMaxNightRunnersMaxSpeedMph)
        << ",\"nightRunnersSpeedUnit\":";
    append_json_string(out, speed_unit);
    out << ",\"nightRunnersCurveEnabled\":"
        << (options.night_runners_curve_enabled ? "true" : "false")
        << ",\"nightRunnersCurveExponent\":"
        << std::fixed << std::setprecision(1)
        << sanitize_night_runners_curve_exponent(
            options.night_runners_curve_exponent)
        << ",\"nightRunnersLazyVolumeEnabled\":"
        << (options.night_runners_lazy_volume_enabled ? "true" : "false")
        << ",\"nightRunnersLazyHoldSeconds\":"
        << std::fixed << std::setprecision(1)
        << sanitize_night_runners_lazy_hold_seconds(
            options.night_runners_lazy_hold_seconds)
        << ",\"nightRunnersLowCutEnabled\":"
        << (options.night_runners_low_cut_enabled ? "true" : "false")
        << ",\"nightRunnersFrequencyCutMode\":";
    append_json_string(out, frequency_cut_mode);
    out << ",\"nightRunnersLowCutAmount\":"
        << std::min(options.night_runners_low_cut_amount_percent,
                    kMaxNightRunnersLowCutAmountPercent)
        << ",\"nightRunnersLowCutFrequencyHz\":"
        << sanitize_night_runners_frequency_cut_hz(
            options.night_runners_low_cut_frequency_hz,
            frequency_cut_mode)
        << ",\"nightRunnersNonDrivingVolumeEnabled\":"
        << (options.night_runners_non_driving_volume_enabled ? "true" : "false")
        << ",\"nightRunnersNonDrivingVolume\":"
        << std::min(options.night_runners_non_driving_volume_percent,
                    kMaxNightRunnersNonDrivingVolumePercent)
        << ",\"nightRunnersDynamicMode\":"
        << (options.night_runners_dynamic_mode ? "true" : "false")
        << ",\"nightRunnersDynamicThresholdMph\":"
        << std::clamp(options.night_runners_dynamic_threshold_mph,
                      kMinNightRunnersDynamicThresholdMph,
                      kMaxNightRunnersDynamicThresholdMph)
        << ",\"nightRunnersDynamicBufferPercent\":"
        << std::min(options.night_runners_dynamic_buffer_percent,
                    kMaxNightRunnersDynamicBufferPercent)
        << ",\"nightRunnersDynamicBufferFillSeconds\":"
        << std::fixed << std::setprecision(1)
        << std::clamp(options.night_runners_dynamic_buffer_fill_seconds,
                      kMinNightRunnersDynamicBufferFillSeconds,
                      kMaxNightRunnersDynamicBufferFillSeconds)
        << ",\"nightRunnersDynamicIncreaseSeconds\":"
        << std::fixed << std::setprecision(1)
        << std::clamp(options.night_runners_dynamic_increase_seconds,
                      kMinNightRunnersDynamicRampSeconds,
                      kMaxNightRunnersDynamicRampSeconds)
        << ",\"nightRunnersDynamicDecreaseSeconds\":"
        << std::fixed << std::setprecision(1)
        << std::clamp(options.night_runners_dynamic_decrease_seconds,
                      kMinNightRunnersDynamicRampSeconds,
                      kMaxNightRunnersDynamicRampSeconds)
        << ",\"nightRunnersDynamicMaxDecayMphS\":"
        << std::min(options.night_runners_dynamic_max_decay_mph_s,
                    kMaxNightRunnersDynamicMaxDecayMphS)
        << ",\"radioLogoAlbumArtEnabled\":"
        << (options.radio_logo_album_art_enabled ? "true" : "false")
        << ",\"radioLogoCustomGraphicEnabled\":"
        << (options.radio_logo_custom_graphic_enabled ? "true" : "false")
        << ",\"radioLogoSpotifyVariant\":";
    append_json_string(out, radio_logo_variant);
    out << ",\"metadataTruncationEnabled\":"
        << (options.metadata_truncation_enabled ? "true" : "false")
        << ",\"metadataTruncationLength\":"
        << sanitize_metadata_truncation_length(
            options.metadata_truncation_length);
    out << "}";
    return out.str();
}

PlaybackOptionsStore::PlaybackOptionsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

void PlaybackOptionsStore::load() {
    PlaybackOptions parsed;
    std::string raw = read_file(path_);
    {
        std::lock_guard lock(mtx_);
        if (!raw.empty() && parse_options_json(raw, parsed)) {
            options_ = parsed;
            log::info("[options] Loaded " + options_to_json(options_));
            return;
        }
        options_ = PlaybackOptions{};
    }
    if (!raw.empty()) {
        log::warn("[options] Invalid options.json; using defaults");
    } else {
        log::info("[options] No options.json; using defaults");
    }
}

PlaybackOptions PlaybackOptionsStore::snapshot() const {
    std::lock_guard lock(mtx_);
    return options_;
}

bool PlaybackOptionsStore::update(const PlaybackOptions& options, std::string* error) {
    if (!is_valid_active_source(options.active_source)) {
        if (error) *error = "invalid activeSource";
        return false;
    }
    if (!is_valid_locale(options.locale)) {
        if (error) *error = "invalid locale";
        return false;
    }
    if (!is_valid_menu_playback(options.menu_playback)) {
        if (error) *error = "invalid menuPlayback";
        return false;
    }
    if (!is_valid_race_start(options.race_start)) {
        if (error) *error = "invalid raceStartPlayback";
        return false;
    }
    if (options.race_start_restart_threshold_s <
            kMinRaceStartRestartThresholdSeconds ||
        options.race_start_restart_threshold_s >
            kMaxRaceStartRestartThresholdSeconds) {
        if (error) *error = "invalid raceStartRestartThresholdSeconds";
        return false;
    }
    if (options.song_start_offset_seconds < kMinSongStartOffsetSeconds ||
        options.song_start_offset_seconds > kMaxSongStartOffsetSeconds) {
        if (error) *error = "invalid songStartOffsetSeconds";
        return false;
    }
    if (!is_valid_volume_normalization(options.volume_normalization)) {
        if (error) *error = "invalid volumeNormalization";
        return false;
    }
    for (float band : options.equalizer_bands_db) {
        if (!std::isfinite(band) || band < kMinEqualizerDb || band > kMaxEqualizerDb) {
            if (error) *error = "invalid equalizerBands";
            return false;
        }
    }
    if (options.local_volume_percent > kMaxLocalVolumePercent) {
        if (error) *error = "invalid localVolume";
        return false;
    }
    if (!is_valid_local_title_metadata_mode(
            options.local_title_metadata_mode)) {
        if (error) *error = "invalid localTitleMetadataMode";
        return false;
    }
    if (!is_valid_local_artist_metadata_mode(
            options.local_artist_metadata_mode)) {
        if (error) *error = "invalid localArtistMetadataMode";
        return false;
    }
    if (options.night_runners_stopped_volume_decrease_percent >
        kMaxNightRunnersStoppedVolumeDecreasePercent) {
        if (error) *error = "invalid nightRunnersStoppedVolumeDecrease";
        return false;
    }
    if (options.night_runners_max_speed_mph < kMinNightRunnersMaxSpeedMph ||
        options.night_runners_max_speed_mph > kMaxNightRunnersMaxSpeedMph) {
        if (error) *error = "invalid nightRunnersMaxSpeedMph";
        return false;
    }
    if (!is_valid_night_runners_speed_unit(options.night_runners_speed_unit)) {
        if (error) *error = "invalid nightRunnersSpeedUnit";
        return false;
    }
    if (!is_valid_night_runners_frequency_cut_mode(
            options.night_runners_frequency_cut_mode)) {
        if (error) *error = "invalid nightRunnersFrequencyCutMode";
        return false;
    }
    if (!std::isfinite(options.night_runners_curve_exponent) ||
        options.night_runners_curve_exponent < kMinNightRunnersCurveExponent ||
        options.night_runners_curve_exponent > kMaxNightRunnersCurveExponent) {
        if (error) *error = "invalid nightRunnersCurveExponent";
        return false;
    }
    if (!std::isfinite(options.night_runners_lazy_hold_seconds) ||
        options.night_runners_lazy_hold_seconds <
            kMinNightRunnersLazyHoldSeconds ||
        options.night_runners_lazy_hold_seconds >
            kMaxNightRunnersLazyHoldSeconds) {
        if (error) *error = "invalid nightRunnersLazyHoldSeconds";
        return false;
    }
    if (options.night_runners_low_cut_amount_percent >
        kMaxNightRunnersLowCutAmountPercent) {
        if (error) *error = "invalid nightRunnersLowCutAmount";
        return false;
    }
    if (options.night_runners_low_cut_frequency_hz <
            kMinNightRunnersLowCutFrequencyHz ||
        options.night_runners_low_cut_frequency_hz >
            kMaxNightRunnersLowCutFrequencyHz) {
        if (error) *error = "invalid nightRunnersLowCutFrequencyHz";
        return false;
    }
    if (options.night_runners_non_driving_volume_percent >
        kMaxNightRunnersNonDrivingVolumePercent) {
        if (error) *error = "invalid nightRunnersNonDrivingVolume";
        return false;
    }
    if (options.night_runners_dynamic_threshold_mph <
            kMinNightRunnersDynamicThresholdMph ||
        options.night_runners_dynamic_threshold_mph >
            kMaxNightRunnersDynamicThresholdMph) {
        if (error) *error = "invalid nightRunnersDynamicThresholdMph";
        return false;
    }
    if (options.night_runners_dynamic_buffer_percent >
        kMaxNightRunnersDynamicBufferPercent) {
        if (error) *error = "invalid nightRunnersDynamicBufferPercent";
        return false;
    }
    if (!std::isfinite(options.night_runners_dynamic_buffer_fill_seconds) ||
        options.night_runners_dynamic_buffer_fill_seconds <
            kMinNightRunnersDynamicBufferFillSeconds ||
        options.night_runners_dynamic_buffer_fill_seconds >
            kMaxNightRunnersDynamicBufferFillSeconds) {
        if (error) *error = "invalid nightRunnersDynamicBufferFillSeconds";
        return false;
    }
    if (!std::isfinite(options.night_runners_dynamic_increase_seconds) ||
        options.night_runners_dynamic_increase_seconds <
            kMinNightRunnersDynamicRampSeconds ||
        options.night_runners_dynamic_increase_seconds >
            kMaxNightRunnersDynamicRampSeconds) {
        if (error) *error = "invalid nightRunnersDynamicIncreaseSeconds";
        return false;
    }
    if (!std::isfinite(options.night_runners_dynamic_decrease_seconds) ||
        options.night_runners_dynamic_decrease_seconds <
            kMinNightRunnersDynamicRampSeconds ||
        options.night_runners_dynamic_decrease_seconds >
            kMaxNightRunnersDynamicRampSeconds) {
        if (error) *error = "invalid nightRunnersDynamicDecreaseSeconds";
        return false;
    }
    if (options.night_runners_dynamic_max_decay_mph_s >
        kMaxNightRunnersDynamicMaxDecayMphS) {
        if (error) *error = "invalid nightRunnersDynamicMaxDecayMphS";
        return false;
    }
    if (!is_valid_radio_logo_spotify_variant(
            options.radio_logo_spotify_variant)) {
        if (error) *error = "invalid radioLogoSpotifyVariant";
        return false;
    }
    if (options.metadata_truncation_length < kMinMetadataTruncationLength ||
        options.metadata_truncation_length > kMaxMetadataTruncationLength) {
        if (error) *error = "invalid metadataTruncationLength";
        return false;
    }
    std::lock_guard lock(mtx_);
    PlaybackOptions previous = options_;
    PlaybackOptions sanitized = options;
    if (!is_valid_local_title_metadata_mode(
            sanitized.local_title_metadata_mode)) {
        sanitized.local_title_metadata_mode = kDefaultLocalTitleMetadataMode;
    }
    if (!is_valid_local_artist_metadata_mode(
            sanitized.local_artist_metadata_mode)) {
        sanitized.local_artist_metadata_mode = kDefaultLocalArtistMetadataMode;
    }
    sanitized.night_runners_low_cut_frequency_hz =
        sanitize_night_runners_frequency_cut_hz(
            sanitized.night_runners_low_cut_frequency_hz,
            sanitized.night_runners_frequency_cut_mode);
    sanitized.metadata_truncation_length =
        sanitize_metadata_truncation_length(
            sanitized.metadata_truncation_length);
    options_ = sanitized;
    if (!save_locked(error)) {
        options_ = previous;
        return false;
    }
    return true;
}

bool PlaybackOptionsStore::save_locked(std::string* error) {
    return atomic_replace_file(path_, options_to_json(options_) + "\n", error);
}

} // namespace bridge
