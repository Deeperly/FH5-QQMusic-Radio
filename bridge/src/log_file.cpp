#include "log_file.h"

#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <deque>

namespace bridge::log {

namespace {

std::ofstream g_file;
std::mutex    g_mtx;

constexpr size_t kTailMax  = 200;  // total log ring
constexpr size_t kErrorMax = 20;   // WARN+ERROR ring (subset)

std::deque<std::string> g_tail;
std::deque<std::string> g_errors;

} // namespace

void init(const std::filesystem::path& path, std::string_view package_version) {
    std::lock_guard lock(g_mtx);
    g_tail.clear();
    g_errors.clear();

#if !defined(SPOTIFY_RADIO_PUBLIC) || defined(SPOTIFY_RADIO_DIAG)
    // Dev build (or diagnostic public build) — write a disk log alongside the
    // DLL. Public release builds skip file I/O entirely unless SPOTIFY_RADIO_DIAG
    // is also defined, in which case we ship a one-off diagnostic build that
    // captures the log for a single repro.
    g_file.open(path, std::ios::trunc);
    if (g_file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::string header = std::string("=== Session started ") + buf;
        if (!package_version.empty()) {
            header += " (version " + std::string(package_version) + ")";
        }
        header += " ===";
        g_file << header << '\n';
        g_file.flush();
        g_tail.push_back(header);
    }
#else
    (void)path;
    (void)package_version;
#endif
}

void shutdown() {
    std::lock_guard lock(g_mtx);
#if !defined(SPOTIFY_RADIO_PUBLIC) || defined(SPOTIFY_RADIO_DIAG)
    if (g_file.is_open()) {
        g_file << "=== Session ended ===\n";
        g_file.close();
    }
#endif
}

void write(std::string_view level, std::string_view msg) {
    std::lock_guard lock(g_mtx);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", std::localtime(&t));

    std::string line;
    line.reserve(64 + msg.size());
    line.append("[").append(time_buf).append("] [");
    line.append(level.data(), level.size()).append("] ");
    line.append(msg.data(), msg.size());

#if !defined(SPOTIFY_RADIO_PUBLIC) || defined(SPOTIFY_RADIO_DIAG)
    if (g_file.is_open()) {
        g_file << line << '\n';
        g_file.flush();
    }
#endif

    g_tail.push_back(line);
    while (g_tail.size() > kTailMax) g_tail.pop_front();

    if (level == "WARN" || level == "ERROR") {
        g_errors.push_back(line);
        while (g_errors.size() > kErrorMax) g_errors.pop_front();
    }
}

std::vector<std::string> tail(size_t max) {
    std::lock_guard lock(g_mtx);
    if (max == 0 || max >= g_tail.size()) {
        return { g_tail.begin(), g_tail.end() };
    }
    return { g_tail.end() - static_cast<std::ptrdiff_t>(max), g_tail.end() };
}

std::vector<std::string> recent_errors(size_t max) {
    std::lock_guard lock(g_mtx);
    if (max == 0 || max >= g_errors.size()) {
        return { g_errors.begin(), g_errors.end() };
    }
    return { g_errors.end() - static_cast<std::ptrdiff_t>(max), g_errors.end() };
}

} // namespace bridge::log
