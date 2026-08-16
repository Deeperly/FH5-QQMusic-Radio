// Metadata injector interface.
//
// Both the cross-process injector (bridge console, RPM/WPM) and the
// in-process injector (version.dll, direct pointers) implement this
// so server.cpp / run_poller work unchanged with either backend.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bridge {

// Cached runtime addresses discovered on attach / discovery.
// Shared between both injector implementations.
struct GameAddresses {
    // _Ref_count_obj2<RadioStreamFmod> vtable (discovered via typedesc scan)
    uintptr_t radio_stream_refcount_vt = 0;

    // All RadioStreamFmod instances (one per radio station, typically 5)
    std::vector<uintptr_t> radio_stream_instances;

    // Currently active RadioStreamFmod instance (non-null FMOD Sound*)
    uintptr_t active_radio_stream = 0;

    // _Ref_count_obj2<SampleProperties@RadioSample> address (heap)
    // Contains SoundName(+0x10), DisplayName(+0x30), Artist(+0x50) as MSVC std::string
    uintptr_t sample_properties = 0;

    bool valid() const { return !radio_stream_instances.empty(); }
};

// Abstract interface for metadata injection into the game HUD.
class IMetadataInjector {
public:
    virtual ~IMetadataInjector() = default;

    struct RadioState {
        bool game_found = false;
        bool attached = false;
        bool discovery_done = false;
        std::string current_sound_name;
        std::string current_artist;
        std::string injected_track;
    };

    // Connect to game process (cross-process) or resolve own module (in-process).
    // Starts async discovery in background. Returns true if initial setup succeeded.
    virtual bool attach() = 0;

    // Check if connection to game is alive.
    virtual bool is_attached() const = 0;

    // Check if discovery is complete and addresses are resolved.
    virtual bool is_ready() const = 0;

    // Release connection / cleanup.
    virtual void detach() = 0;

    // Write Spotify track metadata into the game's SampleProperties struct.
    // Skips write if game already shows our text (cheap read check).
    virtual bool push_metadata(const std::string& track_name, const std::string& artist) = 0;

    // Read current injection state for diagnostics / UI display.
    virtual RadioState get_state() const = 0;
};

} // namespace bridge
