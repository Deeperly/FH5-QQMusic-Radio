// Source-neutral audio source contract.
//
// This intentionally models the controls the game-state bridge already needs.
// Local-file or other bridge-owned sources can implement this later without
// changing the native DSP sink or the menu/race policy loop.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bridge {

class BridgeStateStore;

struct SourceTrack {
    std::string uri;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t duration_ms = 0;
    std::string artwork_key;
    std::string artwork_mime;
    std::vector<uint8_t> artwork_bytes;
    bool artwork_loading = false;
};

struct SourceCapabilities {
    bool pause = true;
    bool resume = true;
    bool restart = true;
    bool next = true;
    bool previous = false;
    bool seek = false;
};

class AudioSource {
public:
    virtual ~AudioSource() = default;

    virtual const char* id() const = 0;
    virtual const char* display_name() const = 0;

    virtual bool is_connected() const = 0;
    virtual bool is_playing() const = 0;
    virtual SourceTrack last_track() const = 0;
    virtual SourceCapabilities capabilities() const { return {}; }

    // Current playback position of the active track, in milliseconds. Returns
    // std::nullopt when the source cannot report a reliable position (not
    // playing, not connected, or no position reported yet). Used by the
    // race-start restart-or-skip policy; an empty result means "skip".
    virtual std::optional<uint32_t> current_position_ms() const {
        return std::nullopt;
    }

    virtual void pause_at_audio_boundary() = 0;
    virtual void resume_rewound(uint32_t rewind_ms) = 0;
    virtual bool restart_current_track() = 0;
    virtual bool next_track() = 0;
    virtual bool previous_track() { return false; }
    virtual bool seek(uint32_t position_ms) { (void)position_ms; return false; }

    // Called when the user changes playback modes. Sources may use this to
    // start/stop external sessions, but PCM ownership is controlled separately.
    virtual void on_activated() {}
    virtual void on_deactivated() {}

    // Source ownership gate. Sources that can continue receiving audio while
    // inactive should drop PCM when disabled.
    virtual void set_pcm_enabled(bool enabled) { (void)enabled; }

    virtual void set_volume_normalization(bool enabled) = 0;
    virtual void set_equalizer(bool enabled,
                               const std::array<float, 5>& bands_db) = 0;

    // Optional source-specific status publisher. Spotify updates most state
    // directly from librespot callbacks; bridge-owned sources can publish from
    // the status sampler so SSE does not need to know their implementation.
    virtual void publish_state(BridgeStateStore& store) const { (void)store; }
};

} // namespace bridge
