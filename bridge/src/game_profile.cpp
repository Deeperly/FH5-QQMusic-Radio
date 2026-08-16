// Per-game profile tables. See game_profile.h.
//
// Signatures and structure offsets are build-specific. Callers validate the
// resolved objects at runtime and disable unsupported paths when validation
// fails.

#include "game_profile.h"
#include "log_file.h"

#include <Windows.h>

#include <mutex>
#include <string>

namespace bridge {

namespace {

// ---- FH6 -----------------------------------------------------------------

// Prologue fingerprints (tie-breakers when an FMOD __FUNCTION__ anchor has
// multiple .rdata copies or consumers). '|' separates alternatives.
constexpr const char* kFH6_PrologueRbxRsi_50 =
    "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00";
constexpr const char* kFH6_PrologueR11_56_170 =
    "4C 8B DC 56 48 81 EC 70 01 00 00"
    "|40 53 55 56 57 41 56 48 81 EC 50 01 00 00";
constexpr const char* kFH6_PrologueRbx_50 =
    "48 89 5C 24 10 57 48 81 EC 50 01 00 00";

constexpr SigProfile::BuildOverride kFH6_BuildOverrides[] = {
    // Steam layout (.text VirtualSize). Generic byte pattern can select a
    // neighboring memory-texture helper; pin the proven raw-2D cache wrapper +
    // family handle-table arrays.
    {0x6322F5E, 0x3A1FEA0, 0xA8CA590, 0xA8CA600},
    // Store / Game Pass layout.
    {0x630E45E, 0x39B74E0, 0, 0},
};

const GameProfile kFH6 = {
    GameId::FH6,
    "FH6",
    L"forzahorizon6.exe",
    /* sig */ {
        "System::createDSP",        kFH6_PrologueR11_56_170,
        "DSP::release",             kFH6_PrologueRbx_50,
        "ChannelControl::addDSP",   kFH6_PrologueR11_56_170,
        "ChannelControl::removeDSP", kFH6_PrologueRbxRsi_50,

        // fmod_handle_resolver (handle decode shift-17 / mask 0xFFF / movzx).
        "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20"
        " 8B F9 8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00"
        " 0F B7 E8 4C 8B F2 4C 8B F9",
        // fmod_handle_unlock ([rcx+0x109F0] lock ptr + null-check stub).
        "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3",

        // radio_state_singleton (rip32, operand at +21).
        "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA"
        " 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 16 48 8D 4C 24 20"
        " E8 ?? ?? ?? ?? 48 8B D0 48 8B CB",
        /*operand=*/21,

        // radio_set_station_by_name.
        "48 89 5C 24 18 56 57 41 57 48 83 EC 30 4C 8B F9 48 8B DA"
        " 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 83 7B 18 0F",

        // logo_create_2d_raw_texture (direct).
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 B9"
        " 48 81 EC E0 00 00 00 48 8B F2 48 8B D9 44 89 45 C7 44 89 4D CB"
        " 8B 45 77 89 45 CF C7 45 D3 01 00 00 00 48 C7 45 D7 1C 00 00 00"
        " 33 FF 48 C7 45 DF 08 00 00 00",
        // logo_texture_system (rip32, operand +34).
        "48 89 7C 24 38 89 7C 24 30 48 89 54 24 28 C6 44 24 20 FF"
        " 4C 8D 4D 2F 4C 8D 45 C7 48 8D 55 7F 48 8B 0D ?? ?? ?? ??"
        " E8 ?? ?? ?? ?? 90 0F 57 C0",
        /*texture_system_operand=*/34,
        // logo_request_manager (rip32, operand +14).
        "41 B8 FF FF FF FF 48 8D 54 24 60 48 8B 0D ?? ?? ?? ??"
        " E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 0C",
        /*request_manager_operand=*/14,
        // logo_resource_registry (rip32, operand +13).
        "41 B8 FF FF FF 7F 48 8B 57 08 48 8B 0D ?? ?? ?? ??"
        " E8 ?? ?? ?? ?? 90 48 8D 4C 24 60",
        /*resource_registry_operand=*/13,
        // logo_inplace_upload (sub_3522560 async upload-queue producer).
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 ?? 55 41 54 41 55 41 56"
        " 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 45 0F B6 F9 48 8B F2"
        " 4C 8B E9 41 83 38 02",

        kFH6_BuildOverrides,
        sizeof(kFH6_BuildOverrides) / sizeof(kFH6_BuildOverrides[0]),

        // FMOD ChannelControl mute/volume levers (runtime-mute path).
        "ChannelControl::setVolume",
        "ChannelControl::setMute",
        "ChannelControl::getNumDSPs",
        "ChannelControl::getDSP",
    },
    /* radio */ {
        /*state_player_offset=*/0x40,
        /*state_race_active_a_offset=*/0x68,
        /*state_race_active_b_offset=*/0x69,
        /*state_menu_open_offset=*/0x6D,
        /*state_race_restart_menu_offset=*/0x80,
        /*player_selected_station_offset=*/0x50,
        /*player_stinger_marker_offset=*/{0x128, 0x1D8, 0x288, 0x338, 0x3E8},
        /*race_menu_stinger_marker=*/0x8A4CE41Bu,
        /*station_name_offset=*/0x200,
        /*sound_system_backref_offset=*/0xC0,
        /*station_name_streamer=*/"Streamer Mode",
        /*station_name_spotify=*/"Spotify Radio",
        /*wired_sound_name=*/"HZ6_R9_PeterBroderick_EyesClosedandTraveling",
    },
};

// ---- FH5 -----------------------------------------------------------------
// Secondary race-state and logo features fail closed when their build-specific
// data is unavailable or does not validate.

const GameProfile kFH5 = {
    GameId::FH5,
    "FH5",
    L"forzahorizon5.exe",
    /* sig */ {
        // FH5 wrappers: same anchors; prologue stack frame is 0x160 (vs FH6
        // 0x150). createDSP + addDSP share a prologue (each still uniquely
        // anchored by its own string).
        "System::createDSP",
        "40 53 55 56 57 41 56 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF",
        "DSP::release",
        "40 57 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF 48 89 9C 24 78 01",
        "ChannelControl::addDSP",
        "40 53 55 56 57 41 56 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF",
        "ChannelControl::removeDSP",
        "48 8B C4 57 48 81 EC 60 01 00 00 48 C7 44 24 40 FE FF FF FF 48 89 58 18",

        // fmod_handle_resolver: identical FMOD handle decoder -> FH6 pattern
        // matches verbatim.
        "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20"
        " 8B F9 8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00"
        " 0F B7 E8 4C 8B F2 4C 8B F9",
        // fmod_handle_unlock: FH5 lock member is +0x115A8 (vs FH6 +0x109F0)
        // and the stub has a frame + inline mutex-release call.
        "48 83 EC 28 48 8B 89 A8 15 01 00 48 85 C9 74 09 E8 ?? ?? ?? ??"
        " 85 C0 75 02 33 C0 48 83 C4 28 C3",

        // radio_state_singleton: FH6 rip32 pattern matches verbatim (operand +21).
        "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA"
        " 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 16 48 8D 4C 24 20"
        " E8 ?? ?? ?? ?? 48 8B D0 48 8B CB",
        /*operand=*/21,

        // radio_set_station_by_name: FH5 selects by interned hash id; no
        // portable anchor yet -> startup nudge unavailable.
        nullptr,

        // Runtime logo is unavailable for this profile.
        nullptr,            // logo_create_2d_raw_texture
        nullptr, 0,         // logo_texture_system (+operand)
        nullptr, 0,         // logo_request_manager (+operand)
        nullptr, 0,         // logo_resource_registry (+operand)
        nullptr,            // logo_inplace_upload

        nullptr, 0,         // no build overrides

        // FMOD ChannelControl mute/volume levers — same FMOD, same anchors.
        "ChannelControl::setVolume",
        "ChannelControl::setMute",
        "ChannelControl::getNumDSPs",
        "ChannelControl::getDSP",
    },
    /* radio */ {
        /*state_player_offset=*/0x40,
        /*state_race_active_a_offset=*/0x68,
        /*state_race_active_b_offset=*/0x69,
        /*state_menu_open_offset=*/0x6D,
        /*state_race_restart_menu_offset=*/0x80,
        /*player_selected_station_offset=*/0x50,
        /*player_stinger_marker_offset=*/{0x128, 0x1D8, 0x288, 0x338, 0x3E8},
        /*race_menu_stinger_marker=*/0x8A4CE41Bu,
        /*station_name_offset=*/0x200,
        /*sound_system_backref_offset=*/0xD8,
        /*station_name_streamer=*/"Streamer Mode",
        /*station_name_spotify=*/"Spotify Radio",
        /*wired_sound_name=*/"HZ5_R5_Metrik_Route174",
    },
};

GameId g_detected = GameId::Unknown;
bool g_detected_done = false;
std::once_flag g_detect_once;

void detect_once() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(::GetModuleHandleW(nullptr), path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_detected = GameId::Unknown;
        g_detected_done = true;
        return;
    }
    // Basename.
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    GameId id = GameId::Unknown;
    if (_wcsicmp(base, kFH6.exe_name) == 0) id = GameId::FH6;
    else if (_wcsicmp(base, kFH5.exe_name) == 0) id = GameId::FH5;

    g_detected = id;
    g_detected_done = true;

    std::string nbase;
    for (const wchar_t* p = base; *p; ++p) {
        nbase.push_back(static_cast<char>(*p));  // exe names are ASCII
    }
    const char* label = (id == GameId::FH6) ? "FH6"
                      : (id == GameId::FH5) ? "FH5"
                      : "Unknown (defaulting to FH6 profile)";
    log::info(std::string("[profile] host=") + nbase + " -> " + label);
}

} // namespace

const GameProfile& fh6_profile() { return kFH6; }
const GameProfile& fh5_profile() { return kFH5; }

GameId detect_game_id() {
    std::call_once(g_detect_once, detect_once);
    return g_detected;
}

const GameProfile& profile_for(GameId id) {
    switch (id) {
        case GameId::FH5: return kFH5;
        case GameId::FH6: return kFH6;
        default:          return kFH6;  // preserve FH6 behavior on unknown host
    }
}

const GameProfile& active_profile() {
    return profile_for(detect_game_id());
}

} // namespace bridge
