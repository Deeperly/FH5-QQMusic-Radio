// Native FMOD DSP integration for the R10/Streamer Mode radio station.
//
// FH6 owns the real radio ChannelControl. The bridge attaches a small FMOD
// DSP to that native channel and feeds Spotify PCM into the game-owned radio
// path, so menu/race/garage filters and station gating remain native.
//
// Required RVAs are resolved at startup by sigscan from stable FMOD/game
// anchors; no literal build-specific addresses are shipped.
// SystemI* discovered live: Sound+0xC0 chain from any active radio Sound.
// Native R10 ChannelControl handles are discovered live by decoding the
// RadioStreamFmod wrapper's encoded FMOD Channel handle.

#pragma once

#include "audio_hook.h"
#include "sigscan.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bridge {

class InProcessInjector;

// ── FMOD v110 ABI ───────────────────────────────────────────────────
// Public layout, version 1.10.x. Size locked by SDK; mismatched cbsize
// causes "Info has invalid cbsize" error at 0x715b550 in fh6 binary.
//
// Forward-declared opaque types — we never dereference these in our code.
struct FMOD_SYSTEM;
struct FMOD_DSP;
struct FMOD_DSP_STATE;

using FMOD_RESULT = int;

constexpr FMOD_RESULT FMOD_OK = 0;

// Function pointer typedefs — calling convention is x64 fastcall.
// C++ instance methods on x64 use rcx=this, rdx=arg1, r8=arg2, r9=arg3.
using SystemCreateDSP_fn = FMOD_RESULT (*)(FMOD_SYSTEM* system,
                                           const void* description,
                                           FMOD_DSP** out_dsp);
using DSPRelease_fn = FMOD_RESULT (*)(FMOD_DSP* dsp);
using ChannelControlAddDSP_fn = FMOD_RESULT (*)(void* channel_control,
                                                int index,
                                                FMOD_DSP* dsp);
using ChannelControlRemoveDSP_fn = FMOD_RESULT (*)(void* channel_control,
                                                   FMOD_DSP* dsp);
// Runtime-mute levers. `bool` is passed as a 4-byte int in the FMOD ABI.
using ChannelControlSetMute_fn = FMOD_RESULT (*)(void* channel_control,
                                                 int mute);
using ChannelControlSetVolume_fn = FMOD_RESULT (*)(void* channel_control,
                                                   float volume);
using ChannelControlGetNumDSPs_fn = FMOD_RESULT (*)(void* channel_control,
                                                    int* numdsps);
using ChannelControlGetDSP_fn = FMOD_RESULT (*)(void* channel_control,
                                                int index, FMOD_DSP** dsp);

using FMOD_DSP_CREATE_CALLBACK = FMOD_RESULT (*)(FMOD_DSP_STATE* dsp_state);
using FMOD_DSP_RELEASE_CALLBACK = FMOD_RESULT (*)(FMOD_DSP_STATE* dsp_state);
using FMOD_DSP_RESET_CALLBACK = FMOD_RESULT (*)(FMOD_DSP_STATE* dsp_state);
using FMOD_DSP_READ_CALLBACK = FMOD_RESULT (*)(
    FMOD_DSP_STATE* dsp_state, float* inbuffer, float* outbuffer,
    unsigned int length, int inchannels, int* outchannels);
using FMOD_DSP_PROCESS_CALLBACK = FMOD_RESULT (*)();
using FMOD_DSP_SETPOSITION_CALLBACK = FMOD_RESULT (*)(
    FMOD_DSP_STATE* dsp_state, unsigned int pos);
using FMOD_DSP_PARAMETER_CALLBACK = FMOD_RESULT (*)();
using FMOD_DSP_SHOULDIPROCESS_CALLBACK = FMOD_RESULT (*)();
using FMOD_DSP_SYSTEM_CALLBACK = FMOD_RESULT (*)();

#pragma pack(push, 8)
struct FMOD_DSP_DESCRIPTION {
    unsigned int pluginsdkversion;
    char name[32];
    unsigned int version;
    int numinputbuffers;
    int numoutputbuffers;
    FMOD_DSP_CREATE_CALLBACK create;
    FMOD_DSP_RELEASE_CALLBACK release;
    FMOD_DSP_RESET_CALLBACK reset;
    FMOD_DSP_READ_CALLBACK read;
    FMOD_DSP_PROCESS_CALLBACK process;
    FMOD_DSP_SETPOSITION_CALLBACK setposition;
    int numparameters;
    void* paramdesc;
    FMOD_DSP_PARAMETER_CALLBACK setparameterfloat;
    FMOD_DSP_PARAMETER_CALLBACK setparameterint;
    FMOD_DSP_PARAMETER_CALLBACK setparameterbool;
    FMOD_DSP_PARAMETER_CALLBACK setparameterdata;
    FMOD_DSP_PARAMETER_CALLBACK getparameterfloat;
    FMOD_DSP_PARAMETER_CALLBACK getparameterint;
    FMOD_DSP_PARAMETER_CALLBACK getparameterbool;
    FMOD_DSP_PARAMETER_CALLBACK getparameterdata;
    FMOD_DSP_SHOULDIPROCESS_CALLBACK shouldiprocess;
    void* userdata;
    FMOD_DSP_SYSTEM_CALLBACK sys_register;
    FMOD_DSP_SYSTEM_CALLBACK sys_deregister;
    FMOD_DSP_SYSTEM_CALLBACK sys_mix;
};
#pragma pack(pop)

static_assert(sizeof(FMOD_DSP_DESCRIPTION) == 0xD8,
              "FMOD_DSP_DESCRIPTION v110 must be 216 bytes");

enum class NativeDspProbeMode : int {
    Off = 0,
    Passthrough = 1,
    Silence = 2,
    Tone = 3,
    Pcm = 4,
};

// Build-specific FMOD handle resolver. Returns FMOD_OK and writes a ChannelI
// pointer plus a lock handle on success.
using FmodHandleResolver_fn = FMOD_RESULT (*)(
    uint32_t handle, void** out_channel_i, void** out_lock);
// Paired unlock helper for the lock object returned by the resolver.
using FmodHandleUnlock_fn = void (*)(void* lock);

// ── FmodInject ──────────────────────────────────────────────────────
//
// Owns:
//  - 16-bit PCM ring buffer (matches librespotc on_audio S16LE format)
//  - SystemI* and native-R10 DSP handle
//  - DSP read callback that drains the ring
//
// Lifetime:
//  - construct early, start() spawns init thread
//  - init thread: wait for radio Sound discovery -> read SystemI* from
//    Sound+0xC0 -> attach DSP when R10 is live
//  - on stop(): remove/release DSP best-effort
class FmodInject {
public:
    // PCM contract — must match what librespotc delivers on_audio.
    static constexpr int kPcmChannels   = 2;
    static constexpr int kPcmSampleRate = 44100;
    // Kept for ABI compatibility with older status/build helpers; native DSP
    // output does not create a bridge-owned FMOD stream.
    static constexpr unsigned int kDecodeBufferFrames = 2048; // ~46 ms @ 44.1 kHz
    // Unity gain: preserve the captured QQ Music level and let FMOD's native
    // radio effects and the game's radio volume control do all scaling.
    static constexpr float kRadioAudibleGain = 1.0f;
    // Native-DSP PCM can pause during menu/lifecycle churn, then resume
    // at mixer cadence. Keep enough headroom to avoid starving immediately
    // after route transitions while still bounding stale backlog.
    static constexpr size_t kRingBytes  = 1u << 21;  // 2 MB

    FmodInject(InProcessInjector& injector, std::filesystem::path dll_dir);
    ~FmodInject();
    FmodInject(const FmodInject&)            = delete;
    FmodInject& operator=(const FmodInject&) = delete;

    void start();
    void stop();

    // Push S16LE interleaved frames. The channel-count overload normalizes
    // mono / odd source layouts to the bridge's stereo PCM contract before
    // queuing. Returns true if accepted; false if ring is too full.
    bool feed_pcm_s16(const int16_t* samples, size_t frame_count);
    bool feed_pcm_s16(const int16_t* samples, size_t frame_count,
                      uint16_t channels);
    bool feed_pcm_s16(const int16_t* samples, size_t frame_count,
                      uint16_t channels, float source_gain);
    bool feed_pcm_float(const float* samples, size_t frame_count);
    size_t pcm_free_bytes() const {
        return pcm_float_mode_.load(std::memory_order_relaxed)
                   ? float_ring_.free_space()
                   : ring_.free_space();
    }
    size_t pcm_buffered_bytes() const {
        return pcm_float_mode_.load(std::memory_order_relaxed)
                   ? float_ring_.available()
                   : ring_.available();
    }
    bool pcm_accepting() const {
        return !local_audio_hold_.load(std::memory_order_acquire) &&
               (output_accepting_.load(std::memory_order_acquire) ||
                prebuffer_audio_.load(std::memory_order_acquire));
    }
    void clear_pcm() {
        ring_.clear();
        float_ring_.clear();
        pcm_float_mode_.store(false, std::memory_order_relaxed);
    }

    bool is_playing() const { return playing_.load(std::memory_order_relaxed); }

    // v1 status accessor — used by BridgeStateStore in dllmain to drive
    // the read-only web UI. Cheap; called at ~1 Hz from sampler thread.
    struct Status {
        bool      audio_active        = false;
        bool      r10_active          = false;
        bool      migrated_radio_bus  = false; // native radio path active
        uint64_t  channel_handle      = 0;
        uint64_t  channel_group       = 0;
        uint64_t  ring_available      = 0;
        float     output_gain         = 0.0f;
        bool      prebuffer_audio     = false;
        bool      local_audio_hold    = false;
        uint64_t  underruns           = 0;
        uint64_t  native_dsp_calls    = 0;
        uint32_t  native_dsp_len      = 0;
        int       native_dsp_in_ch    = 0;
        int       native_dsp_out_ch   = 0;
        uint64_t  native_channel_i    = 0;
        uint64_t  native_channel_vt   = 0;
        uint32_t  native_channel_508_bits = 0;
        uint64_t  native_channel_530  = 0;
        uint32_t  native_channel_568_bits = 0;
        uint64_t  native_send_state_1c0 = 0;
        uint32_t  native_send_tail_nonzero = 0;
        uint64_t  native_send_tail_hash = 0;
        bool      driving_speed_available = false;
        float     driving_speed_mps = 0.0f;
        float     night_runners_volume_factor = 1.0f;
        bool      night_runners_non_driving_volume_active = false;
        float     night_runners_lazy_cooldown = 0.0f;
        float     night_runners_speed_progress = 0.0f;
        float     night_runners_dynamic_peak_mps = 0.0f;
        float     night_runners_dynamic_buffer_mps = 0.0f;
        uint32_t  night_runners_dynamic_state = 0;
    };
    Status status() const;

    // Output gain applied per-sample inside the native FMOD DSP callback.
    // 0.0 = silent, 1.0 = full before the calibrated PCM lift. Bridge poller
    // toggles based on R10/menu state.
    void set_output_gain(float gain) {
        output_gain_.store(gain, std::memory_order_release);
    }
    float output_gain() const {
        return output_gain_.load(std::memory_order_acquire);
    }

    // Optional policy supplied by the persistent options store. FmodInject
    // owns the game-state polling, so it decides whether menu-open should
    // pause Spotify transport or let Spotify continue silently.
    void set_menu_playback_controls(std::function<bool()> pause_in_menus,
                                    std::function<std::string()> race_start_playback,
                                    std::function<bool()> quick_station_skip,
                                    std::function<bool()> is_playing,
                                    std::function<void()> pause,
                                    std::function<void()> resume,
                                    std::function<bool()> restart_current_track,
                                    std::function<bool()> next_track,
                                    std::function<std::optional<uint32_t>()> current_position_ms,
                                    std::function<uint32_t()> race_restart_threshold_s);
    void set_station_change_track(std::function<bool()> next_track);
    void set_night_runners_controls(
        std::function<bool()> enabled,
        std::function<uint32_t()> stopped_volume_decrease_percent,
        std::function<uint32_t()> max_speed_mph,
        std::function<bool()> curve_enabled,
        std::function<float()> curve_exponent,
        std::function<bool()> lazy_volume_enabled,
        std::function<float()> lazy_hold_seconds,
        std::function<bool()> low_cut_enabled,
        std::function<std::string()> frequency_cut_mode,
        std::function<uint32_t()> low_cut_amount_percent,
        std::function<uint32_t()> low_cut_frequency_hz,
        std::function<bool()> non_driving_volume_enabled,
        std::function<uint32_t()> non_driving_volume_percent,
        std::function<std::optional<float>()> speed_mps,
        std::function<bool()> dynamic_mode,
        std::function<uint32_t()> dynamic_threshold_mph,
        std::function<uint32_t()> dynamic_buffer_percent,
        std::function<float()> dynamic_buffer_fill_seconds,
        std::function<float()> dynamic_increase_seconds,
        std::function<float()> dynamic_decrease_seconds,
        std::function<uint32_t()> dynamic_max_decay_mph_s);

    // Injection gate. When `suppressed` returns true (e.g. "Vanilla Streamer
    // Mode" is the active source), the native DSP + runtime-mute path is held
    // off so the game's own curated Streamer Mode audio plays untouched.
    void set_injection_gate(std::function<bool()> suppressed);
private:
    // RVA resolution — copied from InProcessInjector::rvas() at init.
    // Every field is an RVA into forzahorizon6.exe; absolute addr =
    // module_base + RVA. Values are produced by sigscan.cpp at startup;
    // see `bridge/src/sigscan.h` for anchor strategy. If anything we need
    // is zero, resolve_rvas() logs the gap and aborts init gracefully.
    ResolvedRvas rvas_;
    bool resolve_rvas();

    // Discovery — pulls SystemI* from any active radio Sound's +0xC0.
    // Returns true and sets system_ on success.
    bool discover_system();


    // Startup repair: when the game starts with R10 already selected, FH6 may
    // not instantiate the vanilla radio Channel until a station transition
    // occurs. Nudge through the game's own station-name setter once.
    bool try_startup_radio_nudge();
    bool try_radio_station_nudge(const char* reason);
    NativeDspProbeMode native_dsp_probe_mode_requested() const;
    bool native_dsp_consumes_pcm() const {
        return native_dsp_probe_mode_ == NativeDspProbeMode::Pcm;
    }
    bool resolve_native_r10_channel(void** out_channel_i);
    bool install_native_dsp_probe(void* native_channel);
    int inspect_native_dsp_chain(void* channel, int* out_num_dsps = nullptr);
    void maybe_update_native_dsp_probe(bool r10_active, bool menu_open);
    void remove_native_dsp_probe(const char* reason = nullptr);

    // Runtime-mute path (gated behind spotify-radio/runtime-mute.flag). Mutes the
    // native radio channel during the windows our generator DSP is NOT covering
    // it, so the vanilla curated audio is silenced without the media-folder
    // silent-bank. `apply_native_mute` is idempotent (only calls FMOD on a state
    // change); `restore_native_mute` unmutes whatever we last muted.
    bool runtime_mute_enabled();
    void apply_native_mute(void* channel, bool mute);
    void restore_native_mute();

    void native_dsp_gain_thread_fn();
    bool radio_dj_diag_enabled();
    bool night_diag_heartbeat_enabled();
    bool native_dsp_status_logs_enabled();

    // Init thread — runs discovery + spawn, retries until success or stop.
    void init_thread_fn();

    static FMOD_RESULT native_dsp_read_cb(FMOD_DSP_STATE* dsp_state,
                                          float* inbuffer,
                                          float* outbuffer,
                                          unsigned int length,
                                          int inchannels,
                                          int* outchannels);

    InProcessInjector&  injector_;
    std::filesystem::path dll_dir_;
    std::atomic<bool>   running_{false};
    std::atomic<bool>   playing_{false};
    std::atomic<bool>   migrated_to_radio_bus_{false};
    std::atomic<bool>   startup_radio_nudge_done_{false};
    std::atomic<int>    station_nudge_quick_skip_suppress_ticks_{0};
    std::atomic<int>    quick_station_route_guard_ticks_{0};
    std::thread         init_thread_;
    std::thread         gain_thread_;
    std::mutex          mtx_;

    FMOD_SYSTEM*       system_   = nullptr;
    FMOD_DSP*          native_dsp_probe_ = nullptr;
    void*              native_dsp_target_ = nullptr;
    void*              native_dsp_failed_target_ = nullptr;
    bool               native_dsp_probe_enabled_ = false;

    // Runtime-mute state. flag: -1 unchecked, 0 off, 1 on. mute_target_ = the
    // channel we last applied mute to; mute_state_ = the value we applied.
    int                native_mute_flag_state_ = -1;
    int                radio_dj_diag_flag_state_ = -1;
    int                night_diag_heartbeat_flag_state_ = -1;
    int                native_dsp_status_flag_state_ = -1;
    void*              native_mute_target_ = nullptr;
    bool               native_mute_state_ = false;
    NativeDspProbeMode native_dsp_probe_mode_ = NativeDspProbeMode::Off;
    int                native_dsp_status_tick_ = 0;
    int                native_dsp_retry_cooldown_ = 0;
    int                native_dsp_unresolved_ticks_ = 0;
    int                native_dsp_stalled_unresolved_ticks_ = 0;
    int                native_dsp_retarget_ticks_ = 0;
    int                native_dsp_retarget_nudge_cooldown_ = 0;
    int                native_dsp_chain_check_cooldown_ = 0;
    int                native_dsp_chain_missing_checks_ = 0;
    bool               native_dsp_chain_verified_logged_ = false;
    int                native_dsp_same_target_stalled_ticks_ = 0;
    uint64_t           native_dsp_last_unresolved_calls_ = 0;
    uint64_t           native_dsp_last_retarget_calls_ = 0;
    uint64_t           native_dsp_last_same_target_calls_ = 0;
    std::atomic<uint64_t> native_diag_channel_i_{0};
    std::atomic<uint64_t> native_diag_target_handle_{0};
    std::atomic<uint64_t> native_diag_channel_vt_{0};
    std::atomic<uint32_t> native_diag_channel_508_bits_{0};
    std::atomic<uint64_t> native_diag_channel_530_{0};
    std::atomic<uint32_t> native_diag_channel_568_bits_{0};
    std::atomic<uint64_t> native_diag_send_state_1c0_{0};
    std::atomic<uint32_t> native_diag_send_tail_nonzero_{0};
    std::atomic<uint64_t> native_diag_send_tail_hash_{0};

    // PCM ring (S16LE interleaved) filled by librespotc and drained by the
    // native R10 FMOD DSP callback.
    ByteRingBuffer ring_{kRingBytes};
    // High-fidelity QQ Music path: 48 kHz stereo float at unity gain.
    ByteRingBuffer float_ring_{kRingBytes};
    std::atomic<bool> pcm_float_mode_{false};

    std::atomic<uint64_t> pcm_call_count_{0};
    std::atomic<uint64_t> pcm_limiter_limited_samples_{0};
    std::atomic<uint64_t> underrun_count_{0};
    std::atomic<uint64_t> native_dsp_call_count_{0};
    std::atomic<uint32_t> native_dsp_last_len_{0};
    std::atomic<int>      native_dsp_last_inchannels_{0};
    std::atomic<int>      native_dsp_last_outchannels_{0};
    std::atomic<uint64_t> native_dsp_generated_frames_{0};
    double                native_dsp_pcm_phase_ = 0.0;
    bool                  native_dsp_pcm_have_current_ = false;
    bool                  native_dsp_pcm_have_next_ = false;
    int16_t               native_dsp_pcm_current_[kPcmChannels] = {};
    int16_t               native_dsp_pcm_next_[kPcmChannels] = {};

    // Native DSP output gain. Default 0.0 = silent until the control loop
    // confirms R10 active and menu policy allows playback.
    std::atomic<float> output_gain_{0.0f};
    std::atomic<bool>  output_accepting_{false};
    std::atomic<bool>  prebuffer_audio_{false};
    std::atomic<bool>  local_audio_hold_{false};

    mutable std::mutex playback_controls_mtx_;
    std::function<bool()> injection_suppressed_;  // vanilla mode gate
    std::function<bool()> playback_pause_in_menus_;
    std::function<std::string()> playback_race_start_playback_;
    std::function<bool()> playback_quick_station_skip_;
    std::function<bool()> station_change_next_track_;
    std::function<bool()> playback_is_playing_;
    std::function<void()> playback_pause_;
    std::function<void()> playback_resume_;
    std::function<bool()> playback_restart_current_track_;
    std::function<bool()> playback_next_track_;
    std::function<std::optional<uint32_t>()> playback_current_position_ms_;
    std::function<uint32_t()> playback_race_restart_threshold_s_;

    mutable std::mutex night_runners_controls_mtx_;
    std::function<bool()> night_runners_enabled_;
    std::function<uint32_t()> night_runners_stopped_volume_decrease_percent_;
    std::function<uint32_t()> night_runners_max_speed_mph_;
    std::function<bool()> night_runners_curve_enabled_;
    std::function<float()> night_runners_curve_exponent_;
    std::function<bool()> night_runners_lazy_volume_enabled_;
    std::function<float()> night_runners_lazy_hold_seconds_;
    std::function<bool()> night_runners_low_cut_enabled_;
    std::function<std::string()> night_runners_frequency_cut_mode_;
    std::function<uint32_t()> night_runners_low_cut_amount_percent_;
    std::function<uint32_t()> night_runners_low_cut_frequency_hz_;
    std::function<bool()> night_runners_non_driving_volume_enabled_;
    std::function<uint32_t()> night_runners_non_driving_volume_percent_;
    std::function<std::optional<float>()> night_runners_speed_mps_;
    std::function<bool()> night_runners_dynamic_mode_;
    std::function<uint32_t()> night_runners_dynamic_threshold_mph_;
    std::function<uint32_t()> night_runners_dynamic_buffer_percent_;
    std::function<float()> night_runners_dynamic_buffer_fill_seconds_;
    std::function<float()> night_runners_dynamic_increase_seconds_;
    std::function<float()> night_runners_dynamic_decrease_seconds_;
    std::function<uint32_t()> night_runners_dynamic_max_decay_mph_s_;
    std::atomic<bool>  driving_speed_available_{false};
    std::atomic<float> driving_speed_mps_{0.0f};
    std::atomic<float> night_runners_volume_progress_{1.0f};
    std::atomic<float> night_runners_speed_progress_{0.0f};
    std::atomic<float> night_runners_volume_factor_{1.0f};
    std::atomic<float> night_runners_lazy_cooldown_{0.0f};
    std::atomic<bool>  night_runners_non_driving_volume_active_{false};
    std::atomic<bool>  night_runners_low_cut_enabled_state_{false};
    std::atomic<bool>  night_runners_high_cut_state_{false};
    std::atomic<float> night_runners_low_cut_amount_{0.5f};
    std::atomic<float> night_runners_low_cut_frequency_hz_state_{140.0f};
    std::atomic<float> night_runners_dynamic_peak_mps_{0.0f};
    std::atomic<float> night_runners_dynamic_buffer_mps_{0.0f};
    std::atomic<uint32_t> night_runners_dynamic_state_{0};

    float native_dsp_low_cut_z1_l_ = 0.0f;
    float native_dsp_low_cut_z2_l_ = 0.0f;
    float native_dsp_low_cut_z1_r_ = 0.0f;
    float native_dsp_low_cut_z2_r_ = 0.0f;
    float native_dsp_low_cut_smoothed_amount_ = 0.0f;
    float native_dsp_low_cut_smoothed_frequency_hz_ = 140.0f;
    bool native_dsp_low_cut_high_end_ = false;

};

extern FmodInject* g_fmod_inject;

} // namespace bridge
