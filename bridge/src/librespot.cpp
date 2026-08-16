#include "librespot.h"
#include "bridge_state.h"
#include "fmod_inject.h"
#include "log_file.h"

#include <librespotc/librespotc.h>

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <random>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace bridge {

namespace {

constexpr uint32_t kDefaultSpotifyVolume = 32768;
constexpr uint32_t kMaxSpotifyVolume = 65535;
constexpr const char* kVolumeStateFile = "volume.dat";
constexpr const char* kDeviceIdFile = "device-id.dat";
constexpr const char* kLegacyDeviceName = "FH6 Radio";
constexpr const char* kLegacyDeviceId =
    "26d96ec95f15dc791bfada63db0fa6633416d563";

uint32_t load_saved_volume(const std::filesystem::path& cache_dir) {
    std::ifstream in(cache_dir / kVolumeStateFile);
    uint32_t v = kDefaultSpotifyVolume;
    if (!(in >> v)) return kDefaultSpotifyVolume;
    if (v > kMaxSpotifyVolume) return kDefaultSpotifyVolume;
    return v;
}

void save_volume(const std::filesystem::path& cache_dir, uint32_t volume) {
    if (volume > kMaxSpotifyVolume) return;
    std::ofstream out(cache_dir / kVolumeStateFile, std::ios::trunc);
    if (out) out << volume << '\n';
}

bool is_hex_device_id(const std::string& value) {
    if (value.size() != 40) return false;
    for (unsigned char c : value) {
        if (!std::isxdigit(c)) return false;
    }
    return true;
}

std::string normalize_device_id(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string hex_lower(const std::array<uint8_t, 20>& bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

std::string create_device_id() {
    std::array<uint8_t, 20> bytes{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(),
                                        static_cast<ULONG>(bytes.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        std::random_device rd;
        for (uint8_t& b : bytes) {
            b = static_cast<uint8_t>(rd() & 0xffu);
        }
    }
    return hex_lower(bytes);
}

void persist_device_id(const std::filesystem::path& cache_dir,
                       const std::string& value) {
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    const auto path = cache_dir / kDeviceIdFile;
    const auto tmp = cache_dir / "device-id.tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (out) out << value << '\n';
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        ec.clear();
        std::ofstream out(path, std::ios::trunc);
        if (out) out << value << '\n';
    }
}

bool credentials_predate_device_id(const std::filesystem::path& cache_dir) {
    std::error_code ec;
    const auto credentials = cache_dir / "credentials.dat";
    const auto device_id = cache_dir / kDeviceIdFile;
    if (!std::filesystem::exists(credentials, ec)) return false;
    ec.clear();
    if (!std::filesystem::exists(device_id, ec)) return true;
    ec.clear();
    auto credentials_time = std::filesystem::last_write_time(credentials, ec);
    if (ec) return false;
    auto device_id_time = std::filesystem::last_write_time(device_id, ec);
    if (ec) return false;
    return credentials_time < device_id_time;
}

std::string load_or_create_device_id(const std::filesystem::path& cache_dir,
                                     const std::string& base_name) {
    const auto path = cache_dir / kDeviceIdFile;
    const bool legacy_upgrade =
        base_name == kLegacyDeviceName && credentials_predate_device_id(cache_dir);
    {
        std::ifstream in(path);
        std::string value;
        if (in >> value && is_hex_device_id(value)) {
            if (legacy_upgrade && normalize_device_id(value) != kLegacyDeviceId) {
                persist_device_id(cache_dir, kLegacyDeviceId);
                log::info("[librespot] Restored legacy Spotify device identity");
                return kLegacyDeviceId;
            }
            return normalize_device_id(value);
        }
    }

    // Compatibility path for users upgrading from v1.1.1 and older: those
    // installs have cached Spotify credentials keyed to SHA1("FH6 Radio") but
    // no device-id.dat yet. Preserve that identity instead of silently rotating
    // to a new random Connect device.
    if (legacy_upgrade) {
        persist_device_id(cache_dir, kLegacyDeviceId);
        log::info("[librespot] Preserved legacy Spotify device identity");
        return kLegacyDeviceId;
    }

    std::string value = create_device_id();
    persist_device_id(cache_dir, value);
    log::info("[librespot] Created stable Spotify device identity");
    return value;
}

std::string machine_name() {
    char buf[256] = {};
    DWORD len = static_cast<DWORD>(sizeof(buf));
    if (!GetComputerNameExA(ComputerNameDnsHostname, buf, &len) || len == 0) {
        len = static_cast<DWORD>(sizeof(buf));
        if (!GetComputerNameA(buf, &len) || len == 0) return {};
    }

    std::string name(buf, len);
    std::string clean;
    clean.reserve(name.size());
    for (unsigned char c : name) {
        if (c >= 0x21 && c <= 0x7e && c != '(' && c != ')') {
            clean.push_back(static_cast<char>(c));
        }
    }
    return clean;
}

std::string device_name_with_machine_suffix(const std::string& base) {
    std::string pc = machine_name();
    if (pc.empty()) return base;
    if (base.find(" (" + pc + ")") != std::string::npos) return base;
    return base + " (" + pc + ")";
}

uint32_t parse_volume_detail(const std::string& detail) {
    try {
        size_t used = 0;
        unsigned long v = std::stoul(detail, &used, 10);
        if (used == 0 || used != detail.size() || v > kMaxSpotifyVolume) {
            return kMaxSpotifyVolume + 1u;
        }
        return static_cast<uint32_t>(v);
    } catch (...) {
        return kMaxSpotifyVolume + 1u;
    }
}

} // namespace

#if defined(SPOTIFY_RADIO_DIAG)
void LibrespotPlayer::diag_reset_track_audio_counters() {
    diag_track_audio_callbacks_.store(0, std::memory_order_release);
    diag_track_audio_frames_.store(0, std::memory_order_release);
    diag_track_pcm_disabled_drops_.store(0, std::memory_order_release);
    diag_track_no_sink_drops_.store(0, std::memory_order_release);
    diag_track_feed_false_.store(0, std::memory_order_release);
    diag_track_sample_rate_.store(44100, std::memory_order_release);
    diag_track_channels_.store(2, std::memory_order_release);
}

std::string LibrespotPlayer::diag_playback_summary() const {
    LastTrack track;
    {
        std::lock_guard lock(last_track_mtx_);
        track = last_track_;
    }
    uint64_t frames =
        diag_track_audio_frames_.load(std::memory_order_acquire);
    uint32_t rate =
        diag_track_sample_rate_.load(std::memory_order_acquire);
    uint64_t pcm_ms = rate > 0 ? (frames * 1000ull) / rate : 0;
    std::string uri = track.uri.empty() ? "-" : track.uri;
    return std::string("seq=")
        + std::to_string(diag_track_seq_.load(std::memory_order_acquire))
        + " uri=" + uri
        + " pcm_ms=" + std::to_string(pcm_ms)
        + " callbacks="
        + std::to_string(
            diag_track_audio_callbacks_.load(std::memory_order_acquire))
        + " frames=" + std::to_string(frames)
        + " rate=" + std::to_string(rate)
        + " ch="
        + std::to_string(
            diag_track_channels_.load(std::memory_order_acquire))
        + " pcm_disabled_drops="
        + std::to_string(
            diag_track_pcm_disabled_drops_.load(std::memory_order_acquire))
        + " no_sink_drops="
        + std::to_string(
            diag_track_no_sink_drops_.load(std::memory_order_acquire))
        + " feed_false="
        + std::to_string(
            diag_track_feed_false_.load(std::memory_order_acquire));
}

void LibrespotPlayer::diag_log_control_result(const char* action,
                                              bool ok) const {
    log::info(std::string("[skip-diag] host ") + action
              + " ok=" + std::to_string(ok)
              + " " + diag_playback_summary());
}

void LibrespotPlayer::diag_position_loop() {
    using clock = std::chrono::steady_clock;
    constexpr int kIntervalMs = 2000;
    // A backwards jump larger than this, with no track change in between, is
    // a seek/restart rather than normal forward play.
    constexpr uint32_t kRegressionThreshMs = 1500;

    uint64_t last_seq = diag_track_seq_.load(std::memory_order_acquire);
    uint32_t last_pos = 0;
    bool was_playing = false;
    clock::time_point t0{};
    uint32_t pos0 = 0;

    auto pct_of = [](uint32_t pos, uint32_t dur) -> int {
        return dur > 0 ? static_cast<int>((static_cast<uint64_t>(pos) * 100) / dur)
                       : -1;
    };

    while (running_.load(std::memory_order_acquire)) {
        for (int slept = 0; slept < kIntervalMs &&
                            running_.load(std::memory_order_acquire); slept += 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!running_.load(std::memory_order_acquire)) break;

        if (!playing_.load(std::memory_order_acquire) || !session_) {
            was_playing = false;
            last_pos = 0;
            continue;
        }

        const uint64_t seq = diag_track_seq_.load(std::memory_order_acquire);
        const uint32_t pos = session_->current_position_ms();
        const auto now = clock::now();

        // (Re)entering playback or a new track: reset the per-segment baseline
        // so a paused gap or track boundary is not mistaken for a regression.
        bool new_baseline = false;
        if (!was_playing || seq != last_seq) {
            t0 = now;
            pos0 = pos;
            last_seq = seq;
            last_pos = pos;
            was_playing = true;
            new_baseline = true;
        }

        LastTrack track;
        { std::lock_guard lock(last_track_mtx_); track = last_track_; }
        const uint32_t dur = track.duration_ms;
        const uint64_t frames =
            diag_track_audio_frames_.load(std::memory_order_acquire);
        const uint32_t rate =
            diag_track_sample_rate_.load(std::memory_order_acquire);
        const uint64_t pcm_ms = rate > 0 ? (frames * 1000ull) / rate : 0;
        const long long wall_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        const long long adv = static_cast<long long>(pos) - static_cast<long long>(pos0);
        const double ratio = wall_ms > 0 ? static_cast<double>(adv) / wall_ms : 0.0;

        if (!new_baseline && last_pos > pos + kRegressionThreshMs) {
            log::warn("[pos-diag] *** POSITION REGRESSION *** prev_ms="
                      + std::to_string(last_pos) + " now_ms=" + std::to_string(pos)
                      + " drop_ms=" + std::to_string(last_pos - pos)
                      + " dur_ms=" + std::to_string(dur)
                      + " pct_before=" + std::to_string(pct_of(last_pos, dur))
                      + " seq=" + std::to_string(seq)
                      + " uri=" + (track.uri.empty() ? std::string("-") : track.uri)
                      + " — backwards jump (likely cloud seek / restart)");
        }

        log::info("[pos-diag] hb seq=" + std::to_string(seq)
                  + " pos_ms=" + std::to_string(pos)
                  + " dur_ms=" + std::to_string(dur)
                  + " pct=" + std::to_string(pct_of(pos, dur))
                  + " pcm_ms=" + std::to_string(pcm_ms)
                  + " wall_ms=" + std::to_string(wall_ms)
                  + " pos_per_wall=" + std::to_string(ratio)
                  + " rate=" + std::to_string(rate)
                  + " ch=" + std::to_string(
                        diag_track_channels_.load(std::memory_order_acquire)));
        last_pos = pos;
    }
}
#endif

LibrespotPlayer::LibrespotPlayer(LibrespotConfig cfg)
    : cfg_(std::move(cfg)) {}

LibrespotPlayer::~LibrespotPlayer() { stop(); }

void LibrespotPlayer::start(bool advertise_for_selection) {
    std::lock_guard lifecycle_lock(lifecycle_mtx_);
    if (running_.exchange(true)) return;
    advertise_for_selection_.store(advertise_for_selection,
                                   std::memory_order_release);

    librespotc::Config lc;
    lc.device_name = device_name_with_machine_suffix(cfg_.device_name);
    lc.device_id   = load_or_create_device_id(cfg_.cache_dir, cfg_.device_name);
    lc.device_type = librespotc::DeviceType::Speaker;
    lc.cache_dir   = cfg_.cache_dir.string();
    lc.bitrate     = librespotc::Bitrate::K160;
    // Let librespotc apply Spotify Connect volume directly to PCM. FMOD owns
    // only the radio-bus baseline / station gate, avoiding double attenuation.
    lc.apply_volume_gain = true;
    lc.volume_gain_max = 3.0f;
    // Match Spotify/librespot normal playback loudness so playlists do not
    // jump in level between tracks before FMOD's station gate is applied.
    lc.apply_replaygain = cfg_.volume_normalization;
    lc.equalizer.enabled = cfg_.equalizer_enabled;
    lc.equalizer.bands_db = cfg_.equalizer_bands_db;
    lc.initial_volume = load_saved_volume(cfg_.cache_dir);
    // Spotify's public "keymaster" client_id (same one librespot uses) so the
    // OAuth-token login path and client-token attestation work.
    lc.client_id = "65b708073fc0480ea92a077233ca87bd";
    // Consume a one-shot OAuth bootstrap token if the user just logged in via
    // the web UI. connect() tries cache -> oauth_token -> zeroconf, so this is
    // only used until librespotc has cached a reusable credential.
    {
        std::lock_guard oauth_lock(oauth_mtx_);
        if (!pending_oauth_token_.empty()) {
            lc.oauth_token = pending_oauth_token_;
            pending_oauth_token_.clear();
            log::info("[librespot] OAuth bootstrap token set for this login");
        }
    }
    log::info("[librespot] Advertising Spotify Connect device as \""
              + lc.device_name + "\"");
    // First run: user picks "FH6 Radio (PC-NAME)" in their Spotify app. Cached
    // credentials.dat persists in cache_dir for subsequent silent logins.

    // Shipping audio pipeline: librespotc PCM into the native R10 FMOD DSP.
    lc.on_audio = [this](const int16_t* samples, size_t frame_count,
                         const librespotc::AudioFormat& fmt) -> bool {
#if defined(SPOTIFY_RADIO_DIAG)
        diag_track_audio_callbacks_.fetch_add(1, std::memory_order_acq_rel);
        diag_track_sample_rate_.store(fmt.sample_rate, std::memory_order_release);
        diag_track_channels_.store(fmt.channels, std::memory_order_release);
#endif
        if (!pcm_enabled_.load(std::memory_order_acquire)) {
#if defined(SPOTIFY_RADIO_DIAG)
            diag_track_pcm_disabled_drops_.fetch_add(
                static_cast<uint64_t>(frame_count), std::memory_order_acq_rel);
#endif
            return true;
        }
        auto* fi = bridge::g_fmod_inject;
        if (!fi || !fi->is_playing()) {
            // Sink not ready — drop silently, don't backpressure source.
#if defined(SPOTIFY_RADIO_DIAG)
            diag_track_no_sink_drops_.fetch_add(
                static_cast<uint64_t>(frame_count), std::memory_order_acq_rel);
#endif
            static std::atomic<uint64_t> drop{0};
            uint64_t i = drop.fetch_add(1, std::memory_order_relaxed);
            if ((i & 0x3FF) == 0) {
                bridge::log::info("[librespot] on_audio (drop, no sink) frames="
                                  + std::to_string(frame_count)
                                  + " rate=" + std::to_string(fmt.sample_rate)
                                  + " ch=" + std::to_string(fmt.channels));
            }
            return true;
        }
        // Sample format / rate mismatch detection: native DSP PCM currently
        // resamples 44.1 kHz input to the game's 48 kHz mix and normalizes
        // source channel count to the bridge's stereo ring contract.
        if (fmt.sample_rate != bridge::FmodInject::kPcmSampleRate ||
            fmt.channels    != bridge::FmodInject::kPcmChannels) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                bridge::log::warn("[librespot] PCM format mismatch: got rate="
                    + std::to_string(fmt.sample_rate)
                    + " ch=" + std::to_string(fmt.channels)
                    + " want rate=" + std::to_string(bridge::FmodInject::kPcmSampleRate)
                    + " ch=" + std::to_string(bridge::FmodInject::kPcmChannels)
                    + " — FmodInject will normalize channels; sample-rate"
                      " mismatch may still affect pitch");
            }
        }
        bool accepted = fi->feed_pcm_s16(samples, frame_count, fmt.channels);
#if defined(SPOTIFY_RADIO_DIAG)
        if (accepted) {
            diag_track_audio_frames_.fetch_add(
                static_cast<uint64_t>(frame_count), std::memory_order_acq_rel);
        } else {
            diag_track_feed_false_.fetch_add(1, std::memory_order_acq_rel);
        }
#endif
        return accepted;
    };

    lc.on_track_change = [this](const librespotc::TrackInfo& t) {
#if defined(SPOTIFY_RADIO_DIAG)
        std::string previous = diag_playback_summary();
#endif
        log::info("[librespot] TrackChange: " + t.title + " — " + t.artist
                  + " (" + t.track_id + ")");
        std::string uri = "spotify:track:" + t.track_id;
#if defined(SPOTIFY_RADIO_DIAG)
        uint64_t seq =
            diag_track_seq_.fetch_add(1, std::memory_order_acq_rel) + 1;
        log::info("[skip-diag] track boundary previous{" + previous
                  + "} next_seq=" + std::to_string(seq)
                  + " next_uri=" + uri
                  + " next_duration_ms=" + std::to_string(t.duration_ms));
        diag_reset_track_audio_counters();
#endif
        {
            std::lock_guard lock(last_track_mtx_);
            last_track_.uri = uri;
            last_track_.title = t.title;
            last_track_.artist = t.artist;
            last_track_.album = t.album;
            last_track_.duration_ms = t.duration_ms;
            last_track_.artwork_key = t.artwork_key;
            last_track_.artwork_mime = t.artwork_mime;
            last_track_.artwork_bytes.clear();
            last_track_.artwork_loading = t.artwork_loading;
        }
        if (state_store_) {
            state_store_->set_track(uri, t.title, t.artist, t.album, t.duration_ms);
        }
    };

    lc.on_track_artwork = [this](const librespotc::TrackArtwork& artwork) {
        std::string uri = "spotify:track:" + artwork.track_id;
        {
            std::lock_guard lock(last_track_mtx_);
            if (last_track_.uri != uri) return;
            last_track_.artwork_key = artwork.artwork_key;
            last_track_.artwork_mime = artwork.artwork_mime;
            last_track_.artwork_loading = artwork.loading;
            if (artwork.available) {
                last_track_.artwork_bytes = artwork.artwork_bytes;
            } else {
                last_track_.artwork_bytes.clear();
            }
        }
#if !defined(SPOTIFY_RADIO_PUBLIC)
        if (artwork.available) {
            log::info("[librespot] AlbumArt native bytes=" +
                      std::to_string(artwork.artwork_bytes.size()) +
                      " track=" + artwork.track_id);
        }
#endif
    };

    lc.on_event = [this](const librespotc::Event& e) {
        using ET = librespotc::EventType;
        using ES = librespotc::EventSource;
        bool from_cloud = (e.source == ES::Cloud);
        // Diagnostic: delta-ms since previous event of any kind, so a log
        // reader can see the disconnect window relative to the last
        // TrackEnded / PlaybackStarted / TrackChanged.
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        auto prev = last_event_at_.exchange(now);
        long long dt_ms = (prev.time_since_epoch().count() == 0)
            ? 0
            : std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - prev).count();
        auto tag = [dt_ms](std::string_view what) {
            return std::string("[librespot] +") + std::to_string(dt_ms)
                 + "ms " + std::string(what);
        };
#if defined(SPOTIFY_RADIO_DIAG)
        auto skip_diag = [this]() {
            return std::string(" {") + diag_playback_summary() + "}";
        };
#endif
        switch (e.type) {
        case ET::TrackEnded:
            log::info(tag("TrackEnded detail=" + e.detail
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
#if defined(SPOTIFY_RADIO_DIAG)
            {
                uint32_t pos = session_ ? session_->current_position_ms() : 0;
                uint32_t dur = 0;
                { std::lock_guard lock(last_track_mtx_); dur = last_track_.duration_ms; }
                int pct = dur > 0
                    ? static_cast<int>((static_cast<uint64_t>(pos) * 100) / dur)
                    : -1;
                log::info("[pos-diag] TrackEnded at pos_ms=" + std::to_string(pos)
                          + " dur_ms=" + std::to_string(dur)
                          + " pct=" + std::to_string(pct)
                          + " (pct well under 100 = ended early)");
            }
#endif
            if (on_track_ended_) on_track_ended_();
            break;
        case ET::TrackError:
            log::warn(tag("TrackError detail=" + e.detail
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            break;
        case ET::Reconnecting:
            log::info(tag("Reconnecting detail=" + e.detail
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            connected_.store(false, std::memory_order_relaxed);
            if (state_store_) {
                state_store_->set_spotify_connection(false, blob_cached(), device_id_);
            }
            break;
        case ET::Reconnected:
            log::info(tag(std::string("Reconnected")
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            connected_.store(true, std::memory_order_relaxed);
            if (state_store_) {
                state_store_->set_spotify_connection(true, blob_cached(), device_id_);
                state_store_->set_spotify_transferred_away(false);
            }
            break;
        case ET::PlaybackStarted:
            log::info(tag(std::string("PlaybackStarted (")
                      + (from_cloud ? "cloud" : "local") + ")"
#if defined(SPOTIFY_RADIO_DIAG)
                      + skip_diag()
#endif
                      ));
            playing_.store(true, std::memory_order_release);
            if (state_store_) {
                state_store_->set_spotify_playing(true);
                state_store_->set_spotify_transferred_away(false);
            }
            if (on_state_changed_) on_state_changed_(true, from_cloud);
            break;
        case ET::PlaybackPaused:
            log::info(tag(std::string("PlaybackPaused (")
                      + (from_cloud ? "cloud" : "local") + ")"
#if defined(SPOTIFY_RADIO_DIAG)
                      + skip_diag()
#endif
                      ));
            playing_.store(false, std::memory_order_release);
            if (state_store_) state_store_->set_spotify_playing(false);
            if (on_state_changed_) on_state_changed_(false, from_cloud);
            break;
        case ET::TrackChanged:
            log::info(tag(std::string("TrackChanged (")
                      + (from_cloud ? "cloud" : "local") + ") detail=" + e.detail
#if defined(SPOTIFY_RADIO_DIAG)
                      + skip_diag()
#endif
                      ));
            if (on_track_changed_) on_track_changed_(e.detail, "", "", from_cloud);
            break;
        case ET::VolumeChanged:
            // Library applies this to PCM because Config::apply_volume_gain is
            // true. FMOD owns only the radio-bus baseline / station gate.
            log::info(tag("VolumeChanged detail=" + e.detail));
            {
                uint32_t v = parse_volume_detail(e.detail);
                if (v <= kMaxSpotifyVolume) save_volume(cfg_.cache_dir, v);
            }
            break;
        case ET::SkipNextRequested:
            // Library couldn't satisfy locally (no queue, no context). With
            // full SPIRC context echo this is rare. Log + ignore — host
            // does NOT fall back to Web API; users skip via the official
            // app like any other Spotify Connect device.
            log::warn(tag(std::string("SkipNextRequested (no local queue/context)")
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            break;
        case ET::SkipPrevRequested:
            log::warn(tag(std::string("SkipPrevRequested (no local history/context)")
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            break;
        case ET::ContextChanged:
            // New album/playlist loaded; library handles tracks internally.
            // detail = context URI. Useful for HUD later; just log for now.
            log::info(tag("ContextChanged detail=" + e.detail
#if defined(SPOTIFY_RADIO_DIAG)
                          + skip_diag()
#endif
                          ));
            break;
        case ET::BecameInactive:
            // User transferred to another Connect device (phone/desktop).
            // Library has already stopped playback — on_audio won't fire,
            // FmodInject ring will drain to silence naturally. Mark state
            // for the SSE/HUD layer so it reflects "not playing".
            // detail = new active device id (empty if none).
            log::info(tag("BecameInactive — transferred to "
                      + (e.detail.empty() ? std::string("none") : e.detail)
#if defined(SPOTIFY_RADIO_DIAG)
                      + skip_diag()
#endif
                      ));
            playing_.store(false, std::memory_order_release);
            if (state_store_) {
                state_store_->set_spotify_playing(false);
                state_store_->set_spotify_transferred_away(true);
            }
            if (on_state_changed_) on_state_changed_(false, true);
            log::info("[librespot] Spotify Connect transfer respected");
            break;
        }
    };

    session_ = librespotc::Session::create(lc);
    if (!session_) {
        log::error("[librespot] Session::create returned null");
        running_ = false;
        return;
    }

    connect_thread_ = std::thread(&LibrespotPlayer::connect_thread_fn, this);
#if defined(SPOTIFY_RADIO_DIAG)
    diag_pos_thread_ = std::thread(&LibrespotPlayer::diag_position_loop, this);
#endif
    log::info("[librespot] Player started (connect thread spawned)");
}

void LibrespotPlayer::stop() {
    std::lock_guard lifecycle_lock(lifecycle_mtx_);
    if (!running_.exchange(false)) {
        std::lock_guard lock(last_track_mtx_);
        last_track_.artwork_key.clear();
        last_track_.artwork_mime.clear();
        last_track_.artwork_bytes.clear();
        last_track_.artwork_loading = false;
        return;
    }
    if (session_) {
        session_->disconnect();
    }
    connected_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    if (connect_thread_.joinable()) connect_thread_.join();
#if defined(SPOTIFY_RADIO_DIAG)
    if (diag_pos_thread_.joinable()) diag_pos_thread_.join();
#endif
    session_.reset();
    {
        std::lock_guard lock(device_id_mtx_);
        device_id_.clear();
    }
    {
        std::lock_guard lock(last_track_mtx_);
        last_track_.artwork_key.clear();
        last_track_.artwork_mime.clear();
        last_track_.artwork_bytes.clear();
        last_track_.artwork_loading = false;
    }
    if (state_store_) {
        state_store_->set_spotify_connection(false, blob_cached(), "");
        state_store_->set_spotify_playing(false);
        state_store_->set_spotify_transferred_away(false);
    }
    log::info("[librespot] Player stopped");
}

bool LibrespotPlayer::authenticate_with_oauth(const std::string& access_token) {
    if (access_token.empty()) return false;
    {
        std::lock_guard oauth_lock(oauth_mtx_);
        pending_oauth_token_ = access_token;
    }
    log::info("[librespot] OAuth token received; reconnecting with token login");
    // If the player is live (Spotify is the active source, waiting on Zeroconf),
    // restart so connect() picks up the token. stop() cancels the Zeroconf wait
    // and joins the connect thread; start() rebuilds the session with the token.
    // If not currently running, the token is held and used on next activation.
    if (running_.load(std::memory_order_acquire)) {
        stop();
        start(false);  // prefer connect() (cache -> oauth -> zeroconf)
    }
    return true;
}

void LibrespotPlayer::connect_thread_fn() {
    if (!running_.load(std::memory_order_acquire)) {
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[librespot] Connect thread cancelled before network start");
#endif
        return;
    }

    bool prefer_picker =
        advertise_for_selection_.load(std::memory_order_acquire);
    log::info(std::string("[librespot] Connecting (")
              + (prefer_picker ? "zeroconf picker" : "cached → oauth → zeroconf")
              + ")...");
    bool ok = false;
    if (prefer_picker) {
        if (!running_.load(std::memory_order_acquire)) return;
        ok = session_->connect_via_zeroconf(0);
    } else {
        if (!running_.load(std::memory_order_acquire)) return;
        ok = session_->connect();
    }
    if (!ok && running_.load(std::memory_order_acquire)) {
        auto err = session_->last_error_message();
        log::warn(std::string("[librespot] ")
                  + (prefer_picker ? "connect_via_zeroconf" : "connect()")
                  + " failed: " + err + " — entering Zeroconf wait loop");
    }

    // Re-advertise Zeroconf on every failure. Without this loop, when the AP
    // handshake fails AFTER a Spotify-app AddUser (cached or fresh creds),
    // connect_via_zeroconf returns false and the mDNS advertisement dies,
    // making the device vanish from the Spotify device picker until the game
    // is restarted. Loop keeps the device discoverable so transient AP /
    // network problems can self-recover on a subsequent pair attempt.
    int attempts = 0;
    while (!ok && running_.load(std::memory_order_acquire)) {
        ++attempts;
        ok = session_->connect_via_zeroconf(0);
        if (ok) break;
        if (!running_.load(std::memory_order_acquire)) return;
        auto msg  = session_->last_error_message();
        // First miss is loud; subsequent misses log every 5th so a flapping
        // AP doesn't spam the ring buffer / on-disk log.
        if (attempts == 1 || (attempts % 5) == 0) {
            log::warn("[librespot] Zeroconf wait returned without session ("
                      + msg + ") attempt=" + std::to_string(attempts)
                      + " — re-advertising FH6 Radio");
        }
        // Keep the gap tiny. While we're sleeping, no mDNS record is being
        // broadcast, so the device disappears from the Spotify picker. Even
        // for HandshakeFailed (firewall/region/TLS) we want the device to
        // come back fast so the user sees a consistent name on their app —
        // librespotc's internal AP rotation already prevents hammering one host.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (!ok || !running_.load(std::memory_order_acquire)) return;

    {
        std::lock_guard lock(device_id_mtx_);
        device_id_ = session_->spotify_device_id();
    }
    connected_.store(true, std::memory_order_relaxed);
    log::info("[librespot] Connected as device_id=" + device_id_);
    log::info("[librespot] Initial volume detail="
              + std::to_string(session_->current_volume()));
    if (state_store_) {
        state_store_->set_spotify_connection(true, blob_cached(), device_id_);
    }
}

std::string LibrespotPlayer::spotify_device_id() const {
    std::lock_guard lock(device_id_mtx_);
    return device_id_;
}

bool LibrespotPlayer::is_connected() const {
    return connected_.load(std::memory_order_relaxed);
}

bool LibrespotPlayer::is_playing() const {
    return playing_.load(std::memory_order_acquire);
}

void LibrespotPlayer::set_pcm_enabled(bool enabled) {
    pcm_enabled_.store(enabled, std::memory_order_release);
}

bool LibrespotPlayer::play_track(const std::string& uri) {
    if (!session_ || !connected_.load()) return false;
    bool ok = session_->play_track(uri);
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host play_track uri=" + uri
              + " ok=" + std::to_string(ok)
              + " " + diag_playback_summary());
#endif
    return ok;
}

void LibrespotPlayer::pause() {
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host pause before " + diag_playback_summary());
#endif
    if (session_) session_->pause();
}

void LibrespotPlayer::pause_at_audio_boundary() {
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host pause_at_audio_boundary before "
              + diag_playback_summary());
#endif
    if (session_) session_->pause_at_audio_boundary();
}

void LibrespotPlayer::resume() {
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host resume before " + diag_playback_summary());
#endif
    if (session_) session_->resume();
}

void LibrespotPlayer::resume_rewound(uint32_t rewind_ms) {
    if (!session_) return;
    uint32_t pos = session_->current_position_ms();
    uint32_t target = pos > rewind_ms ? pos - rewind_ms : 0;
    if (target != pos) {
        session_->seek(target);
        log::info("[librespot] resume rewind "
                  + std::to_string(pos) + "ms -> "
                  + std::to_string(target) + "ms");
    }
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host resume_rewound rewind_ms="
              + std::to_string(rewind_ms)
              + " session_pos_ms=" + std::to_string(pos)
              + " target_ms=" + std::to_string(target)
              + " " + diag_playback_summary());
#endif
    session_->resume();
}

uint32_t LibrespotPlayer::current_position_ms() const {
    return session_ ? session_->current_position_ms() : 0;
}

bool LibrespotPlayer::seek(uint32_t position_ms) {
    if (!session_) return false;
    bool ok = session_->seek(position_ms);
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host seek target_ms=" + std::to_string(position_ms)
              + " ok=" + std::to_string(ok)
              + " " + diag_playback_summary());
#endif
    return ok;
}

bool LibrespotPlayer::next_track() {
    if (!session_) return false;
    bool ok = session_->next();
#if defined(SPOTIFY_RADIO_DIAG)
    diag_log_control_result("next", ok);
#endif
    return ok;
}

void LibrespotPlayer::stop_track() {
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] host stop_track before " + diag_playback_summary());
#endif
    if (session_) session_->stop_track();
}

void LibrespotPlayer::set_volume_normalization(bool enabled) {
    if (session_) {
        session_->set_replaygain_enabled(enabled);
        session_->set_extra_volume_headroom_enabled(enabled);
    }
}

void LibrespotPlayer::set_equalizer(bool enabled, const std::array<float, 5>& bands_db) {
    if (session_) {
        session_->set_equalizer_bands(bands_db);
        session_->set_equalizer_enabled(enabled);
    }
}

LibrespotPlayer::LastTrack LibrespotPlayer::last_track() const {
    std::lock_guard lock(last_track_mtx_);
    return last_track_;
}

bool LibrespotPlayer::blob_cached() const {
    std::error_code ec;
    return std::filesystem::exists(cfg_.cache_dir / "credentials.dat", ec);
}

} // namespace bridge
