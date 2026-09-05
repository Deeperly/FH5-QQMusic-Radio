#include "qqmusic_source.h"

#include "fmod_inject.h"
#include "log_file.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace bridge {
namespace {

std::wstring lowercase(const std::wstring& value) {
    std::wstring result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t character) {
                       return static_cast<wchar_t>(std::towlower(character));
                   });
    return result;
}

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivationHandler(HANDLE completion_event)
        : completion_event_(completion_event) {}

    STDMETHOD(QueryInterface)(REFIID interface_id, void** object) override {
        if (!object) return E_POINTER;
        if (interface_id == __uuidof(IUnknown) ||
            interface_id == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            interface_id == __uuidof(IAgileObject)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)() override {
        return ++reference_count_;
    }

    STDMETHOD_(ULONG, Release)() override {
        return --reference_count_;
    }

    STDMETHOD(ActivateCompleted)(
        IActivateAudioInterfaceAsyncOperation*) override {
        SetEvent(completion_event_);
        return S_OK;
    }

private:
    HANDLE completion_event_ = nullptr;
    std::atomic<ULONG> reference_count_{1};
};

struct WindowTitleContext {
    DWORD process_id = 0;
    std::wstring title;
};

BOOL CALLBACK find_qqmusic_window_proc(HWND window, LPARAM parameter) {
    auto* context = reinterpret_cast<WindowTitleContext*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != context->process_id || !IsWindowVisible(window)) return TRUE;

    int length = GetWindowTextLengthW(window);
    if (length <= 0) return TRUE;

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    title.resize(static_cast<size_t>(length));
    if (context->title.empty() || title.size() > context->title.size()) {
        context->title = std::move(title);
    }
    return TRUE;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                    static_cast<int>(value.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                            static_cast<int>(value.size()),
                            result.data(), size, nullptr, nullptr);
    }
    return result;
}

void parse_qqmusic_title(const std::wstring& title,
                         std::string& track_name,
                         std::string& artist) {
    const size_t separator = title.rfind(L" - ");
    if (separator == std::wstring::npos) {
        track_name = wide_to_utf8(title);
        artist = "QQ Music";
        return;
    }
    track_name = wide_to_utf8(title.substr(0, separator));
    artist = wide_to_utf8(title.substr(separator + 3));
}

void update_track_metadata(std::uint32_t process_id,
                           SourceTrack& track,
                           std::mutex& track_mutex) {
    WindowTitleContext context{process_id, {}};
    EnumWindows(find_qqmusic_window_proc,
                reinterpret_cast<LPARAM>(&context));
    if (context.title.empty()) return;

    std::string next_title;
    std::string next_artist;
    parse_qqmusic_title(context.title, next_title, next_artist);
    if (next_title.empty() || next_artist.empty()) return;

    std::lock_guard lock(track_mutex);
    if (track.title == next_title && track.artist == next_artist) return;
    track.title = std::move(next_title);
    track.artist = std::move(next_artist);
}

} // namespace

QQMusicSource::QQMusicSource(FeedPcmFn feed_pcm,
                             ClearPcmFn clear_pcm,
                             std::string process_name,
                             std::string executable_path)
    : feed_pcm_(std::move(feed_pcm)),
      clear_pcm_(std::move(clear_pcm)),
      process_name_(
          std::filesystem::path(process_name.empty() ? "QQMusic.exe"
                                                     : process_name)
              .filename()
              .wstring()),
      executable_path_(std::filesystem::path(executable_path)) {
    last_track_.uri = "qqmusic";
    last_track_.title = "QQ Music";
    last_track_.artist = "Live process capture";
    last_track_.album = "QQ Music";
}

QQMusicSource::~QQMusicSource() {
    shutdown();
}

void QQMusicSource::start() {
    // The source manager activates the worker when this source is selected.
}

void QQMusicSource::shutdown() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    capture_active_.store(false, std::memory_order_release);
}

bool QQMusicSource::is_connected() const {
    return capture_active_.load(std::memory_order_acquire);
}

bool QQMusicSource::is_playing() const {
    if (!capture_active_.load(std::memory_order_acquire)) return false;
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now - last_packet_time_)
               .count() < 1000;
}

SourceTrack QQMusicSource::last_track() const {
    std::lock_guard lock(track_mutex_);
    return last_track_;
}

void QQMusicSource::pause_at_audio_boundary() {
    send_media_key(VK_MEDIA_PLAY_PAUSE);
}

void QQMusicSource::resume_rewound(uint32_t) {
    send_media_key(VK_MEDIA_PLAY_PAUSE);
}

bool QQMusicSource::restart_current_track() {
    send_media_key(VK_MEDIA_PREV_TRACK);
    return true;
}

bool QQMusicSource::next_track() {
    send_media_key(VK_MEDIA_NEXT_TRACK);
    return true;
}

bool QQMusicSource::previous_track() {
    send_media_key(VK_MEDIA_PREV_TRACK);
    return true;
}

bool QQMusicSource::seek(uint32_t) {
    return false;
}

void QQMusicSource::on_activated() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { worker_thread_fn(); });
}

void QQMusicSource::on_deactivated() {
    shutdown();
}

void QQMusicSource::set_pcm_enabled(bool enabled) {
    pcm_enabled_.store(enabled, std::memory_order_release);
}

void QQMusicSource::set_volume_normalization(bool) {
    // Volume normalization is handled by the game radio bus.
}

void QQMusicSource::set_equalizer(bool,
                                  const std::array<float, 5>&) {
    // Equalization is handled by the game radio bus.
}

void QQMusicSource::send_media_key(std::uint16_t virtual_key) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtual_key;
    inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;

    SendInput(2, inputs, sizeof(INPUT));
}

bool QQMusicSource::find_qqmusic_process_id(
    std::uint32_t& process_id) const {
    std::vector<std::wstring> candidates = {
        process_name_,
        L"QQMusicExternal.exe",
        L"QQMusicService.exe"
    };

    for (auto& candidate : candidates) {
        candidate = lowercase(candidate);
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring current = lowercase(entry.szExeFile);
            for (const auto& candidate : candidates) {
                if (current == candidate) {
                    process_id = entry.th32ProcessID;
                    found = true;
                    break;
                }
            }
            if (found) break;
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void QQMusicSource::launch_qqmusic_if_needed() const {
    std::error_code error_code;
    const std::filesystem::path path =
        std::filesystem::absolute(executable_path_, error_code);
    if (error_code || path.empty() || !std::filesystem::exists(path)) {
        return;
    }

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    startup_info.wShowWindow = SW_MINIMIZE;

    PROCESS_INFORMATION process_info = {};
    const std::wstring command = path.wstring();

    if (CreateProcessW(command.c_str(),
                       nullptr,
                       nullptr,
                       nullptr,
                       FALSE,
                       0,
                       nullptr,
                       path.parent_path().c_str(),
                       &startup_info,
                       &process_info)) {
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        log::info("[qqmusic] Launched QQ Music");
    }
}

void QQMusicSource::worker_thread_fn() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running_.load(std::memory_order_acquire)) {
        std::uint32_t process_id = 0;
        if (!find_qqmusic_process_id(process_id)) {
            launch_qqmusic_if_needed();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (!capture_process_tree(process_id)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    CoUninitialize();
}

bool QQMusicSource::capture_process_tree(std::uint32_t process_id) {
    HANDLE completion_event =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completion_event) return false;

    ActivationHandler handler(completion_event);

    AUDIOCLIENT_ACTIVATION_PARAMS activation_params = {};
    activation_params.ActivationType =
        AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation_params.ProcessLoopbackParams.TargetProcessId = process_id;
    activation_params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation_property = {};
    activation_property.vt = VT_BLOB;
    activation_property.blob.cbSize = sizeof(activation_params);
    activation_property.blob.pBlobData =
        reinterpret_cast<BYTE*>(CoTaskMemAlloc(sizeof(activation_params)));
    if (!activation_property.blob.pBlobData) {
        CloseHandle(completion_event);
        return false;
    }
    std::memcpy(activation_property.blob.pBlobData,
                &activation_params,
                sizeof(activation_params));

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT result = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activation_property,
        &handler,
        operation.ReleaseAndGetAddressOf());

    CoTaskMemFree(activation_property.blob.pBlobData);
    if (FAILED(result)) {
        log::warn("[qqmusic] ActivateAudioInterfaceAsync failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    if (WaitForSingleObject(completion_event, 5000) != WAIT_OBJECT_0) {
        log::warn("[qqmusic] Audio activation timed out");
        CloseHandle(completion_event);
        return false;
    }

    HRESULT activation_result = E_FAIL;
    Microsoft::WRL::ComPtr<IUnknown> activated_interface;
    result = operation->GetActivateResult(
        &activation_result,
        activated_interface.ReleaseAndGetAddressOf());
    if (FAILED(result) || FAILED(activation_result)) {
        log::warn("[qqmusic] Audio activation failed: " +
                  std::to_string(FAILED(result) ? result : activation_result));
        CloseHandle(completion_event);
        return false;
    }

    Microsoft::WRL::ComPtr<IAudioClient> audio_client;
    result = activated_interface.As(&audio_client);
    if (FAILED(result)) {
        log::warn("[qqmusic] IAudioClient query failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    WAVEFORMATEX capture_format = {};
    capture_format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    capture_format.nChannels = FmodInject::kPcmChannels;
    capture_format.nSamplesPerSec = 48000;
    capture_format.wBitsPerSample = 32;
    capture_format.nBlockAlign =
        capture_format.nChannels * capture_format.wBitsPerSample / 8;
    capture_format.nAvgBytesPerSec =
        capture_format.nSamplesPerSec * capture_format.nBlockAlign;

    result = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0,
        0,
        &capture_format,
        nullptr);
    if (FAILED(result)) {
        log::warn("[qqmusic] IAudioClient::Initialize failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client;
    result = audio_client->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(capture_client.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        log::warn("[qqmusic] IAudioCaptureClient query failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    result = audio_client->SetEventHandle(completion_event);
    if (FAILED(result)) {
        log::warn("[qqmusic] SetEventHandle failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    result = audio_client->Start();
    if (FAILED(result)) {
        log::warn("[qqmusic] Audio capture start failed: " +
                  std::to_string(result));
        CloseHandle(completion_event);
        return false;
    }

    capture_active_.store(true, std::memory_order_release);
    log::info("[qqmusic] Process-loopback capture started: "
              "48000 Hz, stereo, float32");
    update_track_metadata(process_id, last_track_, track_mutex_);
    auto next_metadata_refresh = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(2);

    while (running_.load(std::memory_order_acquire)) {
        const DWORD wait_result =
            WaitForSingleObject(completion_event, 100);
        if (wait_result == WAIT_FAILED) break;
        if (wait_result == WAIT_TIMEOUT) continue;

        while (true) {
            UINT32 packet_length = 0;
            result = capture_client->GetNextPacketSize(&packet_length);
            if (FAILED(result) || packet_length == 0) break;

            BYTE* packet_data = nullptr;
            UINT32 frame_count = 0;
            DWORD flags = 0;
            result = capture_client->GetBuffer(
                &packet_data,
                &frame_count,
                &flags,
                nullptr,
                nullptr);
            if (FAILED(result)) break;

            if (frame_count > 0 &&
                !(flags & AUDCLNT_BUFFERFLAGS_SILENT) &&
                packet_data) {
                last_packet_time_ = std::chrono::steady_clock::now();

                if (pcm_enabled_.load(std::memory_order_acquire)) {
                    feed_pcm_(reinterpret_cast<const float*>(packet_data),
                              frame_count);
                }
            }

            capture_client->ReleaseBuffer(frame_count);
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= next_metadata_refresh) {
            update_track_metadata(process_id, last_track_, track_mutex_);
            next_metadata_refresh = now + std::chrono::seconds(2);
        }
    }

    audio_client->Stop();
    capture_active_.store(false, std::memory_order_release);
    CloseHandle(completion_event);
    log::info("[qqmusic] Process-loopback capture stopped");
    return true;
}

} // namespace bridge
