// Spotify OAuth (Authorization Code + PKCE) for the bridge.
//
// Used as an alternative to Zeroconf pairing: Zeroconf discovery is blocked for
// packaged/GamePass installs (the controller can't reach the in-AppContainer
// pairing HTTP server). OAuth is entirely OUTBOUND — the device fetches a token
// and logs in to Spotify directly — so it works inside the sandbox, and the
// device then appears via Spotify's cloud device list instead of local mDNS.
//
// Uses Spotify's public "keymaster" client_id (the one every librespot project
// uses): no app registration, no per-user cap, loopback redirect, no secret
// (PKCE). The access token only bootstraps the first login; librespotc then
// persists a reusable credential to credentials.dat for silent logins after.

#pragma once

#include <string>

namespace bridge::spotify_oauth {

// Build the Spotify authorize URL. Generates and stores a fresh PKCE verifier +
// CSRF state + the redirect_uri for the subsequent exchange. `redirect_uri` is
// the loopback URL the browser is redirected back to (e.g.
// "http://127.0.0.1:8103/login").
std::string build_authorize_url(const std::string& redirect_uri);

// True if `state` matches the state from the most recent build_authorize_url
// (CSRF guard for the auto-capture redirect handler).
bool state_matches(const std::string& state);

// Exchange an authorization code for an access token over HTTPS (WinHTTP, all
// outbound). Uses the verifier + redirect_uri stored by build_authorize_url.
// Returns true and fills `access_token`; otherwise false and fills `error`.
bool exchange_code(const std::string& code,
                   std::string& access_token,
                   std::string& error);

} // namespace bridge::spotify_oauth
