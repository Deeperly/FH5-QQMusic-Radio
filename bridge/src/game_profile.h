// Build-specific game profiles consumed by the shared signature resolver and
// injection runtime. Profiles isolate values that change independently when a
// supported game is updated.

#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

enum class GameId { Unknown, FH6, FH5 };

// Signature/pattern inputs consumed by resolve_signatures(). Pattern strings
// use the "48 8B ?? ??" hex-with-?? form parsed by sigscan.cpp. A null/empty
// pattern means "not available for this game" — the corresponding RVA stays 0
// and its feature must fail open/closed (the bridge already treats logo +
// station-nudge RVAs as optional).
struct SigProfile {
    // FMOD Core DSP public wrappers: resolved by their `__FUNCTION__` literal
    // in .rdata. `*_prologue` is a fixed-byte fingerprint used as a tie-breaker
    // when the anchor has multiple copies/consumers; '|' separates alternatives.
    // May be empty when the anchor already resolves uniquely.
    const char* createDSP_anchor;
    const char* createDSP_prologue;
    const char* dspRelease_anchor;
    const char* dspRelease_prologue;
    const char* addDSP_anchor;
    const char* addDSP_prologue;
    const char* removeDSP_anchor;
    const char* removeDSP_prologue;

    // FMOD internal handle resolver + paired unlock — no __FUNCTION__ anchor,
    // matched by direct byte pattern in .text.
    const char* fmod_handle_resolver;
    const char* fmod_handle_unlock;

    // RadioState singleton: rip32 anchor whose `mov reg,[rip+disp32]` operand
    // (at byte offset `radio_state_singleton_operand` into the match) points at
    // the global.
    const char* radio_state_singleton;
    int         radio_state_singleton_operand;

    // RadioState station-setter-by-name (optional startup nudge). Nullable:
    // FH5 selects stations by interned hash id and has no portable anchor yet.
    const char* radio_set_station_by_name;

    // Runtime radio-logo resource path (all optional — logo fails open when a
    // pattern is null). Each rip32 entry carries its operand byte offset.
    const char* logo_create_2d_raw_texture;
    const char* logo_texture_system;     int logo_texture_system_operand;
    const char* logo_request_manager;    int logo_request_manager_operand;
    const char* logo_resource_registry;  int logo_resource_registry_operand;
    const char* logo_inplace_upload;

    // Known-layout fixups keyed on .text VirtualSize. Applied after pattern
    // resolution to pin values the generic patterns can mis-select on a
    // specific shipped build. A 0 field means "leave whatever resolved".
    struct BuildOverride {
        size_t    text_size;
        uintptr_t logo_create_2d_raw_texture;
        uintptr_t logo_handle_base_array;
        uintptr_t logo_handle_stride_array;
    };
    const BuildOverride* build_overrides;
    size_t              build_override_count;

    // FMOD ChannelControl mute/volume levers + DSP-chain inspectors. Used by the
    // runtime-mute path (the media-free alternative to the silent-bank). Resolved
    // by their `__FUNCTION__` anchor (each is a single unique copy, so no prologue
    // tie-breaker is needed). May be null — the mute path no-ops if unavailable.
    const char* setVolume_anchor;
    const char* setMute_anchor;
    const char* getNumDSPs_anchor;
    const char* getDSP_anchor;
};

// RadioState/RadioPlayer/RadioStation struct offsets + the station-detection
// string/sound constants. All consumed by injector_inproc.cpp.
struct RadioProfile {
    uintptr_t state_player_offset;            // RadioState -> RadioPlayer*
    uintptr_t state_race_active_a_offset;     // u8
    uintptr_t state_race_active_b_offset;     // u8
    uintptr_t state_menu_open_offset;         // u8
    uintptr_t state_race_restart_menu_offset; // u32 marker
    uintptr_t player_selected_station_offset; // RadioPlayer -> RadioStation*
    uintptr_t player_stinger_marker_offset[5];
    uint32_t  race_menu_stinger_marker;
    uintptr_t station_name_offset;            // RadioStation -> std::string
    // FMOD Sound -> owning SystemI* back-ref. discover_system() reads this from
    // an active radio Sound to recover the FMOD System for createDSP. The offset
    // shifts with the FMOD build (FH6 0xC0, FH5 0xD8).
    uintptr_t sound_system_backref_offset;

    const char* station_name_streamer;        // "Streamer Mode" (target station)
    const char* station_name_spotify;         // rebranded-name fallback
    const char* wired_sound_name;             // HZ?_* SampleProperties sentinel
};

struct GameProfile {
    GameId        id;
    const char*   name;        // "FH6" / "FH5" — log/diagnostic label
    const wchar_t* exe_name;   // lowercase basename, e.g. L"forzahorizon6.exe"
    SigProfile    sig;
    RadioProfile  radio;
};

// Profile tables (defined in game_profile.cpp).
const GameProfile& fh6_profile();
const GameProfile& fh5_profile();

// Detect the host game from the main module's file name. Cached after first
// call. Returns GameId::Unknown if neither known exe matches.
GameId detect_game_id();

// Profile for an explicit id; falls back to FH6 for Unknown so existing
// behavior is preserved on an unrecognized host.
const GameProfile& profile_for(GameId id);

// Convenience: profile_for(detect_game_id()). Detection + selection are cached.
const GameProfile& active_profile();

} // namespace bridge
