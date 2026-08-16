// Active audio source selector.
//
// Keeps game-state control and UI state source-neutral across Spotify,
// AirPlay, Local Files, and future bridge-owned PCM sources.

#pragma once

#include "audio_source.h"

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bridge {

class BridgeStateStore;

class SourceManager {
public:
    void set_state_store(BridgeStateStore* store);
    void register_source(AudioSource& source);
    void set_spotify_source(AudioSource& source);
    void set_local_source(AudioSource& source);
    void set_clear_pcm_callback(std::function<void()> clear_pcm);

    const AudioSource* active_source() const;
    const char* active_source_id() const;
    AudioSource* find_source(std::string_view source_id) const;
    bool switch_to(std::string_view source_id);
    bool activate_current_source();

    bool is_playing() const;
    SourceTrack last_track() const;

    void pause_at_audio_boundary();
    void resume_rewound(uint32_t rewind_ms);
    bool restart_current_track();
    bool next_track();
    bool previous_track();
    bool seek(uint32_t position_ms);
    std::optional<uint32_t> current_position_ms() const;

    // Race-start song offset: arm an offset to apply to the track that the
    // race-start "next" action skips to (baseline_uri = the track before the
    // skip, so we know when the new one has loaded). service_race_start_offset()
    // is polled by the metadata pump and seeks once the new track is playing.
    void request_race_start_offset(uint32_t offset_ms,
                                   const std::string& baseline_uri);
    void service_race_start_offset();

    void set_volume_normalization(bool enabled);
    void set_equalizer(bool enabled, const std::array<float, 5>& bands_db);
    void set_metadata_truncation(bool enabled, uint32_t max_chars);
    void publish_source_states();

private:
    AudioSource* control_source_locked() const;
    AudioSource* source_by_id_locked(std::string_view source_id) const;
    void publish_active_source_locked();

    mutable std::mutex mtx_;
    BridgeStateStore* state_store_ = nullptr;
    std::vector<AudioSource*> sources_;
    AudioSource* fallback_source_ = nullptr;
    AudioSource* active_source_ = nullptr;
    std::function<void()> clear_pcm_;
    std::mutex switch_mtx_;

    // Pending race-start offset (guarded by mtx_).
    uint32_t pending_race_offset_ms_ = 0;       // 0 = not armed
    int pending_race_offset_polls_ = 0;         // countdown (metadata-pump polls)
    std::string pending_race_offset_base_uri_;  // track before the skip

    bool metadata_truncation_enabled_ = true;
    uint32_t metadata_truncation_length_ = 30;
};

} // namespace bridge
