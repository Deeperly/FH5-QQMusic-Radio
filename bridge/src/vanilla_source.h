// Passive "Vanilla Streamer Mode" source.
//
// Selecting this mode means "stay out of the way": the bridge stops injecting
// PCM and (in media-free mode) stops muting the radio channel, so the game's
// own curated Streamer Mode audio plays untouched. It owns no playback, reports
// no track, and exposes no transport — the web UI shows only the Forza logo.
//
// Implemented as a registered AudioSource so it flows through the existing
// find_source / switch_to / source-list plumbing. Switching TO it tears down
// the previous source's session via SourceManager::switch_to (on_deactivated),
// matching the user's "fully passive" choice. The actual injection/mute
// suppression is done by FmodInject's injection gate, which checks for this
// source id.

#pragma once

#include "audio_source.h"

namespace bridge {

class VanillaSource : public AudioSource {
public:
    const char* id() const override { return "vanilla"; }
    const char* display_name() const override { return "Vanilla Streamer Mode"; }

    bool is_connected() const override { return false; }
    bool is_playing() const override { return false; }
    SourceTrack last_track() const override { return {}; }
    SourceCapabilities capabilities() const override {
        return {false, false, false, false, false, false};
    }

    void pause_at_audio_boundary() override {}
    void resume_rewound(uint32_t /*rewind_ms*/) override {}
    bool restart_current_track() override { return false; }
    bool next_track() override { return false; }

    void set_volume_normalization(bool /*enabled*/) override {}
    void set_equalizer(bool /*enabled*/,
                       const std::array<float, 5>& /*bands_db*/) override {}
};

} // namespace bridge
