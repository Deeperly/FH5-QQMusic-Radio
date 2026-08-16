#include "spotify_oauth.h"

#include "log_file.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

namespace bridge::spotify_oauth {

namespace {

// Spotify's public desktop ("keymaster") client_id — the same one librespot,
// go-librespot and spotifyd use for the OAuth path. No registration / user cap.
constexpr const char* kClientId = "65b708073fc0480ea92a077233ca87bd";
constexpr const char* kScopes =
    "streaming app-remote-control user-read-email user-read-private";
constexpr const char* kAuthorizeBase = "https://accounts.spotify.com/authorize";

std::mutex g_mtx;
std::string g_verifier;     // PKCE code_verifier for the in-flight attempt
std::string g_state;        // CSRF state
std::string g_redirect_uri; // redirect used at authorize time (must match exchange)

std::string base64url(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
    }
    if (i + 1 == len) {
        uint32_t n = data[i] << 16;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
    } else if (i + 2 == len) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
    }
    return out;  // no '=' padding (base64url)
}

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> b(n);
    if (::BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(n),
                          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // Extremely unlikely; fall back to a weak source so we never hand back
        // zeroed entropy. (Not security-critical here — PKCE verifier only.)
        for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(i * 131 + 17);
    }
    return b;
}

bool sha256(const std::string& in, std::vector<uint8_t>& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    bool ok = false;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (::BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        if (::BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())),
                             static_cast<ULONG>(in.size()), 0) == 0) {
            out.resize(32);
            ok = ::BCryptFinishHash(h, out.data(), 32, 0) == 0;
        }
        ::BCryptDestroyHash(h);
    }
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string url_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// POST application/x-www-form-urlencoded to an HTTPS host. Returns body in resp.
bool https_post_form(const wchar_t* host, const wchar_t* path,
                     const std::string& body, std::string& resp,
                     std::string& error) {
    bool ok = false;
    HINTERNET hs = ::WinHttpOpen(L"FH6SpotifyRadio/1.0",
                                 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { error = "WinHttpOpen failed"; return false; }
    HINTERNET hc = ::WinHttpConnect(hs, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hc) {
        HINTERNET hr = ::WinHttpOpenRequest(
            hc, L"POST", path, nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hr) {
            const wchar_t* hdr =
                L"Content-Type: application/x-www-form-urlencoded\r\n";
            if (::WinHttpSendRequest(hr, hdr, (DWORD)-1L,
                                     (LPVOID)body.data(), (DWORD)body.size(),
                                     (DWORD)body.size(), 0) &&
                ::WinHttpReceiveResponse(hr, nullptr)) {
                DWORD avail = 0;
                while (::WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!::WinHttpReadData(hr, chunk.data(), avail, &read) || read == 0) {
                        break;
                    }
                    resp.append(chunk.data(), read);
                }
                ok = true;
            } else {
                error = "WinHttp send/receive failed (err=" +
                        std::to_string(::GetLastError()) + ")";
            }
            ::WinHttpCloseHandle(hr);
        } else {
            error = "WinHttpOpenRequest failed";
        }
        ::WinHttpCloseHandle(hc);
    } else {
        error = "WinHttpConnect failed";
    }
    ::WinHttpCloseHandle(hs);
    return ok;
}

// Minimal JSON string-field extractor (same approach as options.cpp).
bool extract_json_string(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return false;
    out.clear();
    for (size_t i = q + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return true;
        if (c == '\\' && i + 1 < json.size()) { out.push_back(json[++i]); continue; }
        out.push_back(c);
    }
    return false;
}

} // namespace

std::string build_authorize_url(const std::string& redirect_uri) {
    auto verifier_bytes = random_bytes(48);          // 48 bytes -> 64 b64url chars
    std::string verifier = base64url(verifier_bytes.data(), verifier_bytes.size());
    std::vector<uint8_t> digest;
    std::string challenge;
    if (sha256(verifier, digest)) {
        challenge = base64url(digest.data(), digest.size());
    }
    auto state_bytes = random_bytes(16);
    std::string state = base64url(state_bytes.data(), state_bytes.size());

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_verifier = verifier;
        g_state = state;
        g_redirect_uri = redirect_uri;
    }

    std::string url = std::string(kAuthorizeBase) +
        "?response_type=code" +
        "&client_id=" + url_encode(kClientId) +
        "&scope=" + url_encode(kScopes) +
        "&redirect_uri=" + url_encode(redirect_uri) +
        "&state=" + url_encode(state) +
        "&code_challenge_method=S256" +
        "&code_challenge=" + url_encode(challenge);
    return url;
}

bool state_matches(const std::string& state) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return !g_state.empty() && state == g_state;
}

bool exchange_code(const std::string& code, std::string& access_token,
                   std::string& error) {
    std::string verifier, redirect;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        verifier = g_verifier;
        redirect = g_redirect_uri;
    }
    if (verifier.empty() || redirect.empty()) {
        error = "no OAuth attempt in progress (start login again)";
        return false;
    }

    std::string body =
        "grant_type=authorization_code" +
        std::string("&code=") + url_encode(code) +
        "&redirect_uri=" + url_encode(redirect) +
        "&client_id=" + url_encode(kClientId) +
        "&code_verifier=" + url_encode(verifier);

    std::string resp;
    if (!https_post_form(L"accounts.spotify.com", L"/api/token", body, resp, error)) {
        log::warn("[oauth] token exchange transport failed: " + error);
        return false;
    }
    if (!extract_json_string(resp, "access_token", access_token) ||
        access_token.empty()) {
        std::string desc;
        extract_json_string(resp, "error_description", desc);
        if (desc.empty()) extract_json_string(resp, "error", desc);
        error = desc.empty() ? "no access_token in token response" : desc;
        log::warn("[oauth] token exchange rejected: " + error);
        return false;
    }
    // One-shot: clear the verifier so a stale code can't be replayed.
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_verifier.clear();
        g_state.clear();
    }
    log::info("[oauth] access token obtained");
    return true;
}

} // namespace bridge::spotify_oauth
