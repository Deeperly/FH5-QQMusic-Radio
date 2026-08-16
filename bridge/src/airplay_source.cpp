#include "airplay_source.h"

#include "bridge_state.h"
#include "fmod_inject.h"
#include "log_file.h"

#include <airplayc/airplayc.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Iphlpapi.lib")

namespace bridge {

namespace {

constexpr float kDefaultAirPlayVolumeDb = -15.0f;
constexpr float kMinAirPlayVolumeDb = -30.0f;
constexpr float kMaxAirPlayVolumeDb = 0.0f;
constexpr float kMaxAirPlayOutputGain = 3.0f;
constexpr float kAirPlayHeadroomCurveExponent = 4.0f;
constexpr const char* kAirPlayVolumeStateFile = "volume-db.dat";

std::string clean_machine_name() {
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

float load_saved_volume_db(const std::filesystem::path& cache_dir) {
    std::ifstream in(cache_dir / kAirPlayVolumeStateFile);
    float db = kDefaultAirPlayVolumeDb;
    if (!(in >> db) || !std::isfinite(db)) return kDefaultAirPlayVolumeDb;
    return std::clamp(db, kMinAirPlayVolumeDb, kMaxAirPlayVolumeDb);
}

void save_volume_db(const std::filesystem::path& cache_dir, float db) {
    if (!std::isfinite(db)) return;
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    std::ofstream out(cache_dir / kAirPlayVolumeStateFile, std::ios::trunc);
    if (out) out << std::clamp(db, kMinAirPlayVolumeDb, kMaxAirPlayVolumeDb) << '\n';
}

bool is_unusable_lan_ipv4(uint32_t host_order_ip) {
    const uint8_t a = static_cast<uint8_t>((host_order_ip >> 24) & 0xff);
    const uint8_t b = static_cast<uint8_t>((host_order_ip >> 16) & 0xff);
    if (a == 0 || a == 127) return true;
    if (a == 169 && b == 254) return true;
    return false;
}

bool adapter_name_looks_virtual(const IP_ADAPTER_ADDRESSES* adapter) {
    if (!adapter || !adapter->FriendlyName) return false;
    std::wstring name(adapter->FriendlyName);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return name.find(L"vmware") != std::wstring::npos ||
           name.find(L"virtual") != std::wstring::npos ||
           name.find(L"hyper-v") != std::wstring::npos ||
           name.find(L"loopback") != std::wstring::npos ||
           name.find(L"tailscale") != std::wstring::npos ||
           name.find(L"zerotier") != std::wstring::npos;
}

std::string select_lan_ipv4() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.assign(size, 0);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) return {};

    std::string fallback;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr ||
                ua->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            auto* addr = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            uint32_t host_ip = ntohl(addr->sin_addr.S_un.S_addr);
            if (is_unusable_lan_ipv4(host_ip)) continue;
            char text[INET_ADDRSTRLEN] = {};
            if (!InetNtopA(AF_INET, &addr->sin_addr, text, sizeof(text))) {
                continue;
            }
            if (fallback.empty()) fallback = text;
            if (!adapter_name_looks_virtual(adapter)) return text;
        }
    }
    return fallback;
}

} // namespace

AirPlayPlayer::AirPlayPlayer(std::string base_device_name,
                             std::filesystem::path cache_dir,
                             FeedPcmFn feed_pcm)
    : base_device_name_(std::move(base_device_name)),
      advertised_device_name_(
          device_name_with_machine_suffix(base_device_name_)),
      cache_dir_(std::move(cache_dir)),
      feed_pcm_(std::move(feed_pcm)) {
    volume_db_ = load_saved_volume_db(cache_dir_);
    volume_gain_ = gain_from_db(volume_db_);
}

AirPlayPlayer::~AirPlayPlayer() {
    stop();
}

bool AirPlayPlayer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return true;
    volume_restore_generation_.fetch_add(1, std::memory_order_acq_rel);
    volume_restore_scheduled_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(mtx_);
        if (session_) return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(cache_dir_, ec);

    airplayc::Config cfg;
    cfg.device_name = advertised_device_name_;
    cfg.manufacturer = "FH6";
    cfg.model = "Speaker";
    cfg.cache_dir = cache_dir_.string();
    cfg.advertise_ipv4 = select_lan_ipv4();
    if (!cfg.advertise_ipv4.empty()) {
        log::info("[airplay] Advertising LAN IPv4 " + cfg.advertise_ipv4);
    } else {
        log::warn("[airplay] No preferred LAN IPv4 found; DNS-SD will choose");
    }
    {
        std::lock_guard lock(mtx_);
        cfg.initial_volume_db = volume_db_;
    }
    cfg.on_audio = [this](const int16_t* pcm, size_t frames,
                          const airplayc::AudioFormat& fmt) {
        return handle_audio(pcm, frames, fmt.sample_rate, fmt.channels,
                            fmt.bits_per_sample);
    };
    cfg.on_event = [this](const airplayc::Event& event) {
        using ET = airplayc::EventType;
        switch (event.type) {
        case ET::ReceiverStarted:
            running_.store(true, std::memory_order_release);
            set_error({});
            log::info("[airplay] Receiver started as \"" +
                      advertised_device_name_ + "\"");
            break;
        case ET::ReceiverStopped:
            running_.store(false, std::memory_order_release);
            connected_.store(false, std::memory_order_release);
            playing_.store(false, std::memory_order_release);
            {
                std::lock_guard lock(mtx_);
                position_known_ = false;
                position_base_ms_ = 0;
                position_sampled_at_ = {};
            }
            log::info("[airplay] Receiver stopped");
            break;
        case ET::ClientConnected:
            connected_.store(true, std::memory_order_release);
            set_error({});
            log::info("[airplay] Client connected");
            schedule_volume_restore("client-connected", 1000);
            break;
        case ET::ClientDisconnected:
            stop_volume_restore_thread();
            connected_.store(false, std::memory_order_release);
            playing_.store(false, std::memory_order_release);
            {
                std::lock_guard lock(mtx_);
                position_known_ = false;
                position_base_ms_ = 0;
                position_sampled_at_ = {};
            }
            log::info("[airplay] Client disconnected");
            break;
        case ET::PlaybackStarted:
            {
                std::lock_guard lock(mtx_);
                if (!position_known_) {
                    reset_position_locked(0);
                } else {
                    position_sampled_at_ = std::chrono::steady_clock::now();
                }
            }
            playing_.store(true, std::memory_order_release);
            set_error({});
            log::info("[airplay] Playback started");
            schedule_volume_restore("playback-started", 150);
            break;
        case ET::PlaybackPaused:
        case ET::PlaybackStopped:
            {
                std::lock_guard lock(mtx_);
                freeze_position_locked();
            }
            playing_.store(false, std::memory_order_release);
            log::info("[airplay] Playback paused/stopped");
            break;
        case ET::StreamError:
            set_error(event.detail.empty() ? "AirPlay stream error"
                                           : event.detail);
            log::warn("[airplay] Stream error: " + last_error());
            break;
        case ET::ProtocolMessage:
            if (event.detail.find("Active-Remote:") != std::string::npos ||
                event.detail.find("DACP-ID:") != std::string::npos) {
                schedule_volume_restore("dacp-headers", 150);
            }
#if defined(SPOTIFY_RADIO_DIAG)
            // RTSP protocol-event telemetry: diagnostic only and verbose (fires
            // per request: OPTIONS/SET_PARAMETER/ANNOUNCE/CoverArt/etc.). Gated
            // so neither the match scan nor the log runs in public builds.
            if (event.detail.find("volume") != std::string::npos ||
                event.detail.find("Volume") != std::string::npos ||
                event.detail.find("OPTIONS ") != std::string::npos ||
                event.detail.find("GET ") != std::string::npos ||
                event.detail.find("POST ") != std::string::npos ||
                event.detail.find("ANNOUNCE ") != std::string::npos ||
                event.detail.find("SETUP ") != std::string::npos ||
                event.detail.find("RECORD ") != std::string::npos ||
                event.detail.find("PAUSE ") != std::string::npos ||
                event.detail.find("FLUSH ") != std::string::npos ||
                event.detail.find("TEARDOWN ") != std::string::npos ||
                event.detail.find("SET_PARAMETER") != std::string::npos ||
                event.detail.find("POST /command") != std::string::npos ||
                event.detail.find("GET_PARAMETER") != std::string::npos ||
                event.detail.find("bplist-summary") != std::string::npos) {
                log::info("[airplay] " + event.detail);
            }
#endif
            break;
        default:
            break;
        }
    };
    cfg.on_metadata = [this](const airplayc::Metadata& metadata) {
        std::lock_guard metadata_lock(mtx_);
        constexpr auto kCoverBeforeMetadataGrace =
            std::chrono::milliseconds(2000);
        const bool same_track =
            current_track_.title == metadata.title &&
            current_track_.artist == metadata.artist &&
            current_track_.album == metadata.album;
        const bool recent_cover =
            !current_track_.artwork_bytes.empty() &&
            last_cover_art_at_ != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - last_cover_art_at_ <=
                kCoverBeforeMetadataGrace;
        if (!same_track) {
            reset_position_locked(0);
        }
        current_track_.uri = "airplay:stream";
        current_track_.title = metadata.title;
        current_track_.artist = metadata.artist;
        current_track_.album = metadata.album;
        current_track_.duration_ms =
            metadata.duration_ms > std::numeric_limits<uint32_t>::max()
                ? 0
                : static_cast<uint32_t>(metadata.duration_ms);
        if (!same_track && !recent_cover) {
            current_track_.artwork_key.clear();
            current_track_.artwork_mime.clear();
            current_track_.artwork_bytes.clear();
            ++artwork_revision_;
        }
    };
    cfg.on_cover_art = [this](const uint8_t* data,
                              size_t bytes,
                              const std::string& mime_type) {
        if (!data || bytes == 0 || bytes > 4 * 1024 * 1024) return;
        std::lock_guard artwork_lock(mtx_);
        current_track_.uri = "airplay:stream";
        current_track_.artwork_mime = mime_type;
        current_track_.artwork_bytes.assign(data, data + bytes);
        last_cover_art_at_ = std::chrono::steady_clock::now();
        current_track_.artwork_key =
            "airplay:" + std::to_string(++artwork_revision_);
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[airplay] CoverArt bytes=" + std::to_string(bytes) +
                  " mime=" + mime_type);
#endif
    };
    cfg.on_volume = [this](float db) {
        apply_volume_db(db);
        log::info("[airplay] VolumeChanged detail=" + std::to_string(db)
                  + " percent=" + std::to_string(percent_from_db(db)));
    };
    cfg.on_progress = [this](const airplayc::PlaybackProgress& progress) {
        if (progress.duration_seconds <= 0.0) return;
        std::lock_guard progress_lock(mtx_);
        if (progress.position_seconds >= 0.0 &&
            progress.position_seconds <
                static_cast<double>(std::numeric_limits<uint32_t>::max()) /
                    1000.0) {
            reset_position_locked(static_cast<uint32_t>(
                progress.position_seconds * 1000.0));
        }
        if (current_track_.duration_ms == 0) {
            current_track_.duration_ms =
                progress.duration_seconds >
                        (std::numeric_limits<uint32_t>::max() / 1000.0)
                    ? 0
                    : static_cast<uint32_t>(progress.duration_seconds * 1000.0);
        }
    };

    auto session = airplayc::Session::create(cfg);
    if (!session) {
        set_error("Could not create AirPlay receiver");
        running_.store(false, std::memory_order_release);
        return false;
    }
    if (!session->start()) {
        set_error(session->last_error_message());
        log::warn("[airplay] Receiver start failed: " + last_error());
        running_.store(false, std::memory_order_release);
        return false;
    }

    {
        std::lock_guard lock(mtx_);
        session_ = std::move(session);
    }
    set_error({});
    return true;
}

void AirPlayPlayer::stop() {
    stop_volume_restore_thread();
    std::lock_guard control_lock(control_mtx_);
    std::unique_ptr<airplayc::Session> session;
    {
        std::lock_guard lock(mtx_);
        session = std::move(session_);
        current_track_ = {};
    }
    if (session) session->stop();
    running_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
}

void AirPlayPlayer::schedule_volume_restore(const char* reason, uint32_t delay_ms) {
    if (volume_restore_scheduled_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const uint64_t generation =
        volume_restore_generation_.load(std::memory_order_acquire);
    const std::string reason_text = reason ? reason : "unknown";
    float restore_db = kDefaultAirPlayVolumeDb;
    {
        std::lock_guard lock(mtx_);
        restore_db = volume_db_;
    }

    std::lock_guard thread_lock(volume_restore_thread_mtx_);
    if (volume_restore_thread_.joinable()) {
        return;
    }

    volume_restore_thread_ = std::jthread(
        [this, generation, delay_ms, reason_text, restore_db](
            std::stop_token stop_token) {
            auto sleep_interruptible = [&stop_token](uint32_t ms) {
                const auto sleep_until =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(ms);
                while (!stop_token.stop_requested() &&
                       std::chrono::steady_clock::now() < sleep_until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                return !stop_token.stop_requested();
            };

            auto generation_alive = [this, generation] {
                return generation ==
                       volume_restore_generation_.load(std::memory_order_acquire);
            };

            if (!sleep_interruptible(delay_ms) || !generation_alive()) return;

            float db = restore_db;
            airplayc::Session* session = nullptr;
            {
                std::lock_guard lock(mtx_);
                session = session_.get();
            }

            bool ok = false;
            if (session) {
                std::lock_guard control_lock(control_mtx_);
                {
                    std::lock_guard lock(mtx_);
                    session = session_.get();
                }
                ok = session && session->set_volume_db(db);
            }
            if (ok) {
                apply_volume_db(db);
            }
            log::info("[airplay] restore volume reason=" + reason_text +
                      " db=" + std::to_string(db) +
                      " percent=" + std::to_string(percent_from_db(db)) +
                      " ok=" + std::to_string(ok));
        });
}

void AirPlayPlayer::stop_volume_restore_thread() {
    volume_restore_generation_.fetch_add(1, std::memory_order_acq_rel);
    volume_restore_scheduled_.store(false, std::memory_order_release);

    std::jthread thread;
    {
        std::lock_guard thread_lock(volume_restore_thread_mtx_);
        thread = std::move(volume_restore_thread_);
    }
    if (thread.joinable()) {
        thread.request_stop();
        thread.join();
    }
}

bool AirPlayPlayer::pause() {
    std::lock_guard control_lock(control_mtx_);
    airplayc::Session* session = nullptr;
    {
        std::lock_guard lock(mtx_);
        session = session_.get();
    }
    return session && session->pause();
}

bool AirPlayPlayer::resume() {
    std::lock_guard control_lock(control_mtx_);
    airplayc::Session* session = nullptr;
    {
        std::lock_guard lock(mtx_);
        session = session_.get();
    }
    return session && session->resume();
}

bool AirPlayPlayer::next_track() {
    std::lock_guard control_lock(control_mtx_);
    airplayc::Session* session = nullptr;
    {
        std::lock_guard lock(mtx_);
        session = session_.get();
    }
    bool ok = session && session->next_track();
    if (ok) {
        std::lock_guard lock(mtx_);
        reset_position_locked(0);
    }
    return ok;
}

bool AirPlayPlayer::previous_track() {
    std::lock_guard control_lock(control_mtx_);
    airplayc::Session* session = nullptr;
    {
        std::lock_guard lock(mtx_);
        session = session_.get();
    }
    bool ok = session && session->previous_track();
    if (ok) {
        std::lock_guard lock(mtx_);
        reset_position_locked(0);
    }
    return ok;
}

void AirPlayPlayer::set_pcm_enabled(bool enabled) {
    pcm_enabled_.store(enabled, std::memory_order_release);
}

bool AirPlayPlayer::is_running() const {
    return running_.load(std::memory_order_acquire);
}

bool AirPlayPlayer::is_connected() const {
    return connected_.load(std::memory_order_acquire);
}

bool AirPlayPlayer::is_playing() const {
    return playing_.load(std::memory_order_acquire);
}

std::optional<uint32_t> AirPlayPlayer::current_position_ms() const {
    std::lock_guard lock(mtx_);
    return current_position_ms_locked(std::chrono::steady_clock::now());
}

std::string AirPlayPlayer::device_name() const {
    return advertised_device_name_;
}

std::string AirPlayPlayer::last_error() const {
    std::lock_guard lock(mtx_);
    return error_;
}

SourceTrack AirPlayPlayer::last_track() const {
    std::lock_guard lock(mtx_);
    return current_track_;
}

float AirPlayPlayer::volume_db() const {
    std::lock_guard lock(mtx_);
    return volume_db_;
}

uint32_t AirPlayPlayer::volume_percent() const {
    return percent_from_db(volume_db());
}

void AirPlayPlayer::publish_state(BridgeStateStore& store) const {
    store.set_airplay_source(true,
                             is_running(),
                             is_connected(),
                             is_playing(),
                             device_name(),
                             volume_db(),
                             volume_percent(),
                             last_error());
}

std::optional<uint32_t> AirPlayPlayer::current_position_ms_locked(
    std::chrono::steady_clock::time_point now) const {
    if (!position_known_) return std::nullopt;
    uint64_t position = position_base_ms_;
    if (playing_.load(std::memory_order_acquire) &&
        position_sampled_at_ != std::chrono::steady_clock::time_point{} &&
        now >= position_sampled_at_) {
        position += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - position_sampled_at_)
                .count());
    }
    if (current_track_.duration_ms != 0) {
        position = std::min<uint64_t>(position, current_track_.duration_ms);
    }
    return static_cast<uint32_t>(
        std::min<uint64_t>(position, std::numeric_limits<uint32_t>::max()));
}

void AirPlayPlayer::reset_position_locked(uint32_t position_ms) {
    position_known_ = true;
    position_base_ms_ = position_ms;
    position_sampled_at_ = std::chrono::steady_clock::now();
}

void AirPlayPlayer::freeze_position_locked() {
    auto current = current_position_ms_locked(std::chrono::steady_clock::now());
    if (current) {
        position_base_ms_ = *current;
        position_sampled_at_ = std::chrono::steady_clock::now();
    }
}

std::string AirPlayPlayer::device_name_with_machine_suffix(
    const std::string& base) {
    std::string pc = clean_machine_name();
    if (pc.empty()) return base;
    if (base.find(" (" + pc + ")") != std::string::npos) return base;
    return base + " (" + pc + ")";
}

float AirPlayPlayer::clamp_volume_db(float db) {
    if (!std::isfinite(db)) return kDefaultAirPlayVolumeDb;
    return std::clamp(db, kMinAirPlayVolumeDb, kMaxAirPlayVolumeDb);
}

float AirPlayPlayer::gain_from_db(float db) {
    db = clamp_volume_db(db);
    // Keep AirPlay's native dB attenuation shape for most of the slider, then
    // add Spotify-like headroom only near the top end so 0 dB can match the
    // perceived impact of Spotify Connect's configured maximum source gain.
    const float attenuation = std::pow(10.0f, db / 20.0f);
    const float normalized =
        (db - kMinAirPlayVolumeDb) / (kMaxAirPlayVolumeDb - kMinAirPlayVolumeDb);
    const float headroom =
        1.0f + (kMaxAirPlayOutputGain - 1.0f) *
                   std::pow(std::clamp(normalized, 0.0f, 1.0f),
                            kAirPlayHeadroomCurveExponent);
    return std::clamp(attenuation * headroom, 0.0f, kMaxAirPlayOutputGain);
}

uint32_t AirPlayPlayer::percent_from_db(float db) {
    db = clamp_volume_db(db);
    const float normalized =
        (db - kMinAirPlayVolumeDb) / (kMaxAirPlayVolumeDb - kMinAirPlayVolumeDb);
    return static_cast<uint32_t>(
        std::lround(std::clamp(normalized, 0.0f, 1.0f) * 100.0f));
}

bool AirPlayPlayer::handle_audio(const int16_t* samples, size_t frame_count,
                                 uint32_t sample_rate, uint16_t channels,
                                 uint16_t bits_per_sample) {
    if (!samples || frame_count == 0) return true;
    if (!pcm_enabled_.load(std::memory_order_acquire)) return true;
    auto* fi = bridge::g_fmod_inject;
    if (!fi || !fi->is_playing() || !fi->pcm_accepting()) return true;

    if (sample_rate != bridge::FmodInject::kPcmSampleRate ||
        channels != bridge::FmodInject::kPcmChannels ||
        bits_per_sample != 16) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            log::warn("[airplay] PCM format mismatch: got rate=" +
                      std::to_string(sample_rate) +
                      " ch=" + std::to_string(channels) +
                      " bits=" + std::to_string(bits_per_sample) +
                      " want rate=" +
                      std::to_string(bridge::FmodInject::kPcmSampleRate) +
                      " ch=" +
                      std::to_string(bridge::FmodInject::kPcmChannels) +
                      " bits=16");
        }
        return true;
    }

    float gain = 1.0f;
    {
        std::lock_guard lock(mtx_);
        gain = volume_gain_;
    }

    return feed_pcm_ ? feed_pcm_(samples, frame_count, gain) : false;
}

void AirPlayPlayer::apply_volume_db(float db) {
    db = clamp_volume_db(db);
    std::lock_guard volume_lock(mtx_);
    volume_db_ = db;
    volume_gain_ = gain_from_db(db);
    save_volume_db(cache_dir_, db);
}

void AirPlayPlayer::set_error(std::string error) {
    std::lock_guard lock(mtx_);
    error_ = std::move(error);
}

AirPlaySource::AirPlaySource(AirPlayPlayer& player) : player_(player) {}

bool AirPlaySource::is_connected() const {
    return player_.is_connected();
}

bool AirPlaySource::is_playing() const {
    return player_.is_playing();
}

std::optional<uint32_t> AirPlaySource::current_position_ms() const {
    return player_.current_position_ms();
}

SourceTrack AirPlaySource::last_track() const {
    return player_.last_track();
}

void AirPlaySource::pause_at_audio_boundary() {
    player_.pause();
}

void AirPlaySource::resume_rewound(uint32_t rewind_ms) {
    (void)rewind_ms;
    player_.resume();
}

bool AirPlaySource::next_track() {
    return player_.next_track();
}

bool AirPlaySource::restart_current_track() {
    return player_.previous_track();
}

bool AirPlaySource::previous_track() {
    return player_.previous_track();
}

void AirPlaySource::on_activated() {
    player_.start();
}

void AirPlaySource::on_deactivated() {
    player_.stop();
}

void AirPlaySource::set_pcm_enabled(bool enabled) {
    player_.set_pcm_enabled(enabled);
}

void AirPlaySource::publish_state(BridgeStateStore& store) const {
    player_.publish_state(store);
}

} // namespace bridge
