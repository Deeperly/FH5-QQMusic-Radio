// Bridge-owned live online radio source.
//
// Searches Radio Browser / accepts manual stream URLs, persists station
// metadata, and decodes live MP3/AAC streams through Media Foundation into the
// same PCM contract as the other bridge-owned sources.

#pragma once

#include "audio_source.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {

class RadioSource final : public AudioSource {
public:
    using FeedPcmFn =
        std::function<bool(const int16_t* samples, size_t frames, float gain)>;
    using FreeBytesFn = std::function<size_t()>;
    using ClearPcmFn = std::function<void()>;

    struct Station {
        std::string id;
        std::string provider;
        std::string stationuuid;
        std::string name;
        std::string stream_url;
        std::string resolved_url;
        std::string homepage;
        std::string favicon;
        std::string tags;
        std::string country_code;
        std::string language;
        std::string codec;
        uint32_t bitrate = 0;
        bool hls = false;
        int64_t added_at_ms = 0;
        int64_t last_played_at_ms = 0;
    };

    RadioSource(std::filesystem::path store_path,
                FeedPcmFn feed_pcm,
                FreeBytesFn free_bytes,
                ClearPcmFn clear_pcm);
    ~RadioSource() override;

    RadioSource(const RadioSource&) = delete;
    RadioSource& operator=(const RadioSource&) = delete;

    void start();
    void shutdown();

    const char* id() const override { return "radio"; }
    const char* display_name() const override { return "Online Radio"; }

    bool is_connected() const override;
    bool is_playing() const override;
    SourceTrack last_track() const override;
    SourceCapabilities capabilities() const override {
        return {true, true, false, false, false, false};
    }

    void pause_at_audio_boundary() override;
    void resume_rewound(uint32_t rewind_ms) override;
    void on_activated() override;
    bool restart_current_track() override { return false; }
    bool next_track() override { return false; }
    bool seek(uint32_t position_ms) override {
        (void)position_ms;
        return false;
    }
    void set_pcm_enabled(bool enabled) override;

    void set_volume_normalization(bool enabled) override { (void)enabled; }
    void set_equalizer(bool enabled, const std::array<float, 5>& bands_db) override {
        (void)enabled;
        (void)bands_db;
    }
    void set_volume_percent(uint32_t percent);
    void publish_state(BridgeStateStore& store) const override;

    std::string search_json(const std::string& q,
                            const std::string& tag,
                            const std::string& country_code,
                            const std::string& language,
                            uint32_t offset,
                            uint32_t limit,
                            std::string* error = nullptr);
    std::string suggestions_json(const std::string& kind,
                                 uint32_t limit,
                                 std::string* error = nullptr);
    std::string favorites_json() const;
    std::string manual_json() const;
    bool add_favorite_json(std::string_view json, std::string* error = nullptr);
    bool remove_favorite(const std::string& id, std::string* error = nullptr);
    bool remove_manual(const std::string& id, std::string* error = nullptr);
    bool play_station_json(std::string_view json, std::string* error = nullptr);
    bool play_station_id(const std::string& id, std::string* error = nullptr);
    bool play_url(const std::string& url,
                  const std::string& name,
                  std::string* error = nullptr);
    void stop_playback();
    void resume_last_station_async();

    std::string last_error() const;
    Station current_station() const;

private:
    struct Store {
        std::string last_station_id;
        bool last_playing = false;
        std::string last_manual_url;
        std::string last_manual_name;
        std::vector<Station> favorites;
        std::vector<Station> recent;
        std::vector<Station> manual;
    };

    void load_store();
    bool save_store_locked(std::string* error = nullptr) const;
    bool play_station(Station station, std::string* error);
    bool start_stream_locked(const Station& station, const std::string& url);
    void stop_stream();
    void start_metadata_locked(const Station& station, const std::string& url);
    void stop_metadata();
    void stream_thread_fn(uint64_t generation, Station station, std::string url);
    void metadata_thread_fn(uint64_t generation, Station station, std::string url);
    void update_stream_title(uint64_t generation,
                             const Station& station,
                             const std::string& stream_title);
    void set_error(const std::string& error);
    void remember_recent_locked(const Station& station);
    std::vector<uint8_t> download_artwork(const std::string& url) const;

    static bool parse_station_json(std::string_view json, Station& out);
    static std::string station_to_json(const Station& station);
    static std::string station_array_json(std::string_view key,
                                          const std::vector<Station>& stations);
    static std::string make_manual_id(const std::string& url);

    std::filesystem::path store_path_;
    FeedPcmFn feed_pcm_;
    FreeBytesFn free_bytes_;
    ClearPcmFn clear_pcm_;

    mutable std::mutex mtx_;
    Store store_;
    Station current_station_;
    SourceTrack current_track_;
    std::string error_;

    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> pcm_enabled_{false};
    std::atomic<bool> stop_stream_{false};
    std::atomic<bool> stop_metadata_{false};
    std::atomic<bool> resume_in_progress_{false};
    std::atomic<float> volume_gain_{1.0f};
    uint64_t stream_generation_ = 0;
    std::thread stream_thread_;
    std::thread metadata_thread_;
    std::thread resume_thread_;
};

} // namespace bridge
