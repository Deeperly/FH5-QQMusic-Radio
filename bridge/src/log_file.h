// File logger for version.dll context (no console available).
//
// Dev builds write <game-dir>/spotify-radio.log. Public builds keep only
// in-memory rings for the status dashboard. Thread-safe, flush-on-write.
// Also keeps an in-memory ring of the last N formatted lines + the last M
// WARN/ERROR lines so the status web UI can render them via SSE without
// reading the file.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace bridge::log {

void init(const std::filesystem::path& path, std::string_view package_version = {});
void shutdown();
void write(std::string_view level, std::string_view msg);

inline void info (std::string_view m) { write("INFO",  m); }
inline void warn (std::string_view m) { write("WARN",  m); }
inline void error(std::string_view m) { write("ERROR", m); }

// Read-only snapshots for the status UI. Each call returns a copy
// of the in-memory rings so the caller can serialize without holding
// the logger mutex.
std::vector<std::string> tail(size_t max = 50);
std::vector<std::string> recent_errors(size_t max = 5);

} // namespace bridge::log
