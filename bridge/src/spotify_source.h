// Spotify AudioSource adapter.
//
// Keeps LibrespotPlayer as the implementation owner while exposing the
// source-neutral control surface used by SourceManager.

#pragma once

#include "audio_source.h"
#include "librespot.h"

namespace bridge {

class SpotifySource final : public AudioSource {
public:
    explicit SpotifySource(LibrespotPlayer& player);

    const char* id() const override;
    const char* display_name() const override;

    bool is_connected() const override;
    bool is_playing() const override;
    SourceTrack last_track() const override;
    std::optional<uint32_t> current_position_ms() const override;

    void pause_at_audio_boundary() override;
    void resume_rewound(uint32_t rewind_ms) override;
    bool restart_current_track() override;
    bool next_track() override;
    bool seek(uint32_t position_ms) override;
    void on_activated() override;
    void on_deactivated() override;
    void set_pcm_enabled(bool enabled) override;

    void set_volume_normalization(bool enabled) override;
    void set_equalizer(bool enabled,
                       const std::array<float, 5>& bands_db) override;
    void publish_state(BridgeStateStore& store) const override;

private:
    LibrespotPlayer& player_;
    bool advertise_for_selection_on_next_activation_ = false;
};

} // namespace bridge
