// FmodInject — see header.

#include "fmod_inject.h"
#include "injector_inproc.h"
#include "game_profile.h"
#include "log_file.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

namespace {

static constexpr bool kNativeDspVerboseStatusLogs = false;

bool safe_read_qword(uintptr_t addr, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_read_dword(uintptr_t addr, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-wrapped call to FMOD's handle resolver. Done in its own free
// function because /EHsc forbids __try in functions with C++ object
// unwinding (which the caller has, due to std::lock_guard / std::string).
int safe_call_handle_resolver(void* fn, uint32_t handle,
                              void** out_channel_i, void** out_lock) {
    using Fn = int (*)(uint32_t, void**, void**);
    __try {
        return reinterpret_cast<Fn>(fn)(handle, out_channel_i, out_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void safe_call_handle_unlock(void* fn, void* lock) {
    if (!fn || !lock) return;
    using Fn = void (*)(void*);
    __try {
        reinterpret_cast<Fn>(fn)(lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best-effort — if unlock SEHs the FMOD state is already wrecked.
    }
}

int safe_call_create_dsp(void* fn, bridge::FMOD_SYSTEM* system,
                         const bridge::FMOD_DSP_DESCRIPTION* desc,
                         bridge::FMOD_DSP** out_dsp) {
    using Fn = bridge::SystemCreateDSP_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(system, desc, out_dsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_dsp_release(void* fn, bridge::FMOD_DSP* dsp) {
    using Fn = bridge::DSPRelease_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(dsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_add_dsp(void* fn, void* channel_control,
                      bridge::FMOD_DSP* dsp) {
    using Fn = bridge::ChannelControlAddDSP_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(channel_control, 0, dsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_remove_dsp(void* fn, void* channel_control,
                         bridge::FMOD_DSP* dsp) {
    using Fn = bridge::ChannelControlRemoveDSP_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(channel_control, dsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_set_mute(void* fn, void* channel_control, bool mute) {
    using Fn = bridge::ChannelControlSetMute_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(channel_control, mute ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_get_num_dsps(void* fn, void* channel_control, int* numdsps) {
    using Fn = bridge::ChannelControlGetNumDSPs_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(channel_control, numdsps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int safe_call_get_dsp(void* fn, void* channel_control, int index,
                      bridge::FMOD_DSP** out_dsp) {
    using Fn = bridge::ChannelControlGetDSP_fn;
    __try {
        return reinterpret_cast<Fn>(fn)(channel_control, index, out_dsp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool safe_call_radio_set_station(void* fn, void* state,
                                 const std::string& station_name) {
    using Fn = void (*)(void*, const std::string*);
    __try {
        reinterpret_cast<Fn>(fn)(state, &station_name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string hex(uintptr_t v) {
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

std::string sanitize_log_field(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == '|') ch = ' ';
    }
    if (value.size() > 96) {
        value.resize(93);
        value += "...";
    }
    return value;
}

std::string radio_dj_diag_signature(
    const bridge::InProcessInjector::RadioStreamDebugSnapshot& snap,
    bool streamer_active,
    bool menu_open,
    bool injection_suppressed) {
    std::string out;
    out.reserve(512);
    out += "streamer=" + std::to_string(streamer_active);
    out += " menu=" + std::to_string(menu_open);
    out += " vanilla_mode=" + std::to_string(injection_suppressed);
    out += " station=\"" + sanitize_log_field(snap.selected_station_name) + "\"";
    out += " station_ptr=" + hex(snap.selected_station);
    out += " streams=" + std::to_string(snap.streams.size());

    size_t active_count = 0;
    for (const auto& e : snap.streams) {
        if (e.active) ++active_count;
    }
    out += " active_streams=" + std::to_string(active_count);

    for (size_t i = 0; i < snap.streams.size(); ++i) {
        const auto& e = snap.streams[i];
        if (!e.active && e.handle == 0 && e.sound_name.empty() &&
            e.display_name.empty()) {
            continue;
        }
        out += " | [" + std::to_string(i) + "]";
        out += " wrapper=" + hex(e.wrapper);
        out += " handle=" + hex(static_cast<uintptr_t>(e.handle));
        out += " active=" + std::to_string(e.active);
        out += " sound_ptr=" + hex(e.fmod_sound);
        out += " props=" + hex(e.sample_properties);
        if (!e.sound_name.empty()) {
            out += " sound=\"" + sanitize_log_field(e.sound_name) + "\"";
        }
        if (!e.display_name.empty()) {
            out += " display=\"" + sanitize_log_field(e.display_name) + "\"";
        }
        if (!e.artist.empty()) {
            out += " artist=\"" + sanitize_log_field(e.artist) + "\"";
        }
    }
    return out;
}

bool is_expected_native_dsp_remove_result(int result) {
    return result == 3 || result == 30;
}

const char* native_dsp_remove_result_class(int result) {
    switch (result) {
        case 3: return "stale-target";
        case 30: return "radio-off";
        default: return "unexpected";
    }
}

const char* native_dsp_probe_mode_name(bridge::NativeDspProbeMode mode) {
    switch (mode) {
        case bridge::NativeDspProbeMode::Passthrough: return "passthrough";
        case bridge::NativeDspProbeMode::Silence: return "silence";
        case bridge::NativeDspProbeMode::Tone: return "tone";
        case bridge::NativeDspProbeMode::Pcm: return "pcm";
        default: return "off";
    }
}

int16_t clamp_s16(float sample) {
    if (sample > 32767.0f) return 32767;
    if (sample < -32768.0f) return -32768;
    return static_cast<int16_t>(sample);
}

float sanitize_source_gain(float gain) {
    if (!std::isfinite(gain)) return 1.0f;
    return std::clamp(gain, 0.0f, 8.0f);
}

float soft_limit_pcm(float sample, bool& limited) {
    if (!std::isfinite(sample)) {
        limited = true;
        return 0.0f;
    }

    constexpr float kKnee = 0.98f;
    constexpr float kCeiling = 0.9995f;
    float abs_sample = std::fabs(sample);
    if (abs_sample <= kKnee) return sample;

    limited = true;
    float range = kCeiling - kKnee;
    float shaped = kKnee + range * (1.0f - std::exp(-(abs_sample - kKnee) / range));
    shaped = std::min(shaped, kCeiling);
    return std::copysign(shaped, sample);
}

int16_t s16_with_gain_limited(int16_t sample, float gain, uint64_t& limited) {
    float normalized = (static_cast<float>(sample) / 32768.0f) * gain;
    bool was_limited = false;
    normalized = soft_limit_pcm(normalized, was_limited);
    if (was_limited) ++limited;
    float scaled = normalized * 32768.0f;
    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return static_cast<int16_t>(std::lrint(scaled));
}

float clamp_float_audio(float sample) {
    if (sample > 1.0f) return 1.0f;
    if (sample < -1.0f) return -1.0f;
    return sample;
}

struct BiquadCoefficients {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
};

BiquadCoefficients make_highpass(float cutoff_hz) {
    constexpr float kSampleRate = 48000.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kQ = 0.70710678f;
    cutoff_hz = std::clamp(cutoff_hz, 40.0f, 320.0f);
    float omega = 2.0f * kPi * cutoff_hz / kSampleRate;
    float sin_omega = std::sin(omega);
    float cos_omega = std::cos(omega);
    float alpha = sin_omega / (2.0f * kQ);

    float b0 = (1.0f + cos_omega) * 0.5f;
    float b1 = -(1.0f + cos_omega);
    float b2 = (1.0f + cos_omega) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_omega;
    float a2 = 1.0f - alpha;

    return {
        b0 / a0,
        b1 / a0,
        b2 / a0,
        a1 / a0,
        a2 / a0,
    };
}

BiquadCoefficients make_lowpass(float cutoff_hz) {
    constexpr float kSampleRate = 48000.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kQ = 0.70710678f;
    cutoff_hz = std::clamp(cutoff_hz, 1200.0f, 12000.0f);
    float omega = 2.0f * kPi * cutoff_hz / kSampleRate;
    float sin_omega = std::sin(omega);
    float cos_omega = std::cos(omega);
    float alpha = sin_omega / (2.0f * kQ);

    float b0 = (1.0f - cos_omega) * 0.5f;
    float b1 = 1.0f - cos_omega;
    float b2 = (1.0f - cos_omega) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cos_omega;
    float a2 = 1.0f - alpha;

    return {
        b0 / a0,
        b1 / a0,
        b2 / a0,
        a1 / a0,
        a2 / a0,
    };
}

float process_biquad(float sample,
                     const BiquadCoefficients& c,
                     float& z1,
                     float& z2) {
    float out = c.b0 * sample + z1;
    z1 = c.b1 * sample - c.a1 * out + z2;
    z2 = c.b2 * sample - c.a2 * out;
    return out;
}

float f32_from_bits(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string read_text_file_first_word(const std::filesystem::path& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[--n] = 0;
    }
    return std::string(buf);
}

} // anonymous namespace

namespace bridge {

FmodInject* g_fmod_inject = nullptr;

FmodInject::FmodInject(InProcessInjector& injector,
                       std::filesystem::path dll_dir)
    : injector_(injector), dll_dir_(std::move(dll_dir)) {
    native_dsp_probe_mode_ = native_dsp_probe_mode_requested();
    native_dsp_probe_enabled_ =
        native_dsp_probe_mode_ != NativeDspProbeMode::Off;
}

FmodInject::~FmodInject() { stop(); }

void FmodInject::set_menu_playback_controls(
    std::function<bool()> pause_in_menus,
    std::function<std::string()> race_start_playback,
    std::function<bool()> quick_station_skip,
    std::function<bool()> is_playing,
    std::function<void()> pause,
    std::function<void()> resume,
    std::function<bool()> restart_current_track,
    std::function<bool()> next_track,
    std::function<std::optional<uint32_t>()> current_position_ms,
    std::function<uint32_t()> race_restart_threshold_s) {
    std::lock_guard lock(playback_controls_mtx_);
    playback_pause_in_menus_ = std::move(pause_in_menus);
    playback_race_start_playback_ = std::move(race_start_playback);
    playback_quick_station_skip_ = std::move(quick_station_skip);
    playback_is_playing_ = std::move(is_playing);
    playback_pause_ = std::move(pause);
    playback_resume_ = std::move(resume);
    playback_restart_current_track_ = std::move(restart_current_track);
    playback_next_track_ = std::move(next_track);
    playback_current_position_ms_ = std::move(current_position_ms);
    playback_race_restart_threshold_s_ = std::move(race_restart_threshold_s);
}

void FmodInject::set_station_change_track(std::function<bool()> next_track) {
    std::lock_guard lock(playback_controls_mtx_);
    station_change_next_track_ = std::move(next_track);
}

void FmodInject::set_injection_gate(std::function<bool()> suppressed) {
    std::lock_guard lock(playback_controls_mtx_);
    injection_suppressed_ = std::move(suppressed);
}

void FmodInject::set_night_runners_controls(
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
    std::function<uint32_t()> dynamic_max_decay_mph_s) {
    std::lock_guard lock(night_runners_controls_mtx_);
    night_runners_enabled_ = std::move(enabled);
    night_runners_stopped_volume_decrease_percent_ =
        std::move(stopped_volume_decrease_percent);
    night_runners_max_speed_mph_ = std::move(max_speed_mph);
    night_runners_curve_enabled_ = std::move(curve_enabled);
    night_runners_curve_exponent_ = std::move(curve_exponent);
    night_runners_lazy_volume_enabled_ = std::move(lazy_volume_enabled);
    night_runners_lazy_hold_seconds_ = std::move(lazy_hold_seconds);
    night_runners_low_cut_enabled_ = std::move(low_cut_enabled);
    night_runners_frequency_cut_mode_ = std::move(frequency_cut_mode);
    night_runners_low_cut_amount_percent_ = std::move(low_cut_amount_percent);
    night_runners_low_cut_frequency_hz_ = std::move(low_cut_frequency_hz);
    night_runners_non_driving_volume_enabled_ =
        std::move(non_driving_volume_enabled);
    night_runners_non_driving_volume_percent_ =
        std::move(non_driving_volume_percent);
    night_runners_speed_mps_ = std::move(speed_mps);
    night_runners_dynamic_mode_ = std::move(dynamic_mode);
    night_runners_dynamic_threshold_mph_ = std::move(dynamic_threshold_mph);
    night_runners_dynamic_buffer_percent_ = std::move(dynamic_buffer_percent);
    night_runners_dynamic_buffer_fill_seconds_ =
        std::move(dynamic_buffer_fill_seconds);
    night_runners_dynamic_increase_seconds_ =
        std::move(dynamic_increase_seconds);
    night_runners_dynamic_decrease_seconds_ =
        std::move(dynamic_decrease_seconds);
    night_runners_dynamic_max_decay_mph_s_ =
        std::move(dynamic_max_decay_mph_s);
}

// ── Lifecycle ──────────────────────────────────────────────────────

void FmodInject::start() {
    if (running_.exchange(true)) return;
    g_fmod_inject = this;
    init_thread_ = std::thread(&FmodInject::init_thread_fn, this);
    log::info("[fmod-inject] start()");
    if (native_dsp_probe_enabled_) {
        log::info(std::string("[native-dsp] native PCM bridge enabled mode=")
                  + native_dsp_probe_mode_name(native_dsp_probe_mode_));
    }
}

void FmodInject::stop() {
    if (!running_.exchange(false)) return;
    if (init_thread_.joinable()) init_thread_.join();
    if (gain_thread_.joinable()) gain_thread_.join();
    remove_native_dsp_probe();
    if (g_fmod_inject == this) g_fmod_inject = nullptr;
    log::info("[fmod-inject] stop()");
}

// ── RVA resolution ────────────────────────────────────────────────

bool FmodInject::resolve_rvas() {
    // All RVAs are sigscan-resolved at startup by InProcessInjector::attach()
    // and stored on the injector. We just snapshot them here so the rest of
    // FmodInject can work against a stable local copy. See sigscan.cpp for
    // the anchor strategy (FMOD __FUNCTION__ strings + .pdata function-start
    // lookup for wrappers; byte patterns + RIP+disp32 decoding for the
    // internal helpers and data globals).
    rvas_ = injector_.rvas();

    // FH6 lazy-decrypts critical .text blocks the first time the game calls
    // into them. Ask the injector to re-scan missing native-DSP prerequisites
    // each pass; once the game warms up, the bytes become scannable.
    //
    // Gate on the DSP wrappers + handle resolver + RadioState singleton only.
    // The startup station-nudge (radio_set_station_by_name) is OPTIONAL — it is
    // null on games with no portable nudge anchor (e.g. FH5 selects stations by
    // hashed id), and the user can tune Streamer Mode manually. Requiring it
    // here wrongly kept native DSP audio dormant on FH5 even though every audio
    // RVA resolved and RadioStreamFmod discovery succeeded.
    if (!rvas_.fmod_dsp_ok() || !rvas_.fmod_handle_ok() ||
        !rvas_.radio_state_ok()) {
        injector_.retry_signatures();
        rvas_ = injector_.rvas();
    }

    if (!rvas_.fmod_dsp_ok() || !rvas_.fmod_handle_ok() ||
        !rvas_.radio_state_ok()) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            log::warn("[native-dsp] required FMOD/radio RVAs missing — "
                      "native DSP audio is dormant "
                      "and will keep retrying every 30s in case the wrapper "
                      "bytes are lazy-decrypted.");
        }
        return false;
    }

    // Announce the recovery exactly once so the log captures the moment the
    // wrapper became scannable.
    static std::atomic<bool> ready_announced{false};
    if (!ready_announced.exchange(true)) {
        log::info("[native-dsp] Required FMOD/radio RVAs resolved; "
                  "native PCM bridge going live.");
    }
    return true;
}

// ── SystemI* discovery ─────────────────────────────────────────────
// Pull SystemI* from any active radio Sound's back-ref pointer.
// Sound layout (see project_fmod_internals.md):
//   wrapper+0x18                       → Sound*
//   Sound+sound_system_backref_offset  → SystemI* (back-ref; FH6 0xC0 / FH5 0xD8)
bool FmodInject::discover_system() {
    const auto& instances = injector_.radio_instances();
    if (instances.empty()) return false;

    const uintptr_t sys_backref_off =
        active_profile().radio.sound_system_backref_offset;

    for (auto inst : instances) {
        uintptr_t sound = 0;
        if (!safe_read_qword(inst + 0x18, sound) || !sound) continue;

        uintptr_t sys = 0;
        if (!safe_read_qword(sound + sys_backref_off, sys) || !sys) continue;

        // Validate vtable is in module range.
        uintptr_t mod_base = injector_.module_base();
        size_t    mod_size = injector_.module_size();
        uintptr_t vt = 0;
        if (!safe_read_qword(sys, vt) || vt < mod_base || vt >= mod_base + mod_size)
            continue;

        system_ = reinterpret_cast<FMOD_SYSTEM*>(sys);
        log::info("[fmod-inject] SystemI* = " + hex(sys)
                  + " (vtable=" + hex(vt) + ")");
        return true;
    }
    return false;
}

// ── PCM intake/status ───────────────────────────────────────────────
bool FmodInject::feed_pcm_s16(const int16_t* samples, size_t frame_count) {
    return feed_pcm_s16(samples, frame_count, kPcmChannels, 1.0f);
}

bool FmodInject::feed_pcm_s16(const int16_t* samples, size_t frame_count,
                              uint16_t channels) {
    return feed_pcm_s16(samples, frame_count, channels, 1.0f);
}

bool FmodInject::feed_pcm_s16(const int16_t* samples, size_t frame_count,
                              uint16_t channels, float source_gain) {
    if (!samples || !frame_count) return true;
    if (channels == 0) return true;
    if (frame_count >
        std::numeric_limits<size_t>::max() / static_cast<size_t>(channels)) {
        return false;
    }

    // During menu-triggered transport pause, reject further PCM and keep the
    // local ring empty so no pre-menu audio leaks after resume.
    if (local_audio_hold_.load(std::memory_order_acquire)) {
        if (pcm_buffered_bytes() > 0) clear_pcm();
        return false;
    }

    // If the game is not currently meant to hear Spotify, do not queue PCM.
    // FMOD may virtualize a mixer-muted Channel and stop pulling callbacks,
    // so accepting audio here would build a stale backlog that plays later.
    if (!output_accepting_.load(std::memory_order_acquire) &&
        !prebuffer_audio_.load(std::memory_order_acquire)) {
        if (pcm_buffered_bytes() > 0) clear_pcm();
        return true;
    }

    if (frame_count >
        std::numeric_limits<size_t>::max() / (kPcmChannels * sizeof(int16_t))) {
        return false;
    }
    size_t bytes = frame_count * kPcmChannels * sizeof(int16_t);
    if (ring_.free_space() < bytes) return false;
    if (pcm_float_mode_.exchange(false, std::memory_order_relaxed)) {
        float_ring_.clear();
    }

    source_gain = sanitize_source_gain(source_gain);
    if (channels == kPcmChannels &&
        std::fabs(source_gain - 1.0f) < 0.001f) {
        return ring_.write(samples, bytes) == bytes;
    }

    thread_local std::vector<int16_t> stereo;
    stereo.resize(frame_count * kPcmChannels);
    uint64_t limited = 0;
    bool apply_gain = std::fabs(source_gain - 1.0f) >= 0.001f;
    if (channels == 1) {
        for (size_t i = 0; i < frame_count; ++i) {
            int16_t sample = apply_gain
                ? s16_with_gain_limited(samples[i], source_gain, limited)
                : samples[i];
            stereo[i * 2] = sample;
            stereo[i * 2 + 1] = sample;
        }
    } else {
        for (size_t i = 0; i < frame_count; ++i) {
            size_t in_base = i * static_cast<size_t>(channels);
            stereo[i * 2] = apply_gain
                ? s16_with_gain_limited(samples[in_base], source_gain, limited)
                : samples[in_base];
            stereo[i * 2 + 1] = apply_gain
                ? s16_with_gain_limited(samples[in_base + 1], source_gain, limited)
                : samples[in_base + 1];
        }
    }
    if (limited) {
        pcm_limiter_limited_samples_.fetch_add(limited, std::memory_order_relaxed);
    }
    return ring_.write(stereo.data(), bytes) == bytes;
}

bool FmodInject::feed_pcm_float(const float* samples, size_t frame_count) {
    if (!samples || !frame_count) return true;

    if (local_audio_hold_.load(std::memory_order_acquire)) {
        if (pcm_buffered_bytes() > 0) clear_pcm();
        return false;
    }
    if (!output_accepting_.load(std::memory_order_acquire) &&
        !prebuffer_audio_.load(std::memory_order_acquire)) {
        if (pcm_buffered_bytes() > 0) clear_pcm();
        return true;
    }
    if (frame_count >
        std::numeric_limits<size_t>::max() /
            (kPcmChannels * sizeof(float))) {
        return false;
    }

    const size_t bytes = frame_count * kPcmChannels * sizeof(float);
    if (float_ring_.free_space() < bytes) return false;
    if (!pcm_float_mode_.exchange(true, std::memory_order_relaxed)) {
        ring_.clear();
    }
    return float_ring_.write(samples, bytes) == bytes;
}

FmodInject::Status FmodInject::status() const {
    Status s;
    s.audio_active       = playing_.load(std::memory_order_acquire);
    s.r10_active         = injector_.is_spotify_station_active();
    s.migrated_radio_bus =
        migrated_to_radio_bus_.load(std::memory_order_acquire);
    s.channel_handle     =
        native_diag_target_handle_.load(std::memory_order_acquire);
    s.channel_group      = 0;
    s.ring_available     = static_cast<uint64_t>(pcm_buffered_bytes());
    s.output_gain        = output_gain_.load(std::memory_order_acquire);
    s.prebuffer_audio    = prebuffer_audio_.load(std::memory_order_acquire);
    s.local_audio_hold   = local_audio_hold_.load(std::memory_order_acquire);
    s.underruns          = underrun_count_.load(std::memory_order_acquire);
    s.native_dsp_calls   = native_dsp_call_count_.load(std::memory_order_acquire);
    s.native_dsp_len     = native_dsp_last_len_.load(std::memory_order_acquire);
    s.native_dsp_in_ch   = native_dsp_last_inchannels_.load(std::memory_order_acquire);
    s.native_dsp_out_ch  = native_dsp_last_outchannels_.load(std::memory_order_acquire);
    s.native_channel_i   = native_diag_channel_i_.load(std::memory_order_acquire);
    s.native_channel_vt  = native_diag_channel_vt_.load(std::memory_order_acquire);
    s.native_channel_508_bits =
        native_diag_channel_508_bits_.load(std::memory_order_acquire);
    s.native_channel_530 = native_diag_channel_530_.load(std::memory_order_acquire);
    s.native_channel_568_bits =
        native_diag_channel_568_bits_.load(std::memory_order_acquire);
    s.native_send_state_1c0 =
        native_diag_send_state_1c0_.load(std::memory_order_acquire);
    s.native_send_tail_nonzero =
        native_diag_send_tail_nonzero_.load(std::memory_order_acquire);
    s.native_send_tail_hash =
        native_diag_send_tail_hash_.load(std::memory_order_acquire);
    s.driving_speed_available =
        driving_speed_available_.load(std::memory_order_acquire);
    s.driving_speed_mps =
        driving_speed_mps_.load(std::memory_order_acquire);
    s.night_runners_volume_factor =
        night_runners_volume_factor_.load(std::memory_order_acquire);
    s.night_runners_non_driving_volume_active =
        night_runners_non_driving_volume_active_.load(std::memory_order_acquire);
    s.night_runners_lazy_cooldown =
        night_runners_lazy_cooldown_.load(std::memory_order_acquire);
    s.night_runners_speed_progress =
        night_runners_speed_progress_.load(std::memory_order_acquire);
    s.night_runners_dynamic_peak_mps =
        night_runners_dynamic_peak_mps_.load(std::memory_order_acquire);
    s.night_runners_dynamic_buffer_mps =
        night_runners_dynamic_buffer_mps_.load(std::memory_order_acquire);
    s.night_runners_dynamic_state =
        night_runners_dynamic_state_.load(std::memory_order_acquire);
    return s;
}

// ── Init thread ────────────────────────────────────────────────────

void FmodInject::init_thread_fn() {
    log::info("[fmod-inject] Init thread started");

    // Do not wait for injector_.is_ready() here: that predicate includes the
    // runtime-logo/Game Ready gate. Native audio must start as soon as metadata
    // discovery has produced the radio wrappers, otherwise logo patch latency
    // can delay FMOD setup and the startup channel bootstrap.
    while (running_.load() && !injector_.discovery_ready())
        std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!running_.load()) return;

    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto audio_init_ready_at = std::chrono::steady_clock::now();
    auto next_startup_nudge = audio_init_ready_at;
    int startup_nudge_attempts = 0;

    // Try resolve + discover + spawn. Retry every ~1 s on failure.
    while (running_.load()) {
        if (!playing_.load(std::memory_order_acquire)) {
            std::lock_guard lock(mtx_);
            if (!resolve_rvas()) {
                // Resolver failed — wait before re-checking so we don't spam logs.
                std::this_thread::sleep_for(std::chrono::seconds(30));
                continue;
            }
            if (!discover_system()) {
                auto now = std::chrono::steady_clock::now();
                bool speed_ready = injector_.vehicle_speed_mps().has_value();
                bool fallback_ready =
                    now - audio_init_ready_at >= std::chrono::seconds(45);
                if (!startup_radio_nudge_done_.load(std::memory_order_acquire) &&
                    (speed_ready || fallback_ready) &&
                    now >= next_startup_nudge) {
                    startup_nudge_attempts++;
                    log::info("[native-dsp] Native FMOD system not visible; "
                              "performing startup radio nudge attempt="
                              + std::to_string(startup_nudge_attempts)
                              + " speed_ready=" + std::to_string(speed_ready)
                              + " fallback_ready="
                              + std::to_string(fallback_ready));
                    bool ok = try_radio_station_nudge("Startup radio nudge");
                    native_dsp_retarget_nudge_cooldown_ = 300;
                    next_startup_nudge =
                        now + (ok ? std::chrono::seconds(15)
                                  : std::chrono::seconds(5));
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            if (!startup_radio_nudge_done_.exchange(
                    true, std::memory_order_acq_rel)) {
                log::info("[native-dsp] Native FMOD system discovered; "
                          "startup bootstrap complete after nudge_attempts="
                          + std::to_string(startup_nudge_attempts));
            }
            playing_.store(true, std::memory_order_release);
            migrated_to_radio_bus_.store(true, std::memory_order_release);
            if (playing_.load(std::memory_order_acquire) &&
                !gain_thread_.joinable()) {
                gain_thread_ = std::thread(&FmodInject::native_dsp_gain_thread_fn, this);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    log::info("[fmod-inject] Init thread exiting");
}

bool FmodInject::try_startup_radio_nudge() {
    bool expected = false;
    if (!startup_radio_nudge_done_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    bool ok = try_radio_station_nudge("Startup radio nudge");
    if (!ok) {
        startup_radio_nudge_done_.store(false, std::memory_order_release);
    }
    return ok;
}

bool FmodInject::try_radio_station_nudge(const char* reason) {
    if (!rvas_.radio_nudge_ok()) {
        const std::string msg =
            std::string("[fmod-inject] ") + reason + " unavailable — "
            "station setter signature missing";
        if (bridge::active_profile().id == bridge::GameId::FH5) {
            log::info(msg + " (expected on FH5; native DSP continues without "
                      "startup station nudge)");
        } else {
            log::warn(msg);
        }
        return false;
    }

    uintptr_t state = 0;
    uintptr_t mod_base = injector_.module_base();
    if (!safe_read_qword(mod_base + rvas_.radio_state_singleton, state) || !state) {
        log::warn(std::string("[fmod-inject] ") + reason
                  + " skipped — RadioState null");
        return false;
    }

    void* set_station =
        reinterpret_cast<void*>(mod_base + rvas_.radio_set_station_by_name);

    log::info(std::string("[fmod-inject] ") + reason
              + ": StationOff -> Streamer Mode");
    station_nudge_quick_skip_suppress_ticks_.store(120,
                                                   std::memory_order_release);
    bool off_ok = safe_call_radio_set_station(set_station, reinterpret_cast<void*>(state),
                                              std::string("StationOff"));
    if (!off_ok) {
        log::warn(std::string("[fmod-inject] ") + reason
                  + " failed at StationOff");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool r10_ok = safe_call_radio_set_station(set_station, reinterpret_cast<void*>(state),
                                              std::string("Streamer Mode"));
    if (!r10_ok) {
        log::warn(std::string("[fmod-inject] ") + reason
                  + " failed at Streamer Mode");
        return false;
    }

    // Force the resolver to re-check after the game has observed a real
    // station transition. Existing topology fallback remains as a guardrail.
    log::info(std::string("[fmod-inject] ") + reason + " complete");
    return true;
}

NativeDspProbeMode FmodInject::native_dsp_probe_mode_requested() const {
#ifdef SPOTIFY_RADIO_DIAG
    wchar_t env[32] = {};
    DWORD n = GetEnvironmentVariableW(L"SPOTIFY_RADIO_NATIVE_DSP_PROBE",
                                      env, (DWORD)(sizeof(env) / sizeof(env[0])));
    if (n > 0 && n < (DWORD)(sizeof(env) / sizeof(env[0]))) {
        if (wcscmp(env, L"1") == 0 || _wcsicmp(env, L"true") == 0 ||
            _wcsicmp(env, L"yes") == 0 || _wcsicmp(env, L"dsp") == 0 ||
            _wcsicmp(env, L"native-dsp") == 0) {
            return NativeDspProbeMode::Passthrough;
        }
        if (_wcsicmp(env, L"dsp-silence") == 0 ||
            _wcsicmp(env, L"silence") == 0) {
            return NativeDspProbeMode::Silence;
        }
        if (_wcsicmp(env, L"dsp-tone") == 0 ||
            _wcsicmp(env, L"tone") == 0) {
            return NativeDspProbeMode::Tone;
        }
        if (_wcsicmp(env, L"dsp-pcm") == 0 ||
            _wcsicmp(env, L"pcm") == 0) {
            return NativeDspProbeMode::Pcm;
        }
    }

    std::error_code ec;
    auto flag = dll_dir_ / "spotify-radio" / "native-stream-probe.flag";
    if (!std::filesystem::exists(flag, ec)) return NativeDspProbeMode::Pcm;
    std::string mode = read_text_file_first_word(flag);
    if (mode == "dsp" || mode == "native-dsp") {
        return NativeDspProbeMode::Passthrough;
    }
    if (mode == "dsp-silence" || mode == "silence") {
        return NativeDspProbeMode::Silence;
    }
    if (mode == "dsp-tone" || mode == "tone") {
        return NativeDspProbeMode::Tone;
    }
    if (mode == "dsp-pcm" || mode == "pcm") {
        return NativeDspProbeMode::Pcm;
    }
    return NativeDspProbeMode::Pcm;
#else
    return NativeDspProbeMode::Pcm;
#endif
}

bool FmodInject::resolve_native_r10_channel(void** out_channel_i) {
    if (!out_channel_i) return false;
    *out_channel_i = nullptr;
    if (!rvas_.fmod_handle_resolver) return false;

    uintptr_t wrapper = 0;
    uint32_t handle = 0;
    bool has_sound = false;
    // In media-free mode (the default) the patched wired sample is absent, so
    // anchor on the active Streamer Mode stream regardless of which track is
    // loaded. We only reach here while Streamer Mode is the selected station.
    if (!injector_.spotify_radio_stream_handle(wrapper, handle, has_sound,
                                               runtime_mute_enabled()) ||
        !handle) {
        return false;
    }

    uintptr_t mod_base = injector_.module_base();
    void* resolver = reinterpret_cast<void*>(mod_base + rvas_.fmod_handle_resolver);
    void* unlock_fn = rvas_.fmod_handle_unlock
        ? reinterpret_cast<void*>(mod_base + rvas_.fmod_handle_unlock)
        : nullptr;

    void* channel_i = nullptr;
    void* lock_obj = nullptr;
    FMOD_RESULT r = safe_call_handle_resolver(resolver, handle, &channel_i, &lock_obj);
    safe_call_handle_unlock(unlock_fn, lock_obj);
    if (r != FMOD_OK || !channel_i) return false;

    uintptr_t vt = 0;
    if (!safe_read_qword(reinterpret_cast<uintptr_t>(channel_i), vt) ||
        vt < mod_base || vt >= mod_base + injector_.module_size()) {
        return false;
    }

    uintptr_t channel_addr = reinterpret_cast<uintptr_t>(channel_i);
    uint32_t channel_508_bits = 0;
    uintptr_t channel_530 = 0;
    uint32_t channel_568_bits = 0;
    uintptr_t send_state = 0;
    uint32_t send_tail_nonzero = 0;
    uint64_t send_tail_hash = 1469598103934665603ull;
    safe_read_dword(channel_addr + 0x508, channel_508_bits);
    safe_read_qword(channel_addr + 0x530, channel_530);
    safe_read_dword(channel_addr + 0x568, channel_568_bits);
    if (safe_read_qword(channel_addr + 0x1C0, send_state) && send_state) {
        for (uintptr_t off = 0x0D0; off <= 0x0F8; off += 8) {
            uintptr_t v = 0;
            if (!safe_read_qword(send_state + off, v)) {
                send_tail_hash ^= 0xBAD00000ull + off;
                send_tail_hash *= 1099511628211ull;
                continue;
            }
            if (v) ++send_tail_nonzero;
            send_tail_hash ^= static_cast<uint64_t>(v);
            send_tail_hash *= 1099511628211ull;
        }
    } else {
        send_state = 0;
        send_tail_hash = 0;
    }
    native_diag_channel_i_.store(channel_addr, std::memory_order_release);
    native_diag_target_handle_.store(handle, std::memory_order_release);
    native_diag_channel_vt_.store(vt, std::memory_order_release);
    native_diag_channel_508_bits_.store(channel_508_bits, std::memory_order_release);
    native_diag_channel_530_.store(channel_530, std::memory_order_release);
    native_diag_channel_568_bits_.store(channel_568_bits, std::memory_order_release);
    native_diag_send_state_1c0_.store(send_state, std::memory_order_release);
    native_diag_send_tail_nonzero_.store(send_tail_nonzero,
                                         std::memory_order_release);
    native_diag_send_tail_hash_.store(send_tail_hash, std::memory_order_release);

    // Public FMOD ChannelControl wrappers take the same opaque handle pointer
    // shape encoded in the native radio handle. The resolver is only a
    // validation step here; passing ChannelI* to addDSP returned FMOD error 30.
    // Parent ChannelGroups bypass some channel-local station/garage processing.
    *out_channel_i = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
    return true;
}

bool FmodInject::install_native_dsp_probe(void* native_channel) {
    if (!native_channel || !rvas_.fmod_dsp_ok() || !system_) return false;

    uintptr_t mod_base = injector_.module_base();
    auto create_dsp = reinterpret_cast<void*>(mod_base + rvas_.systemCreateDSP);
    auto add_dsp = reinterpret_cast<void*>(mod_base + rvas_.channelControlAddDSP);

    FMOD_DSP_DESCRIPTION desc{};
    // FH6 uses FMOD 1.10.x. Try the minor-stable SDK value first; if this
    // build rejects it, retry exact and old values before giving up.
    unsigned int versions[] = {0x00011000u, 0x00011003u, 0x00010000u};
    strcpy_s(desc.name, "FH6 Spotify native PCM");
    desc.version = 1;
    // Generator DSP: we synthesize output from the PCM ring and never read the
    // channel's input, so declare 0 input buffers (per the FMOD plugin RE note).
    // Avoids FMOD running/feeding R10's upstream input chain for a DSP that
    // ignores it. The read callback already tolerates inchannels==0.
    desc.numinputbuffers = 0;
    desc.numoutputbuffers = 1;
    desc.read = &FmodInject::native_dsp_read_cb;
    desc.userdata = this;

    FMOD_DSP* dsp = nullptr;
    FMOD_RESULT create_result = -1;
    unsigned int used_version = 0;
    for (unsigned int version : versions) {
        desc.pluginsdkversion = version;
        dsp = nullptr;
        create_result = safe_call_create_dsp(create_dsp, system_, &desc, &dsp);
        if (create_result == FMOD_OK && dsp) {
            used_version = version;
            break;
        }
    }
    if (create_result != FMOD_OK || !dsp) {
        log::warn("[native-dsp] createDSP failed r="
                  + std::to_string(create_result));
        native_dsp_failed_target_ = native_channel;
        native_dsp_retry_cooldown_ = 20;
        return false;
    }

    FMOD_RESULT add_result = safe_call_add_dsp(add_dsp, native_channel, dsp);
    if (add_result != FMOD_OK) {
        log::warn("[native-dsp] ChannelControl::addDSP failed r="
                  + std::to_string(add_result)
                  + " target=" + hex(reinterpret_cast<uintptr_t>(native_channel)));
        auto release = reinterpret_cast<void*>(mod_base + rvas_.dspRelease);
        safe_call_dsp_release(release, dsp);
        native_dsp_failed_target_ = native_channel;
        native_dsp_retry_cooldown_ = 20;
        return false;
    }

    native_dsp_probe_ = dsp;
    native_dsp_target_ = native_channel;
    native_dsp_failed_target_ = nullptr;
    native_dsp_retry_cooldown_ = 0;
    native_dsp_status_tick_ = 0;
    native_dsp_retarget_ticks_ = 0;
    native_dsp_call_count_.store(0, std::memory_order_relaxed);
    native_dsp_last_retarget_calls_ = 0;
    native_dsp_last_len_.store(0, std::memory_order_relaxed);
    native_dsp_last_inchannels_.store(0, std::memory_order_relaxed);
    native_dsp_last_outchannels_.store(0, std::memory_order_relaxed);
    native_dsp_generated_frames_.store(0, std::memory_order_relaxed);
    native_dsp_unresolved_ticks_ = 0;
    native_dsp_chain_check_cooldown_ = 50;
    native_dsp_chain_missing_checks_ = 0;
    native_dsp_chain_verified_logged_ = false;
    native_dsp_same_target_stalled_ticks_ = 0;
    native_dsp_last_same_target_calls_ = 0;
    native_dsp_pcm_phase_ = 0.0;
    native_dsp_pcm_have_current_ = false;
    native_dsp_pcm_have_next_ = false;
    native_dsp_pcm_current_[0] = 0;
    native_dsp_pcm_current_[1] = 0;
    native_dsp_pcm_next_[0] = 0;
    native_dsp_pcm_next_[1] = 0;

    log::info("[native-dsp] installed DSP mode="
              + std::string(native_dsp_probe_mode_name(native_dsp_probe_mode_))
              + " target="
              + hex(reinterpret_cast<uintptr_t>(native_channel))
              + " dsp=" + hex(reinterpret_cast<uintptr_t>(dsp))
              + " sdk=" + hex(used_version));
    return true;
}

bool FmodInject::runtime_mute_enabled() {
    if (native_mute_flag_state_ >= 0) return native_mute_flag_state_ == 1;
    // Media-free radio is now the DEFAULT: the bridge mutes the Streamer Mode
    // channel at runtime and anchors the DSP on the active stream by station, so
    // the release no longer ships the silent-bank + RadioInfo XML edits (which
    // game updates periodically overwrote). An optional
    // spotify-radio/legacy-radio.flag opts back into the old media-dependent
    // path — that then requires the legacy silent-bank/XML install to mute the
    // vanilla audio.
    std::error_code ec;
    bool legacy = std::filesystem::exists(
        dll_dir_ / "spotify-radio" / "legacy-radio.flag", ec);
    native_mute_flag_state_ = legacy ? 0 : 1;
    log::info(legacy
                  ? std::string("[native-mute] legacy-radio.flag present; "
                    "media-free radio disabled (needs the legacy media install)")
                  : std::string("[native-mute] media-free radio active (setMute ")
                        + (rvas_.fmod_mute_ok() ? "resolved)"
                                                : "UNRESOLVED — no-op)"));
    return native_mute_flag_state_ == 1;
}

bool FmodInject::radio_dj_diag_enabled() {
#if defined(SPOTIFY_RADIO_DIAG)
    if (radio_dj_diag_flag_state_ >= 0) return radio_dj_diag_flag_state_ == 1;
    std::error_code ec;
    bool enabled = std::filesystem::exists(
        dll_dir_ / "spotify-radio" / "radio-dj-diag.flag", ec);
    radio_dj_diag_flag_state_ = enabled ? 1 : 0;
    if (enabled) {
        log::info("[radio-dj-diag] enabled by spotify-radio/radio-dj-diag.flag");
    }
    return enabled;
#else
    return false;
#endif
}

bool FmodInject::night_diag_heartbeat_enabled() {
#if defined(SPOTIFY_RADIO_DIAG)
    if (night_diag_heartbeat_flag_state_ >= 0) {
        return night_diag_heartbeat_flag_state_ == 1;
    }
    std::error_code ec;
    bool enabled = std::filesystem::exists(
        dll_dir_ / "spotify-radio" / "night-diag-heartbeat.flag", ec);
    night_diag_heartbeat_flag_state_ = enabled ? 1 : 0;
    if (enabled) {
        log::info("[night-diag] heartbeat enabled by "
                  "spotify-radio/night-diag-heartbeat.flag");
    }
    return enabled;
#else
    return false;
#endif
}

bool FmodInject::native_dsp_status_logs_enabled() {
#if defined(SPOTIFY_RADIO_DIAG)
    if (native_dsp_status_flag_state_ >= 0) {
        return native_dsp_status_flag_state_ == 1;
    }
    std::error_code ec;
    bool enabled = std::filesystem::exists(
        dll_dir_ / "spotify-radio" / "native-dsp-status.flag", ec);
    native_dsp_status_flag_state_ = enabled ? 1 : 0;
    if (enabled) {
        log::info("[native-dsp] status logging enabled by "
                  "spotify-radio/native-dsp-status.flag");
    }
    return enabled;
#else
    return false;
#endif
}

// NOTE: the previous opt-IN flag (spotify-radio/runtime-mute.flag) is obsolete —
// media-free is now the default above. Kept here as a breadcrumb in case the
// flag-gated behavior needs restoring.

// Mute/unmute the native radio channel, idempotently. Only issues an FMOD call
// when the target channel or desired state actually changes, and never strands a
// previously-muted channel when retargeting.
void FmodInject::apply_native_mute(void* channel, bool mute) {
    if (!runtime_mute_enabled() || !rvas_.fmod_mute_ok() || !channel) return;
    if (native_mute_target_ == channel && native_mute_state_ == mute) return;
    auto set_mute = reinterpret_cast<void*>(
        injector_.module_base() + rvas_.channelControlSetMute);
    if (native_mute_target_ && native_mute_target_ != channel &&
        native_mute_state_) {
        safe_call_set_mute(set_mute, native_mute_target_, false);
    }
    int r = safe_call_set_mute(set_mute, channel, mute);
    native_mute_target_ = channel;
    native_mute_state_ = mute;
    log::info(std::string("[native-mute] setMute(") + (mute ? "true" : "false")
              + ") r=" + std::to_string(r)
              + " channel=" + hex(reinterpret_cast<uintptr_t>(channel)));
}

// Unmute whatever channel we last muted (e.g. when the station goes inactive),
// so we never leave a game channel muted for another station.
void FmodInject::restore_native_mute() {
    if (native_mute_target_ && native_mute_state_ && rvas_.fmod_mute_ok()) {
        auto set_mute = reinterpret_cast<void*>(
            injector_.module_base() + rvas_.channelControlSetMute);
        safe_call_set_mute(set_mute, native_mute_target_, false);
        log::info("[native-mute] restored (unmuted) channel="
                  + hex(reinterpret_cast<uintptr_t>(native_mute_target_)));
    }
    native_mute_target_ = nullptr;
    native_mute_state_ = false;
}

// Check whether the DSP object we created is still present in the currently
// resolved ChannelControl chain. The game can tear down/rebuild that chain
// without changing the encoded channel handle, so pointer equality on the
// handle alone is not a sufficient attachment-health check.
//
// Return values: 1 = present, 0 = checked and missing, -1 = unavailable/error.
int FmodInject::inspect_native_dsp_chain(void* channel, int* out_num_dsps) {
    if (out_num_dsps) *out_num_dsps = -1;
    if (!channel || !native_dsp_probe_ ||
        !rvas_.channelControlGetNumDSPs || !rvas_.channelControlGetDSP) {
        return -1;
    }

    uintptr_t mod_base = injector_.module_base();
    auto get_num_dsps = reinterpret_cast<void*>(
        mod_base + rvas_.channelControlGetNumDSPs);
    auto get_dsp = reinterpret_cast<void*>(
        mod_base + rvas_.channelControlGetDSP);

    int num_dsps = -1;
    int count_result = safe_call_get_num_dsps(get_num_dsps, channel, &num_dsps);
    if (count_result != FMOD_OK || num_dsps < 0 || num_dsps > 256) {
        return -1;
    }
    if (out_num_dsps) *out_num_dsps = num_dsps;

    bool read_failed = false;
    for (int index = 0; index < num_dsps; ++index) {
        FMOD_DSP* dsp = nullptr;
        int result = safe_call_get_dsp(get_dsp, channel, index, &dsp);
        if (result != FMOD_OK) {
            read_failed = true;
            continue;
        }
        if (dsp == native_dsp_probe_) return 1;
    }
    return read_failed ? -1 : 0;
}

void FmodInject::maybe_update_native_dsp_probe(bool r10_active,
                                              bool menu_open) {
    constexpr int kStalledRetargetTicks = 12;
    constexpr int kSameTargetStalledTicks = 150; // 3 seconds at 20 ms/poll.
    constexpr int kChainCheckIntervalTicks = 50; // 1 second at 20 ms/poll.
    constexpr int kChainMissingConfirmations = 2;
    constexpr size_t kCallbackWatchdogBacklogBytes =
        (kPcmSampleRate * kPcmChannels * sizeof(int16_t)) / 4; // 250 ms.
    if (!native_dsp_probe_enabled_) return;
    if (native_dsp_retarget_nudge_cooldown_ > 0) {
        native_dsp_retarget_nudge_cooldown_--;
    }
    if (!r10_active) {
        remove_native_dsp_probe("r10-inactive");
        restore_native_mute();
        return;
    }
    if (!rvas_.fmod_dsp_ok() || !system_) return;

    void* native_channel = nullptr;
    if (!resolve_native_r10_channel(&native_channel)) {
        if (native_dsp_probe_) {
            native_dsp_unresolved_ticks_++;
            uint64_t calls = native_dsp_call_count_.load(std::memory_order_relaxed);
            if (calls == native_dsp_last_unresolved_calls_) {
                native_dsp_stalled_unresolved_ticks_++;
            } else {
                native_dsp_last_unresolved_calls_ = calls;
                native_dsp_stalled_unresolved_ticks_ = 0;
            }
            if (native_dsp_unresolved_ticks_ == 1 ||
                native_dsp_unresolved_ticks_ % 20 == 0) {
                log::info("[native-dsp] target unresolved; keeping existing DSP"
                          " ticks=" + std::to_string(native_dsp_unresolved_ticks_)
                          + " stalled="
                          + std::to_string(native_dsp_stalled_unresolved_ticks_)
                          + " target=" +
                          hex(reinterpret_cast<uintptr_t>(native_dsp_target_))
                          + " calls=" +
                          std::to_string(calls));
            }
            if (native_dsp_consumes_pcm() && !menu_open &&
                native_dsp_stalled_unresolved_ticks_ >= kStalledRetargetTicks &&
                native_dsp_retarget_nudge_cooldown_ <= 0) {
                log::info("[native-dsp] unresolved target is stalled; "
                          "retargeting native R10 to force a fresh handle");
                remove_native_dsp_probe("target-unresolved-stalled");
                try_radio_station_nudge("Native DSP retarget nudge");
                native_dsp_retarget_nudge_cooldown_ = 300;
                return;
            }
            if (native_dsp_unresolved_ticks_ < 80) {
                return;
            }
            if (native_dsp_consumes_pcm()) {
                return;
            }
        }
        remove_native_dsp_probe("target-unresolved");
        return;
    }
    native_dsp_unresolved_ticks_ = 0;
    native_dsp_stalled_unresolved_ticks_ = 0;
    native_dsp_last_unresolved_calls_ = native_dsp_call_count_.load(
        std::memory_order_relaxed);
    // Channel resolved. If our generator DSP is not yet covering THIS channel
    // (install pending, retry cooldown, or about to retarget), mute it so the
    // vanilla curated audio is silenced during the gap. The covering branch
    // below unmutes once the DSP is in. Covers post-detection windows only — the
    // pre-detection station-switch instant is what the live spike measures.
    if (!(native_dsp_probe_ && native_dsp_target_ == native_channel)) {
        apply_native_mute(native_channel, true);
    }
    if (native_dsp_failed_target_ == native_channel &&
        native_dsp_retry_cooldown_ > 0) {
        native_dsp_retry_cooldown_--;
        return;
    }
    if (native_dsp_probe_ && native_dsp_target_ == native_channel) {
        // Our generator DSP is the channel head and replaces the vanilla audio,
        // so the channel must be audible for Spotify to play.
        apply_native_mute(native_channel, false);
        native_dsp_retarget_ticks_ = 0;
        native_dsp_last_retarget_calls_ = native_dsp_call_count_.load(
            std::memory_order_relaxed);

        // A recent game lifecycle can leave the same encoded channel handle
        // resolvable after its DSP chain has been rebuilt. Verify membership
        // periodically, but only act on a reported miss when callbacks are also
        // stalled with real PCM waiting. That second signal makes this safe if
        // a future FMOD build changes getDSP's returned pointer representation.
        if (native_dsp_chain_check_cooldown_ > 0) {
            native_dsp_chain_check_cooldown_--;
        }
        int chain_state = -1;
        int chain_count = -1;
        if (native_dsp_chain_check_cooldown_ <= 0) {
            native_dsp_chain_check_cooldown_ = kChainCheckIntervalTicks;
            chain_state = inspect_native_dsp_chain(native_channel, &chain_count);
            if (chain_state == 1) {
                native_dsp_chain_missing_checks_ = 0;
                if (!native_dsp_chain_verified_logged_) {
                    native_dsp_chain_verified_logged_ = true;
                    log::info("[native-dsp] attachment verified in live DSP "
                              "chain target="
                              + hex(reinterpret_cast<uintptr_t>(native_channel))
                              + " chain_dsps=" + std::to_string(chain_count));
                }
            } else if (chain_state == 0) {
                native_dsp_chain_missing_checks_ =
                    (std::min)(native_dsp_chain_missing_checks_ + 1, 1000);
            } else {
                native_dsp_chain_missing_checks_ = 0;
            }
        }

        uint64_t calls = native_dsp_call_count_.load(std::memory_order_relaxed);
        size_t buffered = pcm_buffered_bytes();
        bool callback_expected =
            native_dsp_consumes_pcm() && !menu_open &&
            output_accepting_.load(std::memory_order_acquire) &&
            buffered >= kCallbackWatchdogBacklogBytes;
        if (callback_expected && calls == native_dsp_last_same_target_calls_) {
            native_dsp_same_target_stalled_ticks_++;
        } else {
            native_dsp_same_target_stalled_ticks_ = 0;
        }
        native_dsp_last_same_target_calls_ = calls;

        if (native_dsp_chain_missing_checks_ >= kChainMissingConfirmations &&
            native_dsp_same_target_stalled_ticks_ >= 25) {
            log::warn("[native-dsp] attachment lost on unchanged target; "
                      "reinstalling DSP target="
                      + hex(reinterpret_cast<uintptr_t>(native_channel))
                      + " chain_dsps=" + std::to_string(chain_count)
                      + " missing_checks="
                      + std::to_string(native_dsp_chain_missing_checks_)
                      + " stalled_ticks="
                      + std::to_string(native_dsp_same_target_stalled_ticks_)
                      + " calls=" + std::to_string(calls)
                      + " ring=" + std::to_string(buffered));
            apply_native_mute(native_channel, true);
            remove_native_dsp_probe("same-target-chain-missing");
            install_native_dsp_probe(native_channel);
            return;
        }

        if (native_dsp_same_target_stalled_ticks_ >=
            kSameTargetStalledTicks) {
            log::warn("[native-dsp] callbacks stalled on unchanged target; "
                      "forcing native radio recovery target="
                      + hex(reinterpret_cast<uintptr_t>(native_channel))
                      + " calls=" + std::to_string(calls)
                      + " ring=" + std::to_string(buffered));
            apply_native_mute(native_channel, true);
            remove_native_dsp_probe("same-target-callback-stalled");

            bool nudged = false;
            if (native_dsp_retarget_nudge_cooldown_ <= 0) {
                nudged = try_radio_station_nudge(
                    "Native DSP same-target stall nudge");
                if (nudged) native_dsp_retarget_nudge_cooldown_ = 300;
            }
            // FH5 has no portable station setter, and an FH6 nudge can fail if
            // RadioState is transiently unavailable. Reinstall immediately on
            // the resolved handle in those cases instead of leaving audio off.
            if (!nudged) install_native_dsp_probe(native_channel);
            return;
        }

        if ((kNativeDspVerboseStatusLogs || native_dsp_status_logs_enabled()) &&
            ++native_dsp_status_tick_ >= 10) {
            native_dsp_status_tick_ = 0;
            log::info("[native-dsp] status calls="
                      + std::to_string(native_dsp_call_count_.load())
                      + " len=" + std::to_string(native_dsp_last_len_.load())
                      + " in_ch=" + std::to_string(native_dsp_last_inchannels_.load())
                      + " out_ch=" + std::to_string(native_dsp_last_outchannels_.load())
                      + " mode=" + native_dsp_probe_mode_name(native_dsp_probe_mode_)
                      + " gain=" + std::to_string(output_gain_.load())
                      + " ring=" + std::to_string(pcm_buffered_bytes())
                      + " underruns=" + std::to_string(underrun_count_.load())
                      + " target=" + hex(reinterpret_cast<uintptr_t>(native_dsp_target_))
#ifdef SPOTIFY_RADIO_DIAG
                      + " channel_i=" + hex(static_cast<uintptr_t>(
                            native_diag_channel_i_.load(std::memory_order_acquire)))
                      + " ch508_bits=" + hex(static_cast<uintptr_t>(
                            native_diag_channel_508_bits_.load(std::memory_order_acquire)))
                      + " ch530=" + hex(static_cast<uintptr_t>(
                            native_diag_channel_530_.load(std::memory_order_acquire)))
                      + " ch568_bits=" + hex(static_cast<uintptr_t>(
                            native_diag_channel_568_bits_.load(std::memory_order_acquire)))
                      + " send1c0=" + hex(static_cast<uintptr_t>(
                            native_diag_send_state_1c0_.load(std::memory_order_acquire)))
                      + " send_tail_nz=" + std::to_string(
                            native_diag_send_tail_nonzero_.load(std::memory_order_acquire))
                      + " send_tail_hash=" + hex(static_cast<uintptr_t>(
                            native_diag_send_tail_hash_.load(std::memory_order_acquire)))
#endif
                      );
        }
        return;
    }

    if (native_dsp_probe_) {
        uint64_t calls = native_dsp_call_count_.load(std::memory_order_relaxed);
        if (calls == native_dsp_last_retarget_calls_) {
            native_dsp_retarget_ticks_++;
        } else {
            native_dsp_last_retarget_calls_ = calls;
            native_dsp_retarget_ticks_ = 0;
        }

        if (native_dsp_retarget_ticks_ == 0 ||
            native_dsp_retarget_ticks_ % 20 == 0) {
            log::info("[native-dsp] resolved target changed; keeping active DSP"
                      " while callbacks advance ticks="
                      + std::to_string(native_dsp_retarget_ticks_)
                      + " old="
                      + hex(reinterpret_cast<uintptr_t>(native_dsp_target_))
                      + " new="
                      + hex(reinterpret_cast<uintptr_t>(native_channel))
                      + " calls=" + std::to_string(calls));
        }

        if (native_dsp_consumes_pcm() &&
            native_dsp_retarget_ticks_ < kStalledRetargetTicks) {
            return;
        }
    }

    remove_native_dsp_probe("retarget");
    install_native_dsp_probe(native_channel);
}

void FmodInject::remove_native_dsp_probe(const char* reason) {
    if (!native_dsp_probe_) return;
    const char* why = reason && reason[0] ? reason : "unspecified";
    uintptr_t mod_base = injector_.module_base();
    void* old_target = native_dsp_target_;
    FMOD_DSP* old_dsp = native_dsp_probe_;
    int remove_result = FMOD_OK;
    int release_result = FMOD_OK;
    if (native_dsp_target_ && rvas_.channelControlRemoveDSP) {
        auto remove_dsp =
            reinterpret_cast<void*>(mod_base + rvas_.channelControlRemoveDSP);
        remove_result = safe_call_remove_dsp(remove_dsp, native_dsp_target_,
                                             native_dsp_probe_);
        if (remove_result != FMOD_OK) {
            std::string msg = "[native-dsp] removeDSP returned r="
                + std::to_string(remove_result)
                + " class=" + native_dsp_remove_result_class(remove_result)
                + " reason=" + why
                + " target=" + hex(reinterpret_cast<uintptr_t>(old_target));
            if (is_expected_native_dsp_remove_result(remove_result)) {
                log::info(msg);
            } else {
                log::warn(msg);
            }
        }
    }
    if (rvas_.dspRelease) {
        auto release = reinterpret_cast<void*>(mod_base + rvas_.dspRelease);
        release_result = safe_call_dsp_release(release, native_dsp_probe_);
        if (release_result != FMOD_OK) {
            log::warn("[native-dsp] DSP::release returned r="
                      + std::to_string(release_result)
                      + " reason=" + why
                      + " dsp=" + hex(reinterpret_cast<uintptr_t>(old_dsp)));
        }
    }
    log::info("[native-dsp] removed reason=" + std::string(why)
              + " target=" + hex(reinterpret_cast<uintptr_t>(old_target))
              + " dsp=" + hex(reinterpret_cast<uintptr_t>(old_dsp))
              + " remove_r=" + std::to_string(remove_result)
              + " release_r=" + std::to_string(release_result)
              + " calls="
              + std::to_string(native_dsp_call_count_.load())
              + " len=" + std::to_string(native_dsp_last_len_.load())
              + " in_ch=" + std::to_string(native_dsp_last_inchannels_.load())
              + " out_ch=" + std::to_string(native_dsp_last_outchannels_.load())
              + " mode=" + native_dsp_probe_mode_name(native_dsp_probe_mode_));
    native_dsp_probe_ = nullptr;
    native_dsp_target_ = nullptr;
    native_diag_target_handle_.store(0, std::memory_order_release);
    native_diag_channel_i_.store(0, std::memory_order_release);
    native_diag_channel_vt_.store(0, std::memory_order_release);
    native_diag_channel_508_bits_.store(0, std::memory_order_release);
    native_diag_channel_530_.store(0, std::memory_order_release);
    native_diag_channel_568_bits_.store(0, std::memory_order_release);
    native_diag_send_state_1c0_.store(0, std::memory_order_release);
    native_diag_send_tail_nonzero_.store(0, std::memory_order_release);
    native_diag_send_tail_hash_.store(0, std::memory_order_release);
    native_dsp_failed_target_ = nullptr;
    native_dsp_retry_cooldown_ = 0;
    native_dsp_status_tick_ = 0;
    native_dsp_unresolved_ticks_ = 0;
    native_dsp_stalled_unresolved_ticks_ = 0;
    native_dsp_retarget_ticks_ = 0;
    native_dsp_chain_check_cooldown_ = 0;
    native_dsp_chain_missing_checks_ = 0;
    native_dsp_chain_verified_logged_ = false;
    native_dsp_same_target_stalled_ticks_ = 0;
    native_dsp_last_same_target_calls_ = native_dsp_call_count_.load(
        std::memory_order_relaxed);
    native_dsp_last_unresolved_calls_ = native_dsp_call_count_.load(
        std::memory_order_relaxed);
    native_dsp_last_retarget_calls_ = native_dsp_last_unresolved_calls_;
}

FMOD_RESULT FmodInject::native_dsp_read_cb(FMOD_DSP_STATE*,
                                           float* inbuffer,
                                           float* outbuffer,
                                           unsigned int length,
                                           int inchannels,
                                           int* outchannels) {
    FmodInject* self = g_fmod_inject;
    NativeDspProbeMode mode = self ? self->native_dsp_probe_mode_
                                   : NativeDspProbeMode::Passthrough;
    int channels = inchannels > 0 ? inchannels : 2;
    if (outchannels) {
        if (*outchannels > 0) channels = *outchannels;
        if (mode == NativeDspProbeMode::Pcm && channels < 2) {
            channels = 2;
        }
        *outchannels = channels;
    } else if (mode == NativeDspProbeMode::Pcm && channels < 2) {
        channels = 2;
    }
    size_t samples = static_cast<size_t>(length) * static_cast<size_t>(channels);
    if (outbuffer) {
        if (mode == NativeDspProbeMode::Silence) {
            std::memset(outbuffer, 0, samples * sizeof(float));
        } else if (mode == NativeDspProbeMode::Tone) {
            constexpr double kSampleRate = 48000.0;
            constexpr double kFrequency = 440.0;
            constexpr double kAmplitude = 0.04;
            uint64_t start_frame = self
                ? self->native_dsp_generated_frames_.fetch_add(
                      length, std::memory_order_relaxed)
                : 0;
            for (unsigned int frame = 0; frame < length; ++frame) {
                double t = static_cast<double>(start_frame + frame) / kSampleRate;
                float sample = static_cast<float>(
                    std::sin(t * kFrequency * 6.2831853071795864769) *
                    kAmplitude);
                for (int ch = 0; ch < channels; ++ch) {
                    outbuffer[static_cast<size_t>(frame) *
                                  static_cast<size_t>(channels) +
                              static_cast<size_t>(ch)] = sample;
                }
            }
        } else if (mode == NativeDspProbeMode::Pcm && self) {
            float gain = self->output_gain_.load(std::memory_order_relaxed);
            if (gain <= 0.0f) {
                std::memset(outbuffer, 0, samples * sizeof(float));
                self->native_dsp_pcm_phase_ = 0.0;
                self->native_dsp_pcm_have_current_ = false;
                self->native_dsp_pcm_have_next_ = false;
                self->native_dsp_low_cut_z1_l_ = 0.0f;
                self->native_dsp_low_cut_z2_l_ = 0.0f;
                self->native_dsp_low_cut_z1_r_ = 0.0f;
                self->native_dsp_low_cut_z2_r_ = 0.0f;
                self->native_dsp_low_cut_smoothed_amount_ = 0.0f;
            } else {
                std::memset(outbuffer, 0, samples * sizeof(float));
                constexpr double kInputPerOutput =
                    static_cast<double>(kPcmSampleRate) / 48000.0;
                float target_low_cut_amount = 0.0f;
                if (self->night_runners_low_cut_enabled_state_.load(
                        std::memory_order_relaxed)) {
                    float configured_amount =
                        self->night_runners_low_cut_amount_.load(
                            std::memory_order_relaxed);
                    float progress =
                        self->night_runners_volume_progress_.load(
                            std::memory_order_relaxed);
                    target_low_cut_amount = std::clamp(configured_amount, 0.0f, 1.0f) *
                                            (1.0f - std::clamp(progress, 0.0f, 1.0f));
                }
                float target_low_cut_frequency =
                    self->night_runners_low_cut_frequency_hz_state_.load(
                        std::memory_order_relaxed);
                bool high_end_cut = self->night_runners_high_cut_state_.load(
                    std::memory_order_relaxed);
                target_low_cut_frequency = high_end_cut
                    ? std::clamp(target_low_cut_frequency, 1200.0f, 12000.0f)
                    : std::clamp(target_low_cut_frequency, 40.0f, 320.0f);
                float smoothing = std::clamp(
                    static_cast<float>(length) / (48000.0f * 0.08f),
                    0.0f, 1.0f);
                if (self->native_dsp_low_cut_high_end_ != high_end_cut) {
                    self->native_dsp_low_cut_z1_l_ = 0.0f;
                    self->native_dsp_low_cut_z2_l_ = 0.0f;
                    self->native_dsp_low_cut_z1_r_ = 0.0f;
                    self->native_dsp_low_cut_z2_r_ = 0.0f;
                    self->native_dsp_low_cut_smoothed_frequency_hz_ =
                        target_low_cut_frequency;
                    self->native_dsp_low_cut_high_end_ = high_end_cut;
                }
                self->native_dsp_low_cut_smoothed_amount_ +=
                    (target_low_cut_amount -
                     self->native_dsp_low_cut_smoothed_amount_) *
                    smoothing;
                self->native_dsp_low_cut_smoothed_frequency_hz_ +=
                    (target_low_cut_frequency -
                     self->native_dsp_low_cut_smoothed_frequency_hz_) *
                    smoothing;
                BiquadCoefficients frequency_cut = high_end_cut
                    ? make_lowpass(self->native_dsp_low_cut_smoothed_frequency_hz_)
                    : make_highpass(self->native_dsp_low_cut_smoothed_frequency_hz_);
                bool apply_low_cut =
                    self->native_dsp_low_cut_smoothed_amount_ > 0.001f;
                bool underrun = false;
                auto read_frame = [self](int16_t (&frame)[kPcmChannels]) {
                    int16_t temp[kPcmChannels] = {};
                    size_t got = self->ring_.read(temp, sizeof(temp));
                    if (got != sizeof(temp)) return false;
                    frame[0] = temp[0];
                    frame[1] = temp[1];
                    return true;
                };
                auto read_float_frame = [self](float (&frame)[kPcmChannels]) {
                    float temp[kPcmChannels] = {};
                    const size_t got = self->float_ring_.read(temp, sizeof(temp));
                    if (got != sizeof(temp)) return false;
                    frame[0] = temp[0];
                    frame[1] = temp[1];
                    return true;
                };
                const bool float_pcm =
                    self->pcm_float_mode_.load(std::memory_order_relaxed);

                for (unsigned int frame = 0; frame < length; ++frame) {
                    float left = 0.0f;
                    float right = 0.0f;
                    if (float_pcm) {
                        float pcm_frame[kPcmChannels] = {};
                        if (!read_float_frame(pcm_frame)) {
                            self->underrun_count_.fetch_add(
                                1, std::memory_order_relaxed);
                            underrun = true;
                            break;
                        }
                        left = pcm_frame[0] * gain;
                        right = pcm_frame[1] * gain;
                    } else {
                        if (!self->native_dsp_pcm_have_current_) {
                            if (!read_frame(self->native_dsp_pcm_current_)) {
                                self->underrun_count_.fetch_add(
                                    1, std::memory_order_relaxed);
                                underrun = true;
                                break;
                            }
                            self->native_dsp_pcm_have_current_ = true;
                        }
                        if (!self->native_dsp_pcm_have_next_) {
                            if (!read_frame(self->native_dsp_pcm_next_)) {
                                self->underrun_count_.fetch_add(
                                    1, std::memory_order_relaxed);
                                underrun = true;
                                break;
                            }
                            self->native_dsp_pcm_have_next_ = true;
                        }

                        const double phase = self->native_dsp_pcm_phase_;
                        const float pcm_gain = gain;
                        left = static_cast<float>(
                            (static_cast<double>(
                                 self->native_dsp_pcm_current_[0]) +
                             (static_cast<double>(
                                  self->native_dsp_pcm_next_[0]) -
                              static_cast<double>(
                                  self->native_dsp_pcm_current_[0])) *
                                 phase) /
                            32768.0 * pcm_gain);
                        right = static_cast<float>(
                            (static_cast<double>(
                                 self->native_dsp_pcm_current_[1]) +
                             (static_cast<double>(
                                  self->native_dsp_pcm_next_[1]) -
                              static_cast<double>(
                                  self->native_dsp_pcm_current_[1])) *
                                 phase) /
                            32768.0 * pcm_gain);
                    }
                    if (apply_low_cut) {
                        float amount = std::clamp(
                            self->native_dsp_low_cut_smoothed_amount_,
                            0.0f, 1.0f);
                        float filtered_left = process_biquad(
                            left, frequency_cut, self->native_dsp_low_cut_z1_l_,
                            self->native_dsp_low_cut_z2_l_);
                        float filtered_right = process_biquad(
                            right, frequency_cut, self->native_dsp_low_cut_z1_r_,
                            self->native_dsp_low_cut_z2_r_);
                        left += (filtered_left - left) * amount;
                        right += (filtered_right - right) * amount;
                    } else {
                        self->native_dsp_low_cut_z1_l_ = 0.0f;
                        self->native_dsp_low_cut_z2_l_ = 0.0f;
                        self->native_dsp_low_cut_z1_r_ = 0.0f;
                        self->native_dsp_low_cut_z2_r_ = 0.0f;
                    }
                    left = clamp_float_audio(left);
                    right = clamp_float_audio(right);
                    size_t out_base = static_cast<size_t>(frame) *
                                      static_cast<size_t>(channels);
                    if (channels == 1) {
                        outbuffer[out_base] = (left + right) * 0.5f;
                    } else {
                        outbuffer[out_base] = left;
                        outbuffer[out_base + 1] = right;
                        for (int ch = 2; ch < channels; ++ch) {
                            outbuffer[out_base + static_cast<size_t>(ch)] =
                                (left + right) * 0.5f;
                        }
                    }

                    if (!float_pcm) {
                        self->native_dsp_pcm_phase_ += kInputPerOutput;
                        while (self->native_dsp_pcm_phase_ >= 1.0) {
                            self->native_dsp_pcm_current_[0] =
                                self->native_dsp_pcm_next_[0];
                            self->native_dsp_pcm_current_[1] =
                                self->native_dsp_pcm_next_[1];
                            if (!read_frame(self->native_dsp_pcm_next_)) {
                                self->native_dsp_pcm_have_next_ = false;
                                self->underrun_count_.fetch_add(
                                    1, std::memory_order_relaxed);
                                underrun = true;
                                break;
                            }
                            self->native_dsp_pcm_phase_ -= 1.0;
                        }
                    }
                    if (!float_pcm && !self->native_dsp_pcm_have_next_) {
                        self->underrun_count_.fetch_add(
                            1, std::memory_order_relaxed);
                        underrun = true;
                        break;
                    }
                }
                if (underrun) {
                    self->native_dsp_pcm_phase_ = 0.0;
                    self->native_dsp_pcm_have_current_ = false;
                    self->native_dsp_pcm_have_next_ = false;
                }
            }
        } else if (inbuffer) {
            std::memcpy(outbuffer, inbuffer, samples * sizeof(float));
        } else {
            std::memset(outbuffer, 0, samples * sizeof(float));
        }
    }
    if (self) {
        self->native_dsp_call_count_.fetch_add(1, std::memory_order_relaxed);
        self->native_dsp_last_len_.store(length, std::memory_order_relaxed);
        self->native_dsp_last_inchannels_.store(inchannels, std::memory_order_relaxed);
        self->native_dsp_last_outchannels_.store(channels, std::memory_order_relaxed);
    }
    return FMOD_OK;
}

// ── Native DSP control thread ──────────────────────────────────────
// Fast poll of game radio/menu/race state. Drives output_gain_ so Spotify
// audio follows R10 and native radio menu gating while FH6 owns the actual
// ChannelControl and all route/filter transitions.
void FmodInject::native_dsp_gain_thread_fn() {
    log::info("[native-dsp] Native PCM control loop started");

    constexpr auto kPoll = std::chrono::milliseconds(20);
    constexpr float kPollSeconds = 0.02f;
    constexpr auto kRaceStartCooldown = std::chrono::seconds(45);
    constexpr auto kRaceRestartMenuCooldown = std::chrono::seconds(5);
    constexpr auto kRaceStartActiveHold = std::chrono::seconds(2);
    constexpr auto kRaceInactiveDebounce = std::chrono::milliseconds(1500);
    constexpr auto kQuickStationSkipWindow = std::chrono::milliseconds(1000);
    constexpr auto kQuickStationSkipInactiveDebounce = std::chrono::milliseconds(110);
    constexpr auto kMenuResumePrebuffer = std::chrono::milliseconds(250);
    constexpr auto kNonDrivingStateDelay = std::chrono::milliseconds(1000);
    constexpr auto kNightRunnersFactorEase = std::chrono::milliseconds(1000);
    // After the lazy-volume hold expires, ease the held volume down toward the
    // live speed-based level instead of snapping. Exponential time constant.
    constexpr float kNightRunnersLazyReleaseTauSec = 0.4f;
    constexpr float kGarageMaxMovingSpeedMps = 0.75f;
    constexpr size_t kMenuResumePrebufferBytes = 44100;
    constexpr size_t kMenuResumePrebufferFloatBytes = 96000;
    bool last_r10_active = false;
    bool last_menu_open = false;
    bool last_local_hold = false;
    bool paused_by_menu = false;
    bool quick_station_skip_armed = false;
    bool quick_station_skip_seen_r10_active = false;
    bool race_state_seen = false;
    bool last_race_policy_active = false;
    bool last_raw_race_active = false;
    bool race_restart_menu_seen = false;
    bool last_race_restart_menu = false;
    bool race_restart_pending = false;
    bool race_restart_pending_from_menu = false;
    bool playback_prebuffering = false;
    bool last_audible = false;
    bool non_driving_volume_state = false;
    bool non_driving_pending_state = false;
    bool non_driving_pending_initialized = false;
    bool last_logged_non_driving_volume_state = false;
    bool last_logged_non_driving_raw_state = false;
    float last_gain = -1.0f;
    float last_logged_night_factor = -1.0f;
    float smoothed_night_runners_factor = 1.0f;
    float night_runners_factor_ease_from = 1.0f;
    bool night_runners_factor_easing = false;
    // Lazy volume reduction: hold the peak speed-progress, then release.
    float lazy_held_progress = 1.0f;
    bool lazy_cooldown_active = false;
    float lazy_cooldown_fraction = 0.0f;
    // Dynamic ("speed-relative") night-runners state machine.
    float dyn_norm = 1.0f;        // volume position within [floor .. full]
    float dyn_peak_mps = 0.0f;    // trailing peak (top of the buffer band)
    float dyn_t_fill = 0.0f;      // seconds the buffer band has been filling
    bool  dyn_decreasing = false; // mode latch: true = decreasing (buffer gone)
    bool  dyn_initialized = false;
    // ~1s speed history ring, used below the threshold to detect "speed up over
    // the last second" (which swells the volume even before the threshold).
    constexpr float kDynAccelWindowSec = 1.0f;
    constexpr int kDynAccelSamples =
        static_cast<int>(kDynAccelWindowSec / kPollSeconds); // ~50 @ 20ms poll
    float dyn_speed_ring[64] = {};
    int   dyn_speed_ring_count = 0;
    int   dyn_speed_ring_head = 0;
#if defined(SPOTIFY_RADIO_DIAG)
    bool diag_state_initialized = false;
    bool diag_last_r10_active = false;
    bool diag_last_menu_open = false;
    bool diag_last_race_active = false;
    bool diag_last_race_policy_active = false;
    bool diag_last_race_restart_menu = false;
    bool diag_last_local_hold = false;
    bool diag_last_speed_available = false;
    bool diag_last_non_driving_candidate = false;
    bool diag_last_non_driving_state = false;
    bool diag_last_garage_candidate = false;
    bool diag_last_race_stinger = false;
    float diag_last_night_factor = -1.0f;
    float diag_last_speed_mps = -1.0f;
#endif
    auto now = std::chrono::steady_clock::now();
    uintptr_t last_station_ptr = 0;
    std::string last_station_name;
    auto next_station_change_poll = now;
    auto quick_station_skip_left_at = now;
    auto quick_station_skip_inactive_since = now;
    auto playback_unmute_at = now;
    auto last_race_start_restart = now - kRaceStartCooldown;
    auto last_race_menu_restart = now - kRaceRestartMenuCooldown;
    auto race_inactive_since = now;
    auto race_active_since = now;
    auto non_driving_pending_since = now;
    auto night_runners_factor_ease_started = now;
    auto lazy_cooldown_started = now;
    auto dyn_prev_now = now;
#if defined(SPOTIFY_RADIO_DIAG)
    auto last_night_diag_log = now - std::chrono::seconds(5);
    auto last_radio_dj_diag_heartbeat = now - std::chrono::seconds(60);
    std::string last_radio_dj_diag_signature;
    bool radio_dj_diag_seen = false;
#endif

    prebuffer_audio_.store(false, std::memory_order_release);
    while (running_.load()) {
        std::this_thread::sleep_for(kPoll);
        if (!running_.load()) break;

        now = std::chrono::steady_clock::now();
        bool r10_active = injector_.is_spotify_station_active();
        bool menu_open = injector_.is_game_menu_open();
        bool race_active = injector_.is_race_active();
        bool race_restart_menu = injector_.is_race_restart_menu_active();
        // "Vanilla Streamer Mode": suppress injection so the game's own curated
        // audio plays. Treated like the station being inactive — the DSP is
        // removed and the channel unmuted (restore_native_mute) inside
        // maybe_update_native_dsp_probe's !active path.
        bool injection_suppressed = false;
        {
            std::lock_guard lock(playback_controls_mtx_);
            injection_suppressed =
                injection_suppressed_ && injection_suppressed_();
        }
        maybe_update_native_dsp_probe(r10_active && !injection_suppressed,
                                      menu_open);

#if defined(SPOTIFY_RADIO_DIAG)
        if (radio_dj_diag_enabled()) {
            auto snap = injector_.radio_stream_debug_snapshot();
            std::string sig = radio_dj_diag_signature(
                snap, r10_active, menu_open, injection_suppressed);
            bool heartbeat =
                now - last_radio_dj_diag_heartbeat >= std::chrono::seconds(60);
            if (!radio_dj_diag_seen || sig != last_radio_dj_diag_signature ||
                heartbeat) {
                log::info(std::string("[radio-dj-diag] ")
                          + (heartbeat && radio_dj_diag_seen ? "heartbeat "
                                                             : "change ")
                          + sig);
                last_radio_dj_diag_signature = std::move(sig);
                radio_dj_diag_seen = true;
                last_radio_dj_diag_heartbeat = now;
            }
        }
#endif

        std::function<bool()> pause_in_menus;
        std::function<std::string()> race_start_playback;
        std::function<bool()> quick_station_skip;
        std::function<bool()> is_playing;
        std::function<void()> pause;
        std::function<void()> resume;
        std::function<bool()> restart_current_track;
        std::function<bool()> next_track;
        std::function<bool()> station_change_next_track;
        std::function<std::optional<uint32_t>()> current_position_ms;
        std::function<uint32_t()> race_restart_threshold_s;
        {
            std::lock_guard lock(playback_controls_mtx_);
            pause_in_menus = playback_pause_in_menus_;
            race_start_playback = playback_race_start_playback_;
            quick_station_skip = playback_quick_station_skip_;
            is_playing = playback_is_playing_;
            pause = playback_pause_;
            resume = playback_resume_;
            restart_current_track = playback_restart_current_track_;
            next_track = playback_next_track_;
            station_change_next_track = station_change_next_track_;
            current_position_ms = playback_current_position_ms_;
            race_restart_threshold_s = playback_race_restart_threshold_s_;
        }

        std::function<bool()> night_runners_enabled;
        std::function<uint32_t()> stopped_volume_decrease_percent;
        std::function<uint32_t()> max_speed_mph;
        std::function<bool()> curve_enabled;
        std::function<float()> curve_exponent;
        std::function<bool()> lazy_volume_enabled;
        std::function<float()> lazy_hold_seconds;
        std::function<bool()> low_cut_enabled;
        std::function<std::string()> frequency_cut_mode;
        std::function<uint32_t()> low_cut_amount_percent;
        std::function<uint32_t()> low_cut_frequency_hz;
        std::function<bool()> non_driving_volume_enabled;
        std::function<uint32_t()> non_driving_volume_percent;
        std::function<std::optional<float>()> speed_mps;
        std::function<bool()> dynamic_mode;
        std::function<uint32_t()> dynamic_threshold_mph;
        std::function<uint32_t()> dynamic_buffer_percent;
        std::function<float()> dynamic_buffer_fill_seconds;
        std::function<float()> dynamic_increase_seconds;
        std::function<float()> dynamic_decrease_seconds;
        std::function<uint32_t()> dynamic_max_decay_mph_s;
        {
            std::lock_guard lock(night_runners_controls_mtx_);
            night_runners_enabled = night_runners_enabled_;
            stopped_volume_decrease_percent =
                night_runners_stopped_volume_decrease_percent_;
            max_speed_mph = night_runners_max_speed_mph_;
            curve_enabled = night_runners_curve_enabled_;
            curve_exponent = night_runners_curve_exponent_;
            lazy_volume_enabled = night_runners_lazy_volume_enabled_;
            lazy_hold_seconds = night_runners_lazy_hold_seconds_;
            low_cut_enabled = night_runners_low_cut_enabled_;
            frequency_cut_mode = night_runners_frequency_cut_mode_;
            low_cut_amount_percent = night_runners_low_cut_amount_percent_;
            low_cut_frequency_hz = night_runners_low_cut_frequency_hz_;
            non_driving_volume_enabled =
                night_runners_non_driving_volume_enabled_;
            non_driving_volume_percent =
                night_runners_non_driving_volume_percent_;
            speed_mps = night_runners_speed_mps_;
            dynamic_mode = night_runners_dynamic_mode_;
            dynamic_threshold_mph = night_runners_dynamic_threshold_mph_;
            dynamic_buffer_percent = night_runners_dynamic_buffer_percent_;
            dynamic_buffer_fill_seconds =
                night_runners_dynamic_buffer_fill_seconds_;
            dynamic_increase_seconds = night_runners_dynamic_increase_seconds_;
            dynamic_decrease_seconds = night_runners_dynamic_decrease_seconds_;
            dynamic_max_decay_mph_s = night_runners_dynamic_max_decay_mph_s_;
        }

        bool pause_transport_in_menu = !pause_in_menus || pause_in_menus();
        std::string race_action =
            race_start_playback ? race_start_playback() : std::string("restart");
        bool restart_policy_enabled = race_action != "ignore";
        bool r10_became_active = r10_active && !last_r10_active;
        bool r10_became_inactive = !r10_active && last_r10_active;
        last_r10_active = r10_active;

        int suppress_ticks =
            station_nudge_quick_skip_suppress_ticks_.load(std::memory_order_acquire);
        bool suppress_quick_skip = suppress_ticks > 0;
        if (suppress_quick_skip) {
            station_nudge_quick_skip_suppress_ticks_.fetch_sub(
                1, std::memory_order_acq_rel);
        }
        bool quick_skip_edges_allowed =
            !suppress_quick_skip && !menu_open && !race_restart_menu;

        if (!quick_skip_edges_allowed) {
            if (quick_station_skip_armed) {
                log::info("[native-dsp] Quick station skip disarmed during "
                          "station/menu recovery");
            }
            quick_station_skip_armed = false;
            quick_station_skip_inactive_since = now;
            quick_station_skip_left_at = now;
        } else if (r10_became_inactive && quick_station_skip_seen_r10_active) {
            quick_station_skip_inactive_since = now;
            quick_station_skip_armed = false;
        } else if (!r10_active && quick_station_skip_seen_r10_active) {
            if (!quick_station_skip_armed &&
                now - quick_station_skip_inactive_since >=
                    kQuickStationSkipInactiveDebounce) {
                quick_station_skip_armed = true;
                quick_station_skip_left_at = quick_station_skip_inactive_since;
            }
            if (quick_station_skip_armed &&
                now - quick_station_skip_left_at > kQuickStationSkipWindow) {
                quick_station_skip_armed = false;
            }
        } else if (r10_became_active) {
            bool was_armed = quick_station_skip_armed;
            quick_station_skip_armed = false;
            if (was_armed) {
                bool within_window =
                    now - quick_station_skip_left_at <= kQuickStationSkipWindow;
                bool enabled = quick_station_skip && quick_station_skip();
                bool playing = !is_playing || is_playing();
#if defined(SPOTIFY_RADIO_DIAG)
                log::info("[skip-diag] quick-station return armed=1"
                          " enabled=" + std::to_string(enabled)
                          + " within_window=" + std::to_string(within_window)
                          + " playing=" + std::to_string(playing)
                          + " has_next_cb=" + std::to_string(next_track != nullptr)
                          + " inactive_ms="
                          + std::to_string(std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                now - quick_station_skip_left_at).count()));
#endif
                if (enabled && within_window && playing && next_track) {
                    bool ok = next_track();
                    clear_pcm();
                    log::info(std::string("[native-dsp] Quick station return — ")
                              + (ok ? "advanced to next Spotify track"
                                    : "could not advance Spotify queue/context"));
                }
            }
        }
        if (r10_active) quick_station_skip_seen_r10_active = true;

        if (now >= next_station_change_poll) {
            next_station_change_poll = now + std::chrono::milliseconds(200);
            auto station_snapshot = injector_.radio_stream_debug_snapshot();
            if (station_snapshot.selected_station_read &&
                station_snapshot.selected_station != 0) {
                const uintptr_t current_station =
                    station_snapshot.selected_station;
                const std::string current_name =
                    station_snapshot.selected_station_name;
                if (last_station_ptr != 0 &&
                    current_station != last_station_ptr &&
                    !current_name.empty() &&
                    current_name != last_station_name &&
                    !menu_open) {
                    bool advanced =
                        station_change_next_track &&
                        station_change_next_track();
                    if (advanced) clear_pcm();
                    log::info(std::string("[native-dsp] Station change — ")
                              + (advanced ? "advanced QQ Music track"
                                          : "could not advance QQ Music track")
                              + " from=\"" + sanitize_log_field(last_station_name)
                              + "\" to=\"" + sanitize_log_field(current_name)
                              + "\"");
                }
                last_station_ptr = current_station;
                last_station_name = current_name;
            }
        }

        bool local_hold = r10_active && menu_open && pause_transport_in_menu;
        if (local_hold != last_local_hold) {
            local_audio_hold_.store(local_hold, std::memory_order_release);
            if (local_hold) {
                clear_pcm();
                bool should_pause = !is_playing || is_playing();
                if (should_pause && pause) {
                    pause();
                    paused_by_menu = true;
                    log::info("[native-dsp] Game menu open — paused Spotify transport");
                } else {
                    log::info("[native-dsp] Game menu open — holding Spotify PCM locally");
                }
            } else {
                if (paused_by_menu && resume) {
                    resume();
                    clear_pcm();
                    playback_prebuffering = true;
                    playback_unmute_at = now + kMenuResumePrebuffer;
                    prebuffer_audio_.store(true, std::memory_order_release);
                    log::info("[native-dsp] Game menu closed — resumed Spotify transport");
                } else if (last_menu_open) {
                    log::info("[native-dsp] Game menu closed — releasing local PCM hold");
                }
                paused_by_menu = false;
            }
            last_local_hold = local_hold;
        }
        if (local_hold && !paused_by_menu && pause) {
            bool should_pause = !is_playing || is_playing();
                if (should_pause) {
                    pause();
                    paused_by_menu = true;
                    clear_pcm();
                log::info("[native-dsp] Game menu hold — paused Spotify transport after external resume");
            }
        }
        last_menu_open = menu_open;

        if (playback_prebuffering &&
            (now >= playback_unmute_at ||
             pcm_buffered_bytes() >=
                 (pcm_float_mode_.load(std::memory_order_relaxed)
                      ? kMenuResumePrebufferFloatBytes
                      : kMenuResumePrebufferBytes) ||
             !r10_active || local_hold)) {
            playback_prebuffering = false;
            prebuffer_audio_.store(false, std::memory_order_release);
            log::info("[native-dsp] playback prebuffer released ring="
                      + std::to_string(pcm_buffered_bytes()));
        }

        if (!race_state_seen || race_active != last_raw_race_active) {
            race_active_since = now;
            last_raw_race_active = race_active;
        }
        const bool race_policy_active =
            race_active &&
            (last_race_policy_active ||
             now - race_active_since >= kRaceStartActiveHold);

        if (!race_active && (!race_state_seen || last_race_policy_active)) {
            race_inactive_since = now;
        }
        if (!race_state_seen) {
            // Preserve startup semantics: if the bridge begins polling while a
            // race is already active, do not synthesize a delayed race-start.
            last_race_policy_active = race_active;
            race_state_seen = true;
        } else if (restart_policy_enabled &&
                   r10_active && race_policy_active &&
                   !last_race_policy_active &&
                   now - race_inactive_since >= kRaceInactiveDebounce &&
                   now - last_race_start_restart >= kRaceStartCooldown) {
            race_restart_pending = true;
            race_restart_pending_from_menu = false;
            log::info("[native-dsp] Race-start state transition detected"
                      " after stable hold");
        }
        if (!race_active) {
            last_race_policy_active = false;
        } else if (race_policy_active) {
            last_race_policy_active = true;
        }

        if (!race_restart_menu_seen) {
            last_race_restart_menu = race_restart_menu;
            race_restart_menu_seen = true;
        } else if (restart_policy_enabled &&
                   r10_active && race_policy_active && last_race_restart_menu &&
                   !race_restart_menu &&
                   now - last_race_menu_restart >= kRaceRestartMenuCooldown) {
            race_restart_pending = true;
            race_restart_pending_from_menu = true;
            log::info("[native-dsp] Race-restart menu close detected");
        }
        last_race_restart_menu = race_restart_menu;

        if (race_restart_pending && !menu_open) {
            race_restart_pending = false;
            bool should_apply =
                restart_policy_enabled && r10_active && race_policy_active;
            bool action_ok = false;
            const char* action_label = "keeping current source position";
            // Resolve the "smart" race-start mode into restart-or-skip by the
            // current song progress: restart when below the threshold, otherwise
            // skip. An unavailable position falls back to skip.
            std::string effective_action = race_action;
            if (should_apply && race_action == "smart") {
                uint32_t threshold_ms = 1000u *
                    (race_restart_threshold_s ? race_restart_threshold_s() : 20u);
                std::optional<uint32_t> pos =
                    current_position_ms ? current_position_ms() : std::nullopt;
                effective_action =
                    (pos.has_value() && *pos < threshold_ms) ? "restart" : "next";
                log::info(std::string("[native-dsp] Race-start smart — pos_ms=")
                          + (pos ? std::to_string(*pos) : std::string("n/a"))
                          + " threshold_ms=" + std::to_string(threshold_ms)
                          + " -> " + effective_action);
            } else if (should_apply &&
                       race_action == "restart_if_past_threshold") {
                uint32_t threshold_ms = 1000u *
                    (race_restart_threshold_s ? race_restart_threshold_s() : 20u);
                std::optional<uint32_t> pos =
                    current_position_ms ? current_position_ms() : std::nullopt;
                effective_action =
                    (pos.has_value() && *pos >= threshold_ms) ? "restart" : "ignore";
                log::info(std::string("[native-dsp] Race-start AirPlay smart — pos_ms=")
                          + (pos ? std::to_string(*pos) : std::string("n/a"))
                          + " threshold_ms=" + std::to_string(threshold_ms)
                          + " -> " + effective_action);
            }
            if (should_apply && effective_action == "next" && next_track) {
#if defined(SPOTIFY_RADIO_DIAG)
                log::info("[skip-diag] race playback action=next from_menu="
                          + std::to_string(race_restart_pending_from_menu)
                          + " r10=" + std::to_string(r10_active)
                          + " race=" + std::to_string(race_active));
#endif
                action_ok = next_track();
                action_label = action_ok
                    ? "advanced to next source track"
                    : "could not advance source queue/context";
            } else if (should_apply && effective_action == "restart" && restart_current_track) {
#if defined(SPOTIFY_RADIO_DIAG)
                log::info("[skip-diag] race playback action=restart from_menu="
                          + std::to_string(race_restart_pending_from_menu)
                          + " r10=" + std::to_string(r10_active)
                          + " race=" + std::to_string(race_active));
#endif
                action_ok = restart_current_track();
                action_label = action_ok
                    ? "restarted current source track"
                    : "could not restart current source track";
            }
            if (action_ok) {
                if (race_restart_pending_from_menu) {
                    last_race_menu_restart = now;
                } else {
                    last_race_start_restart = now;
                }
                clear_pcm();
            }
            log::info(std::string("[native-dsp] ")
                      + (race_restart_pending_from_menu
                             ? "Race restarted"
                             : "Race started")
                      + " — " + action_label);
            race_restart_pending_from_menu = false;
        }

        float current_speed_mps = 0.0f;
        bool speed_available = false;
        float volume_progress = 1.0f;
        float speed_axis_progress = 0.0f;
        float night_runners_factor = 1.0f;
        float dyn_pub_peak_mps = 0.0f;
        float dyn_pub_buffer_mps = 0.0f;
        uint32_t dyn_pub_state = 0;
        bool night_runners_active =
            night_runners_enabled && night_runners_enabled();
        bool non_driving_enabled =
            !non_driving_volume_enabled || non_driving_volume_enabled();
        if (night_runners_active) {
            auto speed = speed_mps ? speed_mps() : std::optional<float>{};
            if (speed && std::isfinite(*speed) && *speed >= 0.0f) {
                speed_available = true;
                current_speed_mps = *speed;
            }
        }
        bool race_stinger_menu = injector_.is_race_stinger_menu_active();
        uint32_t ch508_bits =
            native_diag_channel_508_bits_.load(std::memory_order_acquire);
        uint32_t ch568_bits =
            native_diag_channel_568_bits_.load(std::memory_order_acquire);
        uint64_t channel_i =
            native_diag_channel_i_.load(std::memory_order_acquire);
        float ch508 = f32_from_bits(ch508_bits);
        float ch568 = f32_from_bits(ch568_bits);
        bool moving_with_valid_speed =
            speed_available && current_speed_mps > kGarageMaxMovingSpeedMps;
        bool garage_audio_candidate =
            channel_i != 0 && !menu_open && !race_active &&
            !moving_with_valid_speed &&
            std::isfinite(ch508) && ch508 >= 1.95f && ch508 <= 2.15f &&
            std::isfinite(ch568) && ch568 <= 1.05f;
        bool non_driving_candidate = garage_audio_candidate || race_stinger_menu;
        injector_.update_runtime_logo_game_state(non_driving_candidate,
                                                 garage_audio_candidate,
                                                 race_stinger_menu);
        if (!non_driving_pending_initialized) {
            non_driving_pending_state = non_driving_candidate;
            non_driving_pending_since = now;
            non_driving_pending_initialized = true;
        } else if (non_driving_candidate != non_driving_pending_state) {
            non_driving_pending_state = non_driving_candidate;
            non_driving_pending_since = now;
        }
        if (non_driving_pending_state != non_driving_volume_state &&
            now - non_driving_pending_since >= kNonDrivingStateDelay) {
            non_driving_volume_state = non_driving_pending_state;
            night_runners_factor_ease_from = smoothed_night_runners_factor;
            night_runners_factor_ease_started = now;
            night_runners_factor_easing = true;
            log::info(std::string("[native-dsp] non-driving state committed=")
                      + std::to_string(non_driving_volume_state)
                      + " after_ms="
                      + std::to_string(std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            now - non_driving_pending_since).count())
                      + " raw=" + std::to_string(non_driving_candidate)
                      + " garage_candidate="
                      + std::to_string(garage_audio_candidate)
                      + " race_stinger=" + std::to_string(race_stinger_menu));
        }

        float target_night_runners_factor = 1.0f;
        if (night_runners_active) {
            if (speed_available) {
                uint32_t stopped_decrease =
                    stopped_volume_decrease_percent
                        ? stopped_volume_decrease_percent()
                        : 50u;
                stopped_decrease = std::min<uint32_t>(stopped_decrease, 100u);

                bool dynamic_active = dynamic_mode && dynamic_mode();
                if (dynamic_active) {
                    // ── Dynamic speed-relative model ──────────────────────
                    // Above the threshold: two-mode latch around a buffer band
                    // [peak - buffer, peak].
                    //  - INCREASING while speed stays inside/above the band (the
                    //    band tops out at the trailing peak and the buffer fills
                    //    over fill_sec); volume rises toward full.
                    //  - DECREASING once speed drops out the bottom of the band.
                    //    The buffer then VANISHES and stays gone until a genuine
                    //    speed increase re-arms it, so a falling speed can never
                    //    re-reach the band (no flicker). The peak trails the
                    //    trough down.
                    // Below the threshold: a speed increase over the last ~1s
                    // swells the volume (green); holding/slowing fades it. The
                    // peak indicator tracks the current speed so it does not snap
                    // when the threshold is reached.
                    float dt = std::chrono::duration_cast<
                        std::chrono::duration<float>>(now - dyn_prev_now).count();
                    if (!(dt > 0.0f) || dt > 0.5f) dt = kPollSeconds;
                    dyn_prev_now = now;

                    float floor_factor =
                        1.0f - static_cast<float>(stopped_decrease) / 100.0f;

                    uint32_t threshold_mph = dynamic_threshold_mph
                        ? dynamic_threshold_mph() : 40u;
                    threshold_mph =
                        std::clamp<uint32_t>(threshold_mph, 5u, 250u);
                    constexpr float kMetersPerSecondPerMph = 0.44704f;
                    float threshold_mps = static_cast<float>(threshold_mph) *
                                          kMetersPerSecondPerMph;

                    float buffer_pct = dynamic_buffer_percent
                        ? static_cast<float>(dynamic_buffer_percent()) : 15.0f;
                    buffer_pct = std::clamp(buffer_pct, 0.0f, 100.0f) / 100.0f;

                    float fill_sec = dynamic_buffer_fill_seconds
                        ? dynamic_buffer_fill_seconds() : 3.0f;
                    if (!std::isfinite(fill_sec) || fill_sec < 0.05f)
                        fill_sec = 0.05f;
                    float up_sec = dynamic_increase_seconds
                        ? dynamic_increase_seconds() : 1.5f;
                    if (!std::isfinite(up_sec) || up_sec < 0.05f)
                        up_sec = 0.05f;
                    float down_sec = dynamic_decrease_seconds
                        ? dynamic_decrease_seconds() : 2.5f;
                    if (!std::isfinite(down_sec) || down_sec < 0.05f)
                        down_sec = 0.05f;
                    // Rate (m/s per second) the band top eases down toward the
                    // current speed while above it. 0 = off (peak holds).
                    float max_decay_mps =
                        (dynamic_max_decay_mph_s
                             ? static_cast<float>(dynamic_max_decay_mph_s())
                             : 0.0f) * kMetersPerSecondPerMph;

                    if (!dyn_initialized) {
                        dyn_initialized = true;
                        dyn_decreasing = true; // start faded until the first push
                        dyn_peak_mps = current_speed_mps;
                        dyn_t_fill = 0.0f;
                        dyn_speed_ring_count = 0;
                        dyn_speed_ring_head = 0;
                        float range = 1.0f - floor_factor;
                        dyn_norm = range > 0.0001f
                            ? std::clamp((smoothed_night_runners_factor -
                                          floor_factor) / range, 0.0f, 1.0f)
                            : 1.0f;
                    }

                    bool above = current_speed_mps >= threshold_mps;
                    constexpr float kDynRiseEps = 0.4f; // m/s clear-rise to re-arm
                    constexpr float kDynAccelEps = 0.3f; // m/s rise over ~1s
                    float buffer_mps = 0.0f;

                    // Speed ~1s ago, then push the current sample.
                    float speed_1s_ago = current_speed_mps;
                    if (dyn_speed_ring_count > 0) {
                        int back = dyn_speed_ring_count < kDynAccelSamples
                            ? dyn_speed_ring_count : kDynAccelSamples;
                        int idx = (dyn_speed_ring_head - back) % 64;
                        if (idx < 0) idx += 64;
                        speed_1s_ago = dyn_speed_ring[idx];
                    }
                    dyn_speed_ring[dyn_speed_ring_head] = current_speed_mps;
                    dyn_speed_ring_head = (dyn_speed_ring_head + 1) % 64;
                    if (dyn_speed_ring_count < 64) dyn_speed_ring_count++;
                    bool accelerating =
                        (current_speed_mps - speed_1s_ago) > kDynAccelEps;

                    if (!above) {
                        // Below the threshold: acceleration drives the volume. A
                        // speed increase in the last second swells it (green);
                        // holding/slowing fades it (gray). The peak tracks the
                        // current speed so the indicator does not snap at the
                        // threshold.
                        dyn_peak_mps = current_speed_mps;
                        dyn_t_fill = 0.0f;
                        buffer_mps = 0.0f;
                        if (accelerating) {
                            dyn_decreasing = false;
                            dyn_norm += dt / up_sec;
                        } else {
                            dyn_decreasing = true;
                            dyn_norm -= dt / down_sec;
                        }
                    } else if (!dyn_decreasing) {
                        // INCREASING / holding above threshold: dyn_peak_mps is
                        // the band top.
                        buffer_mps = buffer_pct * dyn_peak_mps *
                                     std::min(1.0f, dyn_t_fill / fill_sec);
                        bool exit_down =
                            current_speed_mps < (dyn_peak_mps - buffer_mps);
                        if (exit_down) {
                            // Speed left the band: latch decreasing. The buffer
                            // vanishes now and stays gone until the next genuine
                            // speed increase, so a falling speed can never
                            // re-reach the band (no flicker).
                            dyn_decreasing = true;
                            dyn_peak_mps = current_speed_mps; // becomes the trough
                            dyn_t_fill = 0.0f;
                            buffer_mps = 0.0f;
                            dyn_norm -= dt / down_sec;
                        } else {
                            if (current_speed_mps > dyn_peak_mps) {
                                dyn_peak_mps = current_speed_mps; // new high
                            } else if (max_decay_mps > 0.0f) {
                                // Ease the band top down toward the current speed
                                // so a slow deceleration keeps the speed inside
                                // the band (volume stays high, band repositions).
                                dyn_peak_mps = std::max(
                                    current_speed_mps,
                                    dyn_peak_mps - max_decay_mps * dt);
                            }
                            dyn_t_fill += dt;
                            buffer_mps = buffer_pct * dyn_peak_mps *
                                         std::min(1.0f, dyn_t_fill / fill_sec);
                            dyn_norm += dt / up_sec;
                        }
                    } else {
                        // DECREASING above threshold: buffer is gone. The peak
                        // trails the trough down; only a clear speed increase
                        // above the trough re-arms increasing.
                        if (current_speed_mps < dyn_peak_mps)
                            dyn_peak_mps = current_speed_mps;
                        bool rise =
                            current_speed_mps > dyn_peak_mps + kDynRiseEps;
                        if (rise) {
                            dyn_decreasing = false;
                            dyn_peak_mps = current_speed_mps; // new band top
                            dyn_t_fill = 0.0f;
                            dyn_norm += dt / up_sec;
                        } else {
                            dyn_norm -= dt / down_sec;
                        }
                    }
                    dyn_norm = std::clamp(dyn_norm, 0.0f, 1.0f);

                    target_night_runners_factor =
                        floor_factor + (1.0f - floor_factor) * dyn_norm;
                    volume_progress = dyn_norm;
                    speed_axis_progress = dyn_norm;

                    dyn_pub_peak_mps = dyn_peak_mps;
                    dyn_pub_buffer_mps = buffer_mps; // 0 while decreasing
                    // Bar fill colour: increasing -> green (1), decreasing ->
                    // gray (0).
                    dyn_pub_state = dyn_decreasing ? 0u : 1u;

                    // Keep legacy lazy state inert while dynamic is driving.
                    lazy_held_progress = dyn_norm;
                    lazy_cooldown_active = false;
                    lazy_cooldown_fraction = 0.0f;
                } else {
                dyn_initialized = false;
                uint32_t max_mph = max_speed_mph ? max_speed_mph() : 120u;
                max_mph = std::clamp<uint32_t>(max_mph, 10u, 250u);

                constexpr float kMetersPerSecondPerMph = 0.44704f;
                float max_mps = static_cast<float>(max_mph) *
                                kMetersPerSecondPerMph;
                float raw_progress = max_mps > 0.0f
                    ? std::clamp(current_speed_mps / max_mps, 0.0f, 1.0f)
                    : 1.0f;

                // Lazy volume reduction operates on the raw speed progress (the
                // "speed peak"): volume still rises instantly with speed, but
                // after a speed peak the progress is held and only eases back
                // down once live speed has stayed below the peak for the
                // configured hold time. Re-reaching the peak resets the hold.
                float effective_progress = raw_progress;
                bool lazy_active = lazy_volume_enabled && lazy_volume_enabled();
                if (lazy_active) {
                    float hold_seconds =
                        lazy_hold_seconds ? lazy_hold_seconds() : 2.0f;
                    if (!std::isfinite(hold_seconds)) hold_seconds = 2.0f;
                    hold_seconds = std::clamp(hold_seconds, 0.5f, 10.0f);
                    constexpr float kLazyEps = 0.0005f;
                    if (raw_progress >= lazy_held_progress - kLazyEps) {
                        lazy_held_progress = raw_progress;
                        lazy_cooldown_active = false;
                        lazy_cooldown_fraction = 0.0f;
                    } else {
                        if (!lazy_cooldown_active) {
                            lazy_cooldown_active = true;
                            lazy_cooldown_started = now;
                        }
                        float elapsed_sec = std::chrono::duration_cast<
                            std::chrono::duration<float>>(
                            now - lazy_cooldown_started).count();
                        if (elapsed_sec < hold_seconds) {
                            lazy_cooldown_fraction = hold_seconds > 0.0f
                                ? std::clamp(elapsed_sec / hold_seconds,
                                             0.0f, 1.0f)
                                : 1.0f;
                        } else {
                            float alpha = 1.0f - std::exp(
                                -kPollSeconds /
                                kNightRunnersLazyReleaseTauSec);
                            lazy_held_progress +=
                                (raw_progress - lazy_held_progress) * alpha;
                            if (lazy_held_progress < raw_progress) {
                                lazy_held_progress = raw_progress;
                            }
                            lazy_cooldown_fraction = 1.0f;
                        }
                    }
                    effective_progress = lazy_held_progress;
                } else {
                    lazy_held_progress = raw_progress;
                    lazy_cooldown_active = false;
                    lazy_cooldown_fraction = 0.0f;
                }

                // The held/effective speed progress drives the graph markers;
                // applying the optional speed curve yields the volume + frequency
                // cut progress, so the lazy hold applies to both volume and the
                // frequency cut.
                speed_axis_progress = effective_progress;
                float curved_progress = effective_progress;
                if (curve_enabled && curve_enabled()) {
                    float exponent = curve_exponent ? curve_exponent() : 2.0f;
                    if (!std::isfinite(exponent)) exponent = 2.0f;
                    exponent = std::clamp(exponent, 1.0f, 4.0f);
                    curved_progress = std::pow(effective_progress, exponent);
                }
                volume_progress = curved_progress;

                float min_factor =
                    1.0f - static_cast<float>(stopped_decrease) / 100.0f;
                target_night_runners_factor =
                    min_factor + (1.0f - min_factor) * volume_progress;
                }  // end legacy (non-dynamic) mapping
            } else {
                dyn_initialized = false;
                lazy_held_progress = 1.0f;
                lazy_cooldown_active = false;
                lazy_cooldown_fraction = 0.0f;
            }
            if (non_driving_enabled && non_driving_volume_state) {
                uint32_t percent = non_driving_volume_percent
                    ? non_driving_volume_percent()
                    : 50u;
                percent = std::min<uint32_t>(percent, 100u);
                target_night_runners_factor =
                    static_cast<float>(percent) / 100.0f;
            }
        }
        bool low_cut_active =
            night_runners_active && speed_available &&
            low_cut_enabled && low_cut_enabled();
        uint32_t low_cut_amount_percent_value =
            low_cut_amount_percent ? low_cut_amount_percent() : 50u;
        low_cut_amount_percent_value =
            std::min<uint32_t>(low_cut_amount_percent_value, 100u);
        uint32_t low_cut_frequency_value =
            low_cut_frequency_hz ? low_cut_frequency_hz() : 140u;
        bool high_end_cut =
            frequency_cut_mode && frequency_cut_mode() == "high";
        low_cut_frequency_value = high_end_cut
            ? std::clamp<uint32_t>(low_cut_frequency_value, 1200u, 12000u)
            : std::clamp<uint32_t>(low_cut_frequency_value, 40u, 320u);
        if (!night_runners_active) {
            night_runners_factor_easing = false;
            smoothed_night_runners_factor = 1.0f;
            night_runners_factor = 1.0f;
            lazy_held_progress = 1.0f;
            lazy_cooldown_active = false;
            lazy_cooldown_fraction = 0.0f;
            dyn_initialized = false;
        } else if (night_runners_factor_easing) {
            auto ease_elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                now - night_runners_factor_ease_started);
            float t = std::clamp(
                static_cast<float>(ease_elapsed.count()) /
                    static_cast<float>(kNightRunnersFactorEase.count()),
                0.0f, 1.0f);
            float eased = t * t * (3.0f - 2.0f * t);
            night_runners_factor =
                night_runners_factor_ease_from +
                (target_night_runners_factor - night_runners_factor_ease_from) *
                    eased;
            smoothed_night_runners_factor = night_runners_factor;
            if (t >= 1.0f) {
                night_runners_factor_easing = false;
                smoothed_night_runners_factor = target_night_runners_factor;
                night_runners_factor = target_night_runners_factor;
            }
        } else {
            smoothed_night_runners_factor = target_night_runners_factor;
            night_runners_factor = target_night_runners_factor;
        }
        driving_speed_available_.store(speed_available, std::memory_order_release);
        driving_speed_mps_.store(current_speed_mps, std::memory_order_release);
        night_runners_volume_progress_.store(volume_progress,
                                             std::memory_order_release);
        night_runners_speed_progress_.store(speed_axis_progress,
                                            std::memory_order_release);
        night_runners_volume_factor_.store(night_runners_factor,
                                           std::memory_order_release);
        night_runners_dynamic_peak_mps_.store(dyn_pub_peak_mps,
                                              std::memory_order_release);
        night_runners_dynamic_buffer_mps_.store(dyn_pub_buffer_mps,
                                                std::memory_order_release);
        night_runners_dynamic_state_.store(dyn_pub_state,
                                           std::memory_order_release);
        float lazy_cooldown_published =
            (night_runners_active && speed_available &&
             !non_driving_volume_state)
                ? lazy_cooldown_fraction
                : 0.0f;
        night_runners_lazy_cooldown_.store(lazy_cooldown_published,
                                           std::memory_order_release);
        night_runners_non_driving_volume_active_.store(
            night_runners_active && non_driving_volume_state,
            std::memory_order_release);
        night_runners_low_cut_enabled_state_.store(low_cut_active,
                                                   std::memory_order_release);
        night_runners_high_cut_state_.store(high_end_cut,
                                            std::memory_order_release);
        night_runners_low_cut_amount_.store(
            static_cast<float>(low_cut_amount_percent_value) / 100.0f,
            std::memory_order_release);
        night_runners_low_cut_frequency_hz_state_.store(
            static_cast<float>(low_cut_frequency_value),
            std::memory_order_release);

        bool audible = r10_active && !local_hold && !playback_prebuffering;
        float target_gain =
            audible ? kRadioAudibleGain * night_runners_factor : 0.0f;
        output_accepting_.store(audible, std::memory_order_release);
#if defined(SPOTIFY_RADIO_DIAG)
        auto pending_age_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(now - non_driving_pending_since).count();
        bool diag_edge =
            !diag_state_initialized ||
            r10_active != diag_last_r10_active ||
            menu_open != diag_last_menu_open ||
            race_active != diag_last_race_active ||
            race_policy_active != diag_last_race_policy_active ||
            race_restart_menu != diag_last_race_restart_menu ||
            local_hold != diag_last_local_hold ||
            speed_available != diag_last_speed_available ||
            non_driving_candidate != diag_last_non_driving_candidate ||
            non_driving_volume_state != diag_last_non_driving_state ||
            garage_audio_candidate != diag_last_garage_candidate ||
            race_stinger_menu != diag_last_race_stinger ||
            std::fabs(night_runners_factor - diag_last_night_factor) >= 0.05f ||
            std::fabs(current_speed_mps - diag_last_speed_mps) >= 2.0f;
        bool night_diag_heartbeat =
            night_diag_heartbeat_enabled() &&
            now - last_night_diag_log >= std::chrono::seconds(1);
        if (diag_edge || night_diag_heartbeat) {
            auto gs = injector_.game_state_debug_snapshot();
            log::info(std::string("[night-diag] r10=")
                      + std::to_string(r10_active)
                      + " menu=" + std::to_string(menu_open)
                      + " local_hold=" + std::to_string(local_hold)
                      + " race=" + std::to_string(race_active)
                      + " race_policy=" + std::to_string(race_policy_active)
                      + " race_restart_menu="
                      + std::to_string(race_restart_menu)
                      + " audible=" + std::to_string(audible)
                      + " target_gain=" + std::to_string(target_gain)
                      + " night_active=" + std::to_string(night_runners_active)
                      + " night_factor=" + std::to_string(night_runners_factor)
                      + " target_night_factor="
                      + std::to_string(target_night_runners_factor)
                      + " speed_available=" + std::to_string(speed_available)
                      + " speed_mps=" + std::to_string(current_speed_mps)
                      + " speed_kmh="
                      + std::to_string(current_speed_mps * 3.6f)
                      + " volume_progress=" + std::to_string(volume_progress)
                      + " non_driving_enabled="
                      + std::to_string(non_driving_enabled)
                      + " non_driving_state="
                      + std::to_string(non_driving_volume_state)
                      + " non_driving_raw="
                      + std::to_string(non_driving_candidate)
                      + " non_driving_pending="
                      + std::to_string(non_driving_pending_state)
                      + " non_driving_pending_age_ms="
                      + std::to_string(pending_age_ms)
                      + " non_driving_easing="
                      + std::to_string(night_runners_factor_easing)
                      + " garage_candidate="
                      + std::to_string(garage_audio_candidate)
                      + " moving_with_speed="
                      + std::to_string(moving_with_valid_speed)
                      + " race_stinger=" + std::to_string(race_stinger_menu)
                      + " channel_i=" + hex(static_cast<uintptr_t>(channel_i))
                      + " ch508=" + std::to_string(ch508)
                      + " ch508_bits=" + hex(static_cast<uintptr_t>(ch508_bits))
                      + " ch568=" + std::to_string(ch568)
                      + " ch568_bits=" + hex(static_cast<uintptr_t>(ch568_bits))
                      + " radio_state=" + hex(gs.radio_state)
                      + " radio_player=" + hex(gs.radio_player)
                      + " raw_menu=" + std::to_string(gs.menu_open)
                      + " raw_race_a=" + std::to_string(gs.race_active_a)
                      + " raw_race_b=" + std::to_string(gs.race_active_b)
                      + " raw_race_restart="
                      + hex(static_cast<uintptr_t>(gs.race_restart_marker))
                      + " stinger=["
                      + hex(static_cast<uintptr_t>(gs.stinger_a)) + ","
                      + hex(static_cast<uintptr_t>(gs.stinger_b)) + ","
                      + hex(static_cast<uintptr_t>(gs.stinger_c)) + ","
                      + hex(static_cast<uintptr_t>(gs.stinger_d)) + ","
                      + hex(static_cast<uintptr_t>(gs.stinger_e)) + "]"
                      + " ring_available="
                      + std::to_string(pcm_buffered_bytes())
                      + " prebuffer=" + std::to_string(playback_prebuffering));
            diag_state_initialized = true;
            diag_last_r10_active = r10_active;
            diag_last_menu_open = menu_open;
            diag_last_race_active = race_active;
            diag_last_race_policy_active = race_policy_active;
            diag_last_race_restart_menu = race_restart_menu;
            diag_last_local_hold = local_hold;
            diag_last_speed_available = speed_available;
            diag_last_non_driving_candidate = non_driving_candidate;
            diag_last_non_driving_state = non_driving_volume_state;
            diag_last_garage_candidate = garage_audio_candidate;
            diag_last_race_stinger = race_stinger_menu;
            diag_last_night_factor = night_runners_factor;
            diag_last_speed_mps = current_speed_mps;
            last_night_diag_log = now;
        }
#endif
        if (std::fabs(target_gain - last_gain) > 0.001f) {
            output_gain_.store(target_gain, std::memory_order_release);
            if (!playback_prebuffering) {
                prebuffer_audio_.store(false, std::memory_order_release);
            }
            if (!audible && !playback_prebuffering) clear_pcm();
            bool log_gain =
                audible != last_audible ||
                std::fabs(night_runners_factor - last_logged_night_factor) >= 0.10f ||
                non_driving_volume_state != last_logged_non_driving_volume_state ||
                non_driving_candidate != last_logged_non_driving_raw_state;
            if (log_gain) {
                log::info(std::string("[native-dsp] gain=")
                          + (audible ? std::to_string(target_gain)
                                     : "0.0")
                          + " r10=" + std::to_string(r10_active)
                          + " menu=" + std::to_string(menu_open)
                          + " local_hold=" + std::to_string(local_hold)
                          + " playback_prebuffer="
                          + std::to_string(playback_prebuffering)
                          + " night_factor="
                          + std::to_string(night_runners_factor)
                          + " speed_available="
                          + std::to_string(speed_available)
                          + " non_driving="
                          + std::to_string(non_driving_volume_state)
                          + " non_driving_raw="
                          + std::to_string(non_driving_candidate)
                          + " non_driving_easing="
                          + std::to_string(night_runners_factor_easing)
                          + " target_night_factor="
                          + std::to_string(target_night_runners_factor)
                          + " garage_candidate="
                          + std::to_string(garage_audio_candidate)
                          + " moving_with_speed="
                          + std::to_string(moving_with_valid_speed)
                          + " race_stinger="
                          + std::to_string(race_stinger_menu)
#if defined(SPOTIFY_RADIO_DIAG)
                          + " ch508_bits="
                          + hex(static_cast<uintptr_t>(ch508_bits))
                          + " ch568_bits="
                          + hex(static_cast<uintptr_t>(ch568_bits))
#endif
                          );
                last_audible = audible;
                last_logged_night_factor = night_runners_factor;
                last_logged_non_driving_volume_state = non_driving_volume_state;
                last_logged_non_driving_raw_state = non_driving_candidate;
            }
            last_gain = target_gain;
        }
    }

    output_gain_.store(0.0f, std::memory_order_release);
    output_accepting_.store(false, std::memory_order_release);
    prebuffer_audio_.store(false, std::memory_order_release);
    local_audio_hold_.store(false, std::memory_order_release);
    driving_speed_available_.store(false, std::memory_order_release);
    driving_speed_mps_.store(0.0f, std::memory_order_release);
    night_runners_volume_progress_.store(1.0f, std::memory_order_release);
    night_runners_speed_progress_.store(0.0f, std::memory_order_release);
    night_runners_volume_factor_.store(1.0f, std::memory_order_release);
    night_runners_lazy_cooldown_.store(0.0f, std::memory_order_release);
    night_runners_non_driving_volume_active_.store(false, std::memory_order_release);
    night_runners_low_cut_enabled_state_.store(false, std::memory_order_release);
    night_runners_high_cut_state_.store(false, std::memory_order_release);
    night_runners_low_cut_amount_.store(0.5f, std::memory_order_release);
    night_runners_low_cut_frequency_hz_state_.store(140.0f,
                                                    std::memory_order_release);
    log::info("[native-dsp] Native PCM control loop exiting");
}

} // namespace bridge
