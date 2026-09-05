// In-process metadata injector — runs inside forzahorizon6.exe.
//
// Discovery is the same RTTI chain walk as injector.cpp but uses direct
// pointer dereference (with SEH) instead of ReadProcessMemory.  String
// reads/writes operate on the live process heap via direct casts.
// VirtualAlloc (not VirtualAllocEx) for SSO-to-heap promotion.

#include "injector_inproc.h"
#include "game_profile.h"
#include "log_file.h"

#include <Psapi.h>
#include <wincodec.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Windowscodecs.lib")

namespace bridge {

// ---- SEH-safe memory helpers (must be standalone functions, no C++ dtors) ----

static bool safe_memcpy(void* dst, const void* src, size_t size) {
    __try {
        memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_qword(uintptr_t addr, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_u8(uintptr_t addr, uint8_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint8_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_u32(uintptr_t addr, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const volatile uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_write_u32(uintptr_t addr, uint32_t value) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#if defined(SPOTIFY_RADIO_DIAG)
static bool safe_write_qword(uintptr_t addr, uintptr_t value) {
    __try {
        *reinterpret_cast<volatile uintptr_t*>(addr) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

static bool safe_write_bytes(uintptr_t addr, const void* src, size_t size) {
    __try {
        memcpy(reinterpret_cast<void*>(addr), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// In-place async GPU re-upload of an existing RGBA8 texture id via the
// engine's concurrent texture upload queue (sub_3522560). No allocation and no
// descriptor-row work; it enqueues an upload command drained by the engine
// resource thread, so it is safe from a worker thread. Destination geometry is
// read by the engine from the existing resource, so the rid must already be a
// created WxH RGBA8 texture. Fails closed unless the rid resolves to a live
// resource object. `texture_system` must be the dereferenced texture system
// (*(module+logo_texture_system_rva)). Build-pinned RVAs are passed in so a
// signature can replace them before production.
// In-place async RGBA8 re-upload given already-resolved pointers
// (the in-place fn, the resource table base, the refcount table base, and the
// dereferenced texture system). Safe to call from any thread (it enqueues onto
// the engine's concurrent texture queue). Fails closed unless rid resolves to a
// live resource object.
// Raw in-place upload of an arbitrary blob (already in the destination format,
// e.g. BC7) into an EXISTING resource id, with caller-supplied row pitch / slice
// / class. Used to upload BC7 into the engine-owned stock logo resource. SEH is
// isolated in this object-unwinding-free function (avoids C2712 at call sites).
static bool inplace_upload_blob_via(uintptr_t inplace_fn,
                                    uintptr_t texture_system, uint32_t rid,
                                    const void* pixels, uint64_t row_pitch,
                                    uint64_t slice, uint8_t class_byte) {
    if (!inplace_fn || !texture_system || !rid || !pixels || !row_pitch) {
        return false;
    }
    struct PayloadDesc {
        const void* px;
        uint64_t row_pitch;
        uint64_t slice;
    } payload{pixels, row_pitch, slice};
    uint32_t rid_arg = rid;
    uint32_t small_desc[6] = {1u, 1u, 0u, 0u, 1u, 0u};
    using Fn = void(__fastcall*)(uintptr_t, uint32_t*, uint32_t*, uint8_t, void*,
                                 uintptr_t, int);
    auto fn = reinterpret_cast<Fn>(inplace_fn);
    __try {
        fn(texture_system, &rid_arg, small_desc, class_byte, &payload, 0, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Fh5UploadSubresourceData {
    const void* p_data;
    intptr_t row_pitch;
    intptr_t slice_pitch;
};

static bool fh5_upload_blob_via_steam(uintptr_t upload_fn,
                                      uintptr_t upload_mgr,
                                      uint32_t upload_handle,
                                      const Fh5UploadSubresourceData& subres) {
    if (!upload_fn || !upload_mgr || !upload_handle || !subres.p_data ||
        !subres.row_pitch || !subres.slice_pitch) {
        return false;
    }
    uint64_t token[2] = {0, 0};
    uint32_t handle = upload_handle;
    using Fn = void(__fastcall*)(uintptr_t, uint64_t*, uint32_t*, uint64_t,
                                 uint32_t, const Fh5UploadSubresourceData*,
                                 uintptr_t, int);
    auto fn = reinterpret_cast<Fn>(upload_fn);
    __try {
        fn(upload_mgr, token, &handle, 0, 1, &subres, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#if defined(SPOTIFY_RADIO_DIAG)
// Rejected FH5 Game Pass staging-copy probe. Kept for future RE only; Release
// builds use the proven BC-aware queue upload path instead.
static bool fh5_upload_blob_via_gamepass_staging(uintptr_t upload_fn,
                                                 uintptr_t graphics_mgr,
                                                 uintptr_t state_fn,
                                                 uint32_t upload_handle,
                                                 const void* pixels,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t format) {
    if (!upload_fn || !graphics_mgr || !upload_handle || !pixels || !width ||
        !height || !format) {
        return false;
    }

    uintptr_t context = 0;
    if (!safe_read_qword(graphics_mgr + 0x48, context) ||
        context < 0x10000ull || context >= 0x0000800000000000ull) {
        return false;
    }

    struct Rect {
        uint32_t left;
        uint32_t top;
        uint32_t right;
        uint32_t bottom;
    } rect{0, 0, width, height};

    uint32_t handle = upload_handle;
    uint8_t barrier_state = 1;
    using StateFn = uint8_t(__fastcall*)();
    using Fn = void(__fastcall*)(uintptr_t, uint32_t*, const Rect*, const void*,
                                 int, uint32_t, uint32_t, uint8_t, uint8_t);
    auto fn = reinterpret_cast<Fn>(upload_fn);
    __try {
        if (state_fn) {
            barrier_state = reinterpret_cast<StateFn>(state_fn)();
        }
        fn(context, &handle, &rect, pixels, static_cast<int>(width), height,
           format, barrier_state, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

static bool fh5_upload_blob_via_gamepass_bcaware_queue(
    uintptr_t upload_fn,
    uintptr_t upload_queue,
    uint32_t upload_handle,
    const Fh5UploadSubresourceData& subres) {
    if (!upload_fn || !upload_queue || !upload_handle || !subres.p_data ||
        !subres.row_pitch || !subres.slice_pitch) {
        return false;
    }
    uint64_t token[2] = {0, 0};
    uint32_t handle = upload_handle;
    using Fn = uintptr_t(__fastcall*)(uintptr_t, uint64_t*, uint32_t*, int,
                                      int, const Fh5UploadSubresourceData*,
                                      uintptr_t, int);
    auto fn = reinterpret_cast<Fn>(upload_fn);
    __try {
        fn(upload_queue, token, &handle, 0, 1, &subres, 0, 0);
        return token[0] != 0 || token[1] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

enum class Fh5LogoUploadAbi {
    SteamLowLevel,
#if defined(SPOTIFY_RADIO_DIAG)
    GamePassStagingCopy,
    GamePassUiNodeSwap,
    GamePassStockSlotSwap,
#endif
    GamePassBcAwareQueue,
};

static bool fh5_upload_blob_via(const Fh5LogoUploadAbi abi,
                                uintptr_t upload_fn,
                                uintptr_t upload_mgr,
                                uintptr_t state_fn,
                                uint32_t upload_handle,
                                uint64_t render_handle,
                                const void* pixels,
                                uint64_t row_pitch,
                                uint64_t slice,
                                uint32_t width,
                                uint32_t height,
                                uint32_t format) {
    if (!upload_fn || !upload_mgr || !upload_handle || !pixels ||
        !row_pitch || !slice) {
        return false;
    }
    const Fh5UploadSubresourceData subres{
        pixels,
        static_cast<intptr_t>(row_pitch),
        static_cast<intptr_t>(slice),
    };
    (void)state_fn;
    (void)render_handle;
    (void)width;
    (void)height;
    (void)format;
    switch (abi) {
    case Fh5LogoUploadAbi::SteamLowLevel:
        return fh5_upload_blob_via_steam(upload_fn, upload_mgr, upload_handle,
                                         subres);
#if defined(SPOTIFY_RADIO_DIAG)
    case Fh5LogoUploadAbi::GamePassStagingCopy:
        return fh5_upload_blob_via_gamepass_staging(
            upload_fn, upload_mgr, state_fn, upload_handle, pixels, width,
            height, format);
    case Fh5LogoUploadAbi::GamePassUiNodeSwap:
        return false;
    case Fh5LogoUploadAbi::GamePassStockSlotSwap:
        return false;
#endif
    case Fh5LogoUploadAbi::GamePassBcAwareQueue:
        return fh5_upload_blob_via_gamepass_bcaware_queue(
            upload_fn, upload_mgr, upload_handle, subres);
    }
    return false;
}

static bool safe_read_i32(uintptr_t addr, int32_t& out) {
    __try {
        out = *reinterpret_cast<const volatile int32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_f32(uintptr_t addr, float& out) {
    __try {
        out = *reinterpret_cast<const volatile float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Scan a memory region for _Ref_count_obj2 instances.  Matches:
//   +0x00: target vtable pointer
//   +0x08/+0x0C: plausible MSVC shared_ptr use/weak counts
//   +0x10: a pointer within [mod_base, mod_base+mod_size) (inner vtable)
// Single SEH frame, raw reads, no per-call overhead.
static int scan_region_validated(uintptr_t start, uintptr_t stop,
                                 uintptr_t target_vt,
                                 uintptr_t mod_base, uintptr_t mod_size,
                                 uintptr_t* out, size_t max_out,
                                 size_t& count) {
    count = 0;
    __try {
        uintptr_t mod_end = mod_base + mod_size;
        for (uintptr_t p = start; p + 0x18 <= stop; p += 16) {
            if (*reinterpret_cast<const volatile uintptr_t*>(p) != target_vt)
                continue;
            // +0x08/+0x0C = _Uses/_Weaks. Real RadioStreamFmod refcounts
            // are small (usually 1/1). Reject transient image/full-scan false
            // positives before discovery can mark the injector ready.
            uint32_t uses = *reinterpret_cast<const volatile uint32_t*>(p + 0x08);
            uint32_t weaks = *reinterpret_cast<const volatile uint32_t*>(p + 0x0C);
            if (uses == 0 || weaks == 0 || uses > 128 || weaks > 128)
                continue;
            // +0x10 = embedded object vtable (RadioStreamFmod) — must be in module
            uintptr_t inner = *reinterpret_cast<const volatile uintptr_t*>(p + 0x10);
            if (inner >= mod_base && inner < mod_end) {
                if (count < max_out) out[count] = p;
                count++;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static std::string hex(uintptr_t v) {
    char buf[20];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

#if defined(SPOTIFY_RADIO_DIAG)
static constexpr bool kRuntimeLogoVerboseLogs = true;
#else
static constexpr bool kRuntimeLogoVerboseLogs = false;
#endif

struct LogoDescriptorRow {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t resource_id = 0;
    uint32_t code = 0;
    uint32_t flags = 0;
};

struct DecodedLogoHandle {
    uint32_t family = 0;
    uint64_t slot = 0;
    uintptr_t table_base = 0;
    uintptr_t stride = 0;
    uintptr_t entry = 0;
    std::array<uintptr_t, 4> qwords{};
};

struct LogoPathHolder {
    const char* label = "";
    uintptr_t holder = 0;
    uintptr_t path_ref = 0;
    uintptr_t path = 0;
    uintptr_t row_addr = 0;
    LogoDescriptorRow row{};
};

struct LogoRowPair {
    LogoPathHolder streamer{};
    LogoPathHolder pulse{};
};

struct LogoCacheRowCandidate {
    const char* label = "";
    uintptr_t row_addr = 0;
    LogoDescriptorRow row{};
};

struct LogoCacheRowPair {
    LogoCacheRowCandidate streamer{};
    LogoCacheRowCandidate pulse{};
};

struct LogoCacheHit {
    const char* source = "";
    uintptr_t request = 0;
    uint32_t type = 0;
    std::string key;
    uintptr_t wrapper = 0;
    uintptr_t code = 0;
    uintptr_t small_handle_pair = 0;
    std::array<uintptr_t, 4> wrapper_qwords{};
    uintptr_t resource_head = 0;
    struct ResourceEntry {
        uintptr_t address = 0;
        uintptr_t vtable = 0;
        uint32_t code = 0;
        uint32_t resource_id = 0;
        uint32_t size_or_flags = 0;
    };
    std::array<ResourceEntry, 4> resources{};
};

struct Fh5LogoUploadSlot {
    uintptr_t resource_offset = 0;
    uintptr_t wrapper = 0;
    uintptr_t render_wrapper = 0;
    uint64_t render_handle = 0;
    uint32_t upload_handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t render_format = 0;
    uint64_t refcount = 0;
    uint64_t render_refcount = 0;
    uint64_t loaded_revision = 0;
    uint64_t uploaded_revision = ~0ull;
    uint32_t loaded_width = 0;
    uint32_t loaded_height = 0;
    uint32_t successful_uploads = 0;
    std::vector<uint8_t> bc7;
};

struct Fh5LogoTarget {
    uintptr_t entry = 0;
    uintptr_t path = 0;
    uintptr_t resource = 0;
    uintptr_t wrapper = 0;
    uintptr_t render_wrapper = 0;
    uint64_t render_handle = 0;
    uint32_t upload_handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t render_format = 0;
    uint64_t refcount = 0;
    uint64_t render_refcount = 0;
    uintptr_t ui_node = 0;
    uintptr_t ui_node_wrapper = 0;
    std::string key;
    uint64_t loaded_revision = 0;
    uint64_t uploaded_revision = ~0ull;
    uint32_t loaded_width = 0;
    uint32_t loaded_height = 0;
    std::vector<uint8_t> bc7;
    std::vector<Fh5LogoUploadSlot> slots;
    int restore_burst = 0;
};

struct RuntimeLogoSettings {
    std::filesystem::path logo_dir;
    bool album_art_enabled = true;
    bool custom_graphic_enabled = false;
    std::string spotify_variant = "white";
    std::string active_source = "spotify";
    std::string artwork_key;
    std::vector<uint8_t> artwork_bytes;
    bool artwork_loading = false;
    uint64_t revision = 0;
};

struct RuntimeLogoImage {
    std::vector<uint32_t> pixels;
    std::string key;
    std::string label;
};

static constexpr uint32_t kUiRenderFullLogoWidth = 392;
static constexpr uint32_t kUiRenderFullLogoHeight = 208;
static constexpr uint32_t kUiRenderFullLogoRgbaBytes =
    kUiRenderFullLogoWidth * kUiRenderFullLogoHeight * 4u;
static constexpr uint32_t kUiRenderFullLogoBc7BlockWidth =
    (kUiRenderFullLogoWidth + 3u) / 4u;
static constexpr uint32_t kUiRenderFullLogoBc7BlockHeight =
    (kUiRenderFullLogoHeight + 3u) / 4u;
static constexpr uint32_t kUiRenderFullLogoBc7RowPitch =
    kUiRenderFullLogoBc7BlockWidth * 16u;
static constexpr uint32_t kUiRenderFullLogoBc7Bytes =
    kUiRenderFullLogoBc7RowPitch * kUiRenderFullLogoBc7BlockHeight;
static constexpr uint32_t kUiRenderFullLogoUploadBytes =
    kUiRenderFullLogoRgbaBytes;
static constexpr uint32_t kUiRenderNoRgbaClass = 0xFFFFFFFFu;
static bool read_png_bgra_wic(const std::filesystem::path& path,
                              uint32_t& width,
                              uint32_t& height,
                              std::vector<uint32_t>& pixels) {
    width = 0;
    height = 0;
    pixels.clear();

    HRESULT init_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needs_uninit = SUCCEEDED(init_hr);
    if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) return false;

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr,
                                                GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand,
                                                &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width > 0 && height > 0 &&
        width <= 4096 && height <= 4096) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        pixels.resize(static_cast<size_t>(width) * height);
        hr = converter->CopyPixels(nullptr, width * 4,
                                   static_cast<UINT>(pixels.size() * 4),
                                   reinterpret_cast<BYTE*>(pixels.data()));
        ok = SUCCEEDED(hr);
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (needs_uninit) ::CoUninitialize();
    if (!ok) {
        width = 0;
        height = 0;
        pixels.clear();
    }
    return ok;
}

static bool read_image_bgra_wic_memory(const std::vector<uint8_t>& data,
                                       uint32_t& width,
                                       uint32_t& height,
                                       std::vector<uint32_t>& pixels) {
    width = 0;
    height = 0;
    pixels.clear();
    if (data.empty() || data.size() > 4 * 1024 * 1024 ||
        data.size() > std::numeric_limits<DWORD>::max()) {
        return false;
    }

    HRESULT init_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needs_uninit = SUCCEEDED(init_hr);
    if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = false;

    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromMemory(
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(data.data())),
            static_cast<DWORD>(data.size()));
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    }
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width > 0 && height > 0 &&
        width <= 4096 && height <= 4096) {
        hr = factory->CreateFormatConverter(&converter);
    }
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        pixels.resize(static_cast<size_t>(width) * height);
        hr = converter->CopyPixels(nullptr, width * 4,
                                   static_cast<UINT>(pixels.size() * 4),
                                   reinterpret_cast<BYTE*>(pixels.data()));
        ok = SUCCEEDED(hr);
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (needs_uninit) ::CoUninitialize();
    if (!ok) {
        width = 0;
        height = 0;
        pixels.clear();
    }
    return ok;
}

static uint64_t fnv1a_64(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t blend_over(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0xFFu) return src;
    if (sa == 0) return dst;
    uint32_t inv = 255u - sa;
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8) & 0xFFu;
    uint32_t sb = src & 0xFFu;
    uint32_t da = (dst >> 24) & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;
    uint32_t out_a = sa + (da * inv + 127u) / 255u;
    uint32_t out_r = (sr * sa + dr * inv + 127u) / 255u;
    uint32_t out_g = (sg * sa + dg * inv + 127u) / 255u;
    uint32_t out_b = (sb * sa + db * inv + 127u) / 255u;
    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static void convert_argb_to_rgba_bytes(std::vector<uint32_t>& pixels) {
    for (uint32_t& px : pixels) {
        px = (px & 0xFF00FF00u) |
             ((px & 0x00FF0000u) >> 16) |
             ((px & 0x000000FFu) << 16);
    }
}

static void copy_rgba_upload_bytes(
    const std::array<uint32_t,
                     kUiRenderFullLogoWidth * kUiRenderFullLogoHeight>& rgba,
    std::array<uint8_t, kUiRenderFullLogoUploadBytes>& out) {
    std::memcpy(out.data(), rgba.data(), out.size());
}

// ---------------------------------------------------------------------------
// Self-contained BC7 encoder (mode 6 only). No external texconv dependency.
// Mode 6 = single subset, full RGBA + alpha, 7-bit endpoints + per-endpoint
// p-bit, 4-bit indices. Block layout (128-bit, LSB-first): mode(7)=0x40, then
// R0 R1 G0 G1 B0 B1 A0 A1 (7 bits each = 56), P0 P1 (1 bit each), then 16 color
// indices (4 bits each; the anchor index[0] is 3 bits, MSB implicit 0).
// Input channel order is byte0=R,1=G,2=B,3=A (RGBA8, as composed by
// prepare_runtime_logo_upload_bytes). Quality is sufficient for the radio
// logo (flat colors / edges / alpha); fail-closed if anything is off.
static const int kBc7M6Weights[16] =
    {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static void bc7_m6_put(uint8_t* blk, int& pos, uint32_t val, int bits) {
    for (int i = 0; i < bits; ++i) {
        if (val & (1u << i)) blk[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7));
        ++pos;
    }
}

static void bc7_encode_block_mode6(const uint8_t px[16][4], uint8_t blk[16]) {
    for (int i = 0; i < 16; ++i) blk[i] = 0;
    int lo[4] = {255, 255, 255, 255};
    int hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        for (int c = 0; c < 4; ++c) {
            const int v = px[i][c];
            if (v < lo[c]) lo[c] = v;
            if (v > hi[c]) hi[c] = v;
        }
    }
    // Fit one endpoint (4 channels) to a shared p-bit + 7-bit quant.
    auto fit_ep = [](const int e[4], int& out_pbit, int q[4]) {
        long long best = 1LL << 60;
        int bp = 0, bq[4] = {0, 0, 0, 0};
        for (int p = 0; p < 2; ++p) {
            long long err = 0;
            int qq[4];
            for (int c = 0; c < 4; ++c) {
                int v = e[c] - p;
                if (v < 0) v = 0;
                int t = (v + 1) >> 1;
                if (t > 127) t = 127;
                qq[c] = t;
                int recon = (t << 1) | p;
                int d = recon - e[c];
                err += static_cast<long long>(d) * d;
            }
            if (err < best) {
                best = err;
                bp = p;
                for (int c = 0; c < 4; ++c) bq[c] = qq[c];
            }
        }
        out_pbit = bp;
        for (int c = 0; c < 4; ++c) q[c] = bq[c];
    };
    // Float endpoints, refined by least squares. Start from the bounding box.
    double fA[4], fB[4];
    for (int c = 0; c < 4; ++c) { fA[c] = lo[c]; fB[c] = hi[c]; }

    int pA = 0, pB = 0, qA[4], qB[4];
    int eA[4], eB[4];
    uint8_t idx[16];
    auto requantize = [&]() {
        int rA[4], rB[4];
        for (int c = 0; c < 4; ++c) {
            rA[c] = (int)(fA[c] + 0.5); if (rA[c] < 0) rA[c] = 0; if (rA[c] > 255) rA[c] = 255;
            rB[c] = (int)(fB[c] + 0.5); if (rB[c] < 0) rB[c] = 0; if (rB[c] > 255) rB[c] = 255;
        }
        fit_ep(rA, pA, qA);
        fit_ep(rB, pB, qB);
        for (int c = 0; c < 4; ++c) {
            eA[c] = (qA[c] << 1) | pA;
            eB[c] = (qB[c] << 1) | pB;
        }
    };
    auto assign_indices = [&]() {
        int dir[4];
        for (int c = 0; c < 4; ++c) dir[c] = eB[c] - eA[c];
        for (int i = 0; i < 16; ++i) {
            int best = 0;
            long long bestd = 1LL << 60;
            for (int w = 0; w < 16; ++w) {
                long long d = 0;
                for (int c = 0; c < 4; ++c) {
                    int interp = eA[c] + ((dir[c] * kBc7M6Weights[w] + 32) >> 6);
                    int e = px[i][c] - interp;
                    d += static_cast<long long>(e) * e;
                }
                if (d < bestd) { bestd = d; best = w; }
            }
            idx[i] = static_cast<uint8_t>(best);
        }
    };
    // Iterate: quantize -> index -> least-squares refit endpoints.
    for (int iter = 0; iter < 3; ++iter) {
        requantize();
        assign_indices();
        // LSQ: minimize sum_i ||px_i - (fA*(1-t)+fB*t)||^2 ; t = weight/64.
        double s0 = 0, s1 = 0, s2 = 0, b0[4] = {0,0,0,0}, b1[4] = {0,0,0,0};
        for (int i = 0; i < 16; ++i) {
            double t = kBc7M6Weights[idx[i]] / 64.0;
            double u = 1.0 - t;
            s0 += u * u; s1 += u * t; s2 += t * t;
            for (int c = 0; c < 4; ++c) {
                b0[c] += u * px[i][c];
                b1[c] += t * px[i][c];
            }
        }
        double det = s0 * s2 - s1 * s1;
        if (det > 1e-6 || det < -1e-6) {
            for (int c = 0; c < 4; ++c) {
                fA[c] = (s2 * b0[c] - s1 * b1[c]) / det;
                fB[c] = (s0 * b1[c] - s1 * b0[c]) / det;
            }
        }
    }
    requantize();
    assign_indices();
    // Anchor index[0] must have its high bit clear (3-bit field). If not, swap
    // endpoints and invert all indices.
    if (idx[0] & 8) {
        std::swap(pA, pB);
        for (int c = 0; c < 4; ++c) std::swap(qA[c], qB[c]);
        for (int i = 0; i < 16; ++i) idx[i] = static_cast<uint8_t>(15 - idx[i]);
    }
    int pos = 0;
    bc7_m6_put(blk, pos, 0x40u, 7);  // mode 6 (six 0s then a 1)
    for (int c = 0; c < 4; ++c) {
        bc7_m6_put(blk, pos, static_cast<uint32_t>(qA[c]), 7);
        bc7_m6_put(blk, pos, static_cast<uint32_t>(qB[c]), 7);
    }
    bc7_m6_put(blk, pos, static_cast<uint32_t>(pA), 1);
    bc7_m6_put(blk, pos, static_cast<uint32_t>(pB), 1);
    bc7_m6_put(blk, pos, idx[0], 3);
    for (int i = 1; i < 16; ++i) bc7_m6_put(blk, pos, idx[i], 4);
}

static bool bc7_encode_image_mode6(const uint32_t* rgba, uint32_t w, uint32_t h,
                                   uint8_t* out, size_t out_size) {
    if (!rgba || !out || (w & 3u) || (h & 3u)) return false;
    const uint32_t bw = w / 4, bh = h / 4;
    if (out_size < static_cast<size_t>(bw) * bh * 16u) return false;
    size_t o = 0;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t px[16][4];
            for (int yy = 0; yy < 4; ++yy) {
                for (int xx = 0; xx < 4; ++xx) {
                    const uint32_t p =
                        rgba[static_cast<size_t>(by * 4 + yy) * w + bx * 4 + xx];
                    const int k = yy * 4 + xx;
                    px[k][0] = static_cast<uint8_t>(p & 0xFFu);          // R
                    px[k][1] = static_cast<uint8_t>((p >> 8) & 0xFFu);   // G
                    px[k][2] = static_cast<uint8_t>((p >> 16) & 0xFFu);  // B
                    px[k][3] = static_cast<uint8_t>((p >> 24) & 0xFFu);  // A
                }
            }
            bc7_encode_block_mode6(px, out + o);
            o += 16;
        }
    }
    return true;
}

static bool prepare_runtime_logo_upload_bytes(
    uint32_t texture_class,
    const std::array<uint32_t,
                     kUiRenderFullLogoWidth * kUiRenderFullLogoHeight>& rgba,
    const std::filesystem::path& logo_dir,
    std::array<uint8_t, kUiRenderFullLogoUploadBytes>& out,
    std::string& out_encoding) {
    if (texture_class != 0x62u) {
        copy_rgba_upload_bytes(rgba, out);
        out_encoding = "rgba";
        return true;
    }

    out.fill(0);
    (void)logo_dir;  // no external tooling needed; encoder is in-process

    // Self-contained in-process BC7 (mode 6, LSQ-refined). No texconv.exe.
    if (bc7_encode_image_mode6(rgba.data(), kUiRenderFullLogoWidth,
                               kUiRenderFullLogoHeight, out.data(),
                               out.size())) {
        out_encoding = "bc7";
        return true;
    }

    // Encoder only fails on bad dimensions (fixed 392x208), so this is a
    // defensive path: fall back to raw RGBA (the stock BC7 resource will then
    // fail closed at validation rather than upload mismatched bytes).
    copy_rgba_upload_bytes(rgba, out);
    out_encoding = "rgba-fallback-bc7-failed";
    return false;
}

static std::vector<uint32_t> compose_logo_canvas(
    const std::vector<uint32_t>& source,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t canvas_width,
    uint32_t canvas_height,
    uint32_t box_size,
    uint32_t left_padding) {
    std::vector<uint32_t> canvas(static_cast<size_t>(canvas_width) *
                                 canvas_height, 0x00000000u);
    if (source.empty() || source_width == 0 || source_height == 0 ||
        canvas_width == 0 || canvas_height == 0 || box_size == 0) {
        return canvas;
    }

    double scale = std::min(static_cast<double>(box_size) / source_width,
                            static_cast<double>(canvas_height) / source_height);
    uint32_t dst_w = std::max(1u, static_cast<uint32_t>(
                                      std::round(source_width * scale)));
    uint32_t dst_h = std::max(1u, static_cast<uint32_t>(
                                      std::round(source_height * scale)));
    dst_w = std::min(dst_w, box_size);
    dst_h = std::min(dst_h, canvas_height);
    uint32_t dst_x = std::min(left_padding, canvas_width - 1);
    if (dst_x + dst_w > canvas_width) dst_w = canvas_width - dst_x;
    uint32_t dst_y = (canvas_height > dst_h) ? (canvas_height - dst_h) / 2 : 0;

    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t sy = std::min(source_height - 1,
                               static_cast<uint32_t>(
                                   (static_cast<uint64_t>(y) * source_height) /
                                   dst_h));
        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t sx = std::min(source_width - 1,
                                   static_cast<uint32_t>(
                                       (static_cast<uint64_t>(x) * source_width) /
                                       dst_w));
            size_t di = static_cast<size_t>(dst_y + y) * canvas_width +
                        (dst_x + x);
            canvas[di] = blend_over(canvas[di],
                                    source[static_cast<size_t>(sy) *
                                           source_width + sx]);
        }
    }
    return canvas;
}

static std::vector<uint32_t> compose_album_art_canvas(
    const std::vector<uint32_t>& source,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t canvas_width,
    uint32_t canvas_height,
    uint32_t square_size,
    uint32_t left_padding,
    uint32_t border_px) {
    constexpr uint32_t kAlbumBorderColor = 0x8AD3D3D3u;
    std::vector<uint32_t> canvas(static_cast<size_t>(canvas_width) *
                                 canvas_height, 0x00000000u);
    if (canvas_width == 0 || canvas_height == 0 || square_size == 0) {
        return canvas;
    }

    uint32_t square_x = std::min(left_padding, canvas_width - 1);
    uint32_t square_w = std::min(square_size, canvas_width - square_x);
    uint32_t square_h = std::min(square_size, canvas_height);
    for (uint32_t y = 0; y < square_h; ++y) {
        for (uint32_t x = 0; x < square_w; ++x) {
            canvas[static_cast<size_t>(y) * canvas_width + square_x + x] =
                blend_over(0x00000000u, kAlbumBorderColor);
        }
    }

    if (source.empty() || source_width == 0 || source_height == 0 ||
        square_w <= border_px * 2 || square_h <= border_px * 2) {
        return canvas;
    }

    uint32_t box_w = square_w - border_px * 2;
    uint32_t box_h = square_h - border_px * 2;
    double scale = std::min(static_cast<double>(box_w) / source_width,
                            static_cast<double>(box_h) / source_height);
    uint32_t dst_w = std::max(1u, static_cast<uint32_t>(
                                      std::round(source_width * scale)));
    uint32_t dst_h = std::max(1u, static_cast<uint32_t>(
                                      std::round(source_height * scale)));
    dst_w = std::min(dst_w, box_w);
    dst_h = std::min(dst_h, box_h);
    uint32_t dst_x = square_x + border_px + (box_w - dst_w) / 2;
    uint32_t dst_y = border_px + (box_h - dst_h) / 2;

    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t sy = std::min(source_height - 1,
                               static_cast<uint32_t>(
                                   (static_cast<uint64_t>(y) * source_height) /
                                   dst_h));
        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t sx = std::min(source_width - 1,
                                   static_cast<uint32_t>(
                                       (static_cast<uint64_t>(x) * source_width) /
                                       dst_w));
            size_t di = static_cast<size_t>(dst_y + y) * canvas_width +
                        (dst_x + x);
            canvas[di] = blend_over(canvas[di],
                                    source[static_cast<size_t>(sy) *
                                           source_width + sx]);
        }
    }
    return canvas;
}

static std::vector<uint32_t> generated_source_icon(std::string_view source,
                                                   std::string_view variant,
                                                   uint32_t size) {
    std::vector<uint32_t> pixels(static_cast<size_t>(size) * size,
                                 0x00000000u);
    auto set = [&](uint32_t x, uint32_t y, uint32_t color) {
        if (x < size && y < size) pixels[static_cast<size_t>(y) * size + x] = color;
    };
    auto fill_rect = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                         uint32_t color) {
        for (uint32_t y = y0; y < y1 && y < size; ++y) {
            for (uint32_t x = x0; x < x1 && x < size; ++x) set(x, y, color);
        }
    };
    auto fill_circle = [&](int cx, int cy, int r, uint32_t color) {
        int rr = r * r;
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                int dx = x - cx;
                int dy = y - cy;
                if (dx * dx + dy * dy <= rr && x >= 0 && y >= 0) {
                    set(static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                        color);
                }
            }
        }
    };

    if (source == "airplay") {
        fill_circle(104, 92, 62, 0xFFFFFFFFu);
        fill_rect(55, 48, 153, 112, 0xFF2D7DFFu);
        for (uint32_t y = 112; y < 166; ++y) {
            uint32_t half = (y - 112) * 58 / 54;
            fill_rect(104 - half, y, 104 + half + 1, y + 1, 0xFF2D7DFFu);
        }
    } else if (source == "local") {
        fill_rect(28, 68, 72, 88, 0xFFFFD15Cu);
        fill_rect(28, 82, 180, 158, 0xFFFFB02Eu);
        fill_rect(42, 96, 168, 146, 0xFFFFC44Au);
    } else {
        uint32_t bg = (variant == "color") ? 0xFF1DB954u : 0xFFFFFFFFu;
        uint32_t fg = (variant == "color") ? 0xFF101010u : 0xFF101010u;
        fill_circle(104, 104, 82, bg);
        for (uint32_t y = 78; y < 90; ++y) fill_rect(55, y, 154, y + 1, fg);
        for (uint32_t y = 103; y < 113; ++y) fill_rect(62, y, 146, y + 1, fg);
        for (uint32_t y = 128; y < 136; ++y) fill_rect(70, y, 134, y + 1, fg);
    }
    return pixels;
}

static RuntimeLogoImage compose_runtime_logo_image(
    const RuntimeLogoSettings& settings) {
    constexpr uint32_t kCustomWidth = 392;
    constexpr uint32_t kCustomHeight = 208;
    constexpr uint32_t kCustomSquareSize = 208;
    constexpr uint32_t kCustomArtworkPaddingLeft = 0;
    constexpr uint32_t kAlbumArtworkBorderPx = 8;

    RuntimeLogoImage image{};
    image.key = "spotify_radio_runtime_logo_default";
    image.label = "default:" + settings.active_source;

    std::vector<uint32_t> source_pixels;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    bool loaded_file = false;
    bool album_art_logo = false;
    bool album_placeholder = false;
    bool full_canvas = false;

    auto try_logo_file = [&](const std::filesystem::path& path) {
        std::vector<uint32_t> decoded;
        uint32_t w = 0;
        uint32_t h = 0;
        if (read_png_bgra_wic(path, w, h, decoded)) {
            source_pixels = std::move(decoded);
            source_width = w;
            source_height = h;
            loaded_file = true;
            return true;
        }
        return false;
    };

    if (settings.active_source == "vanilla") {
        if (try_logo_file(settings.logo_dir / "vanilla.png")) {
            image.key = "spotify_radio_logo_vanilla";
            image.label = "vanilla";
        }
    }

    if (!loaded_file && settings.album_art_enabled &&
        !settings.artwork_bytes.empty()) {
        std::vector<uint32_t> decoded;
        uint32_t w = 0;
        uint32_t h = 0;
        if (read_image_bgra_wic_memory(settings.artwork_bytes, w, h, decoded)) {
            source_pixels = std::move(decoded);
            source_width = w;
            source_height = h;
            loaded_file = true;
            album_art_logo = true;
            uint64_t art_hash = fnv1a_64(settings.artwork_bytes.data(),
                                         settings.artwork_bytes.size());
            image.key = "spotify_radio_logo_album_" +
                        std::to_string(art_hash);
            image.label = "album:" + settings.active_source;
        }
    }

    if (!loaded_file && settings.album_art_enabled &&
        settings.artwork_loading) {
        loaded_file = true;
        album_art_logo = true;
        album_placeholder = true;
        image.key = "spotify_radio_logo_album_pending_" +
                    std::to_string(fnv1a_64(settings.artwork_key.data(),
                                            settings.artwork_key.size()));
        image.label = "album-pending:" + settings.active_source;
    }

    if (!loaded_file && settings.custom_graphic_enabled) {
        auto custom_path = settings.logo_dir / "custom.png";
        if (try_logo_file(custom_path)) {
            image.key = "spotify_radio_logo_custom";
            image.label = "custom";
        }
    }

    if (!loaded_file) {
        std::filesystem::path preferred;
        std::filesystem::path fallback;
        if (settings.active_source == "airplay") {
            preferred = settings.logo_dir / "airplay.png";
            fallback = settings.logo_dir / "spotify-white.png";
        } else if (settings.active_source == "local") {
            preferred = settings.logo_dir / "local-files.png";
            fallback = settings.logo_dir / "spotify-white.png";
        } else if (settings.active_source == "radio") {
            preferred = settings.logo_dir / "online-radio.png";
            fallback = settings.logo_dir / "spotify-white.png";
        } else if (settings.spotify_variant == "color") {
            preferred = settings.logo_dir / "spotify-color.png";
            fallback = settings.logo_dir / "spotify-white.png";
        } else {
            preferred = settings.logo_dir / "spotify-white.png";
            fallback.clear();
        }
        if (!preferred.empty() && try_logo_file(preferred)) {
            image.key = "spotify_radio_logo_" + settings.active_source +
                        "_" + preferred.filename().string() + "_" +
                        settings.spotify_variant;
            image.label = "file:" + preferred.filename().string();
        } else if (!fallback.empty() && try_logo_file(fallback)) {
            image.key = "spotify_radio_logo_spotify_white_fallback";
            image.label = "file:" + fallback.filename().string();
        }
    }

    if (!loaded_file) {
        source_width = kCustomSquareSize;
        source_height = kCustomSquareSize;
        source_pixels = generated_source_icon(settings.active_source,
                                              settings.spotify_variant,
                                              kCustomSquareSize);
        image.key = "spotify_radio_logo_generated_" +
                    settings.active_source + "_" + settings.spotify_variant;
        image.label = "generated:" + settings.active_source;
    }

    if (loaded_file && !album_art_logo &&
        source_width == kCustomWidth && source_height == kCustomHeight) {
        full_canvas = true;
    }

    if (full_canvas) {
        image.pixels = compose_logo_canvas(source_pixels, source_width,
                                           source_height, kCustomWidth,
                                           kCustomHeight, kCustomWidth, 0);
    } else if (album_art_logo) {
        image.pixels = compose_album_art_canvas(
            source_pixels, source_width, source_height, kCustomWidth,
            kCustomHeight, kCustomSquareSize, kCustomArtworkPaddingLeft,
            kAlbumArtworkBorderPx);
    } else {
        image.pixels = compose_logo_canvas(source_pixels, source_width,
                                           source_height, kCustomWidth,
                                           kCustomHeight, kCustomSquareSize,
                                           kCustomArtworkPaddingLeft);
    }
    convert_argb_to_rgba_bytes(image.pixels);
    (void)album_placeholder;
    return image;
}

static bool encode_runtime_logo_bc7_scaled(const RuntimeLogoImage& image,
                                           uint32_t target_w,
                                           uint32_t target_h,
                                           std::vector<uint8_t>& out) {
    out.clear();
    if ((target_w & 3u) || (target_h & 3u) ||
        target_w < 16u || target_h < 16u ||
        target_w > 4096u || target_h > 4096u) {
        return false;
    }
    const size_t hi_count =
        static_cast<size_t>(kUiRenderFullLogoWidth) *
        kUiRenderFullLogoHeight;
    if (image.pixels.size() < hi_count) return false;

    std::vector<uint32_t> scaled;
    const uint32_t* enc_px = image.pixels.data();
    if (target_w != kUiRenderFullLogoWidth ||
        target_h != kUiRenderFullLogoHeight) {
        const uint32_t sw = kUiRenderFullLogoWidth;
        const uint32_t sh = kUiRenderFullLogoHeight;
        scaled.resize(static_cast<size_t>(target_w) * target_h);
        for (uint32_t y = 0; y < target_h; ++y) {
            uint32_t sy0 = static_cast<uint32_t>(
                static_cast<uint64_t>(y) * sh / target_h);
            uint32_t sy1 = static_cast<uint32_t>(
                static_cast<uint64_t>(y + 1u) * sh / target_h);
            if (sy1 <= sy0) sy1 = sy0 + 1u;
            if (sy1 > sh) sy1 = sh;
            for (uint32_t x = 0; x < target_w; ++x) {
                uint32_t sx0 = static_cast<uint32_t>(
                    static_cast<uint64_t>(x) * sw / target_w);
                uint32_t sx1 = static_cast<uint32_t>(
                    static_cast<uint64_t>(x + 1u) * sw / target_w);
                if (sx1 <= sx0) sx1 = sx0 + 1u;
                if (sx1 > sw) sx1 = sw;
                uint32_t acc[4] = {0, 0, 0, 0};
                uint32_t n = 0;
                for (uint32_t sy = sy0; sy < sy1; ++sy) {
                    const uint32_t* row =
                        image.pixels.data() + static_cast<size_t>(sy) * sw;
                    for (uint32_t sx = sx0; sx < sx1; ++sx) {
                        const uint32_t p = row[sx];
                        acc[0] += p & 0xFFu;
                        acc[1] += (p >> 8) & 0xFFu;
                        acc[2] += (p >> 16) & 0xFFu;
                        acc[3] += (p >> 24) & 0xFFu;
                        ++n;
                    }
                }
                if (n == 0u) n = 1u;
                scaled[static_cast<size_t>(y) * target_w + x] =
                    (acc[0] / n) |
                    ((acc[1] / n) << 8) |
                    ((acc[2] / n) << 16) |
                    ((acc[3] / n) << 24);
            }
        }
        enc_px = scaled.data();
    }

    const uint32_t bc7_pitch = ((target_w + 3u) / 4u) * 16u;
    const uint32_t bc7_slice = bc7_pitch * ((target_h + 3u) / 4u);
    out.resize(bc7_slice);
    if (!bc7_encode_image_mode6(enc_px, target_w, target_h,
                                out.data(), out.size())) {
        out.clear();
        return false;
    }
    return true;
}

static bool is_logo_row(const LogoDescriptorRow& row) {
    if (row.format != 0x62) return false;  // BC7_UNORM in the radio-logo rows.
    if ((row.code & 0xFFu) != 0x02u) return false;
    if (row.width < 128 || row.width > 1024) return false;
    if (row.height < 64 || row.height > 512) return false;
    return true;
}

static bool read_logo_row(uintptr_t row_addr, LogoDescriptorRow& row) {
    return safe_read_u32(row_addr + 0x00, row.width) &&
           safe_read_u32(row_addr + 0x04, row.height) &&
           safe_read_u32(row_addr + 0x08, row.format) &&
           safe_read_u32(row_addr + 0x0C, row.resource_id) &&
           safe_read_u32(row_addr + 0x10, row.code) &&
           safe_read_u32(row_addr + 0x14, row.flags);
}

static int scan_region_for_bytes(uintptr_t start, uintptr_t stop,
                                 const char* needle, size_t needle_len,
                                 uintptr_t* out, size_t max_out,
                                 size_t& count) {
    if (!needle || needle_len == 0 || start >= stop || stop - start < needle_len) {
        return 0;
    }
    constexpr size_t kChunk = 64 * 1024;
    std::vector<char> buffer(kChunk + needle_len);
    size_t carry = 0;
    uintptr_t cursor = start;

    while (cursor < stop) {
        size_t to_copy = std::min(kChunk, static_cast<size_t>(stop - cursor));
        if (!safe_memcpy(buffer.data() + carry,
                         reinterpret_cast<const void*>(cursor),
                         to_copy)) {
            carry = 0;
            cursor += to_copy;
            continue;
        }

        size_t span = carry + to_copy;
        uintptr_t span_base = cursor - carry;
        for (size_t i = 0; i + needle_len <= span; ++i) {
            if (buffer[i] != needle[0]) continue;
            bool match = true;
            for (size_t j = 1; j < needle_len; ++j) {
                if (buffer[i + j] != needle[j]) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;
            if (count < max_out) out[count] = span_base + i;
            count++;
            if (count >= max_out) return 0;
        }

        carry = std::min(needle_len - 1, span);
        if (carry > 0) {
            memmove(buffer.data(), buffer.data() + span - carry, carry);
        }
        cursor += to_copy;
    }
    return 0;
}

static int scan_region_for_two_byte_patterns(uintptr_t start, uintptr_t stop,
                                             const char* needle_a, size_t len_a,
                                             uintptr_t* out_a, size_t max_a,
                                             size_t& count_a,
                                             const char* needle_b, size_t len_b,
                                             uintptr_t* out_b, size_t max_b,
                                             size_t& count_b) {
    if (!needle_a || !needle_b || len_a == 0 || len_b == 0 || start >= stop) {
        return 0;
    }
    const size_t max_len = std::max(len_a, len_b);
    if (stop - start < max_len) return 0;

    constexpr size_t kChunk = 64 * 1024;
    std::vector<char> buffer(kChunk + max_len);
    size_t carry = 0;
    uintptr_t cursor = start;

    while (cursor < stop && (count_a < max_a || count_b < max_b)) {
        size_t to_copy = std::min(kChunk, static_cast<size_t>(stop - cursor));
        if (!safe_memcpy(buffer.data() + carry,
                         reinterpret_cast<const void*>(cursor),
                         to_copy)) {
            carry = 0;
            cursor += to_copy;
            continue;
        }

        size_t span = carry + to_copy;
        uintptr_t span_base = cursor - carry;
        for (size_t i = 0; i < span; ++i) {
            if (count_a < max_a && i + len_a <= span &&
                buffer[i] == needle_a[0] &&
                memcmp(buffer.data() + i, needle_a, len_a) == 0) {
                out_a[count_a++] = span_base + i;
            }
            if (count_b < max_b && i + len_b <= span &&
                buffer[i] == needle_b[0] &&
                memcmp(buffer.data() + i, needle_b, len_b) == 0) {
                out_b[count_b++] = span_base + i;
            }
            if (count_a >= max_a && count_b >= max_b) return 0;
        }

        carry = std::min(max_len - 1, span);
        if (carry > 0) {
            memmove(buffer.data(), buffer.data() + span - carry, carry);
        }
        cursor += to_copy;
    }
    return 0;
}

static int scan_region_for_qword_refs(uintptr_t start, uintptr_t stop,
                                      const std::vector<uintptr_t>& targets,
                                      uintptr_t* out, size_t max_out,
                                      size_t& count) {
    if (targets.empty() || start >= stop || stop - start < sizeof(uintptr_t)) {
        return 0;
    }
    __try {
        uintptr_t p = (start + 7) & ~uintptr_t(7);
        uintptr_t end = stop & ~uintptr_t(7);
        for (; p + sizeof(uintptr_t) <= end; p += sizeof(uintptr_t)) {
            uintptr_t v = *reinterpret_cast<const volatile uintptr_t*>(p);
            if (std::find(targets.begin(), targets.end(), v) == targets.end()) {
                continue;
            }
            if (count < max_out) out[count] = p;
            count++;
            if (count >= max_out) return 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int scan_region_for_logo_rows(uintptr_t start, uintptr_t stop,
                                     uintptr_t* out, size_t max_out,
                                     size_t& count) {
    if (start >= stop || stop - start < sizeof(LogoDescriptorRow)) {
        return 0;
    }
    __try {
        uintptr_t p = (start + 7) & ~uintptr_t(7);
        uintptr_t end = stop - sizeof(LogoDescriptorRow);
        for (; p <= end; p += 8) {
            LogoDescriptorRow row{};
            row.width = *reinterpret_cast<const volatile uint32_t*>(p);
            row.height = *reinterpret_cast<const volatile uint32_t*>(p + 0x04);
            row.format = *reinterpret_cast<const volatile uint32_t*>(p + 0x08);
            row.resource_id =
                *reinterpret_cast<const volatile uint32_t*>(p + 0x0C);
            row.code = *reinterpret_cast<const volatile uint32_t*>(p + 0x10);
            row.flags = *reinterpret_cast<const volatile uint32_t*>(p + 0x14);
            if (!is_logo_row(row)) continue;

            if (count < max_out) out[count] = p;
            count++;
            if (count >= max_out) return 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int scan_region_for_logo_rows_by_rid(uintptr_t start, uintptr_t stop,
                                            uint32_t streamer_rid,
                                            uint32_t pulse_rid,
                                            uintptr_t* streamer_out,
                                            size_t max_streamer_out,
                                            size_t& streamer_count,
                                            uintptr_t* pulse_out,
                                            size_t max_pulse_out,
                                            size_t& pulse_count) {
    if (start >= stop || stop - start < sizeof(LogoDescriptorRow)) {
        return 0;
    }
    __try {
        uintptr_t p = (start + 3) & ~uintptr_t(3);
        uintptr_t end = stop - sizeof(LogoDescriptorRow);
        for (; p <= end; p += 4) {
            LogoDescriptorRow row{};
            row.width = *reinterpret_cast<const volatile uint32_t*>(p);
            row.height = *reinterpret_cast<const volatile uint32_t*>(p + 0x04);
            row.format = *reinterpret_cast<const volatile uint32_t*>(p + 0x08);
            row.resource_id =
                *reinterpret_cast<const volatile uint32_t*>(p + 0x0C);
            row.code = *reinterpret_cast<const volatile uint32_t*>(p + 0x10);
            row.flags = *reinterpret_cast<const volatile uint32_t*>(p + 0x14);
            if (!is_logo_row(row)) continue;
            if (row.resource_id == streamer_rid) {
                if (streamer_count < max_streamer_out) {
                    streamer_out[streamer_count] = p;
                }
                streamer_count++;
            } else if (row.resource_id == pulse_rid) {
                if (pulse_count < max_pulse_out) {
                    pulse_out[pulse_count] = p;
                }
                pulse_count++;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static int scan_region_for_logo_rows_by_targets(
    uintptr_t start,
    uintptr_t stop,
    const uint32_t* target_rids,
    size_t target_rid_count,
    const uint32_t* target_codes,
    size_t target_code_count,
    uintptr_t* out,
    size_t max_out,
    size_t& count) {
    if (start >= stop || stop - start < sizeof(LogoDescriptorRow) ||
        ((!target_rids || target_rid_count == 0) &&
         (!target_codes || target_code_count == 0))) {
        return 0;
    }
    __try {
        uintptr_t p = (start + 3) & ~uintptr_t(3);
        uintptr_t end = stop - sizeof(LogoDescriptorRow);
        for (; p <= end; p += 4) {
            LogoDescriptorRow row{};
            row.width = *reinterpret_cast<const volatile uint32_t*>(p);
            row.height = *reinterpret_cast<const volatile uint32_t*>(p + 0x04);
            row.format = *reinterpret_cast<const volatile uint32_t*>(p + 0x08);
            row.resource_id =
                *reinterpret_cast<const volatile uint32_t*>(p + 0x0C);
            row.code = *reinterpret_cast<const volatile uint32_t*>(p + 0x10);
            row.flags = *reinterpret_cast<const volatile uint32_t*>(p + 0x14);
            if (!is_logo_row(row)) continue;

            bool matched = false;
            for (size_t i = 0; i < target_rid_count; ++i) {
                if (target_rids && row.resource_id == target_rids[i]) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                for (size_t i = 0; i < target_code_count; ++i) {
                    if (target_codes && row.code == target_codes[i]) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) continue;

            if (count < max_out) out[count] = p;
            count++;
            if (count >= max_out) return 0;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

struct Fh5LogoBuildProfile {
    size_t    text_size;
    uintptr_t entry_vtable_rva;
    uintptr_t resource_vtable_rva;
    uintptr_t upload_rva;
    uintptr_t upload_state_rva;
    uintptr_t upload_manager_global_rva;
    uintptr_t handle_table_global_rva;
    uintptr_t refcount_table_global_rva;
    Fh5LogoUploadAbi upload_abi;
    bool      upload_enabled;
    uintptr_t ui_node_vtable_rva;
    uintptr_t ui_node_sub_vtable_rva;
    uintptr_t ui_create_custom_texture_rva;
    uintptr_t ui_release_texture_wrapper_rva;
    uintptr_t ui_render_context_global_rva;
    uintptr_t gp_create_texture_resource_rva;
    uintptr_t gp_build_stock_wrapper_rva;
};

static constexpr Fh5LogoBuildProfile kFH5LogoSteamProfile{
    0x67601E4, 0x68AB5B8, 0x67D0B60, 0xCF4EE0, 0,
    0xA0776F0, 0x9029B60, 0x9029B78,
    Fh5LogoUploadAbi::SteamLowLevel, true,
    0, 0, 0, 0, 0, 0, 0,
};

static constexpr Fh5LogoBuildProfile kFH5LogoGamePassProfile{
    0x64854DC, 0x65CE8F8, 0x64F4620, 0xCAAE80, 0,
    0x9C7C6A0, 0x8C49B00, 0x8C49B18,
    Fh5LogoUploadAbi::GamePassBcAwareQueue, true,
    0x669EDE0, 0x669EE20, 0x18E7350, 0x18ECBA0, 0x9D787F0,
    0xBFF7B0, 0x5AD430,
};

#if defined(SPOTIFY_RADIO_DIAG)
static constexpr const char* kFH5GamePassStagingProbeFlag =
    "fh5-gamepass-logo-staging-probe.flag";
static constexpr const char* kFH5GamePassUiNodeSwapFlag =
    "fh5-gamepass-logo-ui-node-swap.flag";
static constexpr const char* kFH5GamePassStockSlotSwapFlag =
    "fh5-gamepass-logo-stock-slot-swap.flag";
static constexpr const char* kFH5GamePassBcAwareUploadFlag =
    "fh5-gamepass-logo-bc-aware-upload.flag";
static constexpr uint32_t kFH5GamePassStagingMaxSuccessfulUploads = 2;
static constexpr uint32_t kFH5GamePassUiNodeSwapMaxSuccessfulUploads = 2;
static constexpr uint32_t kFH5GamePassStockSlotSwapMaxSuccessfulUploads = 1;
static constexpr uint32_t kFH5GamePassBcAwareUploadMaxSuccessfulUploads = 1;
#endif

static const Fh5LogoBuildProfile* fh5_logo_profile_for_text_size(
    size_t text_size) {
    if (text_size == kFH5LogoSteamProfile.text_size) {
        return &kFH5LogoSteamProfile;
    }
    if (text_size == kFH5LogoGamePassProfile.text_size) {
        return &kFH5LogoGamePassProfile;
    }
    return nullptr;
}

static constexpr uintptr_t kFH5LogoPathPtrOffset = 0x90;
static constexpr uintptr_t kFH5LogoPathLenOffset = 0xA0;
static constexpr uintptr_t kFH5LogoPathCapOffset = 0xA8;
static constexpr uintptr_t kFH5LogoSlotHandleOffset = 0x140;
static constexpr uintptr_t kFH5LogoResourceOffset = 0x148;
static constexpr uintptr_t kFH5LogoUploadHandleOffset = 0x18;
static constexpr uintptr_t kFH5LogoSizeMarkerOffset = 0x28;
static constexpr uint32_t kFH5LogoSizeMarker = 0x1688;
#if defined(SPOTIFY_RADIO_DIAG)
static constexpr uintptr_t kFH5GamePassResourceCreateFlagsByteRva = 0x9C883D2;
#endif

struct Fh5LogoScanStats {
    size_t regions = 0;
    size_t bytes = 0;
    size_t key_hits = 0;
    size_t ref_hits = 0;
    size_t vtable_hits = 0;
    size_t candidates = 0;
    const char* mode = "vtable";
};

static std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool fh5_process_pointer(uintptr_t p) {
    return p >= 0x10000ull && p < 0x0000800000000000ull;
}

static bool fh5_read_ascii(uintptr_t ptr, size_t len, std::string& out) {
    if (!fh5_process_pointer(ptr) || len == 0 || len > 512) return false;
    out.assign(len, '\0');
    if (!safe_memcpy(out.data(), reinterpret_cast<const void*>(ptr), len)) {
        out.clear();
        return false;
    }
    if (out.find('\0') != std::string::npos) {
        out.clear();
        return false;
    }
    return true;
}

static bool fh5_read_bc7_wrapper_desc(uintptr_t wrapper,
                                      uint32_t& width,
                                      uint32_t& height,
                                      uint32_t& format) {
    width = 0;
    height = 0;
    format = 0;
    if (!fh5_process_pointer(wrapper)) return false;

    for (uintptr_t off = 0xD0; off <= 0x180; off += 4) {
        uint32_t d0 = 0, d1 = 0, d2 = 0;
        if (!safe_read_u32(wrapper + off, d0) ||
            !safe_read_u32(wrapper + off + 4, d1) ||
            !safe_read_u32(wrapper + off + 8, d2)) {
            continue;
        }
        const uint32_t w = d0 & 0xFFFFu;
        const uint32_t h = d1 & 0xFFFFu;
        const uint32_t fmt = (d2 >> 16) & 0xFFFFu;
        if (w < 16u || h < 16u || w > 4096u || h > 4096u ||
            (w & 3u) || (h & 3u) || fmt != 0x62u) {
            continue;
        }
        width = w;
        height = h;
        format = fmt;
        return true;
    }
    return false;
}

static bool fh5_logo_dims_match_streamer(uint32_t w, uint32_t h) {
    if (w < 128u || w > 1024u || h < 64u || h > 512u) return false;
    const uint64_t a = static_cast<uint64_t>(w) * kUiRenderFullLogoHeight;
    const uint64_t b = static_cast<uint64_t>(h) * kUiRenderFullLogoWidth;
    return a * 100u < b * 125u && b * 100u < a * 125u;
}

static bool fh5_logo_dims_plausible_slot(uint32_t w, uint32_t h) {
    if (w < 128u || w > 1024u || h < 64u || h > 512u) return false;
    return static_cast<uint64_t>(w) * 10u >=
               static_cast<uint64_t>(h) * 12u &&
           static_cast<uint64_t>(w) * 10u <=
               static_cast<uint64_t>(h) * 24u;
}

static bool fh5_collect_logo_upload_slot(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    uintptr_t resource,
    uintptr_t slot_offset,
    uintptr_t handle_table,
    uintptr_t refcount_table,
    Fh5LogoUploadSlot& out) {
    uintptr_t resource_vtable = 0;
    uint64_t render_handle = 0;
    uint32_t upload_handle = 0;
    uint32_t size_marker = 0;
    if (!safe_read_qword(resource + slot_offset, resource_vtable) ||
        resource_vtable != module_base + profile.resource_vtable_rva ||
        !safe_read_qword(resource + slot_offset + 0x10, render_handle) ||
        !safe_read_u32(resource + slot_offset + kFH5LogoUploadHandleOffset,
                       upload_handle) ||
        !safe_read_u32(resource + slot_offset + kFH5LogoSizeMarkerOffset,
                       size_marker) ||
        upload_handle == 0 || size_marker != kFH5LogoSizeMarker) {
        return false;
    }

    const uint32_t idx = upload_handle & 0xFFFFFu;
    const uint32_t render_idx = static_cast<uint32_t>(render_handle & 0xFFFFFu);
    uintptr_t wrapper = 0;
    if (!safe_read_qword(handle_table + sizeof(uintptr_t) * idx, wrapper) ||
        !fh5_process_pointer(wrapper)) {
        return false;
    }
    uintptr_t render_wrapper = 0;
    safe_read_qword(handle_table + sizeof(uintptr_t) * render_idx,
                    render_wrapper);

    uint32_t refcount = 0;
    uint32_t render_refcount = 0;
    if (fh5_process_pointer(refcount_table)) {
        safe_read_u32(refcount_table + sizeof(uint32_t) * idx, refcount);
        safe_read_u32(refcount_table + sizeof(uint32_t) * render_idx,
                      render_refcount);
    }
    if (refcount == 0) return false;

    uint32_t width = 0, height = 0, format = 0;
    if (!fh5_read_bc7_wrapper_desc(wrapper, width, height, format) ||
        !fh5_logo_dims_plausible_slot(width, height)) {
        return false;
    }

    uint32_t render_width = 0, render_height = 0, render_format = 0;
    if (fh5_process_pointer(render_wrapper)) {
        fh5_read_bc7_wrapper_desc(render_wrapper, render_width, render_height,
                                  render_format);
    }

    out.resource_offset = slot_offset;
    out.wrapper = wrapper;
    out.render_wrapper = render_wrapper;
    out.render_handle = render_handle;
    out.upload_handle = upload_handle;
    out.width = width;
    out.height = height;
    out.format = format;
    out.render_width = render_width;
    out.render_height = render_height;
    out.render_format = render_format;
    out.refcount = refcount;
    out.render_refcount = render_refcount;
    return true;
}

static bool fh5_read_ui_wrapper24_desc(uintptr_t wrapper,
                                       uint32_t& width,
                                       uint32_t& height,
                                       uint32_t& format) {
    width = 0;
    height = 0;
    format = 0;
    if (!fh5_process_pointer(wrapper)) return false;
    if (!safe_read_u32(wrapper, width) ||
        !safe_read_u32(wrapper + 4, height) ||
        !safe_read_u32(wrapper + 8, format)) {
        return false;
    }
    return width >= 16u && width <= 4096u && height >= 1u &&
           height <= 4096u;
}

static bool fh5_validate_ui_png_node(const Fh5LogoBuildProfile& profile,
                                     uintptr_t module_base,
                                     uintptr_t entry,
                                     uintptr_t node,
                                     uintptr_t& wrapper) {
    wrapper = 0;
    if (!profile.ui_node_vtable_rva || !fh5_process_pointer(node)) {
        return false;
    }
    uintptr_t vt = 0;
    uintptr_t sub_vt = 0;
    uintptr_t node_entry = 0;
    uintptr_t node_wrapper = 0;
    if (!safe_read_qword(node, vt) ||
        !safe_read_qword(node + 0x30, sub_vt) ||
        !safe_read_qword(node + 0x38, node_wrapper) ||
        !safe_read_qword(node + 0x58, node_entry)) {
        return false;
    }
    if (vt != module_base + profile.ui_node_vtable_rva ||
        sub_vt != module_base + profile.ui_node_sub_vtable_rva ||
        node_entry != entry || !fh5_process_pointer(node_wrapper)) {
        return false;
    }

    uint32_t w = 0, h = 0, f = 0;
    if (!fh5_read_ui_wrapper24_desc(node_wrapper, w, h, f) ||
        !fh5_logo_dims_plausible_slot(w, h)) {
        return false;
    }
    wrapper = node_wrapper;
    return true;
}

static bool fh5_find_ui_png_node_for_entry(const Fh5LogoBuildProfile& profile,
                                           uintptr_t module_base,
                                           uintptr_t entry,
                                           uintptr_t& node,
                                           uintptr_t& wrapper) {
    node = 0;
    wrapper = 0;
    if (!profile.ui_node_vtable_rva || !fh5_process_pointer(entry)) {
        return false;
    }

    constexpr uintptr_t kRadius = 8ull * 1024ull * 1024ull;
    const uintptr_t start_limit = entry > kRadius ? entry - kRadius : 0x10000ull;
    const uintptr_t stop_limit =
        entry + kRadius > entry ? entry + kRadius : 0x0000800000000000ull;
    std::vector<uintptr_t> targets{entry};
    std::array<uintptr_t, 96> refs{};
    size_t count = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = start_limit;
    while (addr < stop_limit &&
           ::VirtualQuery(reinterpret_cast<const void*>(addr), &mbi,
                          sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t region_stop = start + mbi.RegionSize;
        const uintptr_t scan_start = std::max(start, start_limit);
        const uintptr_t scan_stop = std::min(region_stop, stop_limit);
        const DWORD protect = mbi.Protect & 0xFFu;
        const bool readable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            protect != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
        if (readable && scan_stop > scan_start &&
            scan_stop - scan_start >= sizeof(uintptr_t)) {
            scan_region_for_qword_refs(scan_start, scan_stop, targets,
                                       refs.data(), refs.size(), count);
            if (count >= refs.size()) break;
        }
        if (region_stop <= addr) break;
        addr = region_stop;
    }

    const size_t n = std::min(count, refs.size());
    for (size_t i = 0; i < n; ++i) {
        if (refs[i] < 0x58) continue;
        const uintptr_t candidate = refs[i] - 0x58;
        uintptr_t candidate_wrapper = 0;
        if (!fh5_validate_ui_png_node(profile, module_base, entry, candidate,
                                      candidate_wrapper)) {
            continue;
        }
        node = candidate;
        wrapper = candidate_wrapper;
        return true;
    }
    return false;
}

static bool fh5_validate_logo_target(const Fh5LogoBuildProfile& profile,
                                     uintptr_t module_base,
                                     uintptr_t entry,
                                     Fh5LogoTarget& out) {
    uintptr_t entry_vtable = 0;
    uintptr_t path = 0;
    uintptr_t len = 0;
    uintptr_t cap = 0;
    uintptr_t resource = 0;
    if (!safe_read_qword(entry, entry_vtable) ||
        entry_vtable != module_base + profile.entry_vtable_rva ||
        !safe_read_qword(entry + kFH5LogoPathPtrOffset, path) ||
        !safe_read_qword(entry + kFH5LogoPathLenOffset, len) ||
        !safe_read_qword(entry + kFH5LogoPathCapOffset, cap) ||
        !safe_read_qword(entry + kFH5LogoResourceOffset, resource)) {
        return false;
    }
    if (!fh5_process_pointer(path) || len == 0 || len > 260 ||
        cap < len || cap > 300 || !fh5_process_pointer(resource)) {
        return false;
    }

    std::string key;
    if (!fh5_read_ascii(path, static_cast<size_t>(len), key)) return false;
    const std::string key_lower = lower_ascii(key);
    if (key_lower.find("radiologos") == std::string::npos ||
        key_lower.find("streamer_mode.swatchbin") == std::string::npos) {
        return false;
    }

    uintptr_t handle_table = 0;
    uintptr_t refcount_table = 0;
    if (!safe_read_qword(module_base + profile.handle_table_global_rva,
                         handle_table) ||
        !fh5_process_pointer(handle_table)) {
        return false;
    }
    safe_read_qword(module_base + profile.refcount_table_global_rva,
                    refcount_table);

    std::vector<Fh5LogoUploadSlot> slots;
    for (uintptr_t slot_offset = 0; slot_offset <= 0x1A0; slot_offset += 8) {
        Fh5LogoUploadSlot slot{};
        if (!fh5_collect_logo_upload_slot(profile, module_base, resource,
                                          slot_offset, handle_table,
                                          refcount_table, slot)) {
            continue;
        }
        const auto duplicate =
            std::find_if(slots.begin(), slots.end(), [&](const auto& existing) {
                return existing.upload_handle == slot.upload_handle;
            });
        if (duplicate == slots.end()) {
            slots.push_back(std::move(slot));
        }
    }
    if (slots.empty() ||
        !fh5_logo_dims_match_streamer(slots.front().width,
                                      slots.front().height)) {
        return false;
    }
    const auto& primary = slots.front();

    out.entry = entry;
    out.path = path;
    out.resource = resource;
    out.wrapper = primary.wrapper;
    out.render_wrapper = primary.render_wrapper;
    out.render_handle = primary.render_handle;
    out.upload_handle = primary.upload_handle;
    out.width = primary.width;
    out.height = primary.height;
    out.format = primary.format;
    out.render_width = primary.render_width;
    out.render_height = primary.render_height;
    out.render_format = primary.render_format;
    out.refcount = primary.refcount;
    out.render_refcount = primary.render_refcount;
    if (profile.ui_node_vtable_rva) {
        fh5_find_ui_png_node_for_entry(profile, module_base, entry,
                                       out.ui_node, out.ui_node_wrapper);
    }
    out.key = std::move(key);
    out.slots = std::move(slots);
    return true;
}

static bool fh5_add_logo_target_if_valid(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    uintptr_t candidate,
    std::vector<Fh5LogoTarget>& out,
    Fh5LogoScanStats& stats) {
    Fh5LogoTarget target{};
    if (!fh5_validate_logo_target(profile, module_base, candidate, target)) {
        return false;
    }
    ++stats.candidates;
    const bool duplicate =
        std::any_of(out.begin(), out.end(),
                    [&](const Fh5LogoTarget& existing) {
                        return existing.upload_handle == target.upload_handle ||
                               existing.resource == target.resource ||
                               existing.entry == target.entry;
                    });
    if (!duplicate) out.push_back(std::move(target));
    return true;
}

static uintptr_t fh5_logo_key_start_from_suffix(uintptr_t suffix_addr) {
    constexpr char kPrefix[] = "game:";
    constexpr uintptr_t kMaxBacktrack = 160;
    for (uintptr_t back = 0; back <= kMaxBacktrack && suffix_addr >= back;
         ++back) {
        const uintptr_t candidate = suffix_addr - back;
        char prefix[sizeof(kPrefix) - 1]{};
        if (safe_memcpy(prefix, reinterpret_cast<const void*>(candidate),
                        sizeof(prefix)) &&
            std::memcmp(prefix, kPrefix, sizeof(prefix)) == 0) {
            return candidate;
        }
    }
    return 0;
}

static bool fh5_find_streamer_logo_targets_by_key(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    std::vector<Fh5LogoTarget>& out,
    Fh5LogoScanStats& stats,
    bool collect_all) {
    out.clear();
    stats = {};
    stats.mode = "key";

    static constexpr char kNeedle[] =
        "streamer_mode.swatchbin";
    std::array<uintptr_t, 128> key_hits{};
    size_t key_hit_count = 0;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0;
    while (::VirtualQuery(reinterpret_cast<const void*>(addr), &mbi,
                          sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t stop = start + mbi.RegionSize;
        const DWORD protect = mbi.Protect & 0xFFu;
        const bool readable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            protect != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
        if (readable && stop > start && mbi.RegionSize >= sizeof(kNeedle)) {
            ++stats.regions;
            stats.bytes += mbi.RegionSize;
            std::array<uintptr_t, 32> suffix_hits{};
            size_t suffix_hit_count = 0;
            scan_region_for_bytes(start, stop, kNeedle, sizeof(kNeedle) - 1,
                                  suffix_hits.data(), suffix_hits.size(),
                                  suffix_hit_count);
            const size_t suffix_n =
                std::min(suffix_hit_count, suffix_hits.size());
            for (size_t i = 0; i < suffix_n && key_hit_count < key_hits.size();
                 ++i) {
                const uintptr_t key_start =
                    fh5_logo_key_start_from_suffix(suffix_hits[i]);
                if (!key_start) continue;
                const bool duplicate =
                    std::find(key_hits.begin(),
                              key_hits.begin() +
                                  static_cast<ptrdiff_t>(key_hit_count),
                              key_start) !=
                    key_hits.begin() + static_cast<ptrdiff_t>(key_hit_count);
                if (!duplicate) {
                    key_hits[key_hit_count++] = key_start;
                }
            }
            stats.key_hits = key_hit_count;
            if (key_hit_count >= key_hits.size()) break;
        }
        if (stop <= addr) break;
        addr = stop;
        if (addr >= 0x0000800000000000ull) break;
    }

    const size_t hit_n = std::min(key_hit_count, key_hits.size());
    if (hit_n == 0) return false;

    constexpr uintptr_t kRefRadius = 64ull * 1024ull * 1024ull;
    std::array<uintptr_t, 256> refs{};
    for (size_t i = 0; i < hit_n; ++i) {
        const uintptr_t key_ptr = key_hits[i];
        const uintptr_t start_limit =
            key_ptr > kRefRadius ? key_ptr - kRefRadius : 0x10000ull;
        const uintptr_t stop_limit =
            key_ptr + kRefRadius > key_ptr
                ? key_ptr + kRefRadius
                : 0x0000800000000000ull;
        std::vector<uintptr_t> targets{key_ptr};
        size_t ref_count = 0;
        MEMORY_BASIC_INFORMATION rmbi{};
        uintptr_t raddr = start_limit;
        while (raddr < stop_limit &&
               ::VirtualQuery(reinterpret_cast<const void*>(raddr), &rmbi,
                              sizeof(rmbi)) == sizeof(rmbi)) {
            const uintptr_t start =
                reinterpret_cast<uintptr_t>(rmbi.BaseAddress);
            const uintptr_t stop = start + rmbi.RegionSize;
            const uintptr_t scan_start = std::max(start, start_limit);
            const uintptr_t scan_stop = std::min(stop, stop_limit);
            const DWORD protect = rmbi.Protect & 0xFFu;
            const bool readable =
                rmbi.State == MEM_COMMIT && rmbi.Type == MEM_PRIVATE &&
                protect != PAGE_NOACCESS && !(rmbi.Protect & PAGE_GUARD);
            if (readable && scan_stop > scan_start &&
                scan_stop - scan_start >= sizeof(uintptr_t)) {
                const size_t ref_start = ref_count;
                scan_region_for_qword_refs(scan_start, scan_stop, targets,
                                           refs.data(), refs.size(),
                                           ref_count);
                stats.ref_hits += ref_count - ref_start;
                const size_t ref_n = std::min(ref_count, refs.size());
                for (size_t j = ref_start; j < ref_n; ++j) {
                    if (refs[j] < kFH5LogoPathPtrOffset) continue;
                    const uintptr_t candidate =
                        refs[j] - kFH5LogoPathPtrOffset;
                    if (fh5_add_logo_target_if_valid(
                            profile, module_base, candidate, out, stats) &&
                        !collect_all) {
                        return true;
                    }
                }
                if (ref_count >= refs.size()) break;
            }
            if (stop <= raddr) break;
            raddr = stop;
        }
    }
    return !out.empty();
}

static bool fh5_find_streamer_logo_targets(const Fh5LogoBuildProfile& profile,
                                           uintptr_t module_base,
                                           std::vector<Fh5LogoTarget>& out,
                                           Fh5LogoScanStats& stats,
                                           bool collect_all = false) {
    out.clear();
    stats = {};
    auto find_by_vtable = [&]() {
        out.clear();
        stats = {};
        stats.mode = "vtable";

        const uintptr_t entry_vtable = module_base + profile.entry_vtable_rva;
        char needle[sizeof(uintptr_t)]{};
        std::memcpy(needle, &entry_vtable, sizeof(entry_vtable));

        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t addr = 0;
        while (::VirtualQuery(reinterpret_cast<const void*>(addr), &mbi,
                              sizeof(mbi)) == sizeof(mbi)) {
            const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t stop = start + mbi.RegionSize;
            const DWORD protect = mbi.Protect & 0xFFu;
            const bool readable =
                mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                protect != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
            if (readable && stop > start &&
                mbi.RegionSize >= kFH5LogoResourceOffset + sizeof(uintptr_t)) {
                ++stats.regions;
                stats.bytes += mbi.RegionSize;
                std::array<uintptr_t, 1024> hits{};
                size_t hit_count = 0;
                scan_region_for_bytes(start, stop, needle, sizeof(needle),
                                      hits.data(), hits.size(), hit_count);
                stats.vtable_hits += hit_count;
                const size_t n = std::min(hit_count, hits.size());
                for (size_t i = 0; i < n; ++i) {
                    fh5_add_logo_target_if_valid(profile, module_base, hits[i],
                                                 out, stats);
                    if (!collect_all && !out.empty()) return true;
                }
            }
            if (stop <= addr) break;
            addr = stop;
            if (addr >= 0x0000800000000000ull) break;
        }
        return !out.empty();
    };

    if (profile.upload_abi == Fh5LogoUploadAbi::SteamLowLevel) {
        return find_by_vtable();
    }

    if (fh5_find_streamer_logo_targets_by_key(profile, module_base, out,
                                              stats, collect_all)) {
        return true;
    }
    return find_by_vtable();
}

#if defined(SPOTIFY_RADIO_DIAG)
static std::string fh5_probe_qwords(uintptr_t base,
                                    std::initializer_list<uintptr_t> offsets) {
    std::ostringstream ss;
    if (!fh5_process_pointer(base)) {
        ss << "base=" << hex(base) << " invalid";
        return ss.str();
    }
    ss << "base=" << hex(base);
    for (uintptr_t off : offsets) {
        uintptr_t value = 0;
        ss << " +0x" << std::hex << off << "=";
        if (safe_read_qword(base + off, value)) {
            ss << "0x" << value;
        } else {
            ss << "?";
        }
    }
    return ss.str();
}

static std::string fh5_probe_u32s(uintptr_t base,
                                  std::initializer_list<uintptr_t> offsets) {
    std::ostringstream ss;
    if (!fh5_process_pointer(base)) {
        ss << "base=" << hex(base) << " invalid";
        return ss.str();
    }
    ss << "base=" << hex(base);
    for (uintptr_t off : offsets) {
        uint32_t value = 0;
        ss << " +0x" << std::hex << off << "=";
        if (safe_read_u32(base + off, value)) {
            ss << "0x" << value;
        } else {
            ss << "?";
        }
    }
    return ss.str();
}

static std::string fh5_probe_relative(uintptr_t addr, uintptr_t base) {
    if (!fh5_process_pointer(base)) return "?";
    std::ostringstream ss;
    if (addr >= base) {
        ss << "+0x" << std::hex << (addr - base);
    } else {
        ss << "-0x" << std::hex << (base - addr);
    }
    return ss.str();
}

static bool fh5_probe_addr_in(uintptr_t addr, uintptr_t base, uintptr_t size) {
    return fh5_process_pointer(base) && addr >= base && addr < base + size;
}

static void fh5_probe_log_owner_candidate(const char* label,
                                          size_t index,
                                          uintptr_t ref_addr,
                                          uintptr_t value,
                                          const Fh5LogoTarget& t) {
    if (fh5_probe_addr_in(ref_addr, t.entry, 0x180) ||
        fh5_probe_addr_in(ref_addr, t.resource, 0x220) ||
        fh5_probe_addr_in(ref_addr, t.wrapper, 0x220)) {
        return;
    }
    if (value != t.entry && value != t.resource && value != t.wrapper &&
        value != t.path) {
        return;
    }

    const uintptr_t wide =
        ref_addr >= 0x80 ? ref_addr - 0x80 : ref_addr;
    log::info("[fh5-logo-probe] owner-candidate label=" +
              std::string(label ? label : "?") +
              " index=" + std::to_string(index) +
              " ref=" + hex(ref_addr) +
              " value=" + hex(value) +
              " rel_entry=" + fh5_probe_relative(ref_addr, t.entry) +
              " rel_res=" + fh5_probe_relative(ref_addr, t.resource) +
              " wide " +
              fh5_probe_qwords(wide, {0x00, 0x08, 0x10, 0x18, 0x20,
                                      0x28, 0x30, 0x38, 0x40, 0x48,
                                      0x50, 0x58, 0x60, 0x68, 0x70,
                                      0x78, 0x80, 0x88, 0x90, 0x98,
                                      0xA0, 0xA8, 0xB0, 0xB8, 0xC0}));

    constexpr uintptr_t kHypOffsets[] = {0x10, 0x18, 0x20, 0x28, 0x30,
                                         0x38, 0x40, 0x48};
    for (uintptr_t hyp_off : kHypOffsets) {
        const uintptr_t base =
            ref_addr >= hyp_off ? ref_addr - hyp_off : ref_addr;
        log::info("[fh5-logo-probe] owner-hyp label=" +
                  std::string(label ? label : "?") +
                  " index=" + std::to_string(index) +
                  " ref_off=0x" + [&]() {
                      std::ostringstream ss;
                      ss << std::hex << hyp_off;
                      return ss.str();
                  }() +
                  " base=" + hex(base) +
                  " q " +
                  fh5_probe_qwords(base, {0x00, 0x08, 0x10, 0x18, 0x20,
                                          0x28, 0x30, 0x38, 0x40, 0x48,
                                          0x50, 0x58, 0x60, 0x68, 0x70,
                                          0x78, 0x80, 0x88, 0x90}));
        log::info("[fh5-logo-probe] owner-hyp-d label=" +
                  std::string(label ? label : "?") +
                  " index=" + std::to_string(index) +
                  " ref_off=0x" + [&]() {
                      std::ostringstream ss;
                      ss << std::hex << hyp_off;
                      return ss.str();
                  }() +
                  " base=" + hex(base) +
                  " d " +
                  fh5_probe_u32s(base, {0x00, 0x04, 0x08, 0x0C, 0x10,
                                        0x14, 0x18, 0x1C, 0x20, 0x24,
                                        0x28, 0x2C, 0x30, 0x34, 0x38,
                                        0x3C, 0x40, 0x44, 0x48, 0x4C,
                                        0x50, 0x54, 0x58, 0x5C, 0x60}));
    }
}

static void fh5_probe_scan_qword_refs_window(
    const char* label,
    uintptr_t center,
    size_t radius,
    const std::vector<uintptr_t>& filtered,
    const Fh5LogoTarget& t) {
    if (!fh5_process_pointer(center) || filtered.empty()) return;

    const uintptr_t start_limit =
        center > radius ? center - static_cast<uintptr_t>(radius) : 0x10000ull;
    const uintptr_t stop_limit =
        center + static_cast<uintptr_t>(radius) > center
            ? center + static_cast<uintptr_t>(radius)
            : 0x0000800000000000ull;

    std::array<uintptr_t, 96> refs{};
    size_t count = 0;
    size_t regions = 0;
    size_t bytes = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = start_limit;
    while (addr < stop_limit &&
           ::VirtualQuery(reinterpret_cast<const void*>(addr), &mbi,
                          sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t region_stop = start + mbi.RegionSize;
        const uintptr_t scan_start = std::max(start, start_limit);
        const uintptr_t scan_stop = std::min(region_stop, stop_limit);
        const DWORD protect = mbi.Protect & 0xFFu;
        const bool readable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            protect != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
        if (readable && scan_stop > scan_start &&
            scan_stop - scan_start >= sizeof(uintptr_t)) {
            ++regions;
            bytes += scan_stop - scan_start;
            scan_region_for_qword_refs(scan_start, scan_stop, filtered,
                                       refs.data(), refs.size(), count);
            if (count >= refs.size()) break;
        }
        if (region_stop <= addr) break;
        addr = region_stop;
    }

    log::info("[fh5-logo-probe] qword-ref-window label=" +
              std::string(label ? label : "?") +
              " center=" + hex(center) +
              " radius=" + std::to_string(radius) +
              " targets=" + std::to_string(filtered.size()) +
              " refs=" + std::to_string(count) +
              " capped=" + std::to_string(count >= refs.size() ? 1 : 0) +
              " regions=" + std::to_string(regions) +
              " bytes=" + std::to_string(bytes));
    const size_t n = std::min(count, refs.size());
    for (size_t i = 0; i < n; ++i) {
        uintptr_t value = 0;
        safe_read_qword(refs[i], value);
        const uintptr_t ctx = refs[i] >= 0x20 ? refs[i] - 0x20 : refs[i];
        log::info("[fh5-logo-probe] qword-ref-window[" +
                  std::string(label ? label : "?") + ":" +
                  std::to_string(i) + "] at=" + hex(refs[i]) +
                  " rel_entry=" + fh5_probe_relative(refs[i], t.entry) +
                  " rel_res=" + fh5_probe_relative(refs[i], t.resource) +
                  " value=" + hex(value) + " ctx " +
                  fh5_probe_qwords(ctx, {0x00, 0x08, 0x10, 0x18, 0x20,
                                         0x28, 0x30, 0x38, 0x40}));
        fh5_probe_log_owner_candidate(label, i, refs[i], value, t);
    }
}

static void fh5_probe_log_qword_refs(const Fh5LogoTarget& t) {
    std::vector<uintptr_t> filtered;
    std::vector<uintptr_t> targets{
        t.entry,
        t.resource,
        t.wrapper,
        t.render_wrapper,
        t.path,
    };
    for (const auto& slot : t.slots) {
        targets.push_back(slot.wrapper);
        targets.push_back(slot.render_wrapper);
    }
    for (uintptr_t target : targets) {
        if (fh5_process_pointer(target) &&
            std::find(filtered.begin(), filtered.end(), target) ==
                filtered.end()) {
            filtered.push_back(target);
        }
    }
    if (filtered.empty()) return;

    constexpr size_t kEntryRadius = 8ull * 1024ull * 1024ull;
    constexpr size_t kResourceRadius = 2ull * 1024ull * 1024ull;
    constexpr size_t kWrapperRadius = 512ull * 1024ull;
    fh5_probe_scan_qword_refs_window("entry", t.entry, kEntryRadius, filtered,
                                     t);
    fh5_probe_scan_qword_refs_window("resource", t.resource, kResourceRadius,
                                     filtered, t);
    fh5_probe_scan_qword_refs_window("upload-wrapper", t.wrapper,
                                     kWrapperRadius, filtered, t);
    if (fh5_process_pointer(t.render_wrapper) &&
        t.render_wrapper != t.wrapper) {
        fh5_probe_scan_qword_refs_window("render-wrapper", t.render_wrapper,
                                         kWrapperRadius, filtered, t);
    }
}

static void fh5_log_gamepass_streamer_probe(const Fh5LogoTarget& t,
                                            uintptr_t handle_table,
                                            uintptr_t refcount_table) {
    log::info("[fh5-logo-probe] entry.header " +
              fh5_probe_qwords(t.entry, {0x00, 0x08, 0x10, 0x18, 0x20, 0x28,
                                         0x30, 0x38, 0x40, 0x48, 0x50, 0x58,
                                         0x60, 0x68, 0x70, 0x78, 0x80, 0x88}));
    log::info("[fh5-logo-probe] entry.path-resource " +
              fh5_probe_qwords(t.entry, {0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8,
                                         0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8,
                                         0xF0, 0xF8, 0x100, 0x108, 0x110,
                                         0x118, 0x120, 0x128, 0x130, 0x138,
                                         0x140, 0x148, 0x150, 0x158, 0x160}));
    log::info("[fh5-logo-probe] resource.000 " +
              fh5_probe_qwords(t.resource,
                               {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
                                0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68,
                                0x70, 0x78, 0x80, 0x88, 0x90, 0x98}));
    log::info("[fh5-logo-probe] resource.0a0 " +
              fh5_probe_qwords(t.resource,
                               {0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0,
                                0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x100, 0x108,
                                0x110, 0x118, 0x120, 0x128, 0x130, 0x138}));
    log::info("[fh5-logo-probe] resource.140 " +
              fh5_probe_qwords(t.resource,
                               {0x140, 0x148, 0x150, 0x158, 0x160, 0x168,
                                0x170, 0x178, 0x180, 0x188, 0x190, 0x198,
                                0x1A0, 0x1A8, 0x1B0, 0x1B8, 0x1C0}));

    log::info("[fh5-logo-probe] primary-upload-wrapper.header " +
              fh5_probe_qwords(t.wrapper,
                               {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
                                0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68,
                                0x70, 0x78, 0x80, 0x88, 0x90, 0x98}));
    log::info("[fh5-logo-probe] primary-upload-wrapper.desc " +
              fh5_probe_u32s(t.wrapper,
                             {0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4, 0xD8,
                              0xDC, 0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4,
                              0xF8, 0xFC, 0x100, 0x104, 0x108, 0x10C,
                              0x110, 0x114, 0x118, 0x11C, 0x120, 0x124,
                              0x128, 0x12C, 0x130, 0x134, 0x138, 0x13C,
                              0x140, 0x144, 0x148, 0x14C, 0x150, 0x154,
                              0x158, 0x15C, 0x160, 0x164, 0x168, 0x16C,
                              0x170, 0x174, 0x178, 0x17C, 0x180}));
    if (fh5_process_pointer(t.render_wrapper) &&
        t.render_wrapper != t.wrapper) {
        log::info("[fh5-logo-probe] primary-render-wrapper.header " +
                  fh5_probe_qwords(t.render_wrapper,
                                   {0x00, 0x08, 0x10, 0x18, 0x20, 0x28,
                                    0x30, 0x38, 0x40, 0x48, 0x50, 0x58,
                                    0x60, 0x68, 0x70, 0x78, 0x80, 0x88,
                                    0x90, 0x98}));
        log::info("[fh5-logo-probe] primary-render-wrapper.desc " +
                  fh5_probe_u32s(t.render_wrapper,
                                 {0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4,
                                  0xD8, 0xDC, 0xE0, 0xE4, 0xE8, 0xEC,
                                  0xF0, 0xF4, 0xF8, 0xFC, 0x100, 0x104,
                                  0x108, 0x10C, 0x110, 0x114, 0x118,
                                  0x11C, 0x120, 0x124, 0x128, 0x12C,
                                  0x130, 0x134, 0x138, 0x13C, 0x140,
                                  0x144, 0x148, 0x14C, 0x150, 0x154,
                                  0x158, 0x15C, 0x160, 0x164, 0x168,
                                  0x16C, 0x170, 0x174, 0x178, 0x17C,
                                  0x180}));
    }

    for (size_t i = 0; i < t.slots.size(); ++i) {
        const auto& slot = t.slots[i];
        const uintptr_t slot_base = t.resource + slot.resource_offset;
        log::info("[fh5-logo-probe] resource-slot[" + std::to_string(i) +
                  "] off=" + hex(slot.resource_offset) + " q " +
                  fh5_probe_qwords(slot_base,
                                   {0x00, 0x08, 0x10, 0x18, 0x20, 0x28,
                                    0x30, 0x38, 0x40, 0x48, 0x50, 0x58,
                                    0x60}));
        log::info("[fh5-logo-probe] resource-slot[" + std::to_string(i) +
                  "] off=" + hex(slot.resource_offset) + " d " +
                  fh5_probe_u32s(slot_base,
                                 {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14,
                                  0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C,
                                  0x30, 0x34, 0x38, 0x3C}));

        const uint32_t upload_idx = slot.upload_handle & 0xFFFFFu;
        const uint32_t render_idx =
            static_cast<uint32_t>(slot.render_handle & 0xFFFFFu);
        uintptr_t upload_wrapper = 0;
        uintptr_t render_wrapper = 0;
        uint32_t upload_refs = 0;
        uint32_t render_refs = 0;
        if (fh5_process_pointer(handle_table)) {
            safe_read_qword(handle_table + sizeof(uintptr_t) * upload_idx,
                            upload_wrapper);
            safe_read_qword(handle_table + sizeof(uintptr_t) * render_idx,
                            render_wrapper);
        }
        if (fh5_process_pointer(refcount_table)) {
            safe_read_u32(refcount_table + sizeof(uint32_t) * upload_idx,
                          upload_refs);
            safe_read_u32(refcount_table + sizeof(uint32_t) * render_idx,
                          render_refs);
        }
        log::info("[fh5-logo-probe] handle-row[" + std::to_string(i) +
                  "] upload_idx=0x" + [&]() {
                      std::ostringstream ss;
                      ss << std::hex << upload_idx;
                      return ss.str();
                  }() +
                  " upload_wrapper=" + hex(upload_wrapper) +
                  " upload_refs=" + std::to_string(upload_refs) +
                  " render_idx=0x" + [&]() {
                      std::ostringstream ss;
                      ss << std::hex << render_idx;
                      return ss.str();
                  }() +
                  " render_wrapper=" + hex(render_wrapper) +
                  " render_refs=" + std::to_string(render_refs));
    }

    fh5_probe_log_qword_refs(t);
}

static uintptr_t fh5_create_gamepass_ui_logo_wrapper(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    const RuntimeLogoImage& image,
    uint32_t width,
    uint32_t height) {
    if (!profile.ui_create_custom_texture_rva ||
        !profile.ui_render_context_global_rva || image.pixels.empty() ||
        width != kUiRenderFullLogoWidth ||
        height != kUiRenderFullLogoHeight) {
        return 0;
    }
    uintptr_t ctx = 0;
    uintptr_t ctx_graphics = 0;
    uintptr_t ctx_frame_state = 0;
    if (!safe_read_qword(module_base + profile.ui_render_context_global_rva,
                         ctx) ||
        !fh5_process_pointer(ctx) ||
        !safe_read_qword(ctx + 0x20, ctx_graphics) ||
        !fh5_process_pointer(ctx_graphics) ||
        !safe_read_qword(ctx + 0x70, ctx_frame_state) ||
        !fh5_process_pointer(ctx_frame_state)) {
        return 0;
    }

    using Fn = uintptr_t(__fastcall*)(uintptr_t, unsigned int, unsigned int,
                                      int, const void*, int);
    auto fn = reinterpret_cast<Fn>(
        module_base + profile.ui_create_custom_texture_rva);
    constexpr int kGamePassUiColorFormat = 87;
    const int row_pitch = static_cast<int>(width * sizeof(uint32_t));
    __try {
        return fn(ctx, width, height, kGamePassUiColorFormat,
                  image.pixels.data(), row_pitch);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool fh5_release_gamepass_ui_logo_wrapper(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    uintptr_t wrapper) {
    if (!profile.ui_release_texture_wrapper_rva ||
        !profile.ui_render_context_global_rva ||
        !fh5_process_pointer(wrapper)) {
        return false;
    }
    uintptr_t ctx = 0;
    if (!safe_read_qword(module_base + profile.ui_render_context_global_rva,
                         ctx) ||
        !fh5_process_pointer(ctx)) {
        return false;
    }
    using Fn = void(__fastcall*)(uintptr_t, uintptr_t);
    auto fn = reinterpret_cast<Fn>(
        module_base + profile.ui_release_texture_wrapper_rva);
    __try {
        fn(ctx, wrapper);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool fh5_release_gamepass_stock_wrapper(uintptr_t wrapper) {
    if (!fh5_process_pointer(wrapper)) return false;
    uintptr_t vtable = 0;
    uintptr_t release_fn = 0;
    if (!safe_read_qword(wrapper, vtable) ||
        !fh5_process_pointer(vtable) ||
        !safe_read_qword(vtable + 0x10, release_fn) ||
        !fh5_process_pointer(release_fn)) {
        return false;
    }
    using Fn = void(__fastcall*)(uintptr_t);
    __try {
        reinterpret_cast<Fn>(release_fn)(wrapper);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#pragma pack(push, 1)
struct Fh5GamePassTextureDesc {
    uint32_t width;
    uint32_t height;
    uint32_t depth_or_array;
    uint32_t mip_count;
    uint32_t format;
    uint64_t unknown20;
    uint32_t flags28;
    uint64_t unknown32;
    uint32_t flags40;
};
#pragma pack(pop)
static_assert(sizeof(Fh5GamePassTextureDesc) == 0x2C);

static uintptr_t fh5_create_gamepass_stock_logo_wrapper(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    uintptr_t graphics_mgr,
    uintptr_t descriptor_template_wrapper,
    const void* bc7,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    uint32_t row_pitch,
    uint32_t slice_pitch) {
    if (!profile.gp_create_texture_resource_rva ||
        !profile.gp_build_stock_wrapper_rva ||
        !fh5_process_pointer(graphics_mgr) ||
        !fh5_process_pointer(descriptor_template_wrapper) || !bc7 ||
        !fh5_logo_dims_plausible_slot(width, height) ||
        format != 0x62u || row_pitch == 0 || slice_pitch == 0) {
        return 0;
    }

    Fh5GamePassTextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth_or_array = 1;
    desc.mip_count = 1;
    desc.format = format;
    desc.unknown20 = 1;
    desc.flags28 = 0;
    desc.unknown32 = 8;
    desc.flags40 = 0;

    Fh5UploadSubresourceData subres{
        bc7,
        static_cast<intptr_t>(row_pitch),
        static_cast<intptr_t>(slice_pitch),
    };

    uint8_t flags_byte = 0;
    (void)safe_read_u8(module_base + kFH5GamePassResourceCreateFlagsByteRva,
                       flags_byte);
    char flags = static_cast<char>(flags_byte);
    uintptr_t resource_wrapper = 0;
    using CreateFn = volatile uintptr_t*(__fastcall*)(
        uintptr_t, volatile uintptr_t*, const Fh5GamePassTextureDesc*,
        const Fh5UploadSubresourceData*, unsigned char, const char*);
    auto create_fn = reinterpret_cast<CreateFn>(
        module_base + profile.gp_create_texture_resource_rva);
    __try {
        create_fn(graphics_mgr,
                  reinterpret_cast<volatile uintptr_t*>(&resource_wrapper),
                  &desc, &subres, 0xFFu, &flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        resource_wrapper = 0;
    }
    if (!fh5_process_pointer(resource_wrapper)) return 0;

    uintptr_t vtable = 0;
    uint32_t handle = 0;
    if (!safe_read_qword(resource_wrapper, vtable) ||
        vtable != module_base + profile.resource_vtable_rva ||
        !safe_read_u32(resource_wrapper + kFH5LogoUploadHandleOffset, handle) ||
        handle == 0) {
        fh5_release_gamepass_stock_wrapper(resource_wrapper);
        return 0;
    }

    uintptr_t handle_table = 0;
    uintptr_t gpu_wrapper = 0;
    uint32_t actual_w = 0;
    uint32_t actual_h = 0;
    uint32_t actual_fmt = 0;
    if (!safe_read_qword(module_base + profile.handle_table_global_rva,
                         handle_table) ||
        !fh5_process_pointer(handle_table) ||
        !safe_read_qword(handle_table + sizeof(uintptr_t) *
                             static_cast<uintptr_t>(handle & 0xFFFFFu),
                         gpu_wrapper) ||
        !fh5_read_bc7_wrapper_desc(gpu_wrapper, actual_w, actual_h,
                                   actual_fmt) ||
        actual_w != width || actual_h != height || actual_fmt != format) {
        fh5_release_gamepass_stock_wrapper(resource_wrapper);
        return 0;
    }

    std::array<uint8_t, 40> slot_desc{};
    if (!safe_memcpy(slot_desc.data(),
                     reinterpret_cast<const void*>(
                         descriptor_template_wrapper + 0x20),
                     slot_desc.size())) {
        fh5_release_gamepass_stock_wrapper(resource_wrapper);
        return 0;
    }

    uintptr_t slot_wrapper = 0;
    uint32_t slot_handle = handle;
    using BuildFn = volatile uintptr_t*(__fastcall*)(
        uintptr_t, volatile uintptr_t*, const uint32_t*, const void*);
    auto build_fn = reinterpret_cast<BuildFn>(
        module_base + profile.gp_build_stock_wrapper_rva);
    __try {
        build_fn(graphics_mgr,
                 reinterpret_cast<volatile uintptr_t*>(&slot_wrapper),
                 &slot_handle, slot_desc.data());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot_wrapper = 0;
    }
    fh5_release_gamepass_stock_wrapper(resource_wrapper);

    uint32_t slot_wrapper_handle = 0;
    uintptr_t slot_vtable = 0;
    if (!fh5_process_pointer(slot_wrapper) ||
        !safe_read_qword(slot_wrapper, slot_vtable) ||
        slot_vtable != module_base + profile.resource_vtable_rva ||
        !safe_read_u32(slot_wrapper + kFH5LogoUploadHandleOffset,
                       slot_wrapper_handle) ||
        slot_wrapper_handle != handle) {
        fh5_release_gamepass_stock_wrapper(slot_wrapper);
        return 0;
    }

    return slot_wrapper;
}

static bool fh5_swap_gamepass_stock_slot_wrapper(
    const Fh5LogoBuildProfile& profile,
    uintptr_t module_base,
    uintptr_t entry,
    uintptr_t expected_old_wrapper,
    uintptr_t new_wrapper,
    uintptr_t& old_wrapper,
    uint32_t& old_upload_handle,
    uint32_t& new_upload_handle) {
    old_wrapper = 0;
    old_upload_handle = 0;
    new_upload_handle = 0;
    if (!fh5_process_pointer(entry) || !fh5_process_pointer(new_wrapper) ||
        !fh5_process_pointer(expected_old_wrapper)) {
        return false;
    }
    uintptr_t refcount_table = 0;
    if (!safe_read_qword(module_base + profile.refcount_table_global_rva,
                         refcount_table) ||
        !fh5_process_pointer(refcount_table)) {
        return false;
    }
    uint32_t slot_handle = 0;
    uint32_t expected_old_handle = 0;
    uint32_t replacement_handle = 0;
    const uintptr_t slot_addr = entry + kFH5LogoResourceOffset;
    const uintptr_t slot_handle_addr = entry + kFH5LogoSlotHandleOffset;
    uintptr_t current = 0;
    uintptr_t new_vtable = 0;
    uintptr_t old_vtable = 0;
    uintptr_t old_release_fn = 0;
    if (!safe_read_qword(slot_addr, current) ||
        current != expected_old_wrapper ||
        !safe_read_qword(new_wrapper, new_vtable) ||
        new_vtable != module_base + profile.resource_vtable_rva ||
        !safe_read_u32(slot_handle_addr, slot_handle) ||
        !safe_read_u32(expected_old_wrapper + kFH5LogoUploadHandleOffset,
                       expected_old_handle) ||
        !safe_read_u32(new_wrapper + kFH5LogoUploadHandleOffset,
                       replacement_handle) ||
        slot_handle != expected_old_handle || expected_old_handle == 0 ||
        replacement_handle == 0 ||
        expected_old_handle == replacement_handle ||
        !safe_read_qword(expected_old_wrapper, old_vtable) ||
        old_vtable != module_base + profile.resource_vtable_rva ||
        !safe_read_qword(old_vtable + 0x10, old_release_fn) ||
        !fh5_process_pointer(old_release_fn)) {
        return false;
    }

    const uint32_t old_idx = expected_old_handle & 0xFFFFFu;
    const uint32_t new_idx = replacement_handle & 0xFFFFFu;
    uint32_t old_refcount = 0;
    if (!safe_read_u32(refcount_table + sizeof(uint32_t) * old_idx,
                       old_refcount) ||
        old_refcount < 2) {
        return false;
    }

    bool new_ref_incremented = false;
    bool old_slot_ref_decremented = false;
    bool slot_handle_replaced = false;
    long replaced_handle = 0;
    __try {
        ::InterlockedExchangeAdd(
            reinterpret_cast<volatile LONG*>(refcount_table +
                                             sizeof(uint32_t) * new_idx),
            1);
        new_ref_incremented = true;

        const LONG old_previous = ::InterlockedExchangeAdd(
            reinterpret_cast<volatile LONG*>(refcount_table +
                                             sizeof(uint32_t) * old_idx),
            -1);
        if (old_previous <= 1) {
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * old_idx),
                1);
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * new_idx),
                -1);
            return false;
        }
        old_slot_ref_decremented = true;

        replaced_handle = ::InterlockedExchange(
            reinterpret_cast<volatile LONG*>(slot_handle_addr),
            static_cast<LONG>(replacement_handle));
        slot_handle_replaced = true;
        if (static_cast<uint32_t>(replaced_handle) != expected_old_handle) {
            ::InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                      slot_handle_addr),
                                  replaced_handle);
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * old_idx),
                1);
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * new_idx),
                -1);
            return false;
        }

        old_wrapper = static_cast<uintptr_t>(
            _InterlockedExchange64(
                reinterpret_cast<volatile __int64*>(slot_addr),
                static_cast<__int64>(new_wrapper)));
        if (old_wrapper != expected_old_wrapper) {
            _InterlockedExchange64(
                reinterpret_cast<volatile __int64*>(slot_addr),
                static_cast<__int64>(old_wrapper));
            ::InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                      slot_handle_addr),
                                  replaced_handle);
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * old_idx),
                1);
            ::InterlockedExchangeAdd(
                reinterpret_cast<volatile LONG*>(refcount_table +
                                                 sizeof(uint32_t) * new_idx),
                -1);
            old_wrapper = 0;
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (old_wrapper && old_wrapper != expected_old_wrapper) {
            __try {
                _InterlockedExchange64(
                    reinterpret_cast<volatile __int64*>(slot_addr),
                    static_cast<__int64>(old_wrapper));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            old_wrapper = 0;
        }
        if (slot_handle_replaced) {
            __try {
                ::InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                          slot_handle_addr),
                                      replaced_handle);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (old_slot_ref_decremented) {
            __try {
                ::InterlockedExchangeAdd(
                    reinterpret_cast<volatile LONG*>(
                        refcount_table + sizeof(uint32_t) * old_idx),
                    1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (new_ref_incremented) {
            __try {
                ::InterlockedExchangeAdd(
                    reinterpret_cast<volatile LONG*>(
                        refcount_table + sizeof(uint32_t) * new_idx),
                    -1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return false;
    }

    (void)fh5_release_gamepass_stock_wrapper(old_wrapper);
    old_upload_handle = expected_old_handle;
    new_upload_handle = replacement_handle;
    return true;
}
#endif

// ---- Lifecycle ----

InProcessInjector::~InProcessInjector() {
    detach();
}

bool InProcessInjector::attach() {
    std::lock_guard lock(mtx_);

    if (discovery_done_.load()) return true;
    if (discovery_running_.load()) return true;

    // We're already in the game process — just get our own module info.
    HMODULE game = ::GetModuleHandleW(nullptr);
    if (!game) return false;

    MODULEINFO mi{};
    if (!::GetModuleInformation(::GetCurrentProcess(), game, &mi, sizeof(mi)))
        return false;

    module_base_ = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    module_size_ = mi.SizeOfImage;

    parse_pe_sections();

    log::info("[inject] In-process attach: base=" + hex(module_base_)
              + " size=" + std::to_string(module_size_ / (1024 * 1024)) + "MB");
    if (rdata_offset_ > 0) {
        log::info("[inject] .rdata at +" + hex(rdata_offset_)
                  + " size=" + std::to_string(rdata_size_ / (1024 * 1024)) + "MB");
    }

    // Resolve all FMOD / radio-state RVAs against the live image. Game
    // patches shift these by tens to hundreds of bytes per build, so the
    // mod refuses to bake any hex constants. A miss is non-fatal here:
    // FmodInject inspects rvas_ later and gracefully aborts if anything
    // it needs is zero.
    if (!resolve_signatures(module_base_, module_size_, active_profile(), rvas_,
                            /*quiet=*/true)) {
        log::info("[inject] resolve_signatures returned partial result; "
                  "will retry after FMOD wrappers warm up.");
    }

    start_discovery();
    start_speed_sampler();
    return true;
}

void InProcessInjector::retry_signatures() {
    if (!module_base_) return;
    std::lock_guard lock(mtx_);
    signature_retry_attempts_++;
    // The initial attach pass is quiet because startup timing misses often
    // self-resolve. Log details on the first retry only: if it still fails,
    // that is the second failed attempt and useful for diagnostics. Later
    // retries stay quiet so the UI warning ring is not spammed every 30s.
    bool quiet = signature_retry_attempts_ != 1;
    bool ok = resolve_signatures(module_base_, module_size_, active_profile(),
                                 rvas_, quiet);
    if (!ok && !quiet) {
        log::warn("[inject] resolve_signatures still partial after retry; "
                  "will keep retrying quietly.");
    }
}

bool InProcessInjector::is_attached() const {
    // Always in-process — we ARE the game.
    return module_base_ != 0;
}

void InProcessInjector::detach() {
    radio_logo_probe_running_ = false;
    if (radio_logo_probe_thread_.joinable()) {
        radio_logo_probe_thread_.join();
    }
    radio_logo_probe_required_ = false;
    radio_logo_probe_finished_ = true;
    speed_sampler_running_ = false;
    if (speed_sampler_thread_.joinable()) {
        speed_sampler_thread_.join();
    }
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
    std::lock_guard lock(mtx_);
    discovery_done_ = false;
    discovery_running_ = false;
    addrs_ = {};
}

void InProcessInjector::start_discovery() {
    if (discovery_running_.load() || discovery_done_.load()) return;
    if (discovery_thread_.joinable()) discovery_thread_.join();

    discovery_running_ = true;
    discovery_thread_ = std::thread([this]() {
        // Wait for game to initialise radio system.
        log::info("[inject] Discovery thread started, waiting 5s for radio init...");
        std::this_thread::sleep_for(std::chrono::seconds(5));

        for (int attempt = 0; attempt < 60; attempt++) {  // 60 * 5s = 5 min
            if (discover_addresses()) {
                discovery_done_ = true;
                discovery_running_ = false;
                log::info("[inject] Discovery complete — ready for metadata push.");
                return;
            }
            if (attempt < 59) {
                log::info("[inject] Discovery attempt " + std::to_string(attempt + 1)
                          + " failed, retrying in 5s...");
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
        discovery_running_ = false;
        log::error("[inject] Discovery gave up after 5 minutes.");
    });
}

void InProcessInjector::configure_runtime_radio_logo(
    std::filesystem::path logo_dir,
    bool album_art_enabled,
    bool custom_graphic_enabled,
    std::string spotify_variant,
    std::string active_source,
    std::string artwork_key,
    std::vector<uint8_t> artwork_bytes,
    bool artwork_loading) {
    if (spotify_variant != "color") spotify_variant = "white";
    if (active_source != "airplay" && active_source != "local" &&
        active_source != "radio" && active_source != "vanilla") {
        active_source = "spotify";
    }
    if (artwork_bytes.size() > 4 * 1024 * 1024) {
        artwork_key.clear();
        artwork_bytes.clear();
    }
    std::lock_guard lock(runtime_radio_logo_mtx_);
    bool artwork_changed =
        runtime_radio_logo_artwork_key_ != artwork_key ||
        runtime_radio_logo_artwork_bytes_.size() != artwork_bytes.size();
    bool changed =
        runtime_radio_logo_dir_ != logo_dir ||
        runtime_radio_logo_album_art_enabled_ != album_art_enabled ||
        runtime_radio_logo_custom_graphic_enabled_ != custom_graphic_enabled ||
        runtime_radio_logo_spotify_variant_ != spotify_variant ||
        runtime_radio_logo_active_source_ != active_source ||
        artwork_changed ||
        runtime_radio_logo_artwork_loading_ != artwork_loading;
    runtime_radio_logo_dir_ = std::move(logo_dir);
    runtime_radio_logo_album_art_enabled_ = album_art_enabled;
    runtime_radio_logo_custom_graphic_enabled_ = custom_graphic_enabled;
    runtime_radio_logo_spotify_variant_ = std::move(spotify_variant);
    runtime_radio_logo_active_source_ = std::move(active_source);
    runtime_radio_logo_artwork_key_ = std::move(artwork_key);
    runtime_radio_logo_artwork_bytes_ = std::move(artwork_bytes);
    runtime_radio_logo_artwork_loading_ = artwork_loading;
    if (changed) {
        ++runtime_radio_logo_revision_;
        runtime_radio_logo_wake_seq_.fetch_add(1, std::memory_order_acq_rel);
        runtime_radio_logo_cv_.notify_all();
        log::info("[logo-config] revision=" +
                  std::to_string(runtime_radio_logo_revision_) +
                  " album_art=" + std::to_string(album_art_enabled ? 1 : 0) +
                  " custom=" + std::to_string(custom_graphic_enabled ? 1 : 0) +
                  " variant=" + runtime_radio_logo_spotify_variant_ +
                  " artwork_changed=" + std::to_string(artwork_changed ? 1 : 0) +
                  " tick=" + std::to_string(::GetTickCount64()));
    }
}

void InProcessInjector::start_runtime_radio_logo_override() {
    radio_logo_probe_required_.store(true, std::memory_order_release);
    radio_logo_probe_finished_.store(false, std::memory_order_release);
    if (active_profile().id == GameId::FH5) {
        ui_stock_upload_test_.store(true, std::memory_order_release);
        if (radio_logo_probe_running_.exchange(true)) return;
        if (radio_logo_probe_thread_.joinable()) radio_logo_probe_thread_.join();
        radio_logo_probe_thread_ =
            std::thread([this]() { fh5_radio_logo_probe_loop(); });
        return;
    }
    // In-place BC7 upload into the engine-owned stock
    // Streamer logo resource. Survives garage/race/menu transitions, repaints
    // live, no created texture / alias / row recycle. Fails closed (no upload,
    // no crash) if the stock resource chain does not validate on this build,
    // and fails open on the Game-Ready gate after the timeout.
    ui_stock_upload_test_.store(true, std::memory_order_release);
    if (radio_logo_probe_running_.exchange(true)) return;
    if (radio_logo_probe_thread_.joinable()) radio_logo_probe_thread_.join();
    radio_logo_probe_thread_ =
        std::thread([this]() {
            radio_logo_cache_probe_loop(false, false, true, false);
        });
}

void InProcessInjector::radio_logo_cache_probe_loop(bool apply_vanilla_swap,
                                                    bool apply_custom_art,
                                                    bool stock_resource_validate,
                                                    bool inplace_update) {
    using namespace std::chrono_literals;

    constexpr uintptr_t kStockTextureWrapperVtableRva = 0x637C8B8;
    constexpr uintptr_t kUiResourceTableGlobalRva = 0x8EB3678;
    constexpr uintptr_t kUiResourceRefcountGlobalRva = 0x8EB3690;
    // In-place texture update primitive. Enqueues an async
    // GPU re-upload onto the engine's concurrent texture queue, drained by the
    // resource thread, against an EXISTING resource id. Dest geometry is read
    // by the engine from the existing resource, so the rid must already be a
    // created WxH RGBA8 texture. NOTE build-pinned RVA (build 23370889); a
    // signature must be added before this is promoted to production. Calling
    // (not patching) a resident game function does not trip .text CRC.
    constexpr uintptr_t kInPlaceTexUploadRva = 0x3522560;
    constexpr size_t kHandleEntryBytes = 32;
    constexpr size_t kMaxLinearRequests = 200000;
    constexpr size_t kMaxTreeNodes = 20000;
    constexpr size_t kMaxHits = 128;
    constexpr uintptr_t kRowScanRadius = 64ull * 1024ull * 1024ull;
    constexpr size_t kMaxCacheRows = 64;
    uintptr_t request_manager_rva = 0;
    uintptr_t texture_system_rva = 0;
    uintptr_t resource_registry_rva = 0;
    uintptr_t create_2d_raw_texture_rva = 0;
    uintptr_t linear_begin_rva = 0;
    uintptr_t linear_end_rva = 0;
    uintptr_t tree_header_rva = 0;
    uintptr_t handle_base_array_rva = 0;
    uintptr_t handle_stride_array_rva = 0;

    auto lower_copy = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };

    auto is_process_pointer = [](uintptr_t p) {
        return p >= 0x10000ull && p < 0x0000800000000000ull;
    };

    auto is_module_pointer = [&](uintptr_t p) {
        return p >= module_base_ && p < module_base_ + module_size_;
    };

    auto add_hit = [&](uintptr_t request,
                       const char* source,
                       std::vector<LogoCacheHit>& hits,
                       std::vector<uintptr_t>& seen) {
        if (!is_process_pointer(request)) return;
        if (std::find(seen.begin(), seen.end(), request) != seen.end()) return;
        seen.push_back(request);

        uintptr_t vtable = 0;
        uint32_t type = 0;
        uintptr_t wrapper = 0;
        if (!safe_read_qword(request, vtable) ||
            !is_module_pointer(vtable) ||
            !safe_read_u32(request + 0x8C, type) ||
            !safe_read_qword(request + 0x80, wrapper)) {
            return;
        }

        auto key_opt = read_game_string(request + 0x90);
        if (!key_opt || key_opt->empty()) return;

        std::string key_lower = lower_copy(*key_opt);
        const bool radio_logo_key =
            key_lower.find("radiologos") != std::string::npos ||
            key_lower.find("radio_logo") != std::string::npos ||
            key_lower.find("streamer_mode") != std::string::npos ||
            key_lower.find("horizon_pulse") != std::string::npos;
        if (!radio_logo_key) return;

        LogoCacheHit hit{};
        hit.source = source;
        hit.request = request;
        hit.type = type;
        hit.key = *key_opt;
        hit.wrapper = wrapper;
        if (is_process_pointer(wrapper)) {
            safe_read_qword(wrapper + 0x10, hit.code);
            safe_read_qword(wrapper + 0x18, hit.small_handle_pair);
            for (size_t i = 0; i < hit.wrapper_qwords.size(); ++i) {
                safe_read_qword(wrapper + i * sizeof(uintptr_t),
                                hit.wrapper_qwords[i]);
            }
        }
        safe_read_qword(request + 0x150, hit.resource_head);
        if (is_process_pointer(hit.resource_head)) {
            for (size_t i = 0; i < hit.resources.size(); ++i) {
                auto& entry = hit.resources[i];
                entry.address = hit.resource_head + i * 0x50;
                safe_read_qword(entry.address, entry.vtable);
                if (!is_module_pointer(entry.vtable)) {
                    entry.address = 0;
                    entry.vtable = 0;
                    continue;
                }
                safe_read_u32(entry.address + 0x10, entry.code);
                safe_read_u32(entry.address + 0x1C, entry.resource_id);
                safe_read_u32(entry.address + 0x28, entry.size_or_flags);
            }
        }

        if (hits.size() < kMaxHits) {
            hits.push_back(std::move(hit));
        }
    };

    auto walk_linear = [&](std::vector<LogoCacheHit>& hits,
                           std::vector<uintptr_t>& seen,
                           size_t& requests_seen) -> bool {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        uintptr_t cap = 0;
        if (!linear_begin_rva || !linear_end_rva ||
            !safe_read_qword(module_base_ + linear_begin_rva, begin) ||
            !safe_read_qword(module_base_ + linear_end_rva, end) ||
            !safe_read_qword(module_base_ + linear_end_rva + sizeof(uintptr_t), cap)) {
            return false;
        }
        if (!is_process_pointer(begin) || end < begin || cap < end ||
            ((end - begin) % sizeof(uintptr_t)) != 0) {
            return false;
        }

        const size_t count = (end - begin) / sizeof(uintptr_t);
        if (count > kMaxLinearRequests) return false;
        requests_seen += count;

        for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
            uintptr_t request = 0;
            if (!safe_read_qword(p, request)) continue;
            add_hit(request, "linear", hits, seen);
        }
        return true;
    };

    auto tree_node_is_nil = [&](uintptr_t node, bool& out) -> bool {
        uint8_t value = 1;
        if (!is_process_pointer(node) || !safe_read_u8(node + 25, value)) {
            return false;
        }
        out = value != 0;
        return true;
    };

    auto tree_successor = [&](uintptr_t node, uintptr_t header,
                              uintptr_t& next) -> bool {
        uintptr_t right = 0;
        if (!safe_read_qword(node + 16, right)) return false;
        bool right_nil = true;
        if (!tree_node_is_nil(right, right_nil)) return false;
        if (!right_nil) {
            uintptr_t cur = right;
            for (size_t guard = 0; guard < kMaxTreeNodes; ++guard) {
                uintptr_t left = 0;
                if (!safe_read_qword(cur, left)) return false;
                bool left_nil = true;
                if (!tree_node_is_nil(left, left_nil)) return false;
                if (left_nil) {
                    next = cur;
                    return true;
                }
                cur = left;
            }
            return false;
        }

        uintptr_t parent = 0;
        if (!safe_read_qword(node + 8, parent)) return false;
        for (size_t guard = 0; guard < kMaxTreeNodes; ++guard) {
            if (parent == header) {
                next = header;
                return true;
            }
            bool parent_nil = true;
            if (!tree_node_is_nil(parent, parent_nil)) return false;
            if (parent_nil) {
                next = parent;
                return true;
            }

            uintptr_t parent_right = 0;
            if (!safe_read_qword(parent + 16, parent_right)) return false;
            if (node != parent_right) {
                next = parent;
                return true;
            }
            node = parent;
            if (!safe_read_qword(parent + 8, parent)) return false;
        }
        return false;
    };

    auto tree_header_global_valid = [&](uintptr_t header_rva) -> bool {
        uintptr_t header = 0;
        uintptr_t left = 0;
        uintptr_t parent = 0;
        uintptr_t right = 0;
        uintptr_t count = 0;
        bool nil = false;
        return header_rva &&
               safe_read_qword(module_base_ + header_rva, header) &&
               is_process_pointer(header) &&
               tree_node_is_nil(header, nil) && nil &&
               safe_read_qword(header, left) && is_process_pointer(left) &&
               safe_read_qword(header + 8, parent) &&
               is_process_pointer(parent) &&
               safe_read_qword(header + 16, right) &&
               is_process_pointer(right) &&
               safe_read_qword(module_base_ + header_rva + 8, count) &&
               count <= kMaxTreeNodes;
    };

    auto walk_tree = [&](std::vector<LogoCacheHit>& hits,
                         std::vector<uintptr_t>& seen,
                         size_t& nodes_seen,
                         size_t& requests_seen) -> bool {
        uintptr_t header = 0;
        if (!tree_header_rva ||
            !safe_read_qword(module_base_ + tree_header_rva, header) ||
            !is_process_pointer(header)) {
            return false;
        }
        uintptr_t node = 0;
        if (!safe_read_qword(header, node)) return false;

        while (node && node != header && nodes_seen < kMaxTreeNodes) {
            bool nil = true;
            if (!tree_node_is_nil(node, nil)) return false;
            if (nil) break;
            ++nodes_seen;

            uintptr_t vec_begin = 0;
            uintptr_t vec_end = 0;
            if (safe_read_qword(node + 40, vec_begin) &&
                safe_read_qword(node + 48, vec_end) &&
                is_process_pointer(vec_begin) &&
                vec_end >= vec_begin &&
                ((vec_end - vec_begin) % sizeof(uintptr_t)) == 0) {
                const size_t count = (vec_end - vec_begin) / sizeof(uintptr_t);
                if (count <= kMaxLinearRequests) {
                    requests_seen += count;
                    for (uintptr_t p = vec_begin; p < vec_end; p += sizeof(uintptr_t)) {
                        uintptr_t request = 0;
                        if (!safe_read_qword(p, request)) continue;
                        add_hit(request, "tree", hits, seen);
                    }
                }
            }

            uintptr_t next = 0;
            if (!tree_successor(node, header, next)) return false;
            node = next;
        }
        return nodes_seen < kMaxTreeNodes;
    };

    auto log_hit = [](const LogoCacheHit& hit) {
        std::string key = hit.key;
        if (key.size() > 220) {
            key.resize(220);
            key += "...";
        }
        std::ostringstream ss;
        ss << "[logo-probe-cache] source=" << hit.source
           << " request=" << hex(hit.request)
           << " type=" << hit.type
           << " key=\"" << key << "\""
           << " wrapper=" << hex(hit.wrapper)
           << " code=0x" << std::hex << hit.code
           << " small=0x" << hit.small_handle_pair
           << " wq0=" << hit.wrapper_qwords[0]
           << " wq1=" << hit.wrapper_qwords[1]
           << " wq2=" << hit.wrapper_qwords[2]
           << " wq3=" << hit.wrapper_qwords[3]
           << " resource_head=" << hex(hit.resource_head)
           << std::dec;
        for (size_t i = 0; i < hit.resources.size(); ++i) {
            const auto& entry = hit.resources[i];
            if (!entry.address) continue;
            ss << " res" << i << "=" << hex(entry.address)
               << "/vt=" << hex(entry.vtable)
               << "/code=0x" << std::hex << entry.code
               << "/rid=0x" << entry.resource_id
               << "/size=0x" << entry.size_or_flags
               << std::dec;
        }
        log::info(ss.str());
    };

    constexpr uint32_t kCustomWidth = 392;
    constexpr uint32_t kCustomHeight = 208;
    constexpr uint32_t kCustomSquareSize = 208;
    constexpr uint32_t kCustomArtworkPaddingLeft = 0;
    constexpr uint32_t kAlbumArtworkBorderPx = 8;

    auto runtime_logo_settings = [&]() {
        RuntimeLogoSettings settings{};
        std::lock_guard lock(runtime_radio_logo_mtx_);
        settings.logo_dir = runtime_radio_logo_dir_;
        settings.album_art_enabled = runtime_radio_logo_album_art_enabled_;
        settings.custom_graphic_enabled =
            runtime_radio_logo_custom_graphic_enabled_;
        settings.spotify_variant = runtime_radio_logo_spotify_variant_;
        settings.active_source = runtime_radio_logo_active_source_;
        settings.artwork_key = runtime_radio_logo_artwork_key_;
        settings.artwork_bytes = runtime_radio_logo_artwork_bytes_;
        settings.artwork_loading = runtime_radio_logo_artwork_loading_;
        settings.revision = runtime_radio_logo_revision_;
        return settings;
    };

    const int max_iterations =
        (apply_custom_art ||
         ui_stock_upload_test_.load(std::memory_order_acquire))
            ? std::numeric_limits<int>::max()
            : (stock_resource_validate ? 180
                                       : (apply_vanilla_swap ? 1800 : 30));
    constexpr int kRuntimeLogoFailOpenIterations = 30; // 60s at 2s cadence.
    bool initial_cache_swap_done = false;
    size_t total_cache_swap_writes = 0;
    bool runtime_logo_signature_wait_logged = false;
    bool logo_resource_cache_ready = false;
    uintptr_t logged_logo_tree_offset = 0;

    log::info(std::string(apply_custom_art
                              ? "[logo-runtime] runtime radio logo override enabled"
                              : "[logo-probe-cache] resource request index probe enabled; mode=")
              + (apply_custom_art
                     ? ""
                     : (stock_resource_validate
                            ? "stock-resource-validate"
                            : (apply_vanilla_swap ? "cache-swap-refresh"
                                                  : "read-only")))
              + ((!apply_custom_art && apply_vanilla_swap)
                     ? "; releases readiness after first verified patch and refreshes for 60m"
                     : ""));

    for (int iter = 0;
         radio_logo_probe_running_.load() && iter < max_iterations;
         ++iter) {
        if (apply_custom_art && !rvas_.runtime_logo_ok()) {
            retry_signatures();
            if (!rvas_.runtime_logo_ok() && !runtime_logo_signature_wait_logged) {
                runtime_logo_signature_wait_logged = true;
                log::info("[logo-runtime] runtime logo signatures incomplete; "
                          "waiting for late resource/handle table initialization");
            }
        } else if (!apply_custom_art &&
                   (!rvas_.logo_request_manager ||
                    (stock_resource_validate && !rvas_.logo_texture_system) ||
                    (ui_stock_upload_test_.load(std::memory_order_acquire) &&
                     !rvas_.runtime_logo_stock_ok()))) {
            // Keep retrying until the stock upload signature set resolves;
            // sub_3522560 may be .text-encrypted at rest until the game first
            // loads the dial and calls into the upload path.
            retry_signatures();
        }

        request_manager_rva = rvas_.logo_request_manager;
        texture_system_rva = rvas_.logo_texture_system;
        resource_registry_rva = rvas_.logo_resource_registry;
        create_2d_raw_texture_rva = rvas_.logo_create_2d_raw_texture;
        linear_begin_rva = 0;
        linear_end_rva = 0;
        tree_header_rva = 0;
        handle_base_array_rva = rvas_.logo_handle_base_array;
        handle_stride_array_rva = rvas_.logo_handle_stride_array;

        uintptr_t manager = 0;
        if (request_manager_rva) {
            safe_read_qword(module_base_ + request_manager_rva, manager);
        }
        const bool manager_valid = is_process_pointer(manager);
        if (manager_valid) {
            // The vector triple immediately precedes the MSVC map header.
            // Find the header by its sentinel instead of pinning offsets: the
            // August 2026 update removed one earlier field and shifted the
            // legacy +0x50 header to +0x48 while preserving both containers.
            for (uintptr_t offset = 0x20; offset <= 0x80; offset += 8) {
                const uintptr_t candidate = request_manager_rva + offset;
                if (!tree_header_global_valid(candidate)) continue;
                tree_header_rva = candidate;
                linear_begin_rva = candidate - 0x18;
                linear_end_rva = candidate - 0x10;
                if (logged_logo_tree_offset != offset) {
                    logged_logo_tree_offset = offset;
                    log::info("[logo-runtime] detected request-cache layout "
                              "tree_offset=" + hex(offset));
                }
                break;
            }
        }
        if (apply_custom_art && manager && !manager_valid &&
            !initial_cache_swap_done) {
            initial_cache_swap_done = true;
            radio_logo_probe_finished_.store(true,
                                             std::memory_order_release);
            log::warn("[logo-runtime] resource manager RVA resolved to an "
                      "invalid pointer (" + hex(manager) + "); runtime logo "
                      "override disabled for this game build");
        }

        std::vector<LogoCacheHit> hits;
        std::vector<uintptr_t> seen;
        size_t linear_requests = 0;
        size_t tree_nodes = 0;
        size_t tree_requests = 0;
        const bool linear_ok =
            manager_valid && walk_linear(hits, seen, linear_requests);
        const bool tree_ok =
            manager_valid && walk_tree(hits, seen, tree_nodes, tree_requests);

        std::sort(hits.begin(), hits.end(),
                  [](const LogoCacheHit& a, const LogoCacheHit& b) {
                      if (a.key != b.key) return a.key < b.key;
                      return a.request < b.request;
                  });

        const bool verbose_iteration =
#if defined(SPOTIFY_RADIO_DIAG)
            !stock_resource_validate &&
            (!apply_vanilla_swap || !initial_cache_swap_done);
#else
            false;
#endif
        const bool summary_iteration =
#if defined(SPOTIFY_RADIO_DIAG)
            verbose_iteration || stock_resource_validate ||
            (apply_vanilla_swap && (iter % 30) == 0);
#else
            stock_resource_validate && ((iter % 15) == 0);
#endif
        if (summary_iteration) {
            log::info("[logo-probe-cache] iter=" + std::to_string(iter)
                      + " manager=" + hex(manager)
                      + " linear_ok=" + std::to_string(linear_ok)
                      + " linear_requests=" + std::to_string(linear_requests)
                      + " tree_ok=" + std::to_string(tree_ok)
                      + " tree_nodes=" + std::to_string(tree_nodes)
                      + " tree_requests=" + std::to_string(tree_requests)
                      + " hits=" + std::to_string(hits.size())
                      + " initial_done="
                      + std::to_string(initial_cache_swap_done)
                      + " total_writes="
                      + std::to_string(total_cache_swap_writes));
        }

        bool saw_streamer = false;
        bool saw_pulse = false;
        const LogoCacheHit* streamer_hit = nullptr;
        const LogoCacheHit* pulse_hit = nullptr;
        for (const auto& hit : hits) {
            std::string key_lower = lower_copy(hit.key);
            if (key_lower.find("streamer_mode") != std::string::npos) {
                saw_streamer = true;
                streamer_hit = &hit;
            }
            if (key_lower.find("horizon_pulse") != std::string::npos) {
                saw_pulse = true;
                pulse_hit = &hit;
            }
            if (verbose_iteration) log_hit(hit);
        }
        if (saw_streamer && streamer_hit && streamer_hit->resource_head) {
            logo_resource_cache_ready = true;
        }

#if defined(SPOTIFY_RADIO_DIAG)
        // Full candidate dump for failing-machine diagnosis: every radiologos
        // cache hit + all of its resource entries + the sig-resolve state. On a
        // machine where the logo never changes this shows whether the streamer
        // request exists under a different key/path, whether resources[0] holds
        // the rid/size we expect, and whether the upload sig resolved at all.
        if (stock_resource_validate && (iter < 10 || (iter % 15) == 0)) {
            log::info("[logo-diag] iter=" + std::to_string(iter)
                      + " hits=" + std::to_string(hits.size())
                      + " saw_streamer=" + std::to_string(saw_streamer)
                      + " manager_valid=" + std::to_string(manager_valid)
                      + " stock_ok=" + std::to_string(rvas_.runtime_logo_stock_ok())
                      + " rva_inplace=" + hex(rvas_.logo_inplace_upload)
                      + " rva_reqmgr=" + hex(rvas_.logo_request_manager)
                      + " rva_texsys=" + hex(rvas_.logo_texture_system));
            for (const auto& hit : hits) {
                std::string k = hit.key;
                if (k.size() > 200) { k.resize(200); k += "..."; }
                std::ostringstream hs;
                hs << "[logo-diag]   hit key=\"" << k << "\""
                   << " type=" << hit.type
                   << " head=" << hex(hit.resource_head);
                for (size_t i = 0; i < hit.resources.size(); ++i) {
                    const auto& e = hit.resources[i];
                    if (!e.address) continue;
                    hs << " r" << i << "{vt=" << hex(e.vtable)
                       << "/inmod=" << (is_module_pointer(e.vtable) ? 1 : 0)
                       << "/rid=0x" << std::hex << e.resource_id
                       << "/size=0x" << e.size_or_flags << std::dec << "}";
                }
                log::info(hs.str());
            }
        }
#endif

        if (stock_resource_validate) {
            // Replace EVERY cached streamer_mode.swatchbin variant. The game
            // caches the hi-res (\textures\hires\anthem\...) and/or the low-res
            // (\textures\anthem\...) one depending on display settings; some
            // machines have both. Upload to each at its own geometry so whatever
            // variant the dial actually reads is covered.
            int streamer_swatch_seen = 0;
            for (const auto& hit : hits) {
                std::string hit_key_lower = lower_copy(hit.key);
                if (hit_key_lower.find("streamer_mode.swatchbin") ==
                        std::string::npos) {
                    continue;
                }
                ++streamer_swatch_seen;
                const LogoCacheHit* stock_streamer_hit = &hit;
                const auto& streamer_res = stock_streamer_hit->resources[0];
                const uint32_t rid = streamer_res.resource_id;
                const uint32_t resource_index = rid & 0xFFFFFu;
                std::string key_lower = lower_copy(stock_streamer_hit->key);
                const bool is_streamer_swatch =
                    key_lower.find("streamer_mode.swatchbin") !=
                        std::string::npos;
                // sub_3522560 copies per the resource's OWN dimensions, so the
                // BC7 blob must match the live resource exactly. That size is
                // NOT constant: the stock dial-logo texture scales with UI /
                // display resolution (commonly 196x104, but 392x208 for some
                // users), so any fixed guess tiles the logo for one group and
                // over-fills it for another. We read the resource's real
                // width/height below (resource_object +0x88 / +0x8C) and size
                // the blob to that; is_hires_streamer only selects the cache
                // slot and the fallback guess.
                const bool is_hires_streamer =
                    is_streamer_swatch &&
                    key_lower.find("\\hires\\") != std::string::npos;
                // Prefer the sig-resolved wrapper vtable for cross-version; fall
                // back to the build-23370889 constant for the read-only probe.
                const uintptr_t expected_vtable =
                    module_base_ + (rvas_.logo_stock_wrapper_vtable
                                        ? rvas_.logo_stock_wrapper_vtable
                                        : kStockTextureWrapperVtableRva);
                const bool wrapper_vtable_ok =
                    streamer_res.vtable == expected_vtable;

                uintptr_t texture_system = 0;
                uintptr_t texture_system_field48 = 0;
                if (texture_system_rva) {
                    safe_read_qword(module_base_ + texture_system_rva,
                                    texture_system);
                    if (is_process_pointer(texture_system)) {
                        safe_read_qword(texture_system + 0x48,
                                        texture_system_field48);
                    }
                }

                uintptr_t resource_table = 0;
                uintptr_t resource_refcounts = 0;
                safe_read_qword(
                    module_base_ + (rvas_.logo_resource_table
                                        ? rvas_.logo_resource_table
                                        : kUiResourceTableGlobalRva),
                    resource_table);
                safe_read_qword(
                    module_base_ + (rvas_.logo_resource_refcounts
                                        ? rvas_.logo_resource_refcounts
                                        : kUiResourceRefcountGlobalRva),
                    resource_refcounts);

                uintptr_t resource_object = 0;
                uintptr_t resource_vtable = 0;
                uint32_t resource_refcount = 0;
                if (is_process_pointer(resource_table) && rid) {
                    safe_read_qword(resource_table +
                                        sizeof(uintptr_t) * resource_index,
                                    resource_object);
                    if (is_process_pointer(resource_object)) {
                        safe_read_qword(resource_object, resource_vtable);
                    }
                }
                if (is_process_pointer(resource_refcounts) && rid) {
                    safe_read_u32(resource_refcounts +
                                      sizeof(uint32_t) * resource_index,
                                  resource_refcount);
                }

                // The resource's REAL dimensions: width @ +0x88, height @ +0x8C
                // (both stored as u16). Size the BC7 blob to this, not a
                // per-resolution guess. Accept it only if it is a sane,
                // logo-shaped texture (same ~392:208 aspect, within bounds) so a
                // mis-resolved object on an unknown build falls back to the
                // cache-variant guess instead of encoding garbage dimensions.
                uint32_t res_w = 0, res_h = 0;
                if (is_process_pointer(resource_object)) {
                    uint32_t rw = 0, rh = 0;
                    if (safe_read_u32(resource_object + 0x88, rw)) {
                        res_w = rw & 0xFFFFu;
                    }
                    if (safe_read_u32(resource_object + 0x8C, rh)) {
                        res_h = rh & 0xFFFFu;
                    }
                }
                const bool res_dims_ok =
                    res_w >= 16u && res_w <= 4096u && res_h >= 16u &&
                    res_h <= 4096u &&
                    static_cast<uint64_t>(res_w) * kUiRenderFullLogoHeight *
                            100u <
                        static_cast<uint64_t>(res_h) * kUiRenderFullLogoWidth *
                            113u &&
                    static_cast<uint64_t>(res_h) * kUiRenderFullLogoWidth *
                            100u <
                        static_cast<uint64_t>(res_w) * kUiRenderFullLogoHeight *
                            113u;
                const uint32_t target_w =
                    res_dims_ok ? res_w : (is_hires_streamer ? 392u : 196u);
                const uint32_t target_h =
                    res_dims_ok ? res_h : (is_hires_streamer ? 208u : 104u);

                std::array<uintptr_t, 8> stock_qwords{};
                if (streamer_res.address) {
                    for (size_t i = 0; i < stock_qwords.size(); ++i) {
                        safe_read_qword(streamer_res.address +
                                            i * sizeof(uintptr_t),
                                        stock_qwords[i]);
                    }
                }

                // Cross-version validation gate. Only upload when the rid maps to
                // a REAL, live texture resource: exact streamer_mode.swatchbin
                // key + descriptor size 0x1688 + an in-EXE cache wrapper vtable +
                // a resolved texture system + the sig-resolved upload fn + the rid
                // resolving through the resource table to a live object that has a
                // valid vtable.
                //
                // The last requirement (resource_resolves) is CRITICAL and was
                // missing: on some Store/Game-Pass builds the resource constants
                // mis-resolve, so the rid does NOT back a live resource
                // (resource_table garbage, resource_object/vt = 0). Calling the
                // engine upload there makes it allocate ~GBs per call — a
                // commit-charge runaway that maxes out system memory (a few users
                // hit 32GB committed + an un-dismissable Windows low-memory
                // warning). When the chain does not fully resolve we now SKIP the
                // upload (logo simply stays vanilla on that build) rather than
                // risk that leak.
                const bool resource_resolves =
                    is_process_pointer(resource_table) &&
                    is_process_pointer(resource_object) &&
                    is_process_pointer(resource_vtable);
                const bool valid =
                    is_streamer_swatch &&
                    streamer_res.address &&
                    rid &&
                    streamer_res.size_or_flags == 0x1688 &&
                    is_module_pointer(streamer_res.vtable) &&
                    is_process_pointer(texture_system) &&
                    rvas_.logo_inplace_upload != 0;

                if (valid || summary_iteration) {
                    std::string key = stock_streamer_hit->key;
                    if (key.size() > 220) {
                        key.resize(220);
                        key += "...";
                    }
                    std::ostringstream ss;
                    ss << "[logo-probe-stock] request="
                       << hex(stock_streamer_hit->request)
                       << " key=\"" << key << "\""
                       << " is_hires=" << is_hires_streamer
                       << " tw=" << target_w << " th=" << target_h
                       << " resource_head="
                       << hex(stock_streamer_hit->resource_head)
                       << " entry=" << hex(streamer_res.address)
                       << " wrapper_vt=" << hex(streamer_res.vtable)
                       << " expected_vt=" << hex(expected_vtable)
                       << " wrapper_vt_ok=" << wrapper_vtable_ok
                       << " cache_code=0x" << std::hex << streamer_res.code
                       << " rid=0x" << rid
                       << " size=0x" << streamer_res.size_or_flags
                       << std::dec
                       << " texture_system_global="
                       << hex(module_base_ + texture_system_rva)
                       << " texture_system=" << hex(texture_system)
                       << " texture_system_48="
                       << hex(texture_system_field48)
                       << " resource_table=" << hex(resource_table)
                       << " resource_refcounts=" << hex(resource_refcounts)
                       << " resource_index=0x" << std::hex << resource_index
                       << std::dec
                       << " resource_object=" << hex(resource_object)
                       << " resource_vt=" << hex(resource_vtable)
                       << " resource_refcount="
                       << resource_refcount
                       << " q0=" << hex(stock_qwords[0])
                       << " q1=" << hex(stock_qwords[1])
                       << " q2=" << hex(stock_qwords[2])
                       << " q3=" << hex(stock_qwords[3])
                       << " q4=" << hex(stock_qwords[4])
                       << " q5=" << hex(stock_qwords[5])
                       << " v[swatch=" << is_streamer_swatch
                       << ",addr=" << (streamer_res.address ? 1 : 0)
                       << ",rid=" << (rid ? 1 : 0)
                       << ",size=" << (streamer_res.size_or_flags == 0x1688 ? 1 : 0)
                       << ",vtmod=" << (is_module_pointer(streamer_res.vtable) ? 1 : 0)
                       << ",texsys=" << (is_process_pointer(texture_system) ? 1 : 0)
                       << ",resolv=" << resource_resolves
                       << ",rva=" << (rvas_.logo_inplace_upload ? 1 : 0) << "]"
                       << " valid=" << valid;
                    log::info(ss.str());
                }

                if (valid && ui_stock_upload_test_.load(
                                 std::memory_order_acquire)) {
                    // B PRODUCTION: upload the REAL composed logo, BC7-encoded,
                    // into the engine-owned stock streamer-logo resource (rid)
                    // via the in-place queue primitive. The stock resource
                    // survives garage/race/menu transitions and is already wired
                    // to the dial, so NO created texture, NO rowbind, NO recycle.
                    // sub_3522560 copies per the resource's OWN dimensions, so
                    // the BC7 blob is sized to the resource's real width/height
                    // read above (target_w x target_h) -- commonly 196x104 or
                    // 392x208, but whatever the live resource actually is.
                    // class 0x0A (fmt 0x62). Encode
                    // only on logo revision change (cached per variant); upload
                    // only on content change or a post-transition restore burst
                    // (leak fix below) -- NOT every tick.
                    const uint32_t bc7_pitch = ((target_w + 3u) / 4u) * 16u;
                    const uint32_t bc7_slice =
                        bc7_pitch * ((target_h + 3u) / 4u);
                    const int var_idx = is_hires_streamer ? 0 : 1;
                    static uint64_t stock_loaded_rev[2] = {0, 0};
                    static std::vector<uint8_t> stock_bc7[2];
                    // Re-uploading every tick makes the
                    // engine's texture-upload queue accumulate ~6 GiB staging
                    // blocks (commit runaway -> ~30 GB). Upload only when the logo
                    // content changes (revision) or for a short burst after a
                    // game-state transition (garage/race/non-driving) -- which is
                    // when the engine re-streams the stock resource and wipes our
                    // BC7. Idle otherwise, so the engine's transient staging frees
                    // (~70 s) instead of growing.
                    static constexpr int kLogoRestoreBurstTicks = 4;
                    static uint64_t stock_uploaded_rev[2] = {~0ull, ~0ull};
                    static int stock_restore_burst[2] = {0, 0};
                    static uint64_t last_game_state_sig = ~0ull;
                    const uint64_t game_state_sig =
                        (runtime_logo_garage_candidate_.load(
                             std::memory_order_acquire) ? 1ull : 0ull) |
                        (runtime_logo_non_driving_candidate_.load(
                             std::memory_order_acquire) ? 2ull : 0ull) |
                        (runtime_logo_race_stinger_.load(
                             std::memory_order_acquire) ? 4ull : 0ull);
                    if (game_state_sig != last_game_state_sig) {
                        last_game_state_sig = game_state_sig;
                        stock_restore_burst[0] = kLogoRestoreBurstTicks;
                        stock_restore_burst[1] = kLogoRestoreBurstTicks;
                    }
                    RuntimeLogoSettings settings = runtime_logo_settings();
                    if (settings.revision != stock_loaded_rev[var_idx] ||
                        stock_bc7[var_idx].size() != bc7_slice) {
                        RuntimeLogoImage image =
                            compose_runtime_logo_image(settings);
                        // compose_runtime_logo_image always composes at the 392x208
                        // canvas. Box-resample it to the resource's REAL size
                        // (down for the common low-res dial, up for high-DPI) so
                        // the BC7 blob matches the resource exactly and never
                        // tiles or over-fills.
                        std::vector<uint8_t> out;
                        if (encode_runtime_logo_bc7_scaled(image, target_w,
                                                           target_h, out)) {
                            stock_bc7[var_idx] = std::move(out);
                            stock_loaded_rev[var_idx] = settings.revision;
                            log::info(
                                "[logo-probe-stock-upload] encoded logo "
                                "revision=" +
                                std::to_string(settings.revision) +
                                " key=" + image.key + " dims=" +
                                std::to_string(target_w) + "x" +
                                std::to_string(target_h) + " bytes=" +
                                std::to_string(bc7_slice));
                        } else {
                            log::warn("[logo-probe-stock-upload] BC7 encode "
                                      "failed dims=" +
                                      std::to_string(target_w) + "x" +
                                      std::to_string(target_h));
                        }
                    }
                    bool up_ok = false;
                    const bool logo_content_changed =
                        stock_uploaded_rev[var_idx] != settings.revision;
                    const bool logo_restore_pending =
                        stock_restore_burst[var_idx] > 0;
                    if ((logo_content_changed || logo_restore_pending) &&
                        stock_bc7[var_idx].size() == bc7_slice &&
                        rvas_.logo_inplace_upload) {
                        up_ok = inplace_upload_blob_via(
                            module_base_ + rvas_.logo_inplace_upload,
                            texture_system, rid, stock_bc7[var_idx].data(),
                            static_cast<uint64_t>(bc7_pitch),
                            static_cast<uint64_t>(bc7_slice), 0x0Au);
                        if (up_ok) {
                            stock_uploaded_rev[var_idx] = settings.revision;
                            if (stock_restore_burst[var_idx] > 0) {
                                --stock_restore_burst[var_idx];
                            }
                        }
                    }
                    if (summary_iteration) {
                        log::info("[logo-probe-stock-upload] real BC7 upload "
                                  "rid=0x" + [&]() { std::ostringstream s; s << std::hex << rid; return s.str(); }() +
                                  " dims=" + std::to_string(target_w) + "x" + std::to_string(target_h) +
                                  " loaded_rev=" + std::to_string(stock_loaded_rev[var_idx]) +
                                  " bc7_bytes=" + std::to_string(stock_bc7[var_idx].size()) +
                                  " upload_ok=" + std::to_string(up_ok ? 1 : 0) +
                                  " garage=" + std::to_string(runtime_logo_garage_candidate_.load(std::memory_order_acquire) ? 1 : 0));
                    }
                    if (!initial_cache_swap_done) {
                        initial_cache_swap_done = true;
                        radio_logo_probe_finished_.store(
                            true, std::memory_order_release);
                    }
                    // do NOT break: keep walking the per-variant cache hits.
                    // Re-upload is now gated (content change / post-transition
                    // burst) so the stock resource still restores after a
                    // re-stream, without the every-tick engine-staging leak.
                } else if (valid) {
                    initial_cache_swap_done = true;
                    radio_logo_probe_finished_.store(
                        true, std::memory_order_release);
                    log::info("[logo-probe-stock] stock Streamer resource "
                              "chain validated; next probe can test bounded "
                              "engine upload against the existing rid");
                }
            }  // for each cached streamer_mode.swatchbin variant
            if (!streamer_swatch_seen && summary_iteration) {
                log::info("[logo-probe-stock] waiting for exact Streamer "
                          "stock cache request");
            }
        }
        if ((apply_custom_art ||
             ui_stock_upload_test_.load(std::memory_order_acquire)) &&
            !initial_cache_swap_done &&
            iter + 1 >= kRuntimeLogoFailOpenIterations) {
            initial_cache_swap_done = true;
            radio_logo_probe_finished_.store(true, std::memory_order_release);
            log::info("[logo-runtime] first logo patch did not verify before timeout; injector readiness released, refresh loop continues");
        }

        if (apply_custom_art ||
            ui_stock_upload_test_.load(std::memory_order_acquire)) {
            // Wake immediately when a logo change bumps the wake seq (Web UI /
            // source / artwork), and still tick every 2s as a re-upload
            // persistence backstop for the stock-resource path.
            uint64_t wake_seq =
                runtime_radio_logo_wake_seq_.load(std::memory_order_acquire);
            std::unique_lock wake_lock(runtime_radio_logo_cv_mtx_);
            runtime_radio_logo_cv_.wait_for(wake_lock, 2s, [&]() {
                return !radio_logo_probe_running_.load(std::memory_order_acquire) ||
                       runtime_radio_logo_wake_seq_.load(
                           std::memory_order_acquire) != wake_seq;
            });
        } else {
            std::this_thread::sleep_for(2s);
        }
    }

    if (stock_resource_validate && !initial_cache_swap_done) {
        log::warn("[logo-probe-stock] stock Streamer resource chain did not "
                  "validate before timeout");
    }
    radio_logo_probe_finished_.store(true, std::memory_order_release);
    radio_logo_probe_running_.store(false, std::memory_order_release);
    log::info("[logo-probe-cache] resource request index probe finished");
}

void InProcessInjector::fh5_radio_logo_probe_loop() {
    using namespace std::chrono_literals;

    constexpr int kRuntimeLogoFailOpenIterations = 30; // 60s at 2s cadence.
    constexpr int kLogoRestoreBurstTicks = 4;
    constexpr uint64_t kUnresolvedLogoTargetScanMs = 1000;
    constexpr uint64_t kResolvedLogoTargetTickMs = 2000;

    auto finish = [&]() {
        radio_logo_probe_finished_.store(true, std::memory_order_release);
        radio_logo_probe_running_.store(false, std::memory_order_release);
    };

    const Fh5LogoBuildProfile* profile =
        fh5_logo_profile_for_text_size(text_size_);
    if (!profile) {
        log::warn("[fh5-logo] unsupported FH5 .text size " +
                  hex(static_cast<uintptr_t>(text_size_)) +
                  "; leaving stock Streamer Mode logo unchanged");
        finish();
        return;
    }
    radio_logo_probe_finished_.store(true, std::memory_order_release);
    if (!profile->upload_enabled) {
        log::warn("[fh5-logo] FH5 .text size " +
                  hex(static_cast<uintptr_t>(text_size_)) +
                  " has an opt-in logo profile; probing handles without "
                  "uploading unless the probe flag is present");
    }
    const bool gamepass_bcaware_queue_profile =
        profile->upload_abi == Fh5LogoUploadAbi::GamePassBcAwareQueue;
#if defined(SPOTIFY_RADIO_DIAG)
    const bool gamepass_staging_profile =
        profile->upload_abi == Fh5LogoUploadAbi::GamePassStagingCopy;
    const bool gamepass_ui_node_swap_profile =
        profile->upload_abi == Fh5LogoUploadAbi::GamePassUiNodeSwap;
    const bool gamepass_stock_slot_swap_profile =
        profile->upload_abi == Fh5LogoUploadAbi::GamePassStockSlotSwap;
    std::filesystem::path gamepass_staging_probe_flag;
    std::filesystem::path gamepass_ui_node_swap_flag;
    std::filesystem::path gamepass_stock_slot_swap_flag;
    std::filesystem::path gamepass_bcaware_upload_flag;
    {
        std::lock_guard lock(runtime_radio_logo_mtx_);
        if (!runtime_radio_logo_dir_.empty()) {
            gamepass_staging_probe_flag =
                runtime_radio_logo_dir_.parent_path() /
                kFH5GamePassStagingProbeFlag;
            gamepass_ui_node_swap_flag =
                runtime_radio_logo_dir_.parent_path() /
                kFH5GamePassUiNodeSwapFlag;
            gamepass_stock_slot_swap_flag =
                runtime_radio_logo_dir_.parent_path() /
                kFH5GamePassStockSlotSwapFlag;
            gamepass_bcaware_upload_flag =
                runtime_radio_logo_dir_.parent_path() /
                kFH5GamePassBcAwareUploadFlag;
        }
    }

    auto gamepass_staging_probe_enabled = [&]() {
        if (!gamepass_staging_profile ||
            gamepass_staging_probe_flag.empty()) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(gamepass_staging_probe_flag, ec) && !ec;
    };

    auto gamepass_ui_node_swap_enabled = [&]() {
        if (!gamepass_ui_node_swap_profile ||
            gamepass_ui_node_swap_flag.empty()) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(gamepass_ui_node_swap_flag, ec) && !ec;
    };

    auto gamepass_stock_slot_swap_enabled = [&]() {
        if (!gamepass_stock_slot_swap_profile ||
            gamepass_stock_slot_swap_flag.empty()) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(gamepass_stock_slot_swap_flag, ec) &&
               !ec;
    };

    auto gamepass_bcaware_upload_enabled = [&]() {
        if (!gamepass_bcaware_queue_profile ||
            gamepass_bcaware_upload_flag.empty()) {
            return false;
        }
        std::error_code ec;
        return std::filesystem::exists(gamepass_bcaware_upload_flag, ec) &&
               !ec;
    };
#endif

    auto is_process_pointer = [](uintptr_t p) {
        return p >= 0x10000ull && p < 0x0000800000000000ull;
    };

    auto runtime_logo_settings = [&]() {
        RuntimeLogoSettings settings{};
        std::lock_guard lock(runtime_radio_logo_mtx_);
        settings.logo_dir = runtime_radio_logo_dir_;
        settings.album_art_enabled = runtime_radio_logo_album_art_enabled_;
        settings.custom_graphic_enabled =
            runtime_radio_logo_custom_graphic_enabled_;
        settings.spotify_variant = runtime_radio_logo_spotify_variant_;
        settings.active_source = runtime_radio_logo_active_source_;
        settings.artwork_key = runtime_radio_logo_artwork_key_;
        settings.artwork_bytes = runtime_radio_logo_artwork_bytes_;
        settings.artwork_loading = runtime_radio_logo_artwork_loading_;
        settings.revision = runtime_radio_logo_revision_;
        return settings;
    };

    std::vector<Fh5LogoTarget> targets;
    bool initial_done = true;
    uint64_t last_game_state_sig = ~0ull;
    uint64_t last_scan_ms = 0;
    uint64_t next_all_res_scan_ms = 0;
    bool scan_logged_wait = false;
    bool all_res_scan_done = false;
    bool all_res_scan_started = false;
    uint64_t target_generation = 0;
    struct Fh5AllResScanResult {
        uint64_t generation = 0;
        bool found = false;
        std::vector<Fh5LogoTarget> targets;
        Fh5LogoScanStats stats{};
    };
    std::future<Fh5AllResScanResult> all_res_future;
    uint32_t gamepass_bcaware_upload_successes = 0;
#if defined(SPOTIFY_RADIO_DIAG)
    uint32_t gamepass_staging_successes = 0;
    uint32_t gamepass_ui_node_swap_successes = 0;
    uint32_t gamepass_stock_slot_swap_successes = 0;
#endif

    log::info("[fh5-logo] runtime Streamer Mode logo override enabled");

    for (int iter = 0; radio_logo_probe_running_.load(); ++iter) {
        const uint64_t loop_now = ::GetTickCount64();
        const uint64_t target_scan_retry_ms =
            targets.empty() ? kUnresolvedLogoTargetScanMs
                            : kResolvedLogoTargetTickMs;
        if (targets.empty() &&
            (last_scan_ms == 0 ||
             loop_now - last_scan_ms >= target_scan_retry_ms)) {
            last_scan_ms = loop_now;
            Fh5LogoScanStats stats{};
            const bool found = fh5_find_streamer_logo_targets(
                *profile, module_base_, targets, stats, false);
            if (found) {
                ++target_generation;
                next_all_res_scan_ms = loop_now + 1000;
                all_res_scan_done = false;
                all_res_scan_started = false;
            }
            const bool log_scan =
#if defined(SPOTIFY_RADIO_DIAG)
                iter < 3 || found || (iter % 15) == 0;
#else
                found || !scan_logged_wait;
#endif
            if (log_scan) {
                log::info("[fh5-logo] scan iter=" + std::to_string(iter) +
                          " mode=" + std::string(stats.mode) +
                          " found=" + std::to_string(found ? 1 : 0) +
                          " targets=" + std::to_string(targets.size()) +
                          " regions=" + std::to_string(stats.regions) +
                          " bytes=" + std::to_string(stats.bytes) +
                          " key_hits=" + std::to_string(stats.key_hits) +
                          " ref_hits=" + std::to_string(stats.ref_hits) +
                          " vt_hits=" + std::to_string(stats.vtable_hits) +
                          " candidates=" +
                          std::to_string(stats.candidates));
                scan_logged_wait = true;
            }
            for (const auto& t : targets) {
#if defined(SPOTIFY_RADIO_DIAG)
                std::ostringstream slots;
                for (const auto& slot : t.slots) {
                    if (slots.tellp() > 0) slots << ",";
                    slots << "off=0x" << std::hex << slot.resource_offset
                          << ":u=0x" << slot.upload_handle << ":r=0x"
                          << slot.render_handle << std::dec << ":"
                          << slot.width << "x" << slot.height;
                }
                log::info("[fh5-logo] target entry=" + hex(t.entry) +
                          " res=" + hex(t.resource) +
                          " ui_node=" + hex(t.ui_node) +
                          " ui_node_wrapper=" + hex(t.ui_node_wrapper) +
                          " render_wrapper=" + hex(t.render_wrapper) +
                          " upload_wrapper=" + hex(t.wrapper) +
                          " render_handle=0x" + [&]() {
                              std::ostringstream ss;
                              ss << std::hex << t.render_handle;
                              return ss.str();
                          }() +
                          " render_idx20=0x" + [&]() {
                              std::ostringstream ss;
                              ss << std::hex << (t.render_handle & 0xFFFFFull);
                              return ss.str();
                          }() +
                          " upload_handle=0x" + [&]() {
                              std::ostringstream ss;
                              ss << std::hex << t.upload_handle;
                              return ss.str();
                          }() +
                          " upload_dims=" + std::to_string(t.width) + "x" +
                          std::to_string(t.height) +
                          " render_dims=" + std::to_string(t.render_width) +
                          "x" + std::to_string(t.render_height) +
                          " refs=" + std::to_string(t.refcount) +
                          "/" + std::to_string(t.render_refcount) +
                          " slots=" + std::to_string(t.slots.size()) +
                          " [" + slots.str() + "]" +
                          " key=\"" + t.key + "\"");
#else
                log::info("[fh5-logo] target resolved mode=" +
                          std::string(stats.mode) +
                          " slots=" + std::to_string(t.slots.size()) +
                          " key=\"" + t.key + "\"");
#endif
            }
            const bool upload_allowed =
                profile->upload_enabled
#if defined(SPOTIFY_RADIO_DIAG)
                || gamepass_staging_probe_enabled() ||
                gamepass_ui_node_swap_enabled() ||
                gamepass_stock_slot_swap_enabled() ||
                gamepass_bcaware_upload_enabled()
#endif
                ;
            if (found && !upload_allowed) {
#if defined(SPOTIFY_RADIO_DIAG)
                if (gamepass_staging_profile || gamepass_ui_node_swap_profile ||
                    gamepass_bcaware_queue_profile) {
                    uintptr_t handle_table = 0;
                    uintptr_t refcount_table = 0;
                    safe_read_qword(module_base_ + profile->handle_table_global_rva,
                                    handle_table);
                    safe_read_qword(module_base_ + profile->refcount_table_global_rva,
                                    refcount_table);
                    for (const auto& t : targets) {
                        fh5_log_gamepass_streamer_probe(t, handle_table,
                                                        refcount_table);
                    }
                }
                finish();
                log::info("[fh5-logo] read-only logo probe stopped" +
                          (gamepass_staging_profile
                               ? std::string("; create spotify-radio\\") +
                                     kFH5GamePassStagingProbeFlag +
                                     " to enable the bounded staging-copy probe"
                           : gamepass_ui_node_swap_profile
                               ? std::string("; create spotify-radio\\") +
                                     kFH5GamePassUiNodeSwapFlag +
                                     " to enable the bounded UI-node wrapper-swap probe"
                           : gamepass_stock_slot_swap_profile
                               ? std::string("; create spotify-radio\\") +
                                     kFH5GamePassStockSlotSwapFlag +
                                     " to enable the bounded stock-slot wrapper-swap probe"
                           : gamepass_bcaware_queue_profile
                               ? std::string("; create spotify-radio\\") +
                                     kFH5GamePassBcAwareUploadFlag +
                                     " to enable the bounded BC-aware queue-upload probe"
                                : std::string{}));
#else
                log::info("[fh5-logo] read-only logo probe stopped");
#endif
                return;
            }
        }
        if (!targets.empty() && !all_res_scan_done &&
            next_all_res_scan_ms != 0 && loop_now >= next_all_res_scan_ms &&
            !all_res_scan_started) {
            const Fh5LogoBuildProfile profile_copy = *profile;
            const uintptr_t base_copy = module_base_;
            const uint64_t generation = target_generation;
            all_res_scan_started = true;
            all_res_future = std::async(
                std::launch::async,
                [profile_copy, base_copy, generation]() mutable {
                    Fh5AllResScanResult result{};
                    result.generation = generation;
                    result.found = fh5_find_streamer_logo_targets(
                        profile_copy, base_copy, result.targets, result.stats,
                        true);
                    return result;
                });
        }
        if (all_res_scan_started && all_res_future.valid() &&
            all_res_future.wait_for(0s) == std::future_status::ready) {
            Fh5AllResScanResult result = all_res_future.get();
            all_res_scan_started = false;
            all_res_scan_done = true;
            if (result.found && result.generation == target_generation &&
                !targets.empty()) {
                const size_t before = targets.size();
                for (auto& candidate : result.targets) {
                    const bool duplicate =
                        std::any_of(targets.begin(), targets.end(),
                                    [&](const Fh5LogoTarget& existing) {
                                        return existing.entry == candidate.entry ||
                                               existing.resource ==
                                                   candidate.resource ||
                                               existing.upload_handle ==
                                                   candidate.upload_handle;
                                    });
                    if (!duplicate) {
                        targets.push_back(std::move(candidate));
                    }
                }
                if (targets.size() != before) {
                    log::info("[fh5-logo] additional Streamer logo targets="
                              + std::to_string(targets.size() - before) +
                              " total=" + std::to_string(targets.size()));
                }
#if defined(SPOTIFY_RADIO_DIAG)
                log::info("[fh5-logo] all-resolution scan mode=" +
                          std::string(result.stats.mode) +
                          " found=" + std::to_string(result.found ? 1 : 0) +
                          " targets=" +
                          std::to_string(result.targets.size()) +
                          " regions=" +
                          std::to_string(result.stats.regions) +
                          " bytes=" + std::to_string(result.stats.bytes) +
                          " key_hits=" +
                          std::to_string(result.stats.key_hits) +
                          " ref_hits=" +
                          std::to_string(result.stats.ref_hits) +
                          " vt_hits=" +
                          std::to_string(result.stats.vtable_hits) +
                          " candidates=" +
                          std::to_string(result.stats.candidates));
#endif
            }
        }

        uintptr_t upload_mgr = 0;
        safe_read_qword(module_base_ + profile->upload_manager_global_rva,
                        upload_mgr);
        const bool upload_mgr_ok = is_process_pointer(upload_mgr);

        const uint64_t game_state_sig =
            (runtime_logo_garage_candidate_.load(std::memory_order_acquire)
                 ? 1ull
                 : 0ull) |
            (runtime_logo_non_driving_candidate_.load(
                 std::memory_order_acquire)
                 ? 2ull
                 : 0ull) |
            (runtime_logo_race_stinger_.load(std::memory_order_acquire) ? 4ull
                                                                        : 0ull);
        if (game_state_sig != last_game_state_sig) {
            last_game_state_sig = game_state_sig;
            for (auto& t : targets) t.restore_burst = kLogoRestoreBurstTicks;
        }

        RuntimeLogoSettings settings = runtime_logo_settings();
        bool any_target_valid = false;
        bool any_upload_ok = false;
        bool any_target_invalid = false;

        for (auto& target : targets) {
            Fh5LogoTarget latest{};
            if (!fh5_validate_logo_target(*profile, module_base_, target.entry,
                                          latest)) {
                any_target_invalid = true;
                continue;
            }
            any_target_valid = true;
            auto previous_slots = std::move(target.slots);
            for (auto& slot : latest.slots) {
                const auto old =
                    std::find_if(previous_slots.begin(), previous_slots.end(),
                                 [&](const auto& existing) {
                                     return existing.upload_handle ==
                                                slot.upload_handle &&
                                            existing.width == slot.width &&
                                            existing.height == slot.height;
                                 });
                if (old != previous_slots.end()) {
                    slot.loaded_revision = old->loaded_revision;
                    slot.uploaded_revision = old->uploaded_revision;
                    slot.loaded_width = old->loaded_width;
                    slot.loaded_height = old->loaded_height;
                    slot.successful_uploads = old->successful_uploads;
                    slot.bc7 = std::move(old->bc7);
                }
            }
            target.path = latest.path;
            target.resource = latest.resource;
            target.wrapper = latest.wrapper;
            target.render_wrapper = latest.render_wrapper;
            target.render_handle = latest.render_handle;
            target.upload_handle = latest.upload_handle;
            target.width = latest.width;
            target.height = latest.height;
            target.format = latest.format;
            target.render_width = latest.render_width;
            target.render_height = latest.render_height;
            target.render_format = latest.render_format;
            target.refcount = latest.refcount;
            target.render_refcount = latest.render_refcount;
            target.ui_node = latest.ui_node;
            target.ui_node_wrapper = latest.ui_node_wrapper;
            target.key = std::move(latest.key);
            target.slots = std::move(latest.slots);

            bool uploaded_this_iter = false;
#if defined(SPOTIFY_RADIO_DIAG)
            if (gamepass_ui_node_swap_profile) {
                const bool content_changed =
                    target.uploaded_revision != settings.revision;
                const bool restore_pending = target.restore_burst > 0;
                const bool upload_allowed = gamepass_ui_node_swap_enabled();
                const bool cap_available =
                    gamepass_ui_node_swap_successes <
                    kFH5GamePassUiNodeSwapMaxSuccessfulUploads;
                bool upload_ok = false;
                uintptr_t old_wrapper = 0;
                uintptr_t new_wrapper = 0;
                RuntimeLogoImage image{};
                if (upload_allowed && cap_available &&
                    (content_changed || restore_pending)) {
                    if (!target.ui_node ||
                        !fh5_validate_ui_png_node(*profile, module_base_,
                                                  target.entry, target.ui_node,
                                                  old_wrapper)) {
                        log::warn("[fh5-logo] ui-node-swap skipped; Streamer "
                                  "binding node is missing or stale");
                    } else {
                        image = compose_runtime_logo_image(settings);
                        new_wrapper = fh5_create_gamepass_ui_logo_wrapper(
                            *profile, module_base_, image, target.width,
                            target.height);
                        if (new_wrapper &&
                            safe_write_qword(target.ui_node + 0x38,
                                             new_wrapper)) {
                            target.ui_node_wrapper = new_wrapper;
                            target.uploaded_revision = settings.revision;
                            ++gamepass_ui_node_swap_successes;
                            uploaded_this_iter = true;
                            any_upload_ok = true;
                            upload_ok = true;
                        } else if (new_wrapper) {
                            fh5_release_gamepass_ui_logo_wrapper(
                                *profile, module_base_, new_wrapper);
                            new_wrapper = 0;
                        }
                    }
                }

                const bool summary_iteration =
#if defined(SPOTIFY_RADIO_DIAG)
                    iter < 5 || upload_ok || (iter % 15) == 0;
#else
                    upload_ok;
#endif
                if (summary_iteration) {
                    log::info("[fh5-logo] ui-node-swap iter=" +
                              std::to_string(iter) +
                              " node=" + hex(target.ui_node) +
                              " old_wrapper=" + hex(old_wrapper) +
                              " new_wrapper=" + hex(new_wrapper) +
                              " current_wrapper=" +
                              hex(target.ui_node_wrapper) +
                              " dims=" + std::to_string(target.width) + "x" +
                              std::to_string(target.height) +
                              " fmt=87 allowed=" +
                              std::to_string(upload_allowed ? 1 : 0) +
                              " cap=" +
                              std::to_string(cap_available ? 1 : 0) +
                              " gp_successes=" +
                              std::to_string(gamepass_ui_node_swap_successes) +
                              " content=" +
                              std::to_string(content_changed ? 1 : 0) +
                              " restore=" +
                              std::to_string(restore_pending ? 1 : 0) +
                              " release_old=0 key=\"" + image.key +
                              "\" ok=" + std::to_string(upload_ok ? 1 : 0));
                }
                if (uploaded_this_iter && target.restore_burst > 0) {
                    --target.restore_burst;
                }
                continue;
            }
            if (gamepass_stock_slot_swap_profile) {
                const uint32_t bc7_pitch = ((target.width + 3u) / 4u) * 16u;
                const uint32_t bc7_slice =
                    bc7_pitch * ((target.height + 3u) / 4u);
                RuntimeLogoImage image{};
                if (target.loaded_revision != settings.revision ||
                    target.loaded_width != target.width ||
                    target.loaded_height != target.height ||
                    target.bc7.size() != bc7_slice) {
                    image = compose_runtime_logo_image(settings);
                    std::vector<uint8_t> encoded;
                    if (encode_runtime_logo_bc7_scaled(
                            image, target.width, target.height, encoded)) {
                        target.bc7 = std::move(encoded);
                        target.loaded_revision = settings.revision;
                        target.loaded_width = target.width;
                        target.loaded_height = target.height;
                        log::info("[fh5-logo] stock-slot encoded revision=" +
                                  std::to_string(settings.revision) +
                                  " key=" + image.key +
                                  " dims=" + std::to_string(target.width) +
                                  "x" + std::to_string(target.height) +
                                  " bytes=" +
                                  std::to_string(target.bc7.size()));
                    } else {
                        log::warn("[fh5-logo] stock-slot BC7 encode failed "
                                  "dims=" +
                                  std::to_string(target.width) + "x" +
                                  std::to_string(target.height));
                    }
                }

                const bool content_changed =
                    target.uploaded_revision != settings.revision;
                const bool restore_pending = target.restore_burst > 0;
                const bool upload_allowed =
                    gamepass_stock_slot_swap_enabled();
                const bool cap_available =
                    gamepass_stock_slot_swap_successes <
                    kFH5GamePassStockSlotSwapMaxSuccessfulUploads;
                bool upload_ok = false;
                uintptr_t old_wrapper = 0;
                uintptr_t new_wrapper = 0;
                uint32_t old_upload_handle = 0;
                uint32_t new_upload_handle = 0;
                const uintptr_t expected_old_wrapper = target.resource;
                if (upload_allowed && upload_mgr_ok && cap_available &&
                    target.bc7.size() == bc7_slice &&
                    (content_changed || restore_pending)) {
                    new_wrapper = fh5_create_gamepass_stock_logo_wrapper(
                        *profile, module_base_, upload_mgr,
                        expected_old_wrapper, target.bc7.data(), target.width,
                        target.height, target.format, bc7_pitch, bc7_slice);
                    if (new_wrapper &&
                        fh5_swap_gamepass_stock_slot_wrapper(
                            *profile, module_base_, target.entry,
                            expected_old_wrapper, new_wrapper, old_wrapper,
                            old_upload_handle, new_upload_handle)) {
                        target.resource = new_wrapper;
                        target.upload_handle = new_upload_handle;
                        target.uploaded_revision = settings.revision;
                        ++gamepass_stock_slot_swap_successes;
                        uploaded_this_iter = true;
                        any_upload_ok = true;
                        upload_ok = true;
                    } else if (new_wrapper) {
                        fh5_release_gamepass_stock_wrapper(new_wrapper);
                        new_wrapper = 0;
                    }
                }

                const bool summary_iteration =
#if defined(SPOTIFY_RADIO_DIAG)
                    iter < 5 || upload_ok || (iter % 15) == 0;
#else
                    upload_ok;
#endif
                if (summary_iteration) {
                    log::info("[fh5-logo] stock-slot-swap iter=" +
                              std::to_string(iter) +
                              " entry=" + hex(target.entry) +
                              " old_wrapper=" + hex(old_wrapper) +
                              " expected_old=" + hex(expected_old_wrapper) +
                              " new_wrapper=" + hex(new_wrapper) +
                              " old_handle=0x" + [&]() {
                                  std::ostringstream hs;
                                  hs << std::hex << old_upload_handle;
                                  return hs.str();
                              }() +
                              " new_handle=0x" + [&]() {
                                  std::ostringstream hs;
                                  hs << std::hex << new_upload_handle;
                                  return hs.str();
                              }() +
                              " dims=" + std::to_string(target.width) + "x" +
                              std::to_string(target.height) +
                              " fmt=0x" + [&]() {
                                  std::ostringstream fs;
                                  fs << std::hex << target.format;
                                  return fs.str();
                              }() +
                              " bytes=" + std::to_string(target.bc7.size()) +
                              " graphics_mgr=" + hex(upload_mgr) +
                              " allowed=" +
                              std::to_string(upload_allowed ? 1 : 0) +
                              " cap=" +
                              std::to_string(cap_available ? 1 : 0) +
                              " gp_successes=" +
                              std::to_string(
                                  gamepass_stock_slot_swap_successes) +
                              " content=" +
                              std::to_string(content_changed ? 1 : 0) +
                              " restore=" +
                              std::to_string(restore_pending ? 1 : 0) +
                              " key=\"" + image.key +
                              "\" ok=" + std::to_string(upload_ok ? 1 : 0));
                }
                if (uploaded_this_iter && target.restore_burst > 0) {
                    --target.restore_burst;
                }
                continue;
            }
#endif
            for (auto& slot : target.slots) {
                const uint32_t bc7_pitch = ((slot.width + 3u) / 4u) * 16u;
                const uint32_t bc7_slice =
                    bc7_pitch * ((slot.height + 3u) / 4u);
                if (slot.loaded_revision != settings.revision ||
                    slot.loaded_width != slot.width ||
                    slot.loaded_height != slot.height ||
                    slot.bc7.size() != bc7_slice) {
                RuntimeLogoImage image = compose_runtime_logo_image(settings);
                std::vector<uint8_t> encoded;
                    if (encode_runtime_logo_bc7_scaled(
                            image, slot.width, slot.height, encoded)) {
                    slot.bc7 = std::move(encoded);
                    slot.loaded_revision = settings.revision;
                    slot.loaded_width = slot.width;
                    slot.loaded_height = slot.height;
#if defined(SPOTIFY_RADIO_DIAG)
                    std::ostringstream hs;
                    hs << std::hex << slot.upload_handle;
                    log::info("[fh5-logo] encoded revision=" +
                              std::to_string(settings.revision) +
                              " key=" + image.key +
                                  " handle=0x" + hs.str() +
                              " dims=" + std::to_string(slot.width) + "x" +
                              std::to_string(slot.height) +
                              " bytes=" +
                              std::to_string(slot.bc7.size()));
#endif
                } else {
                    log::warn("[fh5-logo] BC7 encode failed dims=" +
                              std::to_string(slot.width) + "x" +
                              std::to_string(slot.height));
                    continue;
                }
            }

            const bool content_changed =
                    slot.uploaded_revision != settings.revision;
            const bool restore_pending = target.restore_burst > 0;
            const bool upload_allowed =
                profile->upload_enabled
#if defined(SPOTIFY_RADIO_DIAG)
                || (gamepass_staging_profile &&
                    gamepass_staging_probe_enabled()) ||
                (gamepass_bcaware_queue_profile &&
                 gamepass_bcaware_upload_enabled())
#endif
                ;
            const bool limited_gamepass_upload =
#if defined(SPOTIFY_RADIO_DIAG)
                !profile->upload_enabled &&
                (gamepass_staging_profile || gamepass_bcaware_queue_profile);
#else
                false;
#endif
            const uint32_t gamepass_upload_successes =
#if defined(SPOTIFY_RADIO_DIAG)
                gamepass_bcaware_queue_profile ? gamepass_bcaware_upload_successes
                                                : gamepass_staging_successes;
            const uint32_t gamepass_upload_cap =
                gamepass_bcaware_queue_profile
                    ? kFH5GamePassBcAwareUploadMaxSuccessfulUploads
                    : kFH5GamePassStagingMaxSuccessfulUploads;
#else
                gamepass_bcaware_upload_successes;
            const uint32_t gamepass_upload_cap = 0;
#endif
            const bool staging_cap_available =
                !limited_gamepass_upload ||
                (slot.successful_uploads == 0 &&
                 gamepass_upload_successes < gamepass_upload_cap);
            const uint64_t destination_handle = slot.upload_handle;
            bool upload_ok = false;
                if (upload_allowed && upload_mgr_ok &&
                    staging_cap_available && slot.bc7.size() == bc7_slice &&
                    (content_changed || restore_pending)) {
                upload_ok = fh5_upload_blob_via(
                    profile->upload_abi,
                    module_base_ + profile->upload_rva, upload_mgr,
                    profile->upload_state_rva
                        ? module_base_ + profile->upload_state_rva
                        : 0,
                    slot.upload_handle, slot.render_handle, slot.bc7.data(),
                    static_cast<uint64_t>(bc7_pitch),
                    static_cast<uint64_t>(bc7_slice), slot.width, slot.height,
                    slot.format);
                if (upload_ok) {
                        slot.uploaded_revision = settings.revision;
                        ++slot.successful_uploads;
#if defined(SPOTIFY_RADIO_DIAG)
                        if (gamepass_staging_profile) {
                            ++gamepass_staging_successes;
                        } else if (gamepass_bcaware_queue_profile) {
                            ++gamepass_bcaware_upload_successes;
                        }
#else
                        if (gamepass_bcaware_queue_profile) {
                            ++gamepass_bcaware_upload_successes;
                        }
#endif
                        uploaded_this_iter = true;
                    any_upload_ok = true;
                }
            }

            const bool summary_iteration =
#if defined(SPOTIFY_RADIO_DIAG)
                iter < 5 || upload_ok || (iter % 15) == 0;
#else
                upload_ok;
#endif
            if (summary_iteration) {
#if defined(SPOTIFY_RADIO_DIAG)
                const uint32_t logged_gamepass_successes =
                    gamepass_bcaware_queue_profile
                        ? gamepass_bcaware_upload_successes
                        : gamepass_staging_successes;
                std::ostringstream hs;
                    hs << std::hex << destination_handle;
                std::ostringstream uh;
                    uh << std::hex << slot.upload_handle;
                std::ostringstream rh;
                    rh << std::hex << slot.render_handle;
                log::info("[fh5-logo] upload iter=" + std::to_string(iter) +
                          " dst=0x" + hs.str() +
                          " upload_handle=0x" + uh.str() +
                          " render_handle=0x" + rh.str() +
                          " dims=" + std::to_string(slot.width) + "x" +
                          std::to_string(slot.height) +
                          " fmt=0x" + [&]() {
                              std::ostringstream fs;
                              fs << std::hex << slot.format;
                              return fs.str();
                          }() +
                          " bytes=" + std::to_string(slot.bc7.size()) +
                          " mgr=" + hex(upload_mgr) +
                          " abi=" +
#if defined(SPOTIFY_RADIO_DIAG)
                          (gamepass_staging_profile
                               ? "gp-staging"
                               : gamepass_bcaware_queue_profile
                                    ? "gp-bc-aware"
                                    : "steam-low") +
#else
                          std::string(gamepass_bcaware_queue_profile
                                          ? "gp-bc-aware"
                                          : "steam-low") +
#endif
                          " allowed=" +
                          std::to_string(upload_allowed ? 1 : 0) +
                          " cap=" +
                          std::to_string(staging_cap_available ? 1 : 0) +
                          " gp_successes=" +
                          std::to_string(logged_gamepass_successes) +
                          " content=" +
                          std::to_string(content_changed ? 1 : 0) +
                          " restore=" +
                          std::to_string(restore_pending ? 1 : 0) +
                          " ok=" + std::to_string(upload_ok ? 1 : 0));
#else
                log::info("[fh5-logo] updated Streamer Mode logo revision=" +
                          std::to_string(settings.revision) +
                          " mode=" +
                          std::string(gamepass_bcaware_queue_profile
                                          ? "gamepass-bc-aware"
                                          : "steam-low"));
#endif
            }
        }
            if (uploaded_this_iter && target.restore_burst > 0) {
                --target.restore_burst;
            }
        }

        if (any_target_invalid) {
            log::warn("[fh5-logo] cached target invalidated; rescanning");
            targets.clear();
            ++target_generation;
            next_all_res_scan_ms = 0;
            all_res_scan_done = false;
        }

        if (!initial_done && any_upload_ok) {
            initial_done = true;
            radio_logo_probe_finished_.store(true, std::memory_order_release);
        }
        if (!initial_done && iter + 1 >= kRuntimeLogoFailOpenIterations) {
            initial_done = true;
            radio_logo_probe_finished_.store(true, std::memory_order_release);
            log::info("[fh5-logo] first logo patch did not verify before "
                      "timeout; injector readiness released, refresh loop "
                      "continues");
        }

        uint64_t wake_seq =
            runtime_radio_logo_wake_seq_.load(std::memory_order_acquire);
        std::unique_lock wake_lock(runtime_radio_logo_cv_mtx_);
        runtime_radio_logo_cv_.wait_for(
            wake_lock, std::chrono::milliseconds(target_scan_retry_ms), [&]() {
            return !radio_logo_probe_running_.load(std::memory_order_acquire) ||
                   runtime_radio_logo_wake_seq_.load(
                       std::memory_order_acquire) != wake_seq;
        });
        (void)any_target_valid;
    }

    finish();
    log::info("[fh5-logo] runtime logo probe stopped");
}

// ---- PE parsing ----

void InProcessInjector::parse_pe_sections() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        module_base_ + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const auto* first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const auto& sec = first[i];
        if (strncmp(reinterpret_cast<const char*>(sec.Name), ".text", 8) == 0) {
            text_size_ = sec.Misc.VirtualSize;
        }
        if (strncmp(reinterpret_cast<const char*>(sec.Name), ".rdata", 8) == 0) {
            rdata_offset_ = sec.VirtualAddress;
            rdata_size_ = sec.Misc.VirtualSize;
        }
    }
}

// ---- Heap scan ----

std::vector<uintptr_t> InProcessInjector::scan_heap(uintptr_t target_vtable) const {
    std::vector<uintptr_t> hits;

    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0;
    size_t bytes_scanned = 0;
    size_t regions_scanned = 0;
    constexpr size_t MAX_REGION = 64 * 1024 * 1024;

    while (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi))) {
        uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

        // Only committed, private, readable, non-executable, non-image regions.
        const DWORD prot = mbi.Protect & 0xFF;
        bool readable = (prot == PAGE_READWRITE) ||
                        (prot == PAGE_WRITECOPY) ||
                        (prot == PAGE_READONLY);
        bool ok = (mbi.State == MEM_COMMIT) &&
                  (mbi.Type == MEM_PRIVATE) &&
                  readable &&
                  !(mbi.Protect & PAGE_GUARD) &&
                  (mbi.RegionSize <= MAX_REGION) &&
                  !(base >= module_base_ && base < module_base_ + module_size_);

        if (ok) {
            regions_scanned++;
            bytes_scanned += mbi.RegionSize;

            uintptr_t start = (base + 15) & ~uintptr_t(15);
            uintptr_t stop  = (base + mbi.RegionSize) & ~uintptr_t(7);

            // Fast validated scan: checks vtable + inner vtable in one pass.
            // Single SEH frame, direct reads.
            constexpr size_t MAX_HITS_PER_REGION = 64;
            uintptr_t region_hits[MAX_HITS_PER_REGION];
            size_t region_count = 0;
            scan_region_validated(start, stop, target_vtable,
                                  module_base_, module_size_,
                                  region_hits, MAX_HITS_PER_REGION,
                                  region_count);
            for (size_t i = 0; i < region_count && i < MAX_HITS_PER_REGION; i++) {
                hits.push_back(region_hits[i]);
            }

            // Early exit once we find validated hits — real instances are
            // contiguous, and inline validation eliminates false positives.
            if (!hits.empty()) break;
        }

        uintptr_t next = base + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    log::info("[inject] Heap scan: " + std::to_string(regions_scanned) + " regions, "
              + std::to_string(bytes_scanned / (1024 * 1024)) + " MB, "
              + std::to_string(hits.size()) + " hits");
    return hits;
}

bool InProcessInjector::refresh_radio_stream_instances() {
    std::lock_guard lock(mtx_);
    if (!discovery_done_.load() || !addrs_.radio_stream_refcount_vt) {
        return false;
    }

    auto hits = scan_heap(addrs_.radio_stream_refcount_vt);
    if (hits.empty()) {
        log::warn("[inject] RadioStreamFmod refresh found no instances");
        return false;
    }

    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());

    bool changed = hits != addrs_.radio_stream_instances;
    addrs_.radio_stream_instances = std::move(hits);
    log::info("[inject] RadioStreamFmod refresh: instances="
              + std::to_string(addrs_.radio_stream_instances.size())
              + " changed=" + std::to_string(changed ? 1 : 0));
    return true;
}

// ---- Discovery ----

bool InProcessInjector::discover_addresses() {
    std::lock_guard lock(mtx_);

    if (discovery_done_ && addrs_.valid()) return true;

    auto t0 = std::chrono::steady_clock::now();

    const uint8_t* image = reinterpret_cast<const uint8_t*>(module_base_);

    // STEP 1: Find _Ref_count_obj2<RadioStreamFmod> typedesc via image string scan.
    //
    // Typedesc layout (x64):
    //   +0x00: pVFTable
    //   +0x08: spare (0)
    //   +0x10: name[] — null-terminated mangled name starting with ".?AV"

    uintptr_t typedesc_addr = 0;
    {
        const char* needle = "RadioStreamFmod";
        constexpr size_t needle_len = 15;

        for (size_t i = 0; i + needle_len < module_size_; i++) {
            if (memcmp(image + i, needle, needle_len) != 0) continue;

            // Check for "_Ref_count_obj2" before this match
            size_t lookback = std::min(i, (size_t)128);
            bool has_refcount = false;
            for (size_t b = 0; b + 15 <= lookback; b++) {
                if (memcmp(image + i - lookback + b, "_Ref_count_obj2", 15) == 0) {
                    has_refcount = true;
                    break;
                }
            }
            if (!has_refcount) continue;

            // Walk back to ".?AV" prefix (typedesc.name start)
            for (size_t b = 1; b < lookback; b++) {
                if (image[i - b] == '.' && i - b + 3 < module_size_ &&
                    image[i - b + 1] == '?' &&
                    image[i - b + 2] == 'A' &&
                    image[i - b + 3] == 'V') {
                    typedesc_addr = module_base_ + (i - b) - 0x10;
                    goto found_typedesc;
                }
            }
        }
    }
found_typedesc:

    if (!typedesc_addr) {
        log::warn("[inject] Could not find _Ref_count_obj2<RadioStreamFmod> typedesc.");
        return false;
    }

    uint32_t typedesc_rva = static_cast<uint32_t>(typedesc_addr - module_base_);
    log::info("[inject] typedesc RVA=" + hex(typedesc_rva));

    // STEP 2: Find COL (Complete Object Locator) referencing this typedesc.
    // COL layout (x64, 24 bytes):
    //   +0x00: u32 signature = 1
    //   +0x04: u32 offset
    //   +0x08: u32 cdOffset
    //   +0x0C: u32 typeDescRVA
    //   +0x10: u32 classHierDescRVA
    //   +0x14: u32 selfRVA

    uintptr_t col_addr = 0;
    {
        for (size_t i = 0; i + 24 <= module_size_; i += 4) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(image + i);
            if (p[3] != typedesc_rva) continue;  // +0x0C
            if (p[0] != 1) continue;              // +0x00 signature
            uint32_t self_rva = p[5];              // +0x14
            if (self_rva == static_cast<uint32_t>(i)) {
                col_addr = module_base_ + i;
                break;
            }
        }
    }

    if (!col_addr) {
        log::warn("[inject] Could not find COL.");
        return false;
    }
    log::info("[inject] COL RVA=" + hex(static_cast<uintptr_t>(col_addr - module_base_)));

    // STEP 3: Find vtable candidates — scan .rdata for qword pointing to COL.
    // vtable[-1] = COL pointer, vtable[0] starts at that address + 8.
    //
    // .rdata can have transient false positives (game modifies pages at runtime),
    // so we collect ALL candidates and validate each via heap scan + refcount
    // check before committing.

    auto scan_for_col = [&](uintptr_t start, size_t size) {
        std::vector<uintptr_t> hits;
        const uint8_t* scan = reinterpret_cast<const uint8_t*>(start);
        for (size_t i = 0; i + 8 <= size; i += 8) {
            if (*reinterpret_cast<const uintptr_t*>(scan + i) == col_addr) {
                hits.push_back(start + i + 8);
            }
        }
        return hits;
    };

    // Scan .rdata first (fast). If all candidates fail validation later,
    // fall back to full-image scan.  We use a two-pass approach:
    //   pass 0 = .rdata only,  pass 1 = full image.
    std::vector<uintptr_t> vtable_candidates;
    int scan_pass = -1;  // will be set during the scan loop below

    // STEPS 4+5: For each vtable candidate, scan heap, validate refcounts,
    // and walk pointer chain to SampleProperties.
    //
    // _Ref_count_obj2:  +0x00 vtable, +0x08 _Uses(u32), +0x0C _Weaks(u32),
    //                   +0x10 RadioStreamFmod starts
    // RadioStreamFmod:  +0x00 own_vtable, +0x08 FMOD_Sound*
    //
    // Chain: inst + 0x58 → deref → +0x18 → deref = SampleProperties
    // SampleProperties: +0x10 SoundName, +0x30 DisplayName, +0x50 Artist

    bool found_valid = false;
    for (scan_pass = 0; scan_pass < 2 && !found_valid; scan_pass++) {
        if (scan_pass == 0 && rdata_offset_ > 0) {
            vtable_candidates = scan_for_col(module_base_ + rdata_offset_, rdata_size_);
            log::info("[inject] .rdata scan: " + std::to_string(vtable_candidates.size())
                      + " candidate(s)");
        } else if (scan_pass == 0) {
            // No .rdata section — skip to full image
            continue;
        } else {
            vtable_candidates = scan_for_col(module_base_, module_size_);
            log::info("[inject] Full image scan: " + std::to_string(vtable_candidates.size())
                      + " candidate(s)");
        }

        for (auto vtable_addr : vtable_candidates) {
            uintptr_t vtable_rva = vtable_addr - module_base_;
            log::info("[inject] Trying vtable RVA=" + hex(vtable_rva));

            // Quick validation: vtable[-1] should still point to COL.
            uintptr_t verify_col = 0;
            if (!safe_read_qword(vtable_addr - 8, verify_col) || verify_col != col_addr) {
                log::info("[inject]   vtable[-1] mismatch (transient match), skipping");
                continue;
            }

            // Additional validation: vtable[0] should point into module code.
            uintptr_t vfunc0 = 0;
            if (!safe_read_qword(vtable_addr, vfunc0) ||
                vfunc0 < module_base_ || vfunc0 >= module_base_ + module_size_) {
                log::info("[inject]   vtable[0] not in module, skipping");
                continue;
            }

            auto instances = scan_heap(vtable_addr);
            if (instances.empty()) {
                log::info("[inject]   No heap instances, skipping");
                continue;
            }

            // Instances already validated by scan (vtable + inner vtable check).
            // Log first few for diagnostics.
            for (size_t i = 0; i < instances.size() && i < 5; i++) {
                uint32_t uses = 0, weaks = 0;
                safe_memcpy(&uses,  reinterpret_cast<void*>(instances[i] + 8),  4);
                safe_memcpy(&weaks, reinterpret_cast<void*>(instances[i] + 12), 4);
                log::info("[inject]   inst " + hex(instances[i])
                          + " uses=" + std::to_string(uses)
                          + " weaks=" + std::to_string(weaks));
            }
            auto& valid_instances = instances;

            log::info("[inject] " + std::to_string(valid_instances.size())
                      + " validated _Ref_count_obj2<RadioStreamFmod> instances");
            addrs_.radio_stream_refcount_vt = vtable_addr;
            addrs_.radio_stream_instances = valid_instances;

            // Walk pointer chain to SampleProperties on each instance.
            for (auto inst : valid_instances) {
                uintptr_t fmod_ptr = 0;
                if (!safe_read_qword(inst + 0x18, fmod_ptr) || fmod_ptr == 0) continue;

                addrs_.active_radio_stream = inst;
                log::info("[inject] Active instance at " + hex(inst)
                          + " FMOD Sound*=" + hex(fmod_ptr));

                uintptr_t ptr1 = 0;
                if (!safe_read_qword(inst + 0x58, ptr1) || ptr1 == 0) {
                    log::warn("[inject] Chain broken at refcount+0x58");
                    continue;
                }

                uintptr_t ptr2 = 0;
                if (!safe_read_qword(ptr1 + 0x18, ptr2) || ptr2 == 0) {
                    log::warn("[inject] Chain broken at deref+0x18");
                    continue;
                }

                addrs_.sample_properties = ptr2;
                log::info("[inject] SampleProperties at " + hex(ptr2));

                auto sound   = read_game_string(ptr2 + 0x10);
                auto display = read_game_string(ptr2 + 0x30);
                auto artist  = read_game_string(ptr2 + 0x50);

                if (sound)   log::info("[inject]   SoundName:   \"" + *sound + "\"");
                if (display) log::info("[inject]   DisplayName: \"" + *display + "\"");
                if (artist)  log::info("[inject]   Artist:      \"" + *artist + "\"");

                break;
            }

            found_valid = true;
            break;
        }
    }

    if (!found_valid) {
        // Discovery retries while the radio heap is still settling. This can
        // happen transiently before a later pass succeeds, so keep it out of
        // the UI warning ring.
        log::info("[inject] All vtable candidates failed validation.");
        return false;
    }

    auto t2 = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t0).count();
    log::info("[inject] Discovery complete in " + std::to_string(total_ms) + "ms");

    return !addrs_.radio_stream_instances.empty();
}

// ---- Pointer chain refresh ----

bool InProcessInjector::refresh_sample_properties() {
    if (addrs_.radio_stream_instances.empty()) return false;

    for (auto inst : addrs_.radio_stream_instances) {
        uintptr_t fmod_ptr = 0;
        if (!safe_read_qword(inst + 0x18, fmod_ptr) || fmod_ptr == 0) continue;

        addrs_.active_radio_stream = inst;

        uintptr_t ptr1 = 0;
        if (!safe_read_qword(inst + 0x58, ptr1) || ptr1 == 0) continue;

        uintptr_t ptr2 = 0;
        if (!safe_read_qword(ptr1 + 0x18, ptr2) || ptr2 == 0) continue;

        addrs_.sample_properties = ptr2;
        return true;
    }

    return false;
}

// ---- Vehicle-speed sampler ----
//
// Night Runners speed source. Previous inline hooks, vtable-slot hooks,
// thread-context probes, HWBP probes, and heap-motion/registry classifiers were
// rejected because they crashed, hung, or selected non-player cars after car
// swaps. The active source resolves the official Data Out packet producer and
// mirrors its local PlayerCarTelemetry root with read-only pointer reads. It
// still derives the local interface/vtable/speed-field facts from the
// publish-speed path, does not call game functions, does not patch game .text,
// does not mutate vtables, and fails closed when validation cannot prove the
// local speed field.

namespace {

// Version-resolving; public releases use the same sampler. The public logging
// policy still prevents diagnostic logs from being written on disk unless
// SPOTIFY_RADIO_DIAG is explicitly defined.
static constexpr bool kSpeedTelemetryVerifierEnabled = true;
#if defined(SPOTIFY_RADIO_DIAG)
static constexpr bool kSpeedTelemetryDetailLogs = true;
#else
static constexpr bool kSpeedTelemetryDetailLogs = false;
#endif

static constexpr auto kSpeedFreshWindow = std::chrono::milliseconds(1500);
static constexpr auto kSpeedSampleInterval = std::chrono::milliseconds(50);
static constexpr float kMaxPlausibleSpeedMps = 140.0f;
static constexpr uintptr_t kDataOutTelemetryListBeginDisp = 0x18;
static constexpr uintptr_t kDataOutTelemetryListEndDisp = 0x20;
static constexpr uintptr_t kDataOutTelemetryConfigDisp = 0x2E0;
static constexpr uintptr_t kDataOutTelemetryActiveDisp = 0x9350;
static constexpr uintptr_t kDataOutTelemetryInactiveDisp = 0x9353;
static constexpr uintptr_t kDataOutTelemetryRetiredDisp = 0x9147;
static constexpr uintptr_t kDataOutTelemetryConfigSuppressedDisp = 0x426;

struct ImageRange {
    uintptr_t start = 0;
    uintptr_t end = 0;
};

struct SpeedTelemetryVtable {
    uintptr_t vtable = 0;
    uintptr_t getter = 0;
    int32_t speed_disp = 0;
};

enum class SpeedTelemetryMode {
    ScalarField,
    VelocityVector,
};

struct SpeedTelemetryResolver {
    uintptr_t local_iface_accessor = 0;
    uintptr_t publish_callsite = 0;
    uintptr_t dataout_source_callsite = 0;
    uintptr_t dataout_accessor_selector_func = 0;
    uintptr_t dataout_list_selector_func = 0;
    uintptr_t dataout_manager_global_rva = 0;
    uintptr_t speed_multiplier_rva = 0;
    uint32_t dataout_manager_provider_disp = 0;
    uint32_t dataout_current_root_disp = 0;
    uint32_t dataout_telemetry_list_owner_disp = 0;
    uint32_t dataout_active_disp = kDataOutTelemetryActiveDisp;
    uint32_t dataout_inactive_disp = kDataOutTelemetryInactiveDisp;
    uint32_t dataout_retired_disp = kDataOutTelemetryRetiredDisp;
    uint32_t dataout_config_suppressed_disp = kDataOutTelemetryConfigSuppressedDisp;
    uint32_t local_iface_disp = 0;
    uint32_t publish_vfunc_offset = 0;
    uint32_t velocity_vfunc_offset = 0;
    float speed_multiplier = 0.0f;
    size_t local_accessor_hits = 0;
    size_t speed_getter_hits = 0;
    SpeedTelemetryMode mode = SpeedTelemetryMode::ScalarField;
    std::vector<SpeedTelemetryVtable> iface_vtables;
};

struct SpeedTelemetryEntry {
    size_t index = 0;
    uintptr_t telemetry = 0;
    uintptr_t iface = 0;
    uintptr_t vtable = 0;
    uintptr_t getter = 0;
    int32_t speed_disp = 0;
    float speed_mps = 0.0f;
};

struct SpeedDataOutStats {
    uintptr_t manager = 0;
    uintptr_t provider = 0;
    uintptr_t root = 0;
    uintptr_t list_owner = 0;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    size_t entries = 0;
    size_t valid = 0;
    size_t null_entries = 0;
    size_t unreadable_entries = 0;
    size_t inactive_entries = 0;
    size_t bad_iface = 0;
    size_t bad_vtable = 0;
    size_t bad_speed = 0;
};

static std::mutex g_speed_direct_resolver_mtx;
static uintptr_t g_speed_direct_resolver_base = 0;
static bool g_speed_direct_resolver_ready = false;
static uint64_t g_speed_direct_last_attempt_ms = 0;
static SpeedTelemetryResolver g_speed_direct_resolver{};

static std::string fmt_float(float v, int precision = 2) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed, std::ios::floatfield);
    oss.precision(precision);
    oss << v;
    return oss.str();
}

static bool get_image_section(uintptr_t module_base,
                              const char* section_name,
                              ImageRange& out) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        module_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* first = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const auto& sec = first[i];
        if (strncmp(reinterpret_cast<const char*>(sec.Name),
                    section_name, 8) == 0) {
            out.start = module_base + sec.VirtualAddress;
            out.end = out.start + sec.Misc.VirtualSize;
            return out.start < out.end;
        }
    }
    return false;
}

static bool pattern_at(const uint8_t* p,
                       const uint8_t* pattern,
                       const char* mask,
                       size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (mask[i] == '?') continue;
        if (p[i] != pattern[i]) return false;
    }
    return true;
}

static std::vector<uintptr_t> find_pattern_in_range(const ImageRange& range,
                                                    const uint8_t* pattern,
                                                    const char* mask,
                                                    size_t len) {
    std::vector<uintptr_t> hits;
    if (!range.start || range.end <= range.start || len == 0 ||
        range.start + len > range.end) {
        return hits;
    }

    size_t first_fixed = 0;
    while (first_fixed < len && mask[first_fixed] == '?') ++first_fixed;
    if (first_fixed == len) return hits;

    const auto* start = reinterpret_cast<const uint8_t*>(range.start);
    const auto* end = reinterpret_cast<const uint8_t*>(range.end - len);
    for (const auto* p = start; p <= end; ++p) {
        if (p[first_fixed] != pattern[first_fixed]) continue;
        if (pattern_at(p, pattern, mask, len)) {
            hits.push_back(reinterpret_cast<uintptr_t>(p));
        }
    }

    return hits;
}

static uintptr_t rip_target(uintptr_t instr, size_t rel_offset, size_t instr_len) {
    int32_t rel = 0;
    if (!safe_read_i32(instr + rel_offset, rel)) return 0;
    return instr + instr_len + static_cast<intptr_t>(rel);
}

static uintptr_t call_target(uintptr_t instr) {
    if (!instr) return 0;
    uint8_t opcode = 0;
    if (!safe_read_u8(instr, opcode) || opcode != 0xE8) return 0;
    return rip_target(instr, 1, 5);
}

static bool read_u32_disp(uintptr_t addr, uint32_t& out) {
    return safe_read_u32(addr, out);
}

static std::string rva_or_zero(uintptr_t module_base, uintptr_t addr) {
    return addr ? hex(addr - module_base) : std::string("0x0");
}

static std::string vtable_list_rvas(uintptr_t module_base,
                                    const std::vector<SpeedTelemetryVtable>& vtables) {
    std::string out;
    for (size_t i = 0; i < vtables.size(); ++i) {
        if (i) out += ",";
        out += hex(vtables[i].vtable - module_base) +
               ":getter=" + hex(vtables[i].getter - module_base) +
               ":disp=" + hex(static_cast<uintptr_t>(
                   static_cast<intptr_t>(vtables[i].speed_disp)));
    }
    return out;
}

static uint64_t monotonic_ms() {
    return static_cast<uint64_t>(::GetTickCount64());
}

static bool parse_dataout_accessor_chain(uintptr_t selector_func,
                                         uint32_t& provider_disp,
                                         uint32_t& current_root_disp,
                                         std::string& error) {
    // Data Out source selector shape:
    //   mov rcx, [rcx + provider_disp]
    //   test rcx, rcx
    //   jnz current_root_accessor
    //   xor eax, eax
    //   ret
    uint8_t head[19]{};
    if (!safe_memcpy(head, reinterpret_cast<const void*>(selector_func),
                     sizeof(head))) {
        error = "dataout selector unreadable";
        return false;
    }
    if (head[0] != 0x48 || head[1] != 0x8B || head[2] != 0x89 ||
        head[7] != 0x48 || head[8] != 0x85 || head[9] != 0xC9 ||
        head[10] != 0x0F || head[11] != 0x85) {
        error = "dataout selector pattern mismatch";
        return false;
    }
    memcpy(&provider_disp, head + 3, sizeof(provider_disp));
    uintptr_t current_accessor = rip_target(selector_func + 10, 2, 6);
    if (!current_accessor) {
        error = "dataout current accessor target unresolved";
        return false;
    }

    uint8_t accessor[8]{};
    if (!safe_memcpy(accessor, reinterpret_cast<const void*>(current_accessor),
                     sizeof(accessor)) ||
        accessor[0] != 0x48 || accessor[1] != 0x8B ||
        accessor[2] != 0x81 || accessor[7] != 0xC3) {
        error = "dataout current accessor pattern mismatch";
        return false;
    }
    memcpy(&current_root_disp, accessor + 3, sizeof(current_root_disp));
    return true;
}

static bool parse_dataout_list_selector(uintptr_t list_selector_func,
                                        uint32_t& list_owner_disp,
                                        std::string& error) {
    // The producer helper fetches the telemetry list owner with:
    //   mov rbp, [rcx + list_owner_disp]
    // in the function prologue area. Parse this instead of baking the RVA.
    constexpr size_t kScanBytes = 96;
    uint8_t bytes[kScanBytes]{};
    if (!safe_memcpy(bytes, reinterpret_cast<const void*>(list_selector_func),
                     sizeof(bytes))) {
        error = "dataout list selector unreadable";
        return false;
    }
    for (size_t i = 0; i + 7 <= kScanBytes; ++i) {
        if (bytes[i] == 0x48 && bytes[i + 1] == 0x8B &&
            bytes[i + 2] == 0xA9) {
            memcpy(&list_owner_disp, bytes + i + 3, sizeof(list_owner_disp));
            return true;
        }
    }
    error = "dataout list owner displacement missing";
    return false;
}

static bool parse_byte_accessor_disp(uintptr_t func, uint32_t& disp) {
    uint8_t bytes[8]{};
    if (!safe_memcpy(bytes, reinterpret_cast<const void*>(func), sizeof(bytes))) {
        return false;
    }
    if (bytes[0] == 0x0F && bytes[1] == 0xB6 && bytes[2] == 0x81 &&
        bytes[7] == 0xC3) {
        memcpy(&disp, bytes + 3, sizeof(disp));
        return true;
    }
    return false;
}

static bool parse_lea_accessor_disp(uintptr_t func, uint32_t& disp) {
    uint8_t bytes[8]{};
    if (!safe_memcpy(bytes, reinterpret_cast<const void*>(func), sizeof(bytes))) {
        return false;
    }
    if (bytes[0] == 0x48 && bytes[1] == 0x8D && bytes[2] == 0x81 &&
        bytes[7] == 0xC3) {
        memcpy(&disp, bytes + 3, sizeof(disp));
        return true;
    }
    return false;
}

static bool parse_velocity_vector_getter(uintptr_t func, int32_t& disp) {
    uint8_t bytes[16]{};
    if (!safe_memcpy(bytes, reinterpret_cast<const void*>(func), sizeof(bytes))) {
        return false;
    }
    if (bytes[0] == 0x0F && bytes[1] == 0x10 && bytes[2] == 0x81 &&
        bytes[7] == 0x48 && bytes[8] == 0x8B && bytes[9] == 0xC2 &&
        bytes[10] == 0x0F && bytes[11] == 0x29 && bytes[12] == 0x02 &&
        bytes[13] == 0xC3) {
        memcpy(&disp, bytes + 3, sizeof(disp));
        return true;
    }
    return false;
}

static bool parse_dataout_entry_state_filters(uintptr_t list_selector_func,
                                              uint32_t& inactive_disp,
                                              uint32_t& active_disp,
                                              uint32_t& config_suppressed_disp,
                                              std::string& error) {
    constexpr size_t kScanBytes = 192;
    uint8_t bytes[kScanBytes]{};
    if (!safe_memcpy(bytes, reinterpret_cast<const void*>(list_selector_func),
                     sizeof(bytes))) {
        error = "dataout list selector unreadable";
        return false;
    }

    std::vector<uint32_t> byte_disps;
    for (size_t i = 0; i + 5 <= kScanBytes; ++i) {
        if (bytes[i] != 0xE8) continue;
        int32_t rel = 0;
        memcpy(&rel, bytes + i + 1, sizeof(rel));
        uintptr_t target = list_selector_func + i + 5 + rel;
        uint32_t disp = 0;
        if (parse_byte_accessor_disp(target, disp)) {
            byte_disps.push_back(disp);
        }
    }
    if (byte_disps.size() < 2) {
        error = "dataout active/inactive accessors missing";
        return false;
    }
    inactive_disp = byte_disps[0];
    active_disp = byte_disps[1];

    for (size_t i = 0; i + 7 <= kScanBytes; ++i) {
        if (bytes[i] == 0x44 && bytes[i + 1] == 0x38 && bytes[i + 2] == 0xB8) {
            memcpy(&config_suppressed_disp, bytes + i + 3,
                   sizeof(config_suppressed_disp));
            return true;
        }
    }

    error = "dataout config suppressed displacement missing";
    return false;
}

static const SpeedTelemetryVtable* find_speed_vtable(
    const std::vector<SpeedTelemetryVtable>& vtables,
    uintptr_t vtable) {
    auto it = std::find_if(vtables.begin(), vtables.end(),
                           [vtable](const SpeedTelemetryVtable& item) {
                               return item.vtable == vtable;
                           });
    return it == vtables.end() ? nullptr : &*it;
}

static bool resolve_speed_telemetry(uintptr_t module_base,
                                    size_t module_size,
                                    SpeedTelemetryResolver& out,
                                    std::string& error) {
    (void)module_size;
    ImageRange text{};
    ImageRange rdata{};
    if (!get_image_section(module_base, ".text", text)) {
        error = "missing .text";
        return false;
    }
    if (!get_image_section(module_base, ".rdata", rdata)) {
        error = "missing .rdata";
        return false;
    }

    // These leaf accessors are intentionally generic: class layouts can shift
    // between FH6 patches. The semantic publish-speed callsite below supplies
    // the authoritative local-interface displacement, and the vtable scan
    // supplies the active speed getter/displacement.
    constexpr uint8_t kLocalIfaceAccessor[] =
        {0x48, 0x8B, 0x81, 0x00, 0x00, 0x00, 0x00, 0xC3};
    constexpr char kLocalIfaceAccessorMask[] = "xxx????x";
    auto local_hits = find_pattern_in_range(text, kLocalIfaceAccessor,
                                            kLocalIfaceAccessorMask,
                                            sizeof(kLocalIfaceAccessor));
    out.local_accessor_hits = local_hits.size();

    constexpr uint8_t kSpeedGetter[] =
        {0xF3, 0x0F, 0x10, 0x81, 0x00, 0x00, 0x00, 0x00, 0xC3};
    constexpr char kSpeedGetterMask[] = "xxxx????x";
    auto speed_hits = find_pattern_in_range(text, kSpeedGetter,
                                            kSpeedGetterMask,
                                            sizeof(kSpeedGetter));
    out.speed_getter_hits = speed_hits.size();
    if (speed_hits.empty()) {
        error = "no generic speed getter candidates";
        return false;
    }

    constexpr uint8_t kPublishPattern[] = {
        0x48, 0x8B, 0x11, 0xFF, 0x92, 0xA8, 0x05, 0x00, 0x00,
        0x48, 0x8B, 0x8F, 0x00, 0x00, 0x00, 0x00,
        0x33, 0xD2, 0xF3, 0x0F, 0x59, 0x05
    };
    constexpr char kPublishMask[] = "xxxxxxxxxxxx????xxxxxx";
    auto publish_hits = find_pattern_in_range(text, kPublishPattern,
                                              kPublishMask,
                                              sizeof(kPublishPattern));
    bool scalar_publish_ok = false;
    std::string scalar_error;
    if (publish_hits.size() == 1) {
        out.publish_callsite = publish_hits[0];
        uint32_t publish_iface_disp = 0;
        uintptr_t multiplier_addr = rip_target(out.publish_callsite + 18, 4, 8);
        out.speed_multiplier_rva = multiplier_addr ? multiplier_addr - module_base : 0;
        if (read_u32_disp(out.publish_callsite + 5, out.publish_vfunc_offset) &&
            read_u32_disp(out.publish_callsite + 12, publish_iface_disp) &&
            multiplier_addr &&
            safe_read_f32(multiplier_addr, out.speed_multiplier) &&
            std::fabs(out.speed_multiplier - 2.23694f) <= 0.0005f) {
            scalar_publish_ok = true;
            out.mode = SpeedTelemetryMode::ScalarField;
            out.local_iface_disp = publish_iface_disp;
            for (uintptr_t hit : local_hits) {
                uint32_t disp = 0;
                if (read_u32_disp(hit + 3, disp) && disp == out.local_iface_disp) {
                    out.local_iface_accessor = hit;
                    break;
                }
            }
        } else {
            scalar_error = "publish scalar validation failed";
        }
    } else {
        scalar_error = "publish callsite hits=" + std::to_string(publish_hits.size());
    }

    struct DataOutSourceHit {
        uintptr_t callsite = 0;
        uintptr_t manager_global = 0;
        uintptr_t selector_func = 0;
        uintptr_t list_selector_func = 0;
        uint32_t provider_disp = 0;
        uint32_t current_root_disp = 0;
        uint32_t list_owner_disp = 0;
    };
    std::vector<DataOutSourceHit> dataout_hits;
    auto consider_dataout_hit = [&](uintptr_t hit,
                                    size_t manager_rel_offset,
                                    size_t manager_instr_len,
                                    size_t selector_call_offset) {
        uintptr_t manager_global =
            rip_target(hit, manager_rel_offset, manager_instr_len);
        uintptr_t selector = call_target(hit + selector_call_offset);
        if (!manager_global || !selector) return;

        uint32_t provider_disp = 0;
        uint32_t current_root_disp = 0;
        std::string parse_error;
        if (!parse_dataout_accessor_chain(selector, provider_disp,
                                          current_root_disp, parse_error)) {
            return;
        }

        uintptr_t list_selector = 0;
        for (uintptr_t p = hit + selector_call_offset + 5;
             p + 5 < hit + 80; ++p) {
            uint8_t bytes[4]{};
            if (!safe_memcpy(bytes, reinterpret_cast<const void*>(p),
                             sizeof(bytes))) {
                break;
            }
            if (bytes[0] == 0x48 && bytes[1] == 0x8B &&
                bytes[2] == 0xC8 && bytes[3] == 0xE8) {
                list_selector = call_target(p + 3);
                break;
            }
        }
        if (!list_selector) return;

        uint32_t list_owner_disp = 0;
        if (!parse_dataout_list_selector(list_selector, list_owner_disp,
                                         parse_error)) {
            return;
        }

        uint32_t inactive_disp = kDataOutTelemetryInactiveDisp;
        uint32_t active_disp = kDataOutTelemetryActiveDisp;
        uint32_t config_suppressed_disp = kDataOutTelemetryConfigSuppressedDisp;
        (void)parse_dataout_entry_state_filters(list_selector, inactive_disp,
                                                active_disp,
                                                config_suppressed_disp,
                                                parse_error);

        dataout_hits.push_back({hit, manager_global, selector, list_selector,
                                provider_disp, current_root_disp,
                                list_owner_disp});
        out.dataout_inactive_disp = inactive_disp;
        out.dataout_active_disp = active_disp;
        out.dataout_config_suppressed_disp = config_suppressed_disp;
    };

    constexpr uint8_t kDataOutSourceMovEdx[] = {
        0xBA, 0x01, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,
        0xE8, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xC0
    };
    constexpr char kDataOutSourceMovEdxMask[] = "xxxxxxxx????x????xxx";
    for (uintptr_t hit : find_pattern_in_range(text, kDataOutSourceMovEdx,
                                                kDataOutSourceMovEdxMask,
                                                sizeof(kDataOutSourceMovEdx))) {
        consider_dataout_hit(hit, 8, 12, 12);
    }

    constexpr uint8_t kDataOutSourceMovR10d[] = {
        0x41, 0x8B, 0xD5,
        0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,
        0xE8, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xC0
    };
    constexpr char kDataOutSourceMovR10dMask[] = "xxxxxx????x????xxx";
    for (uintptr_t hit : find_pattern_in_range(text, kDataOutSourceMovR10d,
                                                kDataOutSourceMovR10dMask,
                                                sizeof(kDataOutSourceMovR10d))) {
        consider_dataout_hit(hit, 6, 10, 10);
    }

    if (dataout_hits.empty()) {
        error = "dataout local telemetry source unresolved";
        return false;
    }

    const DataOutSourceHit& first = dataout_hits.front();
    bool consistent = std::all_of(
        dataout_hits.begin(), dataout_hits.end(),
        [&](const DataOutSourceHit& hit) {
            return hit.manager_global == first.manager_global &&
                   hit.selector_func == first.selector_func &&
                   hit.list_selector_func == first.list_selector_func &&
                   hit.provider_disp == first.provider_disp &&
                   hit.current_root_disp == first.current_root_disp &&
                   hit.list_owner_disp == first.list_owner_disp;
        });
    if (!consistent) {
        error = "dataout local telemetry source ambiguous";
        return false;
    }
    out.dataout_source_callsite = first.callsite;
    out.dataout_accessor_selector_func = first.selector_func;
    out.dataout_list_selector_func = first.list_selector_func;
    out.dataout_manager_global_rva = first.manager_global - module_base;
    out.dataout_manager_provider_disp = first.provider_disp;
    out.dataout_current_root_disp = first.current_root_disp;
    out.dataout_telemetry_list_owner_disp = first.list_owner_disp;

    if (scalar_publish_ok) {
        for (uintptr_t p = rdata.start; p + sizeof(uintptr_t) <= rdata.end; p += 8) {
            uintptr_t q = 0;
            if (!safe_read_qword(p, q)) break;
            if (std::find(speed_hits.begin(), speed_hits.end(), q) == speed_hits.end()) {
                continue;
            }

            constexpr uint32_t kMinLikelyCarSpeedDisp = 0x14E0;
            constexpr uint32_t kMaxLikelyCarSpeedDisp = 0x1500;
            uint32_t speed_disp = 0;
            // The +0x5A8 slot appears on many generic telemetry interfaces; the
            // publish path speed scalar stays in this car-dynamics field family.
            if (!read_u32_disp(q + 4, speed_disp) ||
                speed_disp < kMinLikelyCarSpeedDisp ||
                speed_disp > kMaxLikelyCarSpeedDisp) {
                continue;
            }

            uintptr_t vtable = p - out.publish_vfunc_offset;
            if (vtable < rdata.start || vtable >= rdata.end) continue;
            if (find_speed_vtable(out.iface_vtables, vtable)) continue;

            out.iface_vtables.push_back({vtable, q, static_cast<int32_t>(speed_disp)});
        }
        std::sort(out.iface_vtables.begin(), out.iface_vtables.end(),
                  [](const SpeedTelemetryVtable& a, const SpeedTelemetryVtable& b) {
                      return a.vtable < b.vtable;
                  });
        if (out.iface_vtables.empty()) {
            error = "no interface vtables point at generic speed getters";
            return false;
        }

        return true;
    }

    // Current FH6 Steam update: the Data Out producer no longer uses the old
    // scalar speed getter. It selects the active telemetry entry, reads the
    // local interface ref at telemetry+disp, then pulls the velocity vector
    // through a vtable getter. Derive the interface displacement and vector
    // getter from that producer instead of reviving registry ranking.
    constexpr size_t kProducerScanBytes = 1024;
    uint8_t producer[kProducerScanBytes]{};
    if (!safe_memcpy(producer,
                     reinterpret_cast<const void*>(out.dataout_source_callsite),
                     sizeof(producer))) {
        error = scalar_error + "; dataout producer unreadable";
        return false;
    }

    uintptr_t local_accessor = 0;
    uint32_t local_iface_disp = 0;
    size_t local_accessor_site = 0;
    for (size_t i = 0; i + 5 < kProducerScanBytes; ++i) {
        if (producer[i] != 0xE8) continue;
        int32_t rel = 0;
        memcpy(&rel, producer + i + 1, sizeof(rel));
        uintptr_t target = out.dataout_source_callsite + i + 5 + rel;
        uint32_t disp = 0;
        if (!parse_lea_accessor_disp(target, disp)) continue;
        if (disp < 0x1000) continue;
        if (i + 8 < kProducerScanBytes &&
            producer[i + 5] == 0x48 && producer[i + 6] == 0x8B) {
            local_accessor = target;
            local_iface_disp = disp;
            local_accessor_site = i;
            break;
        }
    }
    if (!local_accessor) {
        error = scalar_error + "; dataout local interface accessor missing";
        return false;
    }

    // The Data Out producer asks the same interface for position first and
    // velocity second.  Resolve that ordered pair instead of pinning the
    // velocity slot: the August 2026 FH6 update inserted one virtual method,
    // moving velocity from +0x598 to +0x5A0 without changing this call flow.
    std::vector<uint32_t> vector_vfunc_offsets;
    for (size_t i = local_accessor_site; i + 6 < kProducerScanBytes; ++i) {
        if (producer[i] != 0xFF) continue;
        const uint8_t modrm = producer[i + 1];
        // CALL qword ptr [reg+disp32].  r/m=4 has an extra SIB byte, which
        // this compact parser intentionally rejects.
        if ((modrm & 0xF8) != 0x90 || (modrm & 0x07) == 0x04) continue;

        uint32_t off = 0;
        memcpy(&off, producer + i + 2, sizeof(off));
        if (off < 0x500 || off > 0x700 || (off & 0x7) != 0) continue;
        if (std::find(vector_vfunc_offsets.begin(), vector_vfunc_offsets.end(),
                      off) == vector_vfunc_offsets.end()) {
            vector_vfunc_offsets.push_back(off);
        }
    }
    if (vector_vfunc_offsets.size() < 2) {
        error = scalar_error + "; dataout position/velocity vfunc pair missing";
        return false;
    }
    const uintptr_t velocity_vfunc_offset = vector_vfunc_offsets[1];

    out.mode = SpeedTelemetryMode::VelocityVector;
    out.local_iface_accessor = local_accessor;
    out.local_iface_disp = local_iface_disp;
    out.velocity_vfunc_offset = static_cast<uint32_t>(velocity_vfunc_offset);
    return true;
}

static bool validate_telemetry_speed(const SpeedTelemetryResolver& resolver,
                                     uintptr_t telemetry,
                                     SpeedTelemetryEntry& out) {
    if (!telemetry) return false;

    uintptr_t iface = 0;
    if (!safe_read_qword(telemetry + resolver.local_iface_disp, iface) || !iface) {
        return false;
    }

    uintptr_t vtable = 0;
    if (!safe_read_qword(iface, vtable)) return false;

    float speed = 0.0f;
    uintptr_t getter = 0;
    int32_t speed_disp = 0;

    if (resolver.mode == SpeedTelemetryMode::VelocityVector) {
        if (!resolver.velocity_vfunc_offset ||
            !safe_read_qword(vtable + resolver.velocity_vfunc_offset, getter)) {
            return false;
        }
        if (!parse_velocity_vector_getter(getter, speed_disp)) {
            return false;
        }
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
        uintptr_t vec = static_cast<uintptr_t>(
            static_cast<intptr_t>(iface) + static_cast<intptr_t>(speed_disp));
        if (!safe_read_f32(vec, vx) ||
            !safe_read_f32(vec + sizeof(float), vy) ||
            !safe_read_f32(vec + 2 * sizeof(float), vz) ||
            !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
            return false;
        }
        speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    } else {
        const SpeedTelemetryVtable* vt_info =
            find_speed_vtable(resolver.iface_vtables, vtable);
        if (!vt_info) return false;
        getter = vt_info->getter;
        speed_disp = vt_info->speed_disp;
        uintptr_t scalar = static_cast<uintptr_t>(
            static_cast<intptr_t>(iface) + static_cast<intptr_t>(speed_disp));
        if (!safe_read_f32(scalar, speed)) {
            return false;
        }
    }

    if (!std::isfinite(speed) || speed < 0.0f ||
        speed > kMaxPlausibleSpeedMps) {
        return false;
    }

    out.telemetry = telemetry;
    out.iface = iface;
    out.vtable = vtable;
    out.getter = getter;
    out.speed_disp = speed_disp;
    out.speed_mps = speed;
    return true;
}

static bool read_dataout_local_speed_sample(uintptr_t module_base,
                                            const SpeedTelemetryResolver& resolver,
                                            SpeedTelemetryEntry& selected,
                                            SpeedDataOutStats& stats,
                                            std::string& error) {
    stats = {};
    if (!resolver.dataout_manager_global_rva ||
        !resolver.dataout_manager_provider_disp ||
        !resolver.dataout_current_root_disp ||
        !resolver.dataout_telemetry_list_owner_disp) {
        error = "dataout resolver incomplete";
        return false;
    }

    if (!safe_read_qword(module_base + resolver.dataout_manager_global_rva,
                         stats.manager) ||
        !stats.manager) {
        error = "dataout manager unavailable";
        return false;
    }
    if (!safe_read_qword(stats.manager + resolver.dataout_manager_provider_disp,
                         stats.provider) ||
        !stats.provider) {
        error = "dataout provider unavailable";
        return false;
    }
    if (!safe_read_qword(stats.provider + resolver.dataout_current_root_disp,
                         stats.root) ||
        !stats.root) {
        error = "dataout current root unavailable";
        return false;
    }
    if (!safe_read_qword(stats.root + resolver.dataout_telemetry_list_owner_disp,
                         stats.list_owner) ||
        !stats.list_owner) {
        error = "dataout telemetry list unavailable";
        return false;
    }
    if (!safe_read_qword(stats.list_owner + kDataOutTelemetryListBeginDisp,
                         stats.begin) ||
        !safe_read_qword(stats.list_owner + kDataOutTelemetryListEndDisp,
                         stats.end)) {
        error = "dataout telemetry list bounds unreadable";
        return false;
    }
    if (!stats.begin || !stats.end || stats.end < stats.begin ||
        ((stats.begin | stats.end) & 0x7) != 0) {
        error = "dataout telemetry list bounds invalid";
        return false;
    }
    uintptr_t bytes = stats.end - stats.begin;
    if (bytes > 128 * sizeof(uintptr_t)) {
        error = "dataout telemetry list too large bytes=" +
                std::to_string(bytes);
        return false;
    }
    stats.entries = bytes / sizeof(uintptr_t);
    if (stats.entries == 0) {
        error = "dataout telemetry list empty";
        return false;
    }

    bool have_selected = false;
    for (size_t i = 0; i < stats.entries; ++i) {
        uintptr_t telemetry = 0;
        if (!safe_read_qword(stats.begin + i * sizeof(uintptr_t), telemetry)) {
            stats.unreadable_entries++;
            continue;
        }
        if (!telemetry) {
            stats.null_entries++;
            continue;
        }

        uint8_t inactive = 0;
        uint8_t active = 0;
        uint8_t retired = 0;
        if (!safe_read_u8(telemetry + resolver.dataout_inactive_disp, inactive) ||
            !safe_read_u8(telemetry + resolver.dataout_active_disp, active) ||
            !safe_read_u8(telemetry + resolver.dataout_retired_disp, retired) ||
            inactive != 0 || active != 1 ||
            (resolver.mode == SpeedTelemetryMode::ScalarField && retired != 0)) {
            stats.inactive_entries++;
            continue;
        }

        uintptr_t config = 0;
        if (!safe_read_qword(telemetry + kDataOutTelemetryConfigDisp, config) ||
            !config) {
            stats.inactive_entries++;
            continue;
        }
        uint8_t suppressed = 0;
        if (!safe_read_u8(config + resolver.dataout_config_suppressed_disp,
                          suppressed) ||
            suppressed) {
            stats.inactive_entries++;
            continue;
        }

        SpeedTelemetryEntry entry{};
        entry.index = i;
        uintptr_t iface = 0;
        if (!safe_read_qword(telemetry + resolver.local_iface_disp, iface) ||
            !iface) {
            stats.bad_iface++;
            continue;
        }
        uintptr_t vtable = 0;
        if (!safe_read_qword(iface, vtable) ||
            (resolver.mode == SpeedTelemetryMode::ScalarField &&
             !find_speed_vtable(resolver.iface_vtables, vtable))) {
            stats.bad_vtable++;
            continue;
        }
        if (!validate_telemetry_speed(resolver, telemetry, entry)) {
            stats.bad_speed++;
            continue;
        }
        entry.index = i;
        stats.valid++;

        if (have_selected) {
            error = "dataout telemetry source ambiguous valid=" +
                    std::to_string(stats.valid);
            return false;
        }
        selected = entry;
        have_selected = true;
    }

    if (!have_selected) {
        error = "dataout telemetry source missing";
        return false;
    }
    return true;
}

} // namespace

void InProcessInjector::start_speed_sampler() {
    if (!kSpeedTelemetryVerifierEnabled) return;
    if (speed_sampler_running_.load() || speed_sampler_thread_.joinable()) return;

    speed_sampler_running_ = true;
    speed_sampler_thread_ = std::thread(&InProcessInjector::speed_sampler_loop, this);
}

void InProcessInjector::speed_sampler_loop() {
    using namespace std::chrono_literals;

    if (!kSpeedTelemetryVerifierEnabled) return;

    std::this_thread::sleep_for(5s);

    SpeedTelemetryResolver resolver{};
    std::string resolve_error;
    while (speed_sampler_running_.load(std::memory_order_acquire) &&
           !resolve_speed_telemetry(module_base_, module_size_,
                                    resolver, resolve_error)) {
        if (kSpeedTelemetryDetailLogs) {
            log::info("[speed-dataout] resolver not ready: " + resolve_error);
        }
        std::this_thread::sleep_for(5s);
    }

    if (!speed_sampler_running_.load(std::memory_order_acquire)) {
        if (kSpeedTelemetryDetailLogs) {
            log::info("[speed-dataout] verifier stopped before resolver ready");
        }
        return;
    }

    if (kSpeedTelemetryDetailLogs) {
        log::info(std::string("[speed-dataout] resolver ok localAccessorRva=")
                  + rva_or_zero(module_base_, resolver.local_iface_accessor) +
                  " localAccessorHits=" + std::to_string(resolver.local_accessor_hits) +
                  " localDisp=" + hex(resolver.local_iface_disp) +
                  " genericSpeedGetterHits=" + std::to_string(resolver.speed_getter_hits) +
                  " publishRva=" + hex(resolver.publish_callsite - module_base_) +
                  " vfuncOffset=" + hex(resolver.publish_vfunc_offset) +
                  " multiplierRva=" + hex(resolver.speed_multiplier_rva) +
                  " multiplier=" + fmt_float(resolver.speed_multiplier, 6) +
                  " dataoutSourceRva=" + hex(resolver.dataout_source_callsite - module_base_) +
                  " dataoutAccessorSelectorRva=" + hex(resolver.dataout_accessor_selector_func - module_base_) +
                  " dataoutListSelectorRva=" + hex(resolver.dataout_list_selector_func - module_base_) +
                  " dataoutManagerRva=" + hex(resolver.dataout_manager_global_rva) +
                  " dataoutProviderDisp=" + hex(resolver.dataout_manager_provider_disp) +
                  " dataoutCurrentRootDisp=" + hex(resolver.dataout_current_root_disp) +
                  " dataoutListOwnerDisp=" + hex(resolver.dataout_telemetry_list_owner_disp) +
                  " dataoutActiveDisp=" + hex(resolver.dataout_active_disp) +
                  " dataoutInactiveDisp=" + hex(resolver.dataout_inactive_disp) +
                  " dataoutConfigSuppressedDisp=" + hex(resolver.dataout_config_suppressed_disp) +
                  " speedMode=" + std::string(resolver.mode == SpeedTelemetryMode::VelocityVector
                                                ? "velocity-vector" : "scalar") +
                  " velocityVfuncOffset=" + hex(resolver.velocity_vfunc_offset) +
                  " ifaceVtableRvas=[" + vtable_list_rvas(module_base_,
                                                          resolver.iface_vtables) + "]");

        log::info("[speed-dataout] starting read-only local Data Out sampler");
    }

    uintptr_t last_logged_telemetry = 0;
    uint64_t last_detail_log_ms = 0;
    uint64_t last_unavailable_log_ms = 0;
    while (speed_sampler_running_.load(std::memory_order_acquire)) {
        uint64_t now = monotonic_ms();
        SpeedTelemetryEntry sample{};
        SpeedDataOutStats stats{};
        std::string error;
        if (read_dataout_local_speed_sample(module_base_, resolver, sample,
                                            stats, error)) {
            float speed = sample.speed_mps;
            speed_last_mps_.store(speed, std::memory_order_release);
            speed_last_sample_ms_.store(now, std::memory_order_release);
            speed_capture_count_.fetch_add(1, std::memory_order_acq_rel);

            bool log_detail =
                sample.telemetry != last_logged_telemetry ||
                last_detail_log_ms == 0 ||
                now - last_detail_log_ms >= 2000;
            if (kSpeedTelemetryDetailLogs && log_detail) {
                last_logged_telemetry = sample.telemetry;
                last_detail_log_ms = now;
                log::info("[speed-dataout] sample listIndex="
                          + std::to_string(sample.index)
                          + " telemetry=" + hex(sample.telemetry)
                          + " iface=" + hex(sample.iface)
                          + " vtRva=" + hex(sample.vtable - module_base_)
                          + " getterRva=" + hex(sample.getter - module_base_)
                          + " speedDisp=" + hex(static_cast<uintptr_t>(
                              static_cast<intptr_t>(sample.speed_disp)))
                          + " manager=" + hex(stats.manager)
                          + " provider=" + hex(stats.provider)
                          + " root=" + hex(stats.root)
                          + " listOwner=" + hex(stats.list_owner)
                          + " listEntries=" + std::to_string(stats.entries)
                          + " valid=" + std::to_string(stats.valid)
                          + " speedMps=" + fmt_float(speed, 3)
                          + " speedKmh=" + fmt_float(speed * 3.6f, 1)
                          + " speedMph=" + fmt_float(speed * 2.2369363f, 1));
            }
        } else {
            speed_last_sample_ms_.store(0, std::memory_order_release);
            if (kSpeedTelemetryDetailLogs &&
                (last_unavailable_log_ms == 0 ||
                 now - last_unavailable_log_ms >= 3000)) {
                last_unavailable_log_ms = now;
                log::info("[speed-dataout] unavailable reason=" + error
                          + " listEntries=" + std::to_string(stats.entries)
                          + " valid=" + std::to_string(stats.valid)
                          + " null=" + std::to_string(stats.null_entries)
                          + " unreadable=" + std::to_string(stats.unreadable_entries)
                          + " inactive=" + std::to_string(stats.inactive_entries)
                          + " badIface=" + std::to_string(stats.bad_iface)
                          + " badVt=" + std::to_string(stats.bad_vtable)
                          + " badSpeed=" + std::to_string(stats.bad_speed));
            }
        }

        std::this_thread::sleep_for(kSpeedSampleInterval);
    }

    speed_last_sample_ms_.store(0, std::memory_order_release);
    if (kSpeedTelemetryDetailLogs) {
        log::info("[speed-dataout] verifier stopped");
    }
}

// ---- String read/write ----
//
// MSVC std::string SSO layout (x64, 32 bytes):
//   +0x00: union { char buf[16]; char* ptr; }
//   +0x10: size_t _Mysize
//   +0x18: size_t _Myres (capacity)
// SSO when _Myres < 16.

std::optional<std::string> InProcessInjector::read_game_string(uintptr_t addr) const {
    // Read string header with SEH protection.
    uint8_t header[32];
    if (!safe_memcpy(header, reinterpret_cast<const void*>(addr), 32))
        return std::nullopt;

    size_t sz  = *reinterpret_cast<size_t*>(header + 16);
    size_t cap = *reinterpret_cast<size_t*>(header + 24);
    if (sz > 4096) return std::nullopt;

    if (cap >= 16) {
        // Heap-allocated — header[0..7] is pointer to buffer.
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(header);
        if (ptr < 0x10000) return std::nullopt;

        std::string result(sz, '\0');
        if (!safe_memcpy(result.data(), reinterpret_cast<const void*>(ptr), sz))
            return std::nullopt;
        return result;
    } else {
        // SSO — inline buffer.
        size_t len = strnlen(reinterpret_cast<char*>(header), std::min(sz, (size_t)16));
        return std::string(reinterpret_cast<char*>(header), len);
    }
}

bool InProcessInjector::write_game_string(uintptr_t addr, const std::string& text) {
    // Read current header to get capacity.
    uint8_t header[32];
    if (!safe_memcpy(header, reinterpret_cast<const void*>(addr), 32))
        return false;

    size_t cap = *reinterpret_cast<size_t*>(header + 24);

    auto* str_size = reinterpret_cast<size_t*>(addr + 0x10);
    auto* str_cap  = reinterpret_cast<size_t*>(addr + 0x18);

    if (cap >= 16) {
        // Heap-allocated — header[0..7] is pointer to buffer.
        uintptr_t ptr = *reinterpret_cast<uintptr_t*>(header);
        if (ptr < 0x10000) return false;

        if (text.size() <= cap) {
            // Fits in existing buffer — just copy.
            memcpy(reinterpret_cast<void*>(ptr), text.c_str(), text.size() + 1);
            *str_size = text.size();
            return true;
        }

        // Need bigger buffer — VirtualAlloc (in-process, no Ex).
        size_t new_cap = (text.size() + 32) & ~(size_t)15;
        void* new_buf = ::VirtualAlloc(nullptr, new_cap + 1,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!new_buf) {
            log::error("[inject] VirtualAlloc failed for string realloc");
            return false;
        }
        memcpy(new_buf, text.c_str(), text.size() + 1);
        *reinterpret_cast<char**>(addr) = reinterpret_cast<char*>(new_buf);
        *str_size = text.size();
        *str_cap  = new_cap;
        wgs_alloc_count_.fetch_add(1, std::memory_order_relaxed);
        wgs_alloc_bytes_.fetch_add(new_cap + 1, std::memory_order_relaxed);
        // Old buffer leaked — acceptable, small, one-time per track change.
        return true;
    } else {
        // SSO — inline buffer (cap < 16, max 15 chars + null).
        if (text.size() < 16) {
            memset(reinterpret_cast<void*>(addr), 0, 16);
            memcpy(reinterpret_cast<void*>(addr), text.c_str(), text.size() + 1);
            *str_size = text.size();
            return true;
        }

        // Promote SSO to heap.
        size_t new_cap = (text.size() + 32) & ~(size_t)15;
        void* new_buf = ::VirtualAlloc(nullptr, new_cap + 1,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!new_buf) {
            log::error("[inject] VirtualAlloc failed for SSO promotion");
            return false;
        }
        memcpy(new_buf, text.c_str(), text.size() + 1);
        *reinterpret_cast<char**>(addr) = reinterpret_cast<char*>(new_buf);
        *str_size = text.size();
        *str_cap  = new_cap;
        wgs_alloc_count_.fetch_add(1, std::memory_order_relaxed);
        wgs_alloc_bytes_.fetch_add(new_cap + 1, std::memory_order_relaxed);
        return true;
    }
}

// ---- Injection-station gating ----
//
// Preferred signal, sigscan-resolved at startup:
//   rvas_.radio_state_singleton -> RadioState singleton (data RVA, build-relative)
//   RadioState+0x40 -> RadioPlayer*
//   RadioPlayer+0x50 -> selected RadioStation*
//   RadioStation+0x200 -> MSVC std::string station name
//
// FH5 does not expose the media-free station in its radio wheel, so treat any
// selected station as the injection target. If this chain is not live yet,
// fall back to the older SampleProperties.SoundName sentinel path.
// RadioState/RadioPlayer/RadioStation layout + station-detection string/sound
// constants are per-game: they shift independently when either FH5 or FH6 ships
// an update. They live in the active game profile (game_profile.h); this
// accessor returns the right set for the host title. After a patch, update the
// game's table in game_profile.cpp — not here.
//
// The wired-sound name (radio_layout().wired_sound_name) is the SoundName
// fallback sentinel and must match installer.py WIRED_SOUNDNAME for that game.
static inline const RadioProfile& radio_layout() {
    return active_profile().radio;
}

bool InProcessInjector::is_spotify_station_active() const {
    std::lock_guard lock(mtx_);
    return is_spotify_station_active_locked();
}

bool InProcessInjector::spotify_radio_stream_handle(uintptr_t& wrapper,
                                                    uint32_t& handle,
                                                    bool& has_sound,
                                                    bool media_free) const {
    std::lock_guard lock(mtx_);
    wrapper = 0;
    handle = 0;
    has_sound = false;
    if (!discovery_done_.load()) return false;

    for (uintptr_t inst : addrs_.radio_stream_instances) {
        uint32_t h = 0;
        if (!safe_read_u32(inst + 0x30, h) || h == 0) continue;

        uintptr_t ptr1 = 0;
        uintptr_t props = 0;
        if (!safe_read_qword(inst + 0x58, ptr1) || ptr1 == 0) continue;
        if (!safe_read_qword(ptr1 + 0x18, props) || props == 0) continue;

        auto sound = read_game_string(props + 0x10);
        if (!sound || sound->empty()) continue;
        // Patched-media path: only the wired Spotify sample anchors the channel.
        // Media-free path: the patched single-sample SampleList is gone, so the
        // active Streamer Mode stream plays vanilla curated tracks. Only ONE
        // stream is live at a time (idle slots have a null +0x58, skipped above)
        // and the caller only resolves the channel while Streamer Mode is the
        // selected station, so the active stream IS Streamer Mode's — attach
        // regardless of which track is loaded. (Verified live 2026-06-13: the
        // active slot SoundName was e.g. HZ6_R9_MaryLattimore_..., not the wired
        // HZ6_R9_PeterBroderick_... sample.)
        if (!media_free && *sound != radio_layout().wired_sound_name) continue;

        uintptr_t snd = 0;
        wrapper = inst;
        handle = h;
        has_sound = safe_read_qword(inst + 0x18, snd) && snd != 0;
        return true;
    }

    return false;
}

bool InProcessInjector::is_game_menu_open() const {
    std::lock_guard lock(mtx_);
    return is_game_menu_open_locked();
}

bool InProcessInjector::is_race_active() const {
    std::lock_guard lock(mtx_);
    return is_race_active_locked();
}

bool InProcessInjector::is_race_restart_menu_active() const {
    std::lock_guard lock(mtx_);
    return is_race_restart_menu_active_locked();
}

void InProcessInjector::update_runtime_logo_game_state(
    bool non_driving_candidate,
    bool garage_candidate,
    bool race_stinger) {
    const uint64_t now = monotonic_ms();
    const bool previous_known =
        runtime_logo_game_state_known_.load(std::memory_order_acquire);
    const bool previous_non_driving =
        runtime_logo_non_driving_candidate_.load(std::memory_order_acquire);
    const bool previous_garage =
        runtime_logo_garage_candidate_.load(std::memory_order_acquire);
    const bool previous_race_stinger =
        runtime_logo_race_stinger_.load(std::memory_order_acquire);
    runtime_logo_non_driving_candidate_.store(non_driving_candidate,
                                              std::memory_order_release);
    runtime_logo_garage_candidate_.store(garage_candidate,
                                         std::memory_order_release);
    runtime_logo_race_stinger_.store(race_stinger,
                                     std::memory_order_release);
    if (!previous_known ||
        previous_non_driving != non_driving_candidate ||
        previous_garage != garage_candidate ||
        previous_race_stinger != race_stinger) {
        runtime_logo_state_change_ms_.store(now, std::memory_order_release);
    }
    if (non_driving_candidate || garage_candidate || race_stinger) {
        runtime_logo_last_non_driving_ms_.store(now,
                                                std::memory_order_release);
    }
    runtime_logo_game_state_known_.store(true, std::memory_order_release);
}

std::optional<float> InProcessInjector::vehicle_speed_mps() const {
    if (!kSpeedTelemetryVerifierEnabled) return std::nullopt;

    uint64_t last_ms = speed_last_sample_ms_.load(std::memory_order_acquire);
    uint64_t now_ms = monotonic_ms();
    if (last_ms != 0 && now_ms >= last_ms &&
        now_ms - last_ms <= static_cast<uint64_t>(kSpeedFreshWindow.count())) {
        float speed = speed_last_mps_.load(std::memory_order_acquire);
        if (std::isfinite(speed) && speed >= 0.0f &&
            speed <= kMaxPlausibleSpeedMps) {
            return speed;
        }
    }

    // FH5 has no portable in-memory speed mirror; speed is fed externally via the
    // Forza Data Out UDP listener. With no fresh external sample, fail open
    // (Night Runners stays neutral) rather than run the in-memory resolver.
    if (external_speed_only_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    SpeedTelemetryResolver resolver{};
    {
        std::lock_guard lock(g_speed_direct_resolver_mtx);
        if (g_speed_direct_resolver_base != module_base_) {
            g_speed_direct_resolver_base = module_base_;
            g_speed_direct_resolver_ready = false;
            g_speed_direct_last_attempt_ms = 0;
            g_speed_direct_resolver = {};
        }
        if (!g_speed_direct_resolver_ready &&
            (g_speed_direct_last_attempt_ms == 0 ||
             now_ms - g_speed_direct_last_attempt_ms >= 5000)) {
            g_speed_direct_last_attempt_ms = now_ms;
            SpeedTelemetryResolver candidate{};
            std::string error;
            if (module_base_ && resolve_speed_telemetry(module_base_, module_size_,
                                                        candidate, error)) {
                g_speed_direct_resolver = std::move(candidate);
                g_speed_direct_resolver_ready = true;
                log::info("[speed-dataout] direct resolver ready dataoutManagerRva="
                          + hex(g_speed_direct_resolver.dataout_manager_global_rva)
                          + " dataoutProviderDisp="
                          + hex(g_speed_direct_resolver.dataout_manager_provider_disp)
                          + " dataoutCurrentRootDisp="
                          + hex(g_speed_direct_resolver.dataout_current_root_disp)
                          + " dataoutListOwnerDisp="
                          + hex(g_speed_direct_resolver.dataout_telemetry_list_owner_disp)
                          + " dataoutActiveDisp="
                          + hex(g_speed_direct_resolver.dataout_active_disp)
                          + " dataoutInactiveDisp="
                          + hex(g_speed_direct_resolver.dataout_inactive_disp)
                          + " speedMode="
                          + std::string(g_speed_direct_resolver.mode == SpeedTelemetryMode::VelocityVector
                                            ? "velocity-vector" : "scalar")
                          + " velocityVfuncOffset="
                          + hex(g_speed_direct_resolver.velocity_vfunc_offset)
                          + " ifaceVtableRvas=["
                          + vtable_list_rvas(module_base_,
                                             g_speed_direct_resolver.iface_vtables)
                          + "]");
            }
        }
        if (!g_speed_direct_resolver_ready) {
            return std::nullopt;
        }
        resolver = g_speed_direct_resolver;
    }

    SpeedTelemetryEntry selected{};
    SpeedDataOutStats stats{};
    std::string error;
    if (!read_dataout_local_speed_sample(module_base_, resolver, selected,
                                         stats, error)) {
        return std::nullopt;
    }

    if (!std::isfinite(selected.speed_mps) || selected.speed_mps < 0.0f ||
        selected.speed_mps > kMaxPlausibleSpeedMps) {
        return std::nullopt;
    }

    return selected.speed_mps;
}

void InProcessInjector::push_external_speed_mps(float mps) {
    if (!std::isfinite(mps) || mps < 0.0f || mps > kMaxPlausibleSpeedMps) return;
    speed_last_mps_.store(mps, std::memory_order_release);
    speed_last_sample_ms_.store(monotonic_ms(), std::memory_order_release);
}

void InProcessInjector::set_external_speed_only(bool on) {
    external_speed_only_.store(on, std::memory_order_release);
}

void InProcessInjector::fh5_logo_diag() {
#if defined(SPOTIFY_RADIO_DIAG)
    if (active_profile().id != GameId::FH5 || !module_base_) return;
    if (text_size_ != kFH5LogoSteamProfile.text_size) return;
    // Called from the 50ms metadata pump -> throttle to ~ every 2s.
    static std::atomic<uint32_t> tick{0};
    if ((tick.fetch_add(1, std::memory_order_relaxed) % 40) != 0) return;

    // FH5 build constants for a handle-model sanity check. This vector is not
    // the radio-logo container.
    const uintptr_t vec   = module_base_ + 0xA068060;
    const uintptr_t resvt = module_base_ + 0x67D0B60;
    const uintptr_t htbl_global = module_base_ + 0x9029B60;
    const char* const known_names[] = {
        "DebugWhite",
        "DebugOpaqueBlack",
        "DebugTransparentBlack",
        "DebugRamp",
        "DebugDarkGray",
        "DebugGray",
        "DebugIdentityColorCube",
        "DebugBlackCube",
        "DebugWhiteCube",
        "DebugWhiteVolume",
        "DebugBlackVolume",
        "DefaultNormalMap",
        "DefaultWaterNormalMap",
        "DebugDefault13?",
        "DebugDefault14?",
        "DebugDefault15?",
    };

    uintptr_t begin = 0, end = 0;
    if (!safe_read_qword(vec, begin) || !safe_read_qword(vec + 8, end) ||
        !begin || end <= begin) {
        log::info("[fh5-logo-diag] default texture vector empty/unavailable");
        return;
    }
    size_t n = (end - begin) / 8;
    if (n > 64) n = 64;
    uintptr_t htbl = 0;
    safe_read_qword(htbl_global, htbl);

    // Is our Streamer Mode station currently selected? (raw reads, no lock needed)
    bool streamer = false;
    uintptr_t st = 0, pl = 0, station = 0;
    if (rvas_.radio_state_singleton &&
        safe_read_qword(module_base_ + rvas_.radio_state_singleton, st) && st &&
        safe_read_qword(st + radio_layout().state_player_offset, pl) && pl &&
        safe_read_qword(pl + radio_layout().player_selected_station_offset, station) &&
        station) {
        auto nm = read_game_string(station + radio_layout().station_name_offset);
        streamer = nm && (*nm == radio_layout().station_name_streamer ||
                          *nm == radio_layout().station_name_spotify);
    }

    log::info("[fh5-logo-diag] vec=" + hex(begin) + " entries=" + std::to_string(n) +
              " streamerSelected=" + (streamer ? "1" : "0") +
              " handle_table=" + hex(htbl));
    for (size_t i = 0; i < n; ++i) {
        uintptr_t r = 0;
        if (!safe_read_qword(begin + i * 8, r) || !r) continue;
        uintptr_t vt = 0;
        safe_read_qword(r, vt);
        uint32_t h10 = 0, h18 = 0, d20 = 0, d24 = 0, d30 = 0, d34 = 0;
        uint64_t sz28 = 0;
        safe_read_u32(r + 0x10, h10);
        safe_read_u32(r + 0x18, h18);
        safe_read_u32(r + 0x20, d20);
        safe_read_u32(r + 0x24, d24);
        safe_read_qword(r + 0x28, sz28);
        safe_read_u32(r + 0x30, d30);
        safe_read_u32(r + 0x34, d34);
        uintptr_t o18_mask = 0, o10_mask = 0, o10_slot = 0;
        if (htbl) {
            safe_read_qword(htbl + 8ull * (h18 & 0xFFFFF), o18_mask);
            safe_read_qword(htbl + 8ull * (h10 & 0xFFFFF), o10_mask);
            safe_read_qword(htbl + 8ull * (h10 >> 8), o10_slot);
        }
        const char* name = i < std::size(known_names) ? known_names[i] : "?";
        log::info("[fh5-logo-diag] [" + std::to_string(i) + "] res=" + hex(r) +
                  " name=" + name +
                  (vt == resvt ? " RES" : " vt=" + hex(vt)) +
                  " h10=" + hex(h10) + " h18=" + hex(h18) +
                  " d20=" + hex(d20) + " d24=" + hex(d24) + " sz28=" + hex(sz28) +
                  " d30=" + hex(d30) + " d34=" + hex(d34) +
                  " tbl[h18&mask]=" + hex(o18_mask) +
                  " tbl[h10&mask]=" + hex(o10_mask) +
                  " tbl[h10>>8]=" + hex(o10_slot));
    }
#endif
}

bool InProcessInjector::any_radio_stream_sound_active_locked() const {
    for (uintptr_t inst : addrs_.radio_stream_instances) {
        // RadioStreamFmod object lives at refcount-wrapper + 0x10.
        // Its Sound* field is at object +0x08 → refcount + 0x18.
        uintptr_t snd = 0;
        if (safe_read_qword(inst + 0x18, snd) && snd != 0) return true;
    }
    return false;
}

bool InProcessInjector::is_spotify_station_active_locked() const {
    if (!discovery_done_.load()) return false;

    bool any_active = any_radio_stream_sound_active_locked();

    uintptr_t state = 0;
    uintptr_t player = 0;
    uintptr_t station = 0;
    if (module_base_ && rvas_.radio_state_singleton &&
        safe_read_qword(module_base_ + rvas_.radio_state_singleton, state) && state &&
        safe_read_qword(state + radio_layout().state_player_offset, player) && player &&
        safe_read_qword(player + radio_layout().player_selected_station_offset, station) && station) {
        return true;
    }

    if (!any_active) return false;

    if (!addrs_.sample_properties) return false;
    auto sound = read_game_string(addrs_.sample_properties + 0x10);
    return sound && *sound == radio_layout().wired_sound_name;
}

bool InProcessInjector::is_game_menu_open_locked() const {
    uintptr_t state = 0;
    uint8_t menu_open = 0;
    if (!module_base_ || !rvas_.radio_state_singleton) return false;
    if (!safe_read_qword(module_base_ + rvas_.radio_state_singleton, state) || !state) {
        return false;
    }
    if (!safe_read_u8(state + radio_layout().state_menu_open_offset, menu_open)) return false;
    return menu_open != 0;
}

bool InProcessInjector::is_race_active_locked() const {
    uintptr_t state = 0;
    uint8_t race_a = 0;
    uint8_t race_b = 0;
    if (!module_base_ || !rvas_.radio_state_singleton) return false;
    if (!safe_read_qword(module_base_ + rvas_.radio_state_singleton, state) || !state) {
        return false;
    }
    if (!safe_read_u8(state + radio_layout().state_race_active_a_offset, race_a)) return false;
    if (!safe_read_u8(state + radio_layout().state_race_active_b_offset, race_b)) return false;
    return race_a != 0 && race_b != 0;
}

bool InProcessInjector::is_race_restart_menu_active_locked() const {
    if (!is_race_active_locked()) return false;

    uintptr_t state = 0;
    uint32_t marker = 0;
    if (!module_base_ || !rvas_.radio_state_singleton) return false;
    if (!safe_read_qword(module_base_ + rvas_.radio_state_singleton, state) || !state) {
        return false;
    }
    if (!safe_read_u32(state + radio_layout().state_race_restart_menu_offset, marker)) return false;
    return marker == 0xFFFFFFFFu;
}

bool InProcessInjector::is_race_stinger_menu_active() const {
    std::lock_guard lock(mtx_);
    uintptr_t state = 0;
    uintptr_t player = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    uint32_t e = 0;
    if (!module_base_ || !rvas_.radio_state_singleton) return false;
    if (!safe_read_qword(module_base_ + rvas_.radio_state_singleton, state) || !state) {
        return false;
    }
    if (!safe_read_qword(state + radio_layout().state_player_offset, player) || !player) {
        return false;
    }
    if (!safe_read_u32(player + radio_layout().player_stinger_marker_offset[0], a)) return false;
    if (!safe_read_u32(player + radio_layout().player_stinger_marker_offset[1], b)) return false;
    if (!safe_read_u32(player + radio_layout().player_stinger_marker_offset[2], c)) return false;
    if (!safe_read_u32(player + radio_layout().player_stinger_marker_offset[3], d)) return false;
    if (!safe_read_u32(player + radio_layout().player_stinger_marker_offset[4], e)) return false;
    return a == radio_layout().race_menu_stinger_marker &&
           b == radio_layout().race_menu_stinger_marker &&
           c == radio_layout().race_menu_stinger_marker &&
           d == radio_layout().race_menu_stinger_marker &&
           e == radio_layout().race_menu_stinger_marker;
}

InProcessInjector::GameStateDebugSnapshot
InProcessInjector::game_state_debug_snapshot() const {
    std::lock_guard lock(mtx_);
    GameStateDebugSnapshot out{};
    if (!module_base_ || !rvas_.radio_state_singleton) return out;
    if (!safe_read_qword(module_base_ + rvas_.radio_state_singleton,
                         out.radio_state) ||
        !out.radio_state) {
        return out;
    }
    out.radio_state_read = true;
    safe_read_u8(out.radio_state + radio_layout().state_menu_open_offset,
                 out.menu_open);
    safe_read_u8(out.radio_state + radio_layout().state_race_active_a_offset,
                 out.race_active_a);
    safe_read_u8(out.radio_state + radio_layout().state_race_active_b_offset,
                 out.race_active_b);
    safe_read_u32(out.radio_state + radio_layout().state_race_restart_menu_offset,
                  out.race_restart_marker);
    if (safe_read_qword(out.radio_state + radio_layout().state_player_offset,
                        out.radio_player) &&
        out.radio_player) {
        out.radio_player_read = true;
        safe_read_u32(out.radio_player + radio_layout().player_stinger_marker_offset[0],
                      out.stinger_a);
        safe_read_u32(out.radio_player + radio_layout().player_stinger_marker_offset[1],
                      out.stinger_b);
        safe_read_u32(out.radio_player + radio_layout().player_stinger_marker_offset[2],
                      out.stinger_c);
        safe_read_u32(out.radio_player + radio_layout().player_stinger_marker_offset[3],
                      out.stinger_d);
        safe_read_u32(out.radio_player + radio_layout().player_stinger_marker_offset[4],
                      out.stinger_e);
    }
    return out;
}

InProcessInjector::RadioStreamDebugSnapshot
InProcessInjector::radio_stream_debug_snapshot() const {
    std::lock_guard lock(mtx_);
    RadioStreamDebugSnapshot out{};
    if (!discovery_done_.load()) return out;

    if (module_base_ && rvas_.radio_state_singleton &&
        safe_read_qword(module_base_ + rvas_.radio_state_singleton,
                        out.radio_state) &&
        out.radio_state) {
        out.radio_state_read = true;
        if (safe_read_qword(out.radio_state + radio_layout().state_player_offset,
                            out.radio_player) &&
            out.radio_player) {
            out.radio_player_read = true;
            if (safe_read_qword(
                    out.radio_player +
                        radio_layout().player_selected_station_offset,
                    out.selected_station) &&
                out.selected_station) {
                out.selected_station_read = true;
                auto name = read_game_string(
                    out.selected_station + radio_layout().station_name_offset);
                if (name) out.selected_station_name = *name;
            }
        }
    }

    out.streams.reserve(addrs_.radio_stream_instances.size());
    for (uintptr_t inst : addrs_.radio_stream_instances) {
        RadioStreamDebugEntry e{};
        e.wrapper = inst;
        safe_read_u32(inst + 0x30, e.handle);
        e.active = safe_read_qword(inst + 0x18, e.fmod_sound) &&
                   e.fmod_sound != 0;

        uintptr_t ptr1 = 0;
        if (safe_read_qword(inst + 0x58, ptr1) && ptr1) {
            safe_read_qword(ptr1 + 0x18, e.sample_properties);
        }
        if (e.sample_properties) {
            auto sound = read_game_string(e.sample_properties + 0x10);
            auto display = read_game_string(e.sample_properties + 0x30);
            auto artist = read_game_string(e.sample_properties + 0x50);
            if (sound) e.sound_name = *sound;
            if (display) e.display_name = *display;
            if (artist) e.artist = *artist;
        }
        out.streams.push_back(std::move(e));
    }

    return out;
}

// ---- Metadata push ----

bool InProcessInjector::push_metadata(const std::string& track_name, const std::string& artist) {
    std::lock_guard lock(mtx_);

    if (!discovery_done_.load()) return false;

    if (!refresh_sample_properties()) {
        // Don't log every cycle — this is expected when radio is off.
        return false;
    }

    uintptr_t sp = addrs_.sample_properties;

    // Phase 3h refinement: only inject when R10 (Spotify slot) is the
    // active station and at least one RadioStreamFmod has a live Sound*.
    if (!is_spotify_station_active_locked()) return false;

    // Check if game already shows our text — skip write if so.
    auto current_display = read_game_string(sp + 0x30);
    auto current_artist  = read_game_string(sp + 0x50);
    if (current_display && current_artist &&
        *current_display == track_name && *current_artist == artist) {
        last_injected_track_ = track_name + " - " + artist;
        return true;
    }

    bool ok_name   = write_game_string(sp + 0x30, track_name);
    bool ok_artist = write_game_string(sp + 0x50, artist);

    if (ok_name && ok_artist) {
        std::string key = track_name + " - " + artist;
        if (key != last_injected_track_) {
            log::info("[inject] Wrote: \"" + track_name + "\" by \"" + artist + "\"");
        }
        last_injected_track_ = key;
        return true;
    }

    log::warn("[inject] Write failed — name:" + std::to_string(ok_name)
              + " artist:" + std::to_string(ok_artist));
    return false;
}

// ---- State query ----

IMetadataInjector::RadioState InProcessInjector::get_state() const {
    std::lock_guard lock(mtx_);

    RadioState s;
    s.game_found = true;   // we ARE the game
    s.attached = (module_base_ != 0);
    s.discovery_done = discovery_done_;
    s.injected_track = last_injected_track_;

    if (addrs_.sample_properties) {
        auto sound   = read_game_string(addrs_.sample_properties + 0x10);
        auto display = read_game_string(addrs_.sample_properties + 0x30);
        auto artist  = read_game_string(addrs_.sample_properties + 0x50);
        if (sound)  s.current_sound_name = *sound;
        if (display && !display->empty()) s.current_sound_name = *display;
        if (artist) s.current_artist = *artist;
    }

    return s;
}

} // namespace bridge
