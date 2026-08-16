// Forza "Data Out" UDP telemetry listener. See header.

// winsock2.h must precede any windows.h (pulled in transitively by project
// headers) to avoid the legacy winsock.h clash.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "forza_dataout.h"
#include "log_file.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace bridge {

namespace {

// Forza "Data Out" packet field offsets. These are stable across the Sled (V1,
// 232 B), Car Dash and Horizon formats — the velocity vector lives in the shared
// sled prefix, so reading it is format-agnostic (no need to detect the variant
// or chase the Horizon 12-byte placeholder that shifts the later Speed field).
//   +0   s32 IsRaceOn (0 = in menu / not driving; telemetry stale)
//   +32  f32 VelocityX  (world-space, m/s)
//   +36  f32 VelocityY
//   +40  f32 VelocityZ
// Speed (m/s) == |velocity|.
constexpr int    kOffIsRaceOn = 0;
constexpr int    kOffVelX     = 32;
constexpr int    kOffVelY     = 36;
constexpr int    kOffVelZ     = 40;
constexpr int    kMinPacketLen = 44;     // need through VelocityZ
constexpr float  kMaxPlausibleSpeedMps = 140.0f;  // ~504 km/h

float read_f32(const char* p) {
    float v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

}  // namespace

ForzaDataOut::~ForzaDataOut() { stop(); }

bool ForzaDataOut::start(uint16_t port, SpeedCallback on_speed) {
    if (running_.load(std::memory_order_acquire)) return true;
    port_ = port;
    on_speed_ = std::move(on_speed);

    WSADATA wsa{};
    // Ref-counted; harmless if the process already initialized Winsock elsewhere.
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        wsa_inited_ = true;
    }

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        log::warn("[dataout] socket() failed (err=" +
                  std::to_string(WSAGetLastError()) +
                  "); Data Out speed disabled");
        if (wsa_inited_) { WSACleanup(); wsa_inited_ = false; }
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        log::warn("[dataout] bind 127.0.0.1:" + std::to_string(port) +
                  " failed (err=" + std::to_string(WSAGetLastError()) +
                  "); is another telemetry tool using this port? Data Out speed "
                  "disabled");
        ::closesocket(s);
        if (wsa_inited_) { WSACleanup(); wsa_inited_ = false; }
        return false;
    }

    // Recv timeout so the thread can observe running_ == false and exit promptly.
    DWORD timeout_ms = 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    socket_ = static_cast<uintptr_t>(s);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { run(); });
    log::info("[dataout] listening for Forza Data Out on 127.0.0.1:" +
              std::to_string(port) +
              " — enable Settings>HUD>Data Out (IP 127.0.0.1, this port) to feed "
              "Night Runners speed");
    return true;
}

void ForzaDataOut::run() {
    char buf[2048];
    auto last_log = std::chrono::steady_clock::now();
    bool logged_first = false;

    while (running_.load(std::memory_order_acquire)) {
        int n = ::recvfrom(static_cast<SOCKET>(socket_), buf,
                           static_cast<int>(sizeof(buf)), 0, nullptr, nullptr);
        if (n == SOCKET_ERROR) {
            // Timeout (expected) or socket torn down on stop(); loop re-checks
            // running_. Any other transient error: just retry.
            continue;
        }
        if (n < kMinPacketLen) continue;

        int32_t is_race_on = 0;
        std::memcpy(&is_race_on, buf + kOffIsRaceOn, sizeof(is_race_on));

        float speed = 0.0f;
        if (is_race_on != 0) {
            float vx = read_f32(buf + kOffVelX);
            float vy = read_f32(buf + kOffVelY);
            float vz = read_f32(buf + kOffVelZ);
            if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) {
                continue;
            }
            speed = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (!std::isfinite(speed) || speed < 0.0f ||
                speed > kMaxPlausibleSpeedMps) {
                continue;
            }
        }
        // is_race_on == 0 -> report 0 (stopped) so Night Runners settles to idle.

        if (on_speed_) on_speed_(speed);

        // Throttled confirmation log (diag/non-public builds only).
        auto now = std::chrono::steady_clock::now();
        if (!logged_first ||
            now - last_log >= std::chrono::seconds(2)) {
            logged_first = true;
            last_log = now;
            char line[96];
            std::snprintf(line, sizeof(line),
                          "[dataout] speed=%.2f m/s (%.0f km/h) raceOn=%d",
                          speed, speed * 3.6f, static_cast<int>(is_race_on != 0));
            log::info(line);
        }
    }
}

void ForzaDataOut::stop() {
    running_.store(false, std::memory_order_release);
    if (socket_ != ~static_cast<uintptr_t>(0)) {
        ::closesocket(static_cast<SOCKET>(socket_));
        socket_ = ~static_cast<uintptr_t>(0);
    }
    if (thread_.joinable()) thread_.join();
    if (wsa_inited_) { WSACleanup(); wsa_inited_ = false; }
}

}  // namespace bridge
