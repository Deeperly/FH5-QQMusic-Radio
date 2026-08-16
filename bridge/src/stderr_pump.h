// Redirects the C runtime stderr (fd 2) into bridge::log.
//
// The vendored librespotc lib writes diagnostics via fprintf(stderr,...).
// In a windowless DLL context (no console attached) those bytes go to a
// nowhere handle and are lost. This pump creates an anonymous pipe, dups
// the read end to fd 2, and spawns a worker thread that drains the read
// end line-by-line into bridge::log. Each captured line is tagged
// "[stderr] ...".
//
// Only compiled in when SPOTIFY_RADIO_DIAG is defined. Production builds
// have no pipe overhead.

#pragma once

namespace bridge::stderr_pump {

// Idempotent. Safe to call once during DLL init before any librespotc work.
// Does nothing if the redirect already succeeded.
void start();

// Stops the pump thread and restores best-effort. Call from shutdown.
void stop();

} // namespace bridge::stderr_pump
