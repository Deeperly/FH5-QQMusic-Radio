// Forza "Data Out" UDP telemetry listener.
//
// Forza Horizon/Motorsport ships a built-in "Data Out" feature (Settings -> HUD
// -> Data Out) that broadcasts the car telemetry over UDP in a fixed, documented
// packet format. On games where the bridge has no portable in-memory speed
// mirror (FH5), we read the car speed straight from these packets. The velocity
// vector has a stable documented offset and its magnitude is the speed in m/s.
//
// No external/dynamic dependencies — raw Winsock (ws2_32, already linked). The
// listener binds 127.0.0.1 only and is inert until the user enables Data Out
// in-game pointing at the same port. If the port can't be bound (another tool
// already using it) the feature simply fails open.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace bridge {

class ForzaDataOut {
public:
    using SpeedCallback = std::function<void(float mps)>;

    ForzaDataOut() = default;
    ~ForzaDataOut();
    ForzaDataOut(const ForzaDataOut&) = delete;
    ForzaDataOut& operator=(const ForzaDataOut&) = delete;

    // Bind 127.0.0.1:<port> and spawn the receive thread. `on_speed` is invoked
    // per valid packet with the car speed in m/s (0 while not in a race/freeroam
    // session). Returns false if the socket could not be created/bound.
    bool start(uint16_t port, SpeedCallback on_speed);

    // Stop the receive thread and release the socket. Idempotent.
    void stop();

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    bool wsa_inited_ = false;
    uintptr_t socket_ = ~static_cast<uintptr_t>(0);  // INVALID_SOCKET sentinel
    uint16_t port_ = 0;
    SpeedCallback on_speed_;
};

}  // namespace bridge
