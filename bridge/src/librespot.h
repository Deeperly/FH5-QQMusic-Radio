// librespotc Session lifecycle wrapper.
//
// Owns a librespotc::Session, drives connect (cached blob → Zeroconf),
// pumps PCM into FmodInject, surfaces events to AppState / SSE.
//
// Construction does NOT block. start() spawns a background thread that
// runs Session::connect() and waits for first credentials. While that
// thread is waiting on Zeroconf, the host UI can render a "Pick FH6 Radio
// (PC-NAME) from your Spotify app" prompt.

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace librespotc { class Session; }

namespace bridge {

class BridgeStateStore;

struct LibrespotConfig {
    std::string device_name = "FH6 Radio";
    std::filesystem::path cache_dir;
    bool volume_normalization = true;
    bool equalizer_enabled = false;
    std::array<float, 5> equalizer_bands_db{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

class LibrespotPlayer {
public:
    using TrackChangedCb = std::function<void(const std::string& uri,
                                              const std::string& title,
                                              const std::string& artist,
                                              bool from_cloud)>;
    using TrackEndedCb   = std::function<void()>;
    using StateChangedCb = std::function<void(bool playing, bool from_cloud)>;

    explicit LibrespotPlayer(LibrespotConfig cfg);
    ~LibrespotPlayer();

    LibrespotPlayer(const LibrespotPlayer&) = delete;
    LibrespotPlayer& operator=(const LibrespotPlayer&) = delete;

    void start(bool advertise_for_selection = false);
    void stop();

    // Returns empty until Session::connect() succeeds.
    std::string spotify_device_id() const;
    bool        is_connected() const;
    bool        is_playing() const;
    void        set_pcm_enabled(bool enabled);

    // Direct playback API (local-source events).
    bool play_track(const std::string& spotify_uri);
    void pause();
    void pause_at_audio_boundary();
    void resume();
    void resume_rewound(uint32_t rewind_ms);
    bool seek(uint32_t position_ms);
    uint32_t current_position_ms() const;
    bool next_track();
    void stop_track();
    void set_volume_normalization(bool enabled);
    void set_equalizer(bool enabled, const std::array<float, 5>& bands_db);

    // Latest track metadata captured via librespotc on_track_change. Used
    // by the metadata-pump thread to inject HUD strings each poll cycle.
    // Empty until the first track plays.
    struct LastTrack {
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
    LastTrack last_track() const;

    // True iff credentials.dat exists under cfg_.cache_dir. Surfaced via
    // BridgeStateStore so the UI can show "ready to pair" vs "ready to
    // resume" hint.
    bool blob_cached() const;

    // Bootstrap login with a Spotify OAuth access token (alternative to Zeroconf
    // pairing — works under the GamePass AppContainer since it's outbound-only).
    // Stores the token and reconnects via connect() (cache -> oauth -> zeroconf)
    // so librespotc logs in with it and persists a reusable credential to
    // credentials.dat; subsequent launches then log in silently from cache.
    // Returns true if the token was accepted and a reconnect was triggered.
    bool authenticate_with_oauth(const std::string& access_token);

    // Event hooks — set before start(). Invoked on librespotc's playback
    // thread; do not call any Session method from inside them.
    void set_on_track_changed(TrackChangedCb cb)  { on_track_changed_ = std::move(cb); }
    void set_on_track_ended(TrackEndedCb cb)      { on_track_ended_ = std::move(cb); }
    void set_on_state_changed(StateChangedCb cb)  { on_state_changed_ = std::move(cb); }

    // v1 — push connection / playback / track events into BridgeStateStore.
    // Optional; set before start(). When non-null, the player publishes
    // all relevant state transitions directly into the store so the SSE
    // layer can render them without a separate poller.
    void set_state_store(BridgeStateStore* s) { state_store_ = s; }

private:
    void connect_thread_fn();
#if defined(SPOTIFY_RADIO_DIAG)
    void diag_reset_track_audio_counters();
    std::string diag_playback_summary() const;
    void diag_log_control_result(const char* action, bool ok) const;
    // Background thread: samples session_->current_position_ms() while
    // playing and logs [pos-diag] heartbeat + position-regression lines.
    // Catches the "song restarts at halfway" repro — a backwards position
    // jump with no surrounding event is a silent cloud seek / restart, and
    // a play-advance/wall ratio near 2.0 means the position clock runs 2x.
    void diag_position_loop();
#endif

    LibrespotConfig     cfg_;
    std::unique_ptr<librespotc::Session> session_;
    std::mutex          lifecycle_mtx_;
    std::atomic<bool>   running_{false};
    std::atomic<bool>   connected_{false};
    std::atomic<bool>   playing_{false};
    std::atomic<bool>   pcm_enabled_{true};
    std::atomic<bool>   advertise_for_selection_{false};
    std::thread         connect_thread_;
    mutable std::mutex  device_id_mtx_;
    std::string         device_id_;

    // OAuth bootstrap token, consumed one-shot by the next start() and fed to
    // librespotc as Config::oauth_token. Empty once a credential is cached.
    mutable std::mutex  oauth_mtx_;
    std::string         pending_oauth_token_;

    mutable std::mutex  last_track_mtx_;
    LastTrack           last_track_;

    TrackChangedCb      on_track_changed_;
    TrackEndedCb        on_track_ended_;
    StateChangedCb      on_state_changed_;

    BridgeStateStore*   state_store_ = nullptr;

    // Wall-clock of the previous on_event firing, used so each diagnostic
    // log line carries the delta-ms since the previous event. Lets the log
    // reader bracket the disconnect window against TrackEnded / Reconnecting
    // without needing absolute timestamps to be sub-second-precise.
    std::atomic<std::chrono::steady_clock::time_point> last_event_at_{
        std::chrono::steady_clock::time_point{}};

#if defined(SPOTIFY_RADIO_DIAG)
    std::atomic<uint64_t> diag_track_seq_{0};
    std::atomic<uint64_t> diag_track_audio_callbacks_{0};
    std::atomic<uint64_t> diag_track_audio_frames_{0};
    std::atomic<uint64_t> diag_track_pcm_disabled_drops_{0};
    std::atomic<uint64_t> diag_track_no_sink_drops_{0};
    std::atomic<uint64_t> diag_track_feed_false_{0};
    std::atomic<uint32_t> diag_track_sample_rate_{44100};
    std::atomic<uint16_t> diag_track_channels_{2};
    std::thread           diag_pos_thread_;
#endif
};

} // namespace bridge
