// AirPlay AudioSource adapter.
//
// Owns the FH6-facing AirPlay receiver wrapper and exposes it through the
// source-neutral control surface used by SourceManager.

#pragma once

#include "audio_source.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace airplayc { class Session; }

namespace bridge {

class BridgeStateStore;

class AirPlayPlayer {
public:
    using FeedPcmFn =
        std::function<bool(const int16_t* samples, size_t frames, float gain)>;

    AirPlayPlayer(std::string base_device_name,
                  std::filesystem::path cache_dir,
                  FeedPcmFn feed_pcm);
    ~AirPlayPlayer();

    AirPlayPlayer(const AirPlayPlayer&) = delete;
    AirPlayPlayer& operator=(const AirPlayPlayer&) = delete;

    bool start();
    void stop();
    void set_pcm_enabled(bool enabled);

    bool is_running() const;
    bool is_connected() const;
    bool is_playing() const;
    std::optional<uint32_t> current_position_ms() const;
    std::string device_name() const;
    std::string last_error() const;
    SourceTrack last_track() const;
    void publish_state(BridgeStateStore& store) const;
    float volume_db() const;
    uint32_t volume_percent() const;
    bool pause();
    bool resume();
    bool next_track();
    bool previous_track();

private:
    std::optional<uint32_t> current_position_ms_locked(
        std::chrono::steady_clock::time_point now) const;
    void reset_position_locked(uint32_t position_ms = 0);
    void freeze_position_locked();
    static std::string device_name_with_machine_suffix(const std::string& base);
    static float clamp_volume_db(float db);
    static float gain_from_db(float db);
    static uint32_t percent_from_db(float db);
    void schedule_volume_restore(const char* reason, uint32_t delay_ms);
    void stop_volume_restore_thread();
    bool handle_audio(const int16_t* samples, size_t frame_count,
                      uint32_t sample_rate, uint16_t channels,
                      uint16_t bits_per_sample);
    void apply_volume_db(float db);
    void set_error(std::string error);

    std::string base_device_name_;
    std::string advertised_device_name_;
    std::filesystem::path cache_dir_;
    FeedPcmFn feed_pcm_;

    mutable std::mutex control_mtx_;
    mutable std::mutex mtx_;
    std::unique_ptr<airplayc::Session> session_;
    SourceTrack current_track_;
    bool position_known_ = false;
    uint32_t position_base_ms_ = 0;
    std::chrono::steady_clock::time_point position_sampled_at_{};
    uint64_t artwork_revision_ = 0;
    std::chrono::steady_clock::time_point last_cover_art_at_{};
    std::string error_;
    float volume_db_ = -20.0f;
    float volume_gain_ = 0.1f;
    std::mutex volume_restore_thread_mtx_;
    std::jthread volume_restore_thread_;
    std::atomic<uint64_t> volume_restore_generation_{0};
    std::atomic<bool> volume_restore_scheduled_{false};

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> pcm_enabled_{false};
};

class AirPlaySource final : public AudioSource {
public:
    explicit AirPlaySource(AirPlayPlayer& player);

    const char* id() const override { return "airplay"; }
    const char* display_name() const override { return "AirPlay"; }

    bool is_connected() const override;
    bool is_playing() const override;
    SourceTrack last_track() const override;
    SourceCapabilities capabilities() const override {
        return {true, true, true, true, true, false};
    }

    void pause_at_audio_boundary() override;
    void resume_rewound(uint32_t rewind_ms) override;
    std::optional<uint32_t> current_position_ms() const override;
    // iOS rejects DACP seek, but DACP previous restarts the current item once
    // the sender considers it far enough into the song. We intentionally do
    // not add a hidden early-song guard here; the race-start smart threshold is
    // the user-visible control for conditional AirPlay restarts.
    bool restart_current_track() override;
    bool next_track() override;
    bool previous_track() override;
    void on_activated() override;
    void on_deactivated() override;
    void set_pcm_enabled(bool enabled) override;

    void set_volume_normalization(bool enabled) override { (void)enabled; }
    void set_equalizer(bool enabled,
                       const std::array<float, 5>& bands_db) override {
        (void)enabled;
        (void)bands_db;
    }
    void publish_state(BridgeStateStore& store) const override;

private:
    AirPlayPlayer& player_;
};

} // namespace bridge
