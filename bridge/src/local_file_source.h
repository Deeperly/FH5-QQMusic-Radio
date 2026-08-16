// Bridge-owned local file source.
//
// Decodes user-selected MP3/WAV/FLAC files into the same 44.1 kHz stereo
// S16LE PCM contract used by Spotify, then feeds the native DSP sink.

#pragma once

#include "audio_source.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

class LocalFileSource final : public AudioSource {
public:
    using FeedPcmFn =
        std::function<bool(const int16_t* samples, size_t frames, float gain)>;
    using FreeBytesFn = std::function<size_t()>;
    using ClearPcmFn = std::function<void()>;

    struct TrackInfo {
        uint32_t index = 0;
        std::string path;
        std::string title;
        std::string folder;
    };

    struct QueueEntryInfo {
        uint64_t id = 0;
        TrackInfo track;
    };

    LocalFileSource(FeedPcmFn feed_pcm, FreeBytesFn free_bytes, ClearPcmFn clear_pcm);
    ~LocalFileSource() override;

    LocalFileSource(const LocalFileSource&) = delete;
    LocalFileSource& operator=(const LocalFileSource&) = delete;

    void start();
    void shutdown();

    const char* id() const override { return "local"; }
    const char* display_name() const override { return "Local Files"; }

    bool is_connected() const override;
    bool is_playing() const override;
    SourceTrack last_track() const override;
    std::optional<uint32_t> current_position_ms() const override;
    SourceCapabilities capabilities() const override {
        return {true, true, true, true, true, true};
    }

    void pause_at_audio_boundary() override;
    void resume_rewound(uint32_t rewind_ms) override;
    bool restart_current_track() override;
    bool next_track() override;
    bool previous_track() override;
    bool seek(uint32_t position_ms) override;
    void set_pcm_enabled(bool enabled) override;

    void set_volume_normalization(bool enabled) override;
    void set_equalizer(bool enabled, const std::array<float, 5>& bands_db) override;
    void set_volume_percent(uint32_t percent);
    void set_metadata_display_modes(std::string title_mode,
                                    std::string artist_mode);
    void publish_state(BridgeStateStore& store) const override;

    bool configure(std::string music_dir, bool recursive, bool shuffle,
                   std::string* error = nullptr);
    bool rescan(std::string* error = nullptr);
    void set_shuffle(bool shuffle);
    std::vector<TrackInfo> library_snapshot() const;
    std::vector<QueueEntryInfo> manual_queue_snapshot() const;
    std::vector<TrackInfo> autoplay_queue_snapshot() const;
    bool play_library_index(uint32_t index);
    bool queue_library_index(uint32_t index);
    uint32_t queue_folder(std::string folder);
    bool remove_manual_queue_entry(uint64_t id);

    std::string music_dir() const;
    std::string default_music_dir() const;
    bool recursive() const;
    bool shuffle() const;
    uint32_t track_count() const;
    uint32_t unsupported_count() const;
    uint32_t position_ms() const;
    std::string last_error() const;

private:
    struct Decoder;
    struct Biquad;

    void worker_thread_fn();
    bool open_track_locked(size_t index);
    bool open_path_locked(const std::filesystem::path& path);
    bool open_playable_track_locked(size_t start_index, bool forward);
    bool advance_to_next_track_locked();
    size_t maybe_reshuffle_for_wrap_locked(size_t start_index);
    void rebuild_autoplay_queue_locked();
    TrackInfo track_info_locked(const std::filesystem::path& path,
                                uint32_t index) const;
    void close_decoder_locked();
    bool decoder_is_open_locked() const;
    bool seek_decoder_locked(uint32_t position_ms);
    uint64_t read_decoder_frames_locked(int16_t* samples, uint64_t frames);
    uint32_t decoder_cursor_ms_locked() const;
    void rebuild_equalizer_locked();
    void process_equalizer_locked(int16_t* samples, size_t frames);
    void apply_current_track_metadata_locked(const std::filesystem::path& path);

    static std::string path_to_utf8(const std::filesystem::path& path);
    static std::filesystem::path path_from_utf8(const std::string& path);
    static std::string title_from_path(const std::filesystem::path& path);
    static std::string resolve_default_music_dir();

    FeedPcmFn feed_pcm_;
    FreeBytesFn free_bytes_;
    ClearPcmFn clear_pcm_;

    mutable std::mutex mtx_;
    std::filesystem::path music_dir_;
    std::string default_music_dir_;
    bool recursive_ = true;
    bool shuffle_ = true;
    std::vector<std::filesystem::path> library_;
    std::vector<std::filesystem::path> playlist_;
    size_t cursor_ = 0;
    struct ManualQueueEntry {
        uint64_t id = 0;
        std::filesystem::path path;
    };
    std::vector<ManualQueueEntry> manual_queue_;
    std::vector<std::filesystem::path> autoplay_queue_;
    uint64_t next_queue_id_ = 1;
    uint32_t unsupported_count_ = 0;
    std::string error_;
    std::filesystem::path current_path_;
    SourceTrack current_track_;
    uint32_t position_ms_ = 0;

    std::unique_ptr<Decoder> decoder_;
    std::unique_ptr<Biquad[]> equalizer_;
    bool equalizer_enabled_ = false;
    std::array<float, 5> equalizer_bands_db_{};
    bool volume_normalization_ = true;
    uint32_t volume_percent_ = 100;
    float volume_gain_ = 1.0f;
    float replaygain_gain_ = 1.0f;
    std::string title_metadata_mode_ = "metadata";
    std::string artist_metadata_mode_ = "albumArtist";

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> pcm_enabled_{false};
    std::thread worker_;
};

} // namespace bridge
