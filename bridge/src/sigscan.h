// Startup sigscan resolver — replaces the table of literal hex RVAs that
// FmodInject and InProcessInjector previously baked against build 23271700.
//
// Every game update shifts FMOD/radio RVAs by tens to hundreds of bytes.
// Hard-coded values guarantee the mod breaks on patch day. This resolver
// recovers each RVA at startup by:
//   - String-anchored discovery for FMOD public wrappers: locate the FMOD
//     `__FUNCTION__` literal in .rdata, find the unique `lea r, [rip+disp]`
//     in .text that targets it, and use the PE exception directory
//     (.pdata) to map that LEA back to its containing function start.
//   - Direct byte patterns for internal FMOD helpers that have no
//     __FUNCTION__ anchor (the handle resolver + handle unlock).
//   - RIP+disp32 anchors for the RadioState singleton.
//
// All work is plain C++20 + Win32 PE parsing. No vendored deps, no
// allocations during scan beyond the parse-once .pdata vector.

#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

struct GameProfile;  // game_profile.h — supplies the per-game patterns.

struct ResolvedRvas {
    // FMOD Core DSP wrappers used by the native R10 ChannelControl bridge.
    uintptr_t systemCreateDSP        = 0;
    uintptr_t dspRelease             = 0;
    uintptr_t channelControlAddDSP   = 0;
    uintptr_t channelControlRemoveDSP = 0;

    // FMOD internal handle resolver + paired unlock — heap-only Denuvo-safe
    // path from encoded Channel handle to ChannelI* / lock pair.
    uintptr_t fmod_handle_resolver = 0;
    uintptr_t fmod_handle_unlock   = 0;

    // FMOD ChannelControl mute/volume levers + DSP-chain inspectors. Used by the
    // runtime-mute path (media-free alternative to the silent radio bank).
    // Optional — kept out of all_ok(); the mute path no-ops if unavailable.
    uintptr_t channelControlSetVolume  = 0;
    uintptr_t channelControlSetMute    = 0;
    uintptr_t channelControlGetNumDSPs = 0;
    uintptr_t channelControlGetDSP     = 0;

    // RadioState singleton (qword_A93EBA0). +0x40 holds RadioPlayer*; the
    // station gate walks this chain to detect R10 selection.
    uintptr_t radio_state_singleton = 0;

    // RadioState station setter by name. Signature:
    //   void fn(RadioState* state, const std::string* station_name)
    // `"StationOff"` clears the selected station; `"Streamer Mode"` selects
    // the Spotify/Rebranded R10 slot through the same game-side path used by
    // station cycling.
    uintptr_t radio_set_station_by_name = 0;

    // Runtime radio-logo resource-cache path. These are optional for core
    // audio; when any required logo RVA is unavailable the runtime logo
    // override must fail open instead of using stale build constants.
    uintptr_t logo_request_manager = 0;
    uintptr_t logo_texture_system = 0;
    uintptr_t logo_resource_registry = 0;
    uintptr_t logo_create_2d_raw_texture = 0;
    uintptr_t logo_handle_base_array = 0;
    uintptr_t logo_handle_stride_array = 0;

    // Stock-resource in-place BC7 logo path. `logo_inplace_upload` is the
    // engine async texture-upload-queue producer (sub_3522560). The two table
    // globals + the stock wrapper vtable are used to validate the stock Streamer
    // resource chain (build-match gate) before uploading. When any is
    // unavailable the stock logo path must fail closed (no upload, no crash).
    uintptr_t logo_inplace_upload = 0;
    uintptr_t logo_resource_table = 0;
    uintptr_t logo_resource_refcounts = 0;
    uintptr_t logo_stock_wrapper_vtable = 0;

    bool fmod_dsp_ok() const {
        return systemCreateDSP && dspRelease && channelControlAddDSP &&
               channelControlRemoveDSP;
    }
    bool fmod_handle_ok() const {
        return fmod_handle_resolver && fmod_handle_unlock;
    }
    // Runtime channel mute requires at least setMute (setVolume is a fallback).
    bool fmod_mute_ok() const { return channelControlSetMute != 0; }
    bool radio_state_ok() const { return radio_state_singleton != 0; }
    bool radio_nudge_ok() const {
        return radio_state_singleton && radio_set_station_by_name;
    }
    bool runtime_logo_core_ok() const {
        return logo_request_manager && logo_texture_system &&
               logo_resource_registry && logo_create_2d_raw_texture;
    }
    bool runtime_logo_alias_ok() const {
        return logo_handle_base_array && logo_handle_stride_array;
    }
    bool runtime_logo_ok() const {
        return runtime_logo_core_ok() && runtime_logo_alias_ok();
    }
    // Stock in-place BC7 path: needs the uniquely sig-resolved upload fn, the
    // request manager (to resolve the stock rid via the cache walk), and the
    // texture system. The validation globals (resource table / wrapper vtable)
    // use build constants with fail-closed validation, so they are not required
    // here.
    bool runtime_logo_stock_ok() const {
        return logo_inplace_upload && logo_request_manager &&
               logo_texture_system;
    }

    bool all_ok() const {
        return fmod_dsp_ok() && fmod_handle_ok() && radio_state_ok();
    }
};

// Resolves every RVA in `out` against the loaded image at `module_base`,
// using the per-game patterns/anchors in `gp` (game_profile.h). Logs one
// `[sigscan] <name> RVA=0x...` line per resolution and one
// `[sigscan] FAIL <name> — <reason>` line per miss. Returns true iff every
// required RVA resolved uniquely (see ResolvedRvas::all_ok()).
//
// Patterns that are null in the profile (e.g. FH5 logo / station-nudge) are
// skipped, leaving their RVA 0 so the dependent feature fails open.
//
// Safe to call multiple times. Slots that are already non-zero in `out` are
// skipped — second call retries only the misses. Pass `quiet=true` for
// retry passes to suppress per-call FAIL warns and the init banner; SUCCESS
// resolutions still log so the user log captures the moment an
// initially-encrypted wrapper becomes scannable.
bool resolve_signatures(uintptr_t module_base, size_t module_size,
                        const GameProfile& gp,
                        ResolvedRvas& out, bool quiet = false);

} // namespace bridge
