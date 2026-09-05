// QQ Music process-loopback source.
//
// Captures the audio rendered by a configurable QQ Music process tree and
// feeds it into the same native FMOD radio sink used by the other sources.

#pragma once

#include "audio_source.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace bridge {

class QQMusicSource final : public AudioSource {
public:
    using FeedPcmFn =
        std::function<bool(const float* samples, size_t frames)>;
    using ClearPcmFn = std::function<void()>;

    QQMusicSource(FeedPcmFn feed_pcm,
                  ClearPcmFn clear_pcm,
                  std::string process_name,
                  std::string executable_path);
    ~QQMusicSource() override;

    QQMusicSource(const QQMusicSource&) = delete;
    QQMusicSource& operator=(const QQMusicSource&) = delete;

    void start();
    void shutdown();

    const char* id() const override { return "qqmusic"; }
    const char* display_name() const override { return "QQ Music"; }

    bool is_connected() const override;
    bool is_playing() const override;
    SourceTrack last_track() const override;
    SourceCapabilities capabilities() const override {
        return {true, true, true, true, true, false};
    }

    void pause_at_audio_boundary() override;
    void resume_rewound(uint32_t rewind_ms) override;
    bool restart_current_track() override;
    bool next_track() override;
    bool previous_track() override;
    bool seek(uint32_t position_ms) override;

    void on_activated() override;
    void on_deactivated() override;
    void set_pcm_enabled(bool enabled) override;
    void set_volume_normalization(bool enabled) override;
    void set_equalizer(bool enabled,
                       const std::array<float, 5>& bands_db) override;

private:
    void worker_thread_fn();
    bool capture_process_tree(std::uint32_t process_id);
    bool find_qqmusic_process_id(std::uint32_t& process_id) const;
    void launch_qqmusic_if_needed() const;

    static void send_media_key(std::uint16_t virtual_key);

    FeedPcmFn feed_pcm_;
    ClearPcmFn clear_pcm_;
    std::wstring process_name_;
    std::filesystem::path executable_path_;
    SourceTrack last_track_;

    std::atomic<bool> running_{false};
    std::atomic<bool> capture_active_{false};
    std::atomic<bool> pcm_enabled_{false};
    std::thread worker_;
    std::chrono::steady_clock::time_point last_packet_time_{};
    mutable std::mutex track_mutex_;
};

} // namespace bridge
