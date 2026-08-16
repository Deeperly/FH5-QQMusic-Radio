#include "stderr_pump.h"

#ifdef SPOTIFY_RADIO_DIAG

#include "log_file.h"

#include <Windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>

#include <atomic>
#include <string>
#include <thread>

namespace bridge::stderr_pump {

namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};
std::thread       g_thr;
HANDLE            g_read  = nullptr;
HANDLE            g_write = nullptr;
int               g_saved_fd2 = -1;        // dup of stderr's fd before redirect
int               g_saved_stderr_fd = -1;  // stderr's fd we redirected

void pump_loop() {
    std::string line;
    line.reserve(256);
    char buf[1024];
    DWORD got = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        if (!::ReadFile(g_read, buf, sizeof(buf), &got, nullptr) || got == 0) {
            break;  // pipe closed or broken
        }
        for (DWORD i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    bridge::log::info(std::string("[stderr] ") + line);
                }
                line.clear();
            } else {
                line.push_back(c);
                if (line.size() > 4096) {
                    bridge::log::info(std::string("[stderr] ") + line);
                    line.clear();
                }
            }
        }
    }
    if (!line.empty()) bridge::log::info(std::string("[stderr] ") + line);
}

} // namespace

void start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    // 64 KB buffer keeps short bursty stderr writes from blocking the
    // librespotc thread when the pump is briefly behind.
    if (!::CreatePipe(&g_read, &g_write, &sa, 64 * 1024)) {
        bridge::log::warn("[stderr_pump] CreatePipe failed; librespotc stderr "
                          "will not be captured");
        g_started.store(false);
        return;
    }

    int write_fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(g_write),
                                     _O_TEXT);
    if (write_fd < 0) {
        bridge::log::warn("[stderr_pump] _open_osfhandle failed");
        ::CloseHandle(g_read);  ::CloseHandle(g_write);
        g_read = g_write = nullptr;
        g_started.store(false);
        return;
    }

    // The DLL runs inside a windowed game process with no console — fd 2
    // and the CRT `stderr` FILE* may both be invalid at this point.
    // freopen("nul", "w", stderr) guarantees `stderr` is a valid FILE*
    // bound to some real fd. Then we redirect that fd to our pipe's
    // write end. Bypasses any need to touch fd 2 directly.
    if (!::freopen("nul", "w", stderr)) {
        bridge::log::warn("[stderr_pump] freopen(nul, stderr) failed");
        ::_close(write_fd);
        ::CloseHandle(g_read);
        g_read = g_write = nullptr;
        g_started.store(false);
        return;
    }
    int stderr_fd = ::_fileno(stderr);
    if (stderr_fd < 0) {
        bridge::log::warn("[stderr_pump] _fileno(stderr) returned -1");
        ::_close(write_fd);
        ::CloseHandle(g_read);
        g_read = g_write = nullptr;
        g_started.store(false);
        return;
    }
    // Save stderr's current underlying fd so stop() can restore it.
    g_saved_fd2 = ::_dup(stderr_fd);
    if (::_dup2(write_fd, stderr_fd) != 0) {
        bridge::log::warn("[stderr_pump] _dup2(stderr) failed");
        ::_close(write_fd);
        ::CloseHandle(g_read);
        g_read = g_write = nullptr;
        g_started.store(false);
        return;
    }
    ::_close(write_fd);   // stderr_fd now owns the write end; close the dup
    ::setvbuf(stderr, nullptr, _IONBF, 0);
    g_saved_stderr_fd = stderr_fd;

    g_stop.store(false);
    g_thr = std::thread(pump_loop);
    bridge::log::info("[stderr_pump] librespotc stderr capture active");
}

void stop() {
    if (!g_started.exchange(false)) return;
    g_stop.store(true);
    // Restore stderr's fd to whatever it pointed at before we hijacked it
    // so subsequent fprintf(stderr,...) writes don't try to push into a
    // pipe whose reader is about to disappear.
    if (g_saved_fd2 >= 0 && g_saved_stderr_fd >= 0) {
        ::_dup2(g_saved_fd2, g_saved_stderr_fd);
        ::_close(g_saved_fd2);
        g_saved_fd2 = -1;
        g_saved_stderr_fd = -1;
    }
    if (g_write) { ::CloseHandle(g_write); g_write = nullptr; }
    if (g_thr.joinable()) g_thr.join();
    if (g_read)  { ::CloseHandle(g_read);  g_read  = nullptr; }
}

} // namespace bridge::stderr_pump

#else  // !SPOTIFY_RADIO_DIAG

namespace bridge::stderr_pump {
void start() {}
void stop()  {}
} // namespace bridge::stderr_pump

#endif
