#include "local_file_source.h"

#include "bridge_state.h"
#include "fmod_inject.h"
#include "log_file.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <propkey.h>
#include <propsys.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string_view>
#include <utility>

#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4701)
#endif
#include "../vendor/miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace bridge {

namespace {

constexpr uint32_t kSampleRate = FmodInject::kPcmSampleRate;
constexpr uint32_t kChannels = FmodInject::kPcmChannels;
constexpr size_t kFrameBytes = kChannels * sizeof(int16_t);
constexpr size_t kMaxChunkFrames = 2048;
constexpr size_t kMinBufferedFrames = 256;
constexpr size_t kAutoplayQueueSize = 10;
constexpr size_t kMaxReplayGainTagBytes = 1024 * 1024;

struct ReplayGainMetadata {
    bool has_track_gain = false;
    bool has_album_gain = false;
    bool has_track_peak = false;
    bool has_album_peak = false;
    float track_gain_db = 0.0f;
    float album_gain_db = 0.0f;
    float track_peak = 0.0f;
    float album_peak = 0.0f;
};

struct ReplayGainChoice {
    bool found = false;
    bool used_track = false;
    float gain_db = 0.0f;
    float peak = 0.0f;
    float linear_gain = 1.0f;
};

struct LocalFileMetadata {
    std::string title;
    std::string album_artist;
    std::string artist;
    std::string album;
};

std::string trim_ascii(std::string_view value);

template <class T>
void release_com(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool ensure_media_foundation_started() {
    static std::atomic<bool> attempted{false};
    static std::atomic<bool> ready{false};
    bool expected = false;
    if (attempted.compare_exchange_strong(expected, true)) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(hr)) {
            ready.store(true, std::memory_order_release);
        } else {
            log::warn("[local] Media Foundation startup failed hr=0x"
                      + std::to_string(static_cast<unsigned long>(hr)));
        }
    }
    return ready.load(std::memory_order_acquire);
}

bool ensure_com_initialized_for_thread() {
    thread_local bool initialized = false;
    if (initialized) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        initialized = true;
        return true;
    }
    log::warn("[local] COM init failed hr=" + std::to_string(static_cast<long>(hr)));
    return false;
}

class ScopedComInit {
public:
    ScopedComInit() {
        hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE);
        should_uninit_ = SUCCEEDED(hr_);
    }

    ~ScopedComInit() {
        if (should_uninit_) CoUninitialize();
    }

    bool ready() const {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT hr_ = E_FAIL;
    bool should_uninit_ = false;
};

std::string lower_ext(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool is_supported_audio(const std::filesystem::path& path) {
    auto ext = lower_ext(path);
    return ext == "mp3" || ext == "wav" || ext == "flac";
}

bool is_known_audio(const std::filesystem::path& path) {
    auto ext = lower_ext(path);
    return ext == "mp3" || ext == "wav" || ext == "flac" ||
           ext == "ogg" || ext == "m4a" || ext == "aac" ||
           ext == "opus" || ext == "wma" || ext == "aiff" ||
           ext == "aif";
}

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint32_t read_syncsafe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0] & 0x7f) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7f) << 7) |
           static_cast<uint32_t>(p[3] & 0x7f);
}

std::string ascii_lower(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool parse_float_prefix(std::string_view value, float& out) {
    std::string trimmed = trim_ascii(value);
    if (trimmed.empty() || trimmed.size() > 64) return false;
    char* end = nullptr;
    float parsed = std::strtof(trimmed.c_str(), &end);
    if (!end || end == trimmed.c_str() || !std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

bool parse_replaygain_value(std::string_view value, float& out) {
    if (!parse_float_prefix(value, out)) return false;
    return out >= -60.0f && out <= 60.0f;
}

bool parse_replaygain_peak(std::string_view value, float& out) {
    if (!parse_float_prefix(value, out)) return false;
    return out > 0.0f && out <= 100.0f;
}

void store_replaygain_tag(ReplayGainMetadata& metadata,
                          std::string_view key,
                          std::string_view value) {
    std::string lowered = ascii_lower(trim_ascii(key));
    float parsed = 0.0f;
    if (lowered == "replaygain_track_gain") {
        if (parse_replaygain_value(value, parsed)) {
            metadata.has_track_gain = true;
            metadata.track_gain_db = parsed;
        }
    } else if (lowered == "replaygain_album_gain") {
        if (parse_replaygain_value(value, parsed)) {
            metadata.has_album_gain = true;
            metadata.album_gain_db = parsed;
        }
    } else if (lowered == "replaygain_track_peak") {
        if (parse_replaygain_peak(value, parsed)) {
            metadata.has_track_peak = true;
            metadata.track_peak = parsed;
        }
    } else if (lowered == "replaygain_album_peak") {
        if (parse_replaygain_peak(value, parsed)) {
            metadata.has_album_peak = true;
            metadata.album_peak = parsed;
        }
    }
}

void store_replaygain_comment(ReplayGainMetadata& metadata,
                              std::string_view comment) {
    size_t eq = comment.find('=');
    if (eq == std::string_view::npos) return;
    store_replaygain_tag(metadata, comment.substr(0, eq), comment.substr(eq + 1));
}

std::string ascii_from_text_frame_payload(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (c == 0) {
            continue;
        } else if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

void parse_id3_txxx_frame(const uint8_t* data,
                          size_t size,
                          ReplayGainMetadata& metadata) {
    if (!data || size <= 1 || size > 4096) return;
    std::string text = ascii_from_text_frame_payload(data + 1, size - 1);
    std::string lowered = ascii_lower(text);
    static constexpr std::string_view keys[] = {
        "replaygain_track_gain",
        "replaygain_album_gain",
        "replaygain_track_peak",
        "replaygain_album_peak",
    };
    for (auto key : keys) {
        size_t pos = lowered.find(key);
        if (pos == std::string::npos) continue;
        size_t value_pos = pos + key.size();
        while (value_pos < text.size() &&
               (text[value_pos] == '\n' ||
                text[value_pos] == '=' ||
                text[value_pos] == ':' ||
                std::isspace(static_cast<unsigned char>(text[value_pos])))) {
            ++value_pos;
        }
        store_replaygain_tag(metadata, key, std::string_view(text).substr(value_pos));
    }
}

void parse_id3v2_tag(const std::vector<uint8_t>& tag,
                     ReplayGainMetadata& metadata) {
    if (tag.size() < 10 || tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') {
        return;
    }
    uint8_t major = tag[3];
    if (major < 3 || major > 4) return;
    size_t pos = 10;
    if ((tag[5] & 0x40) && pos + 4 <= tag.size()) {
        uint32_t ext_size = major == 4 ? read_syncsafe32(tag.data() + pos)
                                       : read_be32(tag.data() + pos) + 4;
        if (ext_size == 0 || ext_size > tag.size() - pos) return;
        pos += ext_size;
    }
    while (pos + 10 <= tag.size()) {
        const uint8_t* frame = tag.data() + pos;
        if (frame[0] == 0) break;
        std::string id(reinterpret_cast<const char*>(frame), 4);
        uint32_t frame_size = major == 4 ? read_syncsafe32(frame + 4)
                                         : read_be32(frame + 4);
        pos += 10;
        if (frame_size == 0 || frame_size > tag.size() - pos) break;
        if (id == "TXXX") {
            parse_id3_txxx_frame(tag.data() + pos, frame_size, metadata);
        }
        pos += frame_size;
    }
}

void read_mp3_id3v2_replaygain(const std::filesystem::path& path,
                               ReplayGainMetadata& metadata) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    uint8_t header[10] = {};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() != sizeof(header) ||
        header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return;
    }
    uint32_t tag_size = read_syncsafe32(header + 6);
    size_t total_size = static_cast<size_t>(tag_size) + sizeof(header);
    if (total_size > kMaxReplayGainTagBytes) return;
    std::vector<uint8_t> tag(total_size);
    std::memcpy(tag.data(), header, sizeof(header));
    in.read(reinterpret_cast<char*>(tag.data() + sizeof(header)), tag_size);
    if (static_cast<size_t>(in.gcount()) != tag_size) return;
    parse_id3v2_tag(tag, metadata);
}

void read_mp3_apev2_replaygain(const std::filesystem::path& path,
                               ReplayGainMetadata& metadata) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    in.seekg(0, std::ios::end);
    std::streamoff file_size = in.tellg();
    if (file_size < 32) return;
    in.seekg(file_size - 32, std::ios::beg);
    uint8_t footer[32] = {};
    in.read(reinterpret_cast<char*>(footer), sizeof(footer));
    if (in.gcount() != sizeof(footer) ||
        std::memcmp(footer, "APETAGEX", 8) != 0) {
        return;
    }
    uint32_t tag_size = read_le32(footer + 12);
    uint32_t item_count = read_le32(footer + 16);
    if (tag_size < 32 || tag_size > kMaxReplayGainTagBytes ||
        item_count > 1024 || static_cast<std::streamoff>(tag_size) > file_size) {
        return;
    }
    std::vector<uint8_t> tag(tag_size);
    in.seekg(file_size - static_cast<std::streamoff>(tag_size), std::ios::beg);
    in.read(reinterpret_cast<char*>(tag.data()), tag.size());
    if (static_cast<size_t>(in.gcount()) != tag.size()) return;

    size_t pos = 0;
    if (tag.size() >= 32 && std::memcmp(tag.data(), "APETAGEX", 8) == 0) {
        pos = 32;
    }
    for (uint32_t item = 0; item < item_count && pos + 8 < tag.size(); ++item) {
        uint32_t value_size = read_le32(tag.data() + pos);
        uint32_t flags = read_le32(tag.data() + pos + 4);
        pos += 8;
        size_t key_start = pos;
        while (pos < tag.size() && tag[pos] != 0) ++pos;
        if (pos >= tag.size()) break;
        std::string key(reinterpret_cast<const char*>(tag.data() + key_start),
                        pos - key_start);
        ++pos;
        if (value_size > tag.size() - pos) break;
        bool text_item = ((flags >> 1) & 0x3) == 0;
        if (text_item) {
            std::string value(reinterpret_cast<const char*>(tag.data() + pos),
                              value_size);
            store_replaygain_tag(metadata, key, value);
        }
        pos += value_size;
    }
}

void read_flac_replaygain(const std::filesystem::path& path,
                          ReplayGainMetadata& metadata) {
    auto on_meta = [](void* user, ma_dr_flac_metadata* meta) {
        if (!user || !meta ||
            meta->type != MA_DR_FLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
            return;
        }
        auto* out = static_cast<ReplayGainMetadata*>(user);
        ma_dr_flac_vorbis_comment_iterator iter;
        ma_dr_flac_init_vorbis_comment_iterator(
            &iter,
            meta->data.vorbis_comment.commentCount,
            meta->data.vorbis_comment.pComments);
        ma_uint32 length = 0;
        while (const char* comment =
                   ma_dr_flac_next_vorbis_comment(&iter, &length)) {
            store_replaygain_comment(*out, std::string_view(comment, length));
        }
    };

    ma_dr_flac* flac = ma_dr_flac_open_file_with_metadata_w(
        path.wstring().c_str(), on_meta, &metadata, nullptr);
    if (flac) ma_dr_flac_close(flac);
}

ReplayGainChoice choose_replaygain(const ReplayGainMetadata& metadata) {
    ReplayGainChoice choice;
    if (metadata.has_track_gain) {
        choice.found = true;
        choice.used_track = true;
        choice.gain_db = metadata.track_gain_db;
        if (metadata.has_track_peak) choice.peak = metadata.track_peak;
    } else if (metadata.has_album_gain) {
        choice.found = true;
        choice.used_track = false;
        choice.gain_db = metadata.album_gain_db;
        if (metadata.has_album_peak) choice.peak = metadata.album_peak;
    }
    if (!choice.found) return choice;

    choice.linear_gain = std::pow(10.0f, choice.gain_db / 20.0f);
    if (!std::isfinite(choice.linear_gain)) {
        choice.linear_gain = 1.0f;
    }
    choice.linear_gain = std::min(choice.linear_gain, 1.0f);
    if (choice.peak > 1.0f && std::isfinite(choice.peak)) {
        choice.linear_gain = std::min(choice.linear_gain, 1.0f / choice.peak);
    }
    choice.linear_gain = std::clamp(choice.linear_gain, 0.0f, 1.0f);
    return choice;
}

ReplayGainChoice read_replaygain_metadata(const std::filesystem::path& path) {
    ReplayGainMetadata metadata;
    std::string ext = lower_ext(path);
    if (ext == "flac") {
        read_flac_replaygain(path, metadata);
    } else if (ext == "mp3") {
        read_mp3_id3v2_replaygain(path, metadata);
        read_mp3_apev2_replaygain(path, metadata);
    }
    return choose_replaygain(metadata);
}

bool file_is_all_zero(const std::filesystem::path& path, bool& read_ok) {
    read_ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    read_ok = true;

    char buffer[64 * 1024];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            if (buffer[i] != 0) return false;
        }
    }
    return true;
}

float clamp_audio(float sample) {
    if (sample > 1.0f) return 1.0f;
    if (sample < -1.0f) return -1.0f;
    return sample;
}

int16_t float_to_s16(float sample) {
    sample = clamp_audio(sample) * 32768.0f;
    if (sample > 32767.0f) return 32767;
    if (sample < -32768.0f) return -32768;
    return static_cast<int16_t>(sample);
}

std::string trim_ascii(std::string_view value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string utf8_from_wide(const wchar_t* value, size_t length) {
    if (!value || length == 0 || length > static_cast<size_t>(INT_MAX)) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, static_cast<int>(length),
                        out.data(), needed, nullptr, nullptr);
    return trim_ascii(out);
}

std::string utf8_from_wide_z(const wchar_t* value) {
    return value ? utf8_from_wide(value, std::wcslen(value)) : std::string{};
}

void append_metadata_part(std::string& out, const std::string& part) {
    if (part.empty()) return;
    if (!out.empty()) out += ", ";
    out += part;
}

std::string metadata_string_from_propvariant(const PROPVARIANT& value) {
    std::string out;
    switch (value.vt) {
    case VT_LPWSTR:
        append_metadata_part(out, utf8_from_wide_z(value.pwszVal));
        break;
    case VT_BSTR:
        append_metadata_part(out, utf8_from_wide(
            value.bstrVal, SysStringLen(value.bstrVal)));
        break;
    case VT_VECTOR | VT_LPWSTR:
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i) {
            append_metadata_part(out, utf8_from_wide_z(value.calpwstr.pElems[i]));
        }
        break;
    case VT_VECTOR | VT_BSTR:
        for (ULONG i = 0; i < value.cabstr.cElems; ++i) {
            append_metadata_part(out, utf8_from_wide(
                value.cabstr.pElems[i], SysStringLen(value.cabstr.pElems[i])));
        }
        break;
    default:
        break;
    }
    return out;
}

LocalFileMetadata read_local_file_metadata(const std::filesystem::path& path) {
    LocalFileMetadata metadata;
    ScopedComInit com;
    if (!com.ready()) return metadata;

    IPropertyStore* store = nullptr;
    HRESULT hr = SHGetPropertyStoreFromParsingName(
        path.wstring().c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(&store));
    if (FAILED(hr) || !store) return metadata;

    auto read_property = [&](const PROPERTYKEY& key) {
        PROPVARIANT value;
        PropVariantInit(&value);
        std::string out;
        if (SUCCEEDED(store->GetValue(key, &value))) {
            out = metadata_string_from_propvariant(value);
        }
        PropVariantClear(&value);
        return out;
    };

    metadata.title = read_property(PKEY_Title);
    metadata.album_artist = read_property(PKEY_Music_AlbumArtist);
    metadata.artist = read_property(PKEY_Music_Artist);
    if (metadata.artist.empty()) metadata.artist = read_property(PKEY_Author);
    metadata.album = read_property(PKEY_Music_AlbumTitle);
    store->Release();
    return metadata;
}

void append_unique_metadata_part(std::string& out, const std::string& part) {
    if (part.empty()) return;
    if (!out.empty() && ascii_lower(out) == ascii_lower(part)) return;
    if (!out.empty()) out += ", ";
    out += part;
}

std::string combined_album_artist(const LocalFileMetadata& metadata) {
    std::string out;
    append_unique_metadata_part(out, metadata.album_artist);
    append_unique_metadata_part(out, metadata.artist);
    return out;
}

} // namespace

struct LocalFileSource::Decoder {
    enum class Kind {
        None,
        Miniaudio,
        MediaFoundation,
    };

    ma_decoder decoder{};
    IMFSourceReader* mf_reader = nullptr;
    std::vector<int16_t> mf_pending;
    size_t mf_pending_pos = 0;
    uint64_t mf_frames_read = 0;
    Kind kind = Kind::None;
    uint32_t duration_ms = 0;
};

struct LocalFileSource::Biquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1_l = 0.0f;
    float z2_l = 0.0f;
    float z1_r = 0.0f;
    float z2_r = 0.0f;
};

LocalFileSource::LocalFileSource(FeedPcmFn feed_pcm,
                                 FreeBytesFn free_bytes,
                                 ClearPcmFn clear_pcm)
    : feed_pcm_(std::move(feed_pcm)),
      free_bytes_(std::move(free_bytes)),
      clear_pcm_(std::move(clear_pcm)),
      default_music_dir_(resolve_default_music_dir()),
      decoder_(std::make_unique<Decoder>()),
      equalizer_(std::make_unique<Biquad[]>(5)) {}

LocalFileSource::~LocalFileSource() {
    shutdown();
}

void LocalFileSource::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&LocalFileSource::worker_thread_fn, this);
}

void LocalFileSource::shutdown() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mtx_);
    close_decoder_locked();
}

bool LocalFileSource::is_connected() const {
    std::lock_guard lock(mtx_);
    return !playlist_.empty();
}

bool LocalFileSource::is_playing() const {
    return playing_.load(std::memory_order_acquire);
}

SourceTrack LocalFileSource::last_track() const {
    std::lock_guard lock(mtx_);
    return current_track_;
}

void LocalFileSource::pause_at_audio_boundary() {
    playing_.store(false, std::memory_order_release);
}

void LocalFileSource::resume_rewound(uint32_t rewind_ms) {
    bool can_play = false;
    {
        std::lock_guard lock(mtx_);
        if (!decoder_is_open_locked()) {
            can_play = open_playable_track_locked(cursor_, true);
        } else {
            can_play = true;
        }
        if (can_play && rewind_ms > 0) {
            uint32_t cursor = decoder_cursor_ms_locked();
            uint32_t target = cursor > rewind_ms ? cursor - rewind_ms : 0;
            seek_decoder_locked(target);
        }
    }
    playing_.store(can_play, std::memory_order_release);
}

bool LocalFileSource::restart_current_track() {
    bool ok = seek(0);
    if (ok) playing_.store(true, std::memory_order_release);
    return ok;
}

bool LocalFileSource::next_track() {
    std::lock_guard lock(mtx_);
    if (playlist_.empty()) return false;
    bool keep_playing = playing_.load(std::memory_order_acquire);
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] local manual next cursor="
              + std::to_string(cursor_)
              + " playing=" + std::to_string(keep_playing)
              + " manual_queue=" + std::to_string(manual_queue_.size())
              + " autoplay_queue=" + std::to_string(autoplay_queue_.size())
              + " position_ms=" + std::to_string(position_ms_));
#endif
    if (!advance_to_next_track_locked()) return false;
    playing_.store(keep_playing, std::memory_order_release);
    if (clear_pcm_) clear_pcm_();
    return true;
}

bool LocalFileSource::previous_track() {
    std::lock_guard lock(mtx_);
    if (playlist_.empty()) return false;
    bool keep_playing = playing_.load(std::memory_order_acquire);
    size_t next = cursor_ == 0 ? playlist_.size() - 1 : cursor_ - 1;
    if (!open_playable_track_locked(next, false)) return false;
    playing_.store(keep_playing, std::memory_order_release);
    if (clear_pcm_) clear_pcm_();
    return true;
}

bool LocalFileSource::seek(uint32_t position_ms) {
    std::lock_guard lock(mtx_);
    if (!decoder_is_open_locked()) {
        if (playlist_.empty() || !open_playable_track_locked(cursor_, true)) return false;
    }
    if (!seek_decoder_locked(position_ms)) return false;
    position_ms_ = position_ms;
    if (clear_pcm_) clear_pcm_();
    return true;
}

void LocalFileSource::set_pcm_enabled(bool enabled) {
    pcm_enabled_.store(enabled, std::memory_order_release);
}

void LocalFileSource::set_volume_normalization(bool enabled) {
    std::lock_guard lock(mtx_);
    volume_normalization_ = enabled;
}

void LocalFileSource::set_equalizer(bool enabled,
                                    const std::array<float, 5>& bands_db) {
    std::lock_guard lock(mtx_);
    equalizer_enabled_ = enabled;
    equalizer_bands_db_ = bands_db;
    rebuild_equalizer_locked();
}

void LocalFileSource::set_volume_percent(uint32_t percent) {
    std::lock_guard lock(mtx_);
    volume_percent_ = std::min<uint32_t>(percent, 300);
    volume_gain_ = static_cast<float>(volume_percent_) / 100.0f;
}

void LocalFileSource::set_metadata_display_modes(std::string title_mode,
                                                 std::string artist_mode) {
    std::lock_guard lock(mtx_);
    title_metadata_mode_ = title_mode == "filename" ? "filename" : "metadata";
    if (artist_mode == "folder" || artist_mode == "album") {
        artist_metadata_mode_ = std::move(artist_mode);
    } else {
        artist_metadata_mode_ = "albumArtist";
    }
    if (!current_path_.empty()) {
        apply_current_track_metadata_locked(current_path_);
    }
}

void LocalFileSource::publish_state(BridgeStateStore& store) const {
    std::lock_guard lock(mtx_);
    store.set_local_source(true, !playlist_.empty(),
                           playing_.load(std::memory_order_acquire),
                           path_to_utf8(music_dir_), default_music_dir_,
                           recursive_, shuffle_,
                           static_cast<uint32_t>(playlist_.size()),
                           unsupported_count_, position_ms_, error_);
}

bool LocalFileSource::configure(std::string music_dir, bool recursive,
                                bool shuffle, std::string* error) {
    {
        std::lock_guard lock(mtx_);
        music_dir_ = path_from_utf8(music_dir);
        recursive_ = recursive;
        shuffle_ = shuffle;
    }
    return rescan(error);
}

bool LocalFileSource::rescan(std::string* error) {
    std::vector<std::filesystem::path> next_library;
    uint32_t unsupported = 0;
    std::filesystem::path dir;
    bool recursive = true;
    bool shuffle = true;
    {
        std::lock_guard lock(mtx_);
        dir = music_dir_;
        recursive = recursive_;
        shuffle = shuffle_;
    }

    if (dir.empty()) {
        std::lock_guard lock(mtx_);
        library_.clear();
        playlist_.clear();
        manual_queue_.clear();
        autoplay_queue_.clear();
        unsupported_count_ = 0;
        error_.clear();
        close_decoder_locked();
        playing_.store(false, std::memory_order_release);
        if (error) error->clear();
        return true;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        std::string msg = "Music folder does not exist";
        std::lock_guard lock(mtx_);
        library_.clear();
        playlist_.clear();
        manual_queue_.clear();
        autoplay_queue_.clear();
        unsupported_count_ = 0;
        error_ = msg;
        close_decoder_locked();
        playing_.store(false, std::memory_order_release);
        if (error) *error = msg;
        return false;
    }

    auto add_file = [&](const std::filesystem::path& path) {
        if (is_supported_audio(path)) {
            next_library.push_back(path);
        } else if (is_known_audio(path)) {
            ++unsupported;
        }
    };

    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            add_file(it->path());
        }
    } else {
        for (std::filesystem::directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            add_file(it->path());
        }
    }

    std::sort(next_library.begin(), next_library.end());
    std::vector<std::filesystem::path> next_playlist = next_library;
    if (shuffle && next_playlist.size() > 1) {
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(next_playlist.begin(), next_playlist.end(), rng);
    }

    std::string msg;
    if (ec) {
        msg = "Could not scan every file in the folder";
    } else if (next_library.empty()) {
        msg = "No supported MP3, WAV, or FLAC files found";
    }

    {
        std::lock_guard lock(mtx_);
        library_ = std::move(next_library);
        playlist_ = std::move(next_playlist);
        unsupported_count_ = unsupported;
        cursor_ = 0;
        manual_queue_.clear();
        autoplay_queue_.clear();
        error_ = msg;
        close_decoder_locked();
        current_path_.clear();
        current_track_ = {};
        position_ms_ = 0;
        if (playlist_.empty()) {
            playing_.store(false, std::memory_order_release);
        }
    }
    if (clear_pcm_) clear_pcm_();
    if (error) *error = msg;
    log::info("[local] scanned " + path_to_utf8(dir)
              + " tracks=" + std::to_string(track_count())
              + " unsupported=" + std::to_string(unsupported));
    return msg.empty();
}

void LocalFileSource::set_shuffle(bool shuffle) {
    bool changed = false;
    {
        std::lock_guard lock(mtx_);
        changed = shuffle_ != shuffle;
        shuffle_ = shuffle;
    }
    if (changed) {
        std::string ignored;
        rescan(&ignored);
    }
}

std::vector<LocalFileSource::TrackInfo> LocalFileSource::library_snapshot() const {
    std::lock_guard lock(mtx_);
    std::vector<TrackInfo> out;
    out.reserve(library_.size());
    for (size_t i = 0; i < library_.size(); ++i) {
        out.push_back(track_info_locked(library_[i], static_cast<uint32_t>(i)));
    }
    return out;
}

std::vector<LocalFileSource::QueueEntryInfo> LocalFileSource::manual_queue_snapshot() const {
    std::lock_guard lock(mtx_);
    std::vector<QueueEntryInfo> out;
    out.reserve(manual_queue_.size());
    for (const auto& entry : manual_queue_) {
        QueueEntryInfo item;
        item.id = entry.id;
        auto it = std::find(library_.begin(), library_.end(), entry.path);
        uint32_t index = it == library_.end()
            ? UINT32_MAX
            : static_cast<uint32_t>(std::distance(library_.begin(), it));
        item.track = track_info_locked(entry.path, index);
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<LocalFileSource::TrackInfo> LocalFileSource::autoplay_queue_snapshot() const {
    std::lock_guard lock(mtx_);
    std::vector<TrackInfo> out;
    out.reserve(autoplay_queue_.size());
    for (const auto& path : autoplay_queue_) {
        auto it = std::find(library_.begin(), library_.end(), path);
        uint32_t index = it == library_.end()
            ? UINT32_MAX
            : static_cast<uint32_t>(std::distance(library_.begin(), it));
        out.push_back(track_info_locked(path, index));
    }
    return out;
}

bool LocalFileSource::play_library_index(uint32_t index) {
    std::lock_guard lock(mtx_);
    if (index >= library_.size()) return false;
    bool ok = open_path_locked(library_[index]);
    if (ok) {
        playing_.store(true, std::memory_order_release);
        if (clear_pcm_) clear_pcm_();
    }
    return ok;
}

bool LocalFileSource::queue_library_index(uint32_t index) {
    std::lock_guard lock(mtx_);
    if (index >= library_.size()) return false;
    manual_queue_.push_back({ next_queue_id_++, library_[index] });
    return true;
}

uint32_t LocalFileSource::queue_folder(std::string folder) {
    std::lock_guard lock(mtx_);
    if (!recursive_) return 0;
    uint32_t added = 0;
    for (const auto& path : library_) {
        auto info = track_info_locked(path, 0);
        if (info.folder != folder) continue;
        manual_queue_.push_back({ next_queue_id_++, path });
        ++added;
    }
    return added;
}

bool LocalFileSource::remove_manual_queue_entry(uint64_t id) {
    std::lock_guard lock(mtx_);
    auto it = std::find_if(manual_queue_.begin(), manual_queue_.end(),
                           [id](const ManualQueueEntry& entry) {
                               return entry.id == id;
                           });
    if (it == manual_queue_.end()) return false;
    manual_queue_.erase(it);
    return true;
}

std::string LocalFileSource::music_dir() const {
    std::lock_guard lock(mtx_);
    return path_to_utf8(music_dir_);
}

std::string LocalFileSource::default_music_dir() const {
    return default_music_dir_;
}

bool LocalFileSource::recursive() const {
    std::lock_guard lock(mtx_);
    return recursive_;
}

bool LocalFileSource::shuffle() const {
    std::lock_guard lock(mtx_);
    return shuffle_;
}

uint32_t LocalFileSource::track_count() const {
    std::lock_guard lock(mtx_);
    return static_cast<uint32_t>(playlist_.size());
}

uint32_t LocalFileSource::unsupported_count() const {
    std::lock_guard lock(mtx_);
    return unsupported_count_;
}

uint32_t LocalFileSource::position_ms() const {
    std::lock_guard lock(mtx_);
    return position_ms_;
}

std::optional<uint32_t> LocalFileSource::current_position_ms() const {
    if (!playing_.load(std::memory_order_acquire)) return std::nullopt;
    std::lock_guard lock(mtx_);
    return position_ms_;
}

std::string LocalFileSource::last_error() const {
    std::lock_guard lock(mtx_);
    return error_;
}

void LocalFileSource::worker_thread_fn() {
    std::vector<int16_t> scratch(kMaxChunkFrames * kChannels);
    while (running_.load(std::memory_order_acquire)) {
        if (!playing_.load(std::memory_order_acquire) ||
            !pcm_enabled_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        size_t free_bytes = free_bytes_ ? free_bytes_() : kMaxChunkFrames * kFrameBytes;
        if (free_bytes < kMinBufferedFrames * kFrameBytes) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        size_t want_frames = std::min(kMaxChunkFrames, free_bytes / kFrameBytes);
        ma_uint64 frames_read = 0;
        {
            std::lock_guard lock(mtx_);
            if (playlist_.empty()) {
                playing_.store(false, std::memory_order_release);
                continue;
            }
            if (!decoder_is_open_locked() && !open_playable_track_locked(cursor_, true)) {
                playing_.store(false, std::memory_order_release);
                continue;
            }
            frames_read = read_decoder_frames_locked(
                scratch.data(), static_cast<uint64_t>(want_frames));
            if (frames_read == 0) {
#if defined(SPOTIFY_RADIO_DIAG)
                log::info("[skip-diag] local decoder eof/zero-read"
                          " cursor=" + std::to_string(cursor_)
                          + " position_ms=" + std::to_string(position_ms_)
                          + " duration_ms="
                          + std::to_string(decoder_->duration_ms)
                          + " manual_queue="
                          + std::to_string(manual_queue_.size())
                          + " autoplay_queue="
                          + std::to_string(autoplay_queue_.size()));
#endif
                advance_to_next_track_locked();
                continue;
            }
            process_equalizer_locked(scratch.data(), static_cast<size_t>(frames_read));
            position_ms_ = decoder_cursor_ms_locked();
        }

        if (frames_read > 0 && feed_pcm_) {
            float gain = 1.0f;
            {
                std::lock_guard lock(mtx_);
                gain = volume_gain_;
                if (volume_normalization_) gain *= replaygain_gain_;
            }
            if (!feed_pcm_(scratch.data(), static_cast<size_t>(frames_read), gain)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
}

bool LocalFileSource::open_track_locked(size_t index) {
    if (playlist_.empty()) return false;
    cursor_ = index % playlist_.size();
    const auto path = playlist_[cursor_];
    close_decoder_locked();
    auto set_decode_error = [&]() {
        bool read_ok = false;
        if (file_is_all_zero(path, read_ok) && read_ok) {
            error_ = "File contains no audio data: " + path.filename().string();
            log::warn("[local] all-zero audio file " + path_to_utf8(path));
        } else {
            error_ = "Could not decode " + path.filename().string();
        }
    };

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, kChannels, kSampleRate);
    ma_result r = ma_decoder_init_file_w(path.wstring().c_str(), &cfg, &decoder_->decoder);
    if (r != MA_SUCCESS) {
        log::warn("[local] miniaudio rejected " + path_to_utf8(path)
                  + " result=" + std::to_string(static_cast<int>(r)));
        if (lower_ext(path) == "mp3" &&
            ensure_com_initialized_for_thread() &&
            ensure_media_foundation_started()) {
            IMFSourceReader* reader = nullptr;
            HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(),
                                                     nullptr, &reader);
            if (SUCCEEDED(hr)) {
                IMFMediaType* type = nullptr;
                hr = MFCreateMediaType(&type);
                if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
                if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
                if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(kFrameBytes));
                if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * static_cast<UINT32>(kFrameBytes));
                if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
                if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
                if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
                release_com(type);
                if (SUCCEEDED(hr)) {
                    decoder_->mf_reader = reader;
                    decoder_->kind = Decoder::Kind::MediaFoundation;
                    decoder_->duration_ms = 0;
                    PROPVARIANT duration;
                    PropVariantInit(&duration);
                    if (SUCCEEDED(reader->GetPresentationAttribute(
                            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)) &&
                        duration.vt == VT_UI8) {
                        decoder_->duration_ms =
                            static_cast<uint32_t>(duration.uhVal.QuadPart / 10000ull);
                    }
                    PropVariantClear(&duration);
                    log::info("[local] Media Foundation fallback opened "
                              + path_to_utf8(path));
                } else {
                    release_com(reader);
                    set_decode_error();
                    log::warn("[local] Media Foundation rejected " + path_to_utf8(path)
                              + " hr=" + std::to_string(static_cast<long>(hr)));
                    return false;
                }
            } else {
                set_decode_error();
                log::warn("[local] Media Foundation could not open " + path_to_utf8(path)
                          + " hr=" + std::to_string(static_cast<long>(hr)));
                return false;
            }
        } else {
            set_decode_error();
            return false;
        }
    } else {
        decoder_->kind = Decoder::Kind::Miniaudio;
        decoder_->duration_ms = 0;
        ma_uint64 frames = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_->decoder, &frames) == MA_SUCCESS) {
            decoder_->duration_ms = static_cast<uint32_t>((frames * 1000ull) / kSampleRate);
        }
    }

    current_path_ = path;
    current_track_.uri = "local:" + path_to_utf8(path);
    apply_current_track_metadata_locked(path);
    current_track_.duration_ms = decoder_->duration_ms;
    position_ms_ = 0;
    error_.clear();
    ReplayGainChoice replaygain = read_replaygain_metadata(path);
    replaygain_gain_ = replaygain.found ? replaygain.linear_gain : 1.0f;
#if defined(SPOTIFY_RADIO_DIAG)
    if (replaygain.found) {
        log::info("[local] replaygain "
                  + std::string(replaygain.used_track ? "track" : "album")
                  + " gain_db=" + std::to_string(replaygain.gain_db)
                  + " peak=" + std::to_string(replaygain.peak)
                  + " linear=" + std::to_string(replaygain_gain_));
    } else {
        log::info("[local] replaygain none");
    }
#endif
    rebuild_equalizer_locked();
    log::info("[local] now playing " + path_to_utf8(path));
    rebuild_autoplay_queue_locked();
    return true;
}

bool LocalFileSource::open_path_locked(const std::filesystem::path& path) {
    auto it = std::find(playlist_.begin(), playlist_.end(), path);
    if (it == playlist_.end()) {
        it = std::find(library_.begin(), library_.end(), path);
        if (it == library_.end()) return false;
        playlist_.push_back(path);
        it = playlist_.end() - 1;
    }
    return open_track_locked(static_cast<size_t>(std::distance(playlist_.begin(), it)));
}

bool LocalFileSource::open_playable_track_locked(size_t start_index, bool forward) {
    if (playlist_.empty()) return false;
    if (forward) start_index = maybe_reshuffle_for_wrap_locked(start_index);
    std::string last_error;
    for (size_t attempt = 0; attempt < playlist_.size(); ++attempt) {
        size_t index = 0;
        if (forward) {
            index = (start_index + attempt) % playlist_.size();
        } else {
            size_t start = start_index % playlist_.size();
            index = (start + playlist_.size() - (attempt % playlist_.size())) %
                    playlist_.size();
        }
        if (open_track_locked(index)) return true;
        last_error = error_;
    }
    if (!last_error.empty()) {
        error_ = "No playable MP3, WAV, or FLAC files found";
        log::warn("[local] no playable track after scanning "
                  + std::to_string(playlist_.size()) + " candidates; last="
                  + last_error);
    }
    return false;
}

bool LocalFileSource::advance_to_next_track_locked() {
    while (!manual_queue_.empty()) {
        auto next = manual_queue_.front();
        manual_queue_.erase(manual_queue_.begin());
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] local advance reason=manual_queue path="
                  + path_to_utf8(next.path));
#endif
        if (open_path_locked(next.path)) return true;
    }
    if (!autoplay_queue_.empty()) {
        auto next = autoplay_queue_.front();
        autoplay_queue_.erase(autoplay_queue_.begin());
#if defined(SPOTIFY_RADIO_DIAG)
        log::info("[skip-diag] local advance reason=autoplay_queue path="
                  + path_to_utf8(next));
#endif
        if (open_path_locked(next)) return true;
    }
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] local advance reason=playlist_wrap next_cursor="
              + std::to_string(cursor_ + 1));
#endif
    return open_playable_track_locked(cursor_ + 1, true);
}

size_t LocalFileSource::maybe_reshuffle_for_wrap_locked(size_t start_index) {
    if (shuffle_ && start_index >= playlist_.size() && playlist_.size() > 1) {
        std::filesystem::path previous = playlist_[cursor_ % playlist_.size()];
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(playlist_.begin(), playlist_.end(), rng);
        if (playlist_.front() == previous) {
            auto replacement = std::find_if(
                playlist_.begin() + 1, playlist_.end(),
                [&previous](const std::filesystem::path& path) {
                    return path != previous;
                });
            if (replacement != playlist_.end()) {
                std::iter_swap(playlist_.begin(), replacement);
            }
        }
        auto current = std::find(playlist_.begin(), playlist_.end(), previous);
        if (current != playlist_.end()) {
            cursor_ = static_cast<size_t>(std::distance(playlist_.begin(), current));
        }
        start_index = 0;
        log::info("[local] reshuffled playback order");
    }
    return start_index;
}

void LocalFileSource::rebuild_autoplay_queue_locked() {
    autoplay_queue_.clear();
    if (playlist_.empty()) return;
    size_t start = maybe_reshuffle_for_wrap_locked(cursor_ + 1);
    for (size_t i = 0; i < kAutoplayQueueSize; ++i) {
        autoplay_queue_.push_back(playlist_[(start + i) % playlist_.size()]);
    }
}

LocalFileSource::TrackInfo LocalFileSource::track_info_locked(
    const std::filesystem::path& path,
    uint32_t index) const {
    TrackInfo info;
    info.index = index;
    info.path = path_to_utf8(path);
    info.title = title_from_path(path);
    std::filesystem::path folder = path.parent_path();
    std::error_code ec;
    if (!music_dir_.empty()) {
        folder = std::filesystem::relative(folder, music_dir_, ec);
    }
    if (ec || folder.empty() || folder == ".") {
        info.folder.clear();
    } else {
        info.folder = path_to_utf8(folder);
    }
    return info;
}

void LocalFileSource::close_decoder_locked() {
    if (decoder_->kind == Decoder::Kind::Miniaudio) {
        ma_decoder_uninit(&decoder_->decoder);
    } else if (decoder_->kind == Decoder::Kind::MediaFoundation) {
        release_com(decoder_->mf_reader);
    }
    decoder_->kind = Decoder::Kind::None;
    decoder_->duration_ms = 0;
    decoder_->mf_pending.clear();
    decoder_->mf_pending_pos = 0;
    decoder_->mf_frames_read = 0;
    replaygain_gain_ = 1.0f;
}

bool LocalFileSource::decoder_is_open_locked() const {
    return decoder_->kind != Decoder::Kind::None;
}

bool LocalFileSource::seek_decoder_locked(uint32_t position_ms) {
    if (decoder_->kind == Decoder::Kind::Miniaudio) {
        ma_uint64 target = (static_cast<ma_uint64>(position_ms) * kSampleRate) / 1000u;
        return ma_decoder_seek_to_pcm_frame(&decoder_->decoder, target) == MA_SUCCESS;
    }
    if (decoder_->kind == Decoder::Kind::MediaFoundation && decoder_->mf_reader) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = static_cast<LONGLONG>(position_ms) * 10000ll;
        HRESULT hr = decoder_->mf_reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
        if (FAILED(hr)) return false;
        decoder_->mf_pending.clear();
        decoder_->mf_pending_pos = 0;
        decoder_->mf_frames_read =
            (static_cast<uint64_t>(position_ms) * kSampleRate) / 1000u;
        return true;
    }
    return false;
}

uint64_t LocalFileSource::read_decoder_frames_locked(int16_t* samples, uint64_t frames) {
    if (decoder_->kind == Decoder::Kind::Miniaudio) {
        ma_uint64 frames_read = 0;
        ma_result r = ma_decoder_read_pcm_frames(
            &decoder_->decoder, samples, static_cast<ma_uint64>(frames), &frames_read);
        return r == MA_SUCCESS ? static_cast<uint64_t>(frames_read) : 0;
    }
    if (decoder_->kind != Decoder::Kind::MediaFoundation || !decoder_->mf_reader) return 0;

    uint64_t written = 0;
    auto drain_pending = [&]() {
        size_t pending_samples = decoder_->mf_pending.size() - decoder_->mf_pending_pos;
        size_t wanted_samples = static_cast<size_t>((frames - written) * kChannels);
        size_t copy_samples = std::min(pending_samples, wanted_samples);
        if (copy_samples == 0) return;
        std::memcpy(samples + written * kChannels,
                    decoder_->mf_pending.data() + decoder_->mf_pending_pos,
                    copy_samples * sizeof(int16_t));
        decoder_->mf_pending_pos += copy_samples;
        written += copy_samples / kChannels;
        if (decoder_->mf_pending_pos >= decoder_->mf_pending.size()) {
            decoder_->mf_pending.clear();
            decoder_->mf_pending_pos = 0;
        }
    };

    while (written < frames) {
        drain_pending();
        if (written >= frames) break;

        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = decoder_->mf_reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            release_com(sample);
            break;
        }
        if (!sample) continue;

        IMFMediaBuffer* buffer = nullptr;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (SUCCEEDED(hr) && buffer) {
            BYTE* data = nullptr;
            DWORD max_len = 0;
            DWORD len = 0;
            hr = buffer->Lock(&data, &max_len, &len);
            if (SUCCEEDED(hr) && data && len >= kFrameBytes) {
                len -= len % static_cast<DWORD>(kFrameBytes);
                size_t sample_count = len / sizeof(int16_t);
                size_t offset = decoder_->mf_pending.size();
                decoder_->mf_pending.resize(offset + sample_count);
                std::memcpy(decoder_->mf_pending.data() + offset, data, len);
            }
            if (SUCCEEDED(hr)) buffer->Unlock();
        }
        release_com(buffer);
        release_com(sample);
    }

    decoder_->mf_frames_read += written;
    return written;
}

uint32_t LocalFileSource::decoder_cursor_ms_locked() const {
    if (decoder_->kind == Decoder::Kind::Miniaudio) {
        ma_uint64 cursor = 0;
        if (ma_decoder_get_cursor_in_pcm_frames(&decoder_->decoder, &cursor) == MA_SUCCESS) {
            return static_cast<uint32_t>((cursor * 1000ull) / kSampleRate);
        }
        return position_ms_;
    }
    if (decoder_->kind == Decoder::Kind::MediaFoundation) {
        return static_cast<uint32_t>((decoder_->mf_frames_read * 1000ull) / kSampleRate);
    }
    return 0;
}

void LocalFileSource::rebuild_equalizer_locked() {
    static constexpr float centers[5] = {60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
    static constexpr float q = 1.4142f;
    for (size_t i = 0; i < 5; ++i) {
        Biquad& f = equalizer_[i];
        float gain_db = equalizer_bands_db_[i];
        float a = std::pow(10.0f, gain_db / 40.0f);
        float w0 = 2.0f * 3.14159265358979323846f * centers[i] / kSampleRate;
        float cw = std::cos(w0);
        float sw = std::sin(w0);
        float alpha = sw / (2.0f * q);
        float a0 = 1.0f + alpha / a;
        f.b0 = (1.0f + alpha * a) / a0;
        f.b1 = (-2.0f * cw) / a0;
        f.b2 = (1.0f - alpha * a) / a0;
        f.a1 = (-2.0f * cw) / a0;
        f.a2 = (1.0f - alpha / a) / a0;
        f.z1_l = f.z2_l = f.z1_r = f.z2_r = 0.0f;
    }
}

void LocalFileSource::process_equalizer_locked(int16_t* samples, size_t frames) {
    if (!equalizer_enabled_) return;
    auto process_one = [](Biquad& f, float x, float& z1, float& z2) {
        float y = f.b0 * x + z1;
        z1 = f.b1 * x - f.a1 * y + z2;
        z2 = f.b2 * x - f.a2 * y;
        return y;
    };
    for (size_t i = 0; i < frames; ++i) {
        float left = static_cast<float>(samples[i * 2]) / 32768.0f;
        float right = static_cast<float>(samples[i * 2 + 1]) / 32768.0f;
        for (size_t band = 0; band < 5; ++band) {
            left = process_one(equalizer_[band], left,
                               equalizer_[band].z1_l,
                               equalizer_[band].z2_l);
            right = process_one(equalizer_[band], right,
                                equalizer_[band].z1_r,
                                equalizer_[band].z2_r);
        }
        samples[i * 2] = float_to_s16(left);
        samples[i * 2 + 1] = float_to_s16(right);
    }
}

void LocalFileSource::apply_current_track_metadata_locked(
    const std::filesystem::path& path) {
    const std::string filename_title = title_from_path(path);
    const std::string folder_artist =
        path_to_utf8(path.parent_path().filename());
    const LocalFileMetadata metadata = read_local_file_metadata(path);
    const std::string album_artist = combined_album_artist(metadata);

    if (title_metadata_mode_ == "filename") {
        current_track_.title = filename_title.empty()
            ? metadata.title
            : filename_title;
    } else {
        current_track_.title = metadata.title.empty()
            ? filename_title
            : metadata.title;
    }

    auto first_non_empty = [](std::initializer_list<std::string> values) {
        for (const auto& value : values) {
            if (!value.empty()) return value;
        }
        return std::string{};
    };

    if (artist_metadata_mode_ == "folder") {
        current_track_.artist =
            first_non_empty({folder_artist, album_artist, metadata.album});
    } else if (artist_metadata_mode_ == "album") {
        current_track_.artist =
            first_non_empty({metadata.album, album_artist, folder_artist});
    } else {
        current_track_.artist =
            first_non_empty({album_artist, metadata.album, folder_artist});
    }
    current_track_.album = metadata.album;
}

std::string LocalFileSource::path_to_utf8(const std::filesystem::path& path) {
    if (path.empty()) return {};
    auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::filesystem::path LocalFileSource::path_from_utf8(const std::string& path) {
    if (path.empty()) return {};
    std::u8string u8;
    u8.reserve(path.size());
    for (unsigned char c : path) u8.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(u8);
}

std::string LocalFileSource::title_from_path(const std::filesystem::path& path) {
    auto u8 = path.stem().u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

std::string LocalFileSource::resolve_default_music_dir() {
    wchar_t profile[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return path_to_utf8(std::filesystem::path(profile) / L"Music");
    }
    return {};
}

} // namespace bridge
