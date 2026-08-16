#include "radio_source.h"

#include "bridge_state.h"
#include "fmod_inject.h"
#include "log_file.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace bridge {

namespace {

constexpr uint32_t kSampleRate = FmodInject::kPcmSampleRate;
constexpr uint32_t kChannels = FmodInject::kPcmChannels;
constexpr size_t kFrameBytes = kChannels * sizeof(int16_t);
constexpr size_t kMaxRadioSearchLimit = 50;
constexpr size_t kMaxRadioStoreItems = 100;
constexpr size_t kMaxHttpBodyBytes = 2 * 1024 * 1024;
constexpr size_t kMaxPlaylistBytes = 256 * 1024;
constexpr const wchar_t* kRadioBrowserHost = L"de1.api.radio-browser.info";
constexpr const wchar_t* kUserAgent = L"FH6 Spotify Radio/1.0";

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

template <class T>
void release_com(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

bool ensure_media_foundation_started() {
    static std::atomic<bool> attempted{false};
    static std::atomic<bool> ready{false};
    bool expected = false;
    if (attempted.compare_exchange_strong(expected, true)) {
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(hr)) {
            ready.store(true, std::memory_order_release);
        } else {
            log::warn("[radio] Media Foundation startup failed hr="
                      + std::to_string(static_cast<long>(hr)));
        }
    }
    return ready.load(std::memory_order_acquire);
}

std::wstring wide_from_utf8(const std::string& value) {
    if (value.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()),
                                     nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed);
    return out;
}

std::string utf8_from_wide(std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<size_t>(INT_MAX)) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::string trim_ascii(std::string_view value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool starts_with_ci(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool is_http_url(std::string_view url) {
    return starts_with_ci(url, "http://") || starts_with_ci(url, "https://");
}

void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xf]);
                    out.push_back(hex[c & 0xf]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void append_utf8_codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex4(std::string_view s, uint32_t& out) {
    if (s.size() < 4) return false;
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
        int h = hex_value(s[i]);
        if (h < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(h);
    }
    out = v;
    return true;
}

bool json_string_at(std::string_view json, size_t quote, std::string& out,
                    size_t* end_pos = nullptr) {
    if (quote >= json.size() || json[quote] != '"') return false;
    out.clear();
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') {
            if (end_pos) *end_pos = i + 1;
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= json.size()) return false;
        char e = json[i];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (i + 4 >= json.size() ||
                    !parse_hex4(json.substr(i + 1, 4), cp)) {
                    return false;
                }
                i += 4;
                if (cp >= 0xd800 && cp <= 0xdbff &&
                    i + 6 < json.size() && json[i + 1] == '\\' &&
                    json[i + 2] == 'u') {
                    uint32_t low = 0;
                    if (parse_hex4(json.substr(i + 3, 4), low) &&
                        low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        i += 6;
                    }
                }
                append_utf8_codepoint(out, cp);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

size_t find_json_key(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    return json.find(needle);
}

bool extract_json_string_field(std::string_view json,
                               std::string_view key,
                               std::string& out) {
    size_t key_pos = find_json_key(json, key);
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    if (json.substr(pos, 4) == "null") {
        out.clear();
        return true;
    }
    return json_string_at(json, pos, out);
}

bool extract_json_u32_field(std::string_view json,
                            std::string_view key,
                            uint32_t& out) {
    size_t key_pos = find_json_key(json, key);
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    char* end = nullptr;
    std::string rest(json.substr(pos));
    unsigned long parsed = std::strtoul(rest.c_str(), &end, 10);
    if (!end || end == rest.c_str() || parsed > UINT32_MAX) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool extract_json_i64_field(std::string_view json,
                            std::string_view key,
                            int64_t& out) {
    size_t key_pos = find_json_key(json, key);
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    char* end = nullptr;
    std::string rest(json.substr(pos));
    long long parsed = std::strtoll(rest.c_str(), &end, 10);
    if (!end || end == rest.c_str()) return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

bool extract_json_bool_field(std::string_view json,
                             std::string_view key,
                             bool& out) {
    size_t key_pos = find_json_key(json, key);
    if (key_pos == std::string_view::npos) return false;
    size_t colon = json.find(':', key_pos + key.size() + 2);
    if (colon == std::string_view::npos) return false;
    size_t pos = json.find_first_not_of(" \t\r\n", colon + 1);
    if (pos == std::string_view::npos) return false;
    if (json.substr(pos, 4) == "true" || json.substr(pos, 1) == "1") {
        out = true;
        return true;
    }
    if (json.substr(pos, 5) == "false" || json.substr(pos, 1) == "0") {
        out = false;
        return true;
    }
    return false;
}

std::vector<std::string_view> extract_json_objects(std::string_view json) {
    std::vector<std::string_view> out;
    bool in_string = false;
    bool esc = false;
    int depth = 0;
    size_t start = std::string_view::npos;
    for (size_t i = 0; i < json.size(); ++i) {
        char c = json[i];
        if (in_string) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && start != std::string_view::npos) {
                out.push_back(json.substr(start, i - start + 1));
                start = std::string_view::npos;
            }
        }
    }
    return out;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool atomic_replace_file(const std::filesystem::path& target,
                         const std::string& content,
                         std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create radio station directory";
        return false;
    }
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "could not open temporary radio station file";
            return false;
        }
        out << content;
        if (!out) {
            if (error) *error = "could not write radio station file";
            return false;
        }
    }
    if (!::MoveFileExW(tmp.wstring().c_str(), target.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not replace radio station file";
        return false;
    }
    return true;
}

std::wstring percent_encode_path(std::string_view path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() + 16);
    for (unsigned char c : path) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '.' || c == '~' || c == '/' || c == '?' ||
                  c == '&' || c == '=' || c == ',';
        if (ok) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return wide_from_utf8(out);
}

std::string query_escape(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                  c == '.' || c == '~';
        if (ok) {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xf]);
            out.push_back(hex[c & 0xf]);
        }
    }
    return out;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool parse_http_url(const std::string& url, UrlParts& out) {
    std::wstring wide = wide_from_utf8(url);
    if (wide.empty()) return false;
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) return false;
    if (parts.nScheme != INTERNET_SCHEME_HTTP &&
        parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        out.path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (out.path.empty()) out.path = L"/";
    out.port = parts.nPort;
    return !out.host.empty();
}

std::string absolute_redirect_url(const UrlParts& base, const std::wstring& location) {
    if (location.empty()) return {};
    std::string loc = utf8_from_wide(location);
    if (is_http_url(loc)) return loc;
    if (!loc.empty() && loc[0] == '/') {
        std::string out = base.secure ? "https://" : "http://";
        out += utf8_from_wide(base.host);
        bool default_port = (base.secure && base.port == INTERNET_DEFAULT_HTTPS_PORT) ||
                            (!base.secure && base.port == INTERNET_DEFAULT_HTTP_PORT);
        if (!default_port && base.port != 0) {
            out += ":" + std::to_string(base.port);
        }
        out += loc;
        return out;
    }
    return {};
}

struct HttpResult {
    bool ok = false;
    DWORD status = 0;
    std::string body;
    std::string error;
};

HttpResult http_get_url(const std::string& url,
                        size_t max_body = kMaxHttpBodyBytes,
                        uint32_t timeout_ms = 8000) {
    UrlParts u;
    if (!parse_http_url(url, u)) return {false, 0, {}, "invalid URL"};
    HINTERNET session = WinHttpOpen(
        kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {false, 0, {}, "WinHTTP open failed"};
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return {false, 0, {}, "WinHTTP connect failed"};
    }
    DWORD flags = u.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {false, 0, {}, "WinHTTP request failed"};
    }
    BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (received) {
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &status,
                            &status_size,
                            WINHTTP_NO_HEADER_INDEX);
    }
    std::string body;
    if (received && status >= 200 && status < 400) {
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) break;
            if (body.size() + available > max_body) {
                available = static_cast<DWORD>(max_body - body.size());
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (available == 0 ||
                !WinHttpReadData(request, chunk.data(), available, &read)) {
                break;
            }
            chunk.resize(read);
            body += chunk;
            if (body.size() >= max_body) break;
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!received) return {false, status, {}, "HTTP request failed"};
    if (status < 200 || status >= 400) {
        return {false, status, {}, "HTTP " + std::to_string(status)};
    }
    return {true, status, std::move(body), {}};
}

HttpResult http_get_radio_browser(const std::string& path_query,
                                  uint32_t timeout_ms = 8000) {
    std::string url = "https://de1.api.radio-browser.info" + path_query;
    return http_get_url(url, kMaxHttpBodyBytes, timeout_ms);
}

bool query_custom_u32_header(HINTERNET request,
                             const wchar_t* name,
                             uint32_t& out) {
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CUSTOM,
                             name,
                             buffer,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    wchar_t* end = nullptr;
    unsigned long value = std::wcstoul(buffer, &end, 10);
    if (value == 0 || value > 1024u * 1024u) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool query_header_string(HINTERNET request, DWORD query, std::wstring& out) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX,
                             buffer.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    out = std::move(buffer);
    return true;
}

std::string extract_icy_stream_title(std::string_view metadata) {
    size_t nul = metadata.find('\0');
    if (nul != std::string_view::npos) metadata = metadata.substr(0, nul);
    size_t key = metadata.find("StreamTitle=");
    if (key == std::string_view::npos) return {};
    size_t pos = key + 12;
    if (pos >= metadata.size()) return {};

    char quote = metadata[pos];
    if (quote == '\'' || quote == '"') {
        ++pos;
        size_t end = metadata.find(quote, pos);
        if (end == std::string_view::npos) return {};
        return trim_ascii(metadata.substr(pos, end - pos));
    }

    size_t end = metadata.find(';', pos);
    if (end == std::string_view::npos) end = metadata.size();
    return trim_ascii(metadata.substr(pos, end - pos));
}

std::pair<std::string, std::string> split_stream_title(const std::string& value) {
    std::string stream_title = trim_ascii(value);
    if (stream_title.empty()) return {{}, {}};

    size_t sep = stream_title.find(" - ");
    if (sep != std::string::npos && sep > 0 && sep + 3 < stream_title.size()) {
        std::string artist = trim_ascii(std::string_view(stream_title).substr(0, sep));
        std::string title = trim_ascii(std::string_view(stream_title).substr(sep + 3));
        if (!artist.empty() && !title.empty()) {
            return {std::move(title), std::move(artist)};
        }
    }

    return {std::move(stream_title), "Online Radio"};
}

std::string first_playlist_stream(const std::string& playlist) {
    std::istringstream in(playlist);
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ascii(line);
        if (line.empty() || line[0] == '#') continue;
        std::string lower = lower_ascii(line);
        if (lower.rfind("file", 0) == 0) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) line = trim_ascii(std::string_view(line).substr(eq + 1));
        }
        if (is_http_url(line)) return line;
    }
    return {};
}

std::string resolve_stream_url(const std::string& url, std::string* error) {
    if (!is_http_url(url)) {
        if (error) *error = "stream URL must start with http:// or https://";
        return {};
    }
    std::string lower = lower_ascii(url);
    bool looks_playlist = lower.find(".m3u") != std::string::npos ||
                          lower.find(".pls") != std::string::npos;
    if (!looks_playlist) return url;
    auto res = http_get_url(url, kMaxPlaylistBytes, 5000);
    if (!res.ok) {
        if (error) *error = "could not load playlist: " + res.error;
        return {};
    }
    std::string stream = first_playlist_stream(res.body);
    if (stream.empty()) {
        if (error) *error = "playlist did not contain a stream URL";
        return {};
    }
    return stream;
}

std::string station_artist(const RadioSource::Station& st) {
    (void)st;
    return "Online Radio";
}

std::string station_album(const RadioSource::Station& st) {
    std::string out;
    if (!st.codec.empty()) out += st.codec;
    if (st.bitrate > 0) {
        if (!out.empty()) out += " ";
        out += std::to_string(st.bitrate) + " kbps";
    }
    return out;
}

std::string station_id_from_uuid(const std::string& uuid) {
    return uuid.empty() ? std::string{} : "radiobrowser:" + uuid;
}

RadioSource::Station station_from_radio_browser_object(std::string_view obj) {
    RadioSource::Station st;
    st.provider = "radiobrowser";
    extract_json_string_field(obj, "stationuuid", st.stationuuid);
    extract_json_string_field(obj, "name", st.name);
    extract_json_string_field(obj, "url", st.stream_url);
    extract_json_string_field(obj, "url_resolved", st.resolved_url);
    extract_json_string_field(obj, "homepage", st.homepage);
    extract_json_string_field(obj, "favicon", st.favicon);
    extract_json_string_field(obj, "tags", st.tags);
    extract_json_string_field(obj, "countrycode", st.country_code);
    extract_json_string_field(obj, "language", st.language);
    extract_json_string_field(obj, "codec", st.codec);
    extract_json_u32_field(obj, "bitrate", st.bitrate);
    extract_json_bool_field(obj, "hls", st.hls);
    st.id = station_id_from_uuid(st.stationuuid);
    return st;
}

std::vector<RadioSource::Station> stations_from_radio_browser_json(
    std::string_view json) {
    std::vector<RadioSource::Station> out;
    for (auto obj : extract_json_objects(json)) {
        auto st = station_from_radio_browser_object(obj);
        if (st.id.empty() || st.name.empty() || st.hls) continue;
        out.push_back(std::move(st));
    }
    return out;
}

} // namespace

RadioSource::RadioSource(std::filesystem::path store_path,
                         FeedPcmFn feed_pcm,
                         FreeBytesFn free_bytes,
                         ClearPcmFn clear_pcm)
    : store_path_(std::move(store_path)),
      feed_pcm_(std::move(feed_pcm)),
      free_bytes_(std::move(free_bytes)),
      clear_pcm_(std::move(clear_pcm)) {}

RadioSource::~RadioSource() {
    shutdown();
}

void RadioSource::start() {
    running_.store(true, std::memory_order_release);
    load_store();
    std::lock_guard lock(mtx_);
    if (current_track_.uri.empty()) {
        current_track_.uri = "radio:";
        current_track_.title = "Online Radio";
        current_track_.artist = "Online Radio";
        current_track_.album.clear();
        current_track_.duration_ms = 0;
        current_track_.artwork_key = "online-radio";
        current_track_.artwork_mime.clear();
        current_track_.artwork_bytes.clear();
        current_track_.artwork_loading = false;
    }
}

void RadioSource::shutdown() {
    running_.store(false, std::memory_order_release);
    if (resume_thread_.joinable()) resume_thread_.join();
    stop_metadata();
    stop_stream();
}

bool RadioSource::is_connected() const {
    std::lock_guard lock(mtx_);
    return !current_station_.id.empty();
}

bool RadioSource::is_playing() const {
    return playing_.load(std::memory_order_acquire);
}

SourceTrack RadioSource::last_track() const {
    std::lock_guard lock(mtx_);
    return current_track_;
}

void RadioSource::pause_at_audio_boundary() {
    stop_metadata();
    stop_stream();
}

void RadioSource::resume_rewound(uint32_t rewind_ms) {
    (void)rewind_ms;
    if (playing_.load(std::memory_order_acquire)) return;

    Station station;
    {
        std::lock_guard lock(mtx_);
        station = current_station_;
    }
    if (!station.id.empty()) {
        std::string ignored;
        play_station(std::move(station), &ignored);
    } else {
        resume_last_station_async();
    }
}

void RadioSource::on_activated() {
    if (!playing_.load(std::memory_order_acquire)) {
        resume_last_station_async();
    }
}

void RadioSource::set_pcm_enabled(bool enabled) {
    pcm_enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        stop_metadata();
        stop_stream();
    }
}

void RadioSource::set_volume_percent(uint32_t percent) {
    uint32_t clamped = std::min<uint32_t>(percent, 300);
    volume_gain_.store(static_cast<float>(clamped) / 100.0f,
                       std::memory_order_release);
}

void RadioSource::publish_state(BridgeStateStore& store) const {
    Station st;
    bool connected = false;
    bool playing = is_playing();
    std::string error;
    {
        std::lock_guard lock(mtx_);
        st = current_station_;
        connected = !current_station_.id.empty();
        error = error_;
    }
    store.set_radio_source(true, connected, playing, st.name,
                           st.id, st.codec, st.bitrate, error);
}

std::string RadioSource::search_json(const std::string& q,
                                     const std::string& tag,
                                     const std::string& country_code,
                                     const std::string& language,
                                     uint32_t offset,
                                     uint32_t limit,
                                     std::string* error) {
    limit = std::clamp<uint32_t>(limit == 0 ? 24 : limit,
                                 1, static_cast<uint32_t>(kMaxRadioSearchLimit));
    std::string path = "/json/stations/search?hidebroken=true&limit="
        + std::to_string(limit) + "&offset=" + std::to_string(offset)
        + "&order=clickcount&reverse=true";
    if (!q.empty()) path += "&name=" + query_escape(q);
    if (!tag.empty()) path += "&tag=" + query_escape(tag);
    if (!country_code.empty()) path += "&countrycode=" + query_escape(country_code);
    if (!language.empty()) path += "&language=" + query_escape(language);
    auto res = http_get_radio_browser(path);
    if (!res.ok) {
        if (error) *error = res.error;
        return "{\"stations\":[]}";
    }
    auto stations = stations_from_radio_browser_json(res.body);
    return station_array_json("stations", stations);
}

std::string RadioSource::suggestions_json(const std::string& kind,
                                          uint32_t limit,
                                          std::string* error) {
    limit = std::clamp<uint32_t>(limit == 0 ? 24 : limit,
                                 1, static_cast<uint32_t>(kMaxRadioSearchLimit));
    if (kind == "recent") {
        std::vector<Station> recent;
        {
            std::lock_guard lock(mtx_);
            recent = store_.recent;
        }
        if (!recent.empty()) {
            if (recent.size() > limit) recent.resize(limit);
            return station_array_json("stations", recent);
        }
    }
    std::string endpoint = "/json/stations/topclick/";
    if (kind == "votes") endpoint = "/json/stations/topvote/";
    if (kind == "recent") endpoint = "/json/stations/lastclick/";
    auto res = http_get_radio_browser(endpoint + std::to_string(limit)
                                      + "?hidebroken=true");
    if (!res.ok) {
        if (error) *error = res.error;
        return "{\"stations\":[]}";
    }
    auto stations = stations_from_radio_browser_json(res.body);
    return station_array_json("stations", stations);
}

std::string RadioSource::favorites_json() const {
    std::lock_guard lock(mtx_);
    return station_array_json("stations", store_.favorites);
}

std::string RadioSource::manual_json() const {
    std::lock_guard lock(mtx_);
    std::string out = "{\"lastUrl\":";
    append_json_string(out, store_.last_manual_url);
    out += ",\"lastName\":";
    append_json_string(out, store_.last_manual_name);
    out += ",\"stations\":[";
    for (size_t i = 0; i < store_.manual.size(); ++i) {
        if (i) out += ",";
        out += station_to_json(store_.manual[i]);
    }
    out += "]}";
    return out;
}

bool RadioSource::add_favorite_json(std::string_view json, std::string* error) {
    Station st;
    if (!parse_station_json(json, st) || st.id.empty()) {
        if (error) *error = "invalid station";
        return false;
    }
    if (st.added_at_ms == 0) st.added_at_ms = now_ms();
    std::lock_guard lock(mtx_);
    auto it = std::find_if(store_.favorites.begin(), store_.favorites.end(),
                           [&](const Station& item) { return item.id == st.id; });
    if (it == store_.favorites.end()) {
        store_.favorites.insert(store_.favorites.begin(), st);
    } else {
        st.added_at_ms = it->added_at_ms;
        *it = st;
    }
    if (store_.favorites.size() > kMaxRadioStoreItems) {
        store_.favorites.resize(kMaxRadioStoreItems);
    }
    return save_store_locked(error);
}

bool RadioSource::remove_favorite(const std::string& id, std::string* error) {
    std::lock_guard lock(mtx_);
    auto old_size = store_.favorites.size();
    store_.favorites.erase(
        std::remove_if(store_.favorites.begin(), store_.favorites.end(),
                       [&](const Station& st) { return st.id == id; }),
        store_.favorites.end());
    if (old_size == store_.favorites.size()) {
        if (error) *error = "favorite not found";
        return false;
    }
    return save_store_locked(error);
}

bool RadioSource::remove_manual(const std::string& id, std::string* error) {
    std::lock_guard lock(mtx_);
    auto old_size = store_.manual.size();
    store_.manual.erase(
        std::remove_if(store_.manual.begin(), store_.manual.end(),
                       [&](const Station& st) { return st.id == id; }),
        store_.manual.end());
    if (old_size == store_.manual.size()) {
        if (error) *error = "saved station not found";
        return false;
    }
    return save_store_locked(error);
}

bool RadioSource::play_station_json(std::string_view json, std::string* error) {
    Station st;
    std::string id;
    if (extract_json_string_field(json, "id", id) && !id.empty() &&
        !extract_json_string_field(json, "name", st.name)) {
        return play_station_id(id, error);
    }
    if (!parse_station_json(json, st)) {
        // Support { "station": { ... } } bodies by parsing the first object
        // that actually contains a station name.
        for (auto obj : extract_json_objects(json)) {
            if (parse_station_json(obj, st) && !st.name.empty()) break;
        }
    }
    if (st.id.empty() || st.name.empty()) {
        if (error) *error = "invalid station";
        return false;
    }
    return play_station(std::move(st), error);
}

bool RadioSource::play_station_id(const std::string& id, std::string* error) {
    Station st;
    bool found = false;
    {
        std::lock_guard lock(mtx_);
        auto find_in = [&](const std::vector<Station>& items) {
            auto it = std::find_if(items.begin(), items.end(),
                                   [&](const Station& item) {
                return item.id == id;
            });
            if (it != items.end()) {
                st = *it;
                return true;
            }
            return false;
        };
        found = find_in(store_.favorites) || find_in(store_.recent) ||
                find_in(store_.manual);
    }
    if (!found && starts_with_ci(id, "radiobrowser:")) {
        std::string uuid = id.substr(std::strlen("radiobrowser:"));
        auto res = http_get_radio_browser("/json/stations/byuuid?uuids="
                                          + query_escape(uuid));
        if (res.ok) {
            auto stations = stations_from_radio_browser_json(res.body);
            if (!stations.empty()) {
                st = stations.front();
                found = true;
            }
        }
    }
    if (!found) {
        if (error) *error = "station not found";
        return false;
    }
    return play_station(std::move(st), error);
}

bool RadioSource::play_url(const std::string& url,
                           const std::string& name,
                           std::string* error) {
    std::string resolved = resolve_stream_url(trim_ascii(url), error);
    if (resolved.empty()) return false;
    Station st;
    st.id = make_manual_id(resolved);
    st.provider = "manual";
    st.name = trim_ascii(name).empty() ? resolved : trim_ascii(name);
    st.stream_url = trim_ascii(url);
    st.resolved_url = resolved;
    st.added_at_ms = now_ms();
    {
        std::lock_guard lock(mtx_);
        auto it = std::find_if(store_.manual.begin(), store_.manual.end(),
                               [&](const Station& item) { return item.id == st.id; });
        if (it == store_.manual.end()) {
            store_.manual.insert(store_.manual.begin(), st);
        } else {
            *it = st;
        }
        if (store_.manual.size() > kMaxRadioStoreItems) {
            store_.manual.resize(kMaxRadioStoreItems);
        }
        store_.last_manual_url = st.stream_url;
        store_.last_manual_name = trim_ascii(name);
        save_store_locked(nullptr);
    }
    return play_station(std::move(st), error);
}

void RadioSource::stop_playback() {
    stop_metadata();
    stop_stream();
    std::lock_guard lock(mtx_);
    if (store_.last_playing) {
        store_.last_playing = false;
        save_store_locked(nullptr);
    }
}

void RadioSource::resume_last_station_async() {
    Station station;
    std::string id;
    {
        std::lock_guard lock(mtx_);
        if (!store_.last_playing || store_.last_station_id.empty()) return;
        if (!current_station_.id.empty()) {
            station = current_station_;
        } else {
            id = store_.last_station_id;
        }
    }

    bool expected = false;
    if (!resume_in_progress_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    if (resume_thread_.joinable()) resume_thread_.join();

    resume_thread_ = std::thread([this, station = std::move(station), id]() mutable {
        auto done = [this]() {
            resume_in_progress_.store(false, std::memory_order_release);
        };
        if (!running_.load(std::memory_order_acquire)) {
            done();
            return;
        }
        log::info("[radio] resuming last station "
                  + (!station.id.empty() ? station.id : id));
        std::string error;
        bool ok = false;
        if (!station.id.empty()) {
            ok = play_station(std::move(station), &error);
        } else {
            ok = play_station_id(id, &error);
        }
        if (!ok) {
            log::warn("[radio] resume failed: "
                      + (error.empty() ? std::string("unknown error") : error));
        }
        done();
    });
}

std::string RadioSource::last_error() const {
    std::lock_guard lock(mtx_);
    return error_;
}

RadioSource::Station RadioSource::current_station() const {
    std::lock_guard lock(mtx_);
    return current_station_;
}

void RadioSource::load_store() {
    std::string raw = read_file(store_path_);
    Store next;
    if (!raw.empty()) {
        extract_json_string_field(raw, "lastStationId", next.last_station_id);
        extract_json_bool_field(raw, "lastPlaying", next.last_playing);
        extract_json_string_field(raw, "lastManualUrl", next.last_manual_url);
        extract_json_string_field(raw, "lastManualName", next.last_manual_name);
        auto load_array = [&](std::string_view key, std::vector<Station>& out) {
            // Match the key position only, not station field values: provider
            // strings like "manual" would otherwise hit before the manual array.
            std::string needle = "\"" + std::string(key) + "\":";
            size_t key_pos = raw.find(needle);
            if (key_pos == std::string::npos) return;
            size_t start = raw.find('[', key_pos);
            size_t end = raw.find(']', start == std::string::npos ? key_pos : start);
            if (start == std::string::npos || end == std::string::npos) return;
            for (auto obj : extract_json_objects(std::string_view(raw).substr(start, end - start + 1))) {
                Station st;
                if (parse_station_json(obj, st) && !st.id.empty()) {
                    out.push_back(std::move(st));
                }
            }
        };
        load_array("favorites", next.favorites);
        load_array("recent", next.recent);
        load_array("manual", next.manual);
    }
    std::lock_guard lock(mtx_);
    store_ = std::move(next);
    if (raw.empty()) {
        log::info("[radio] No radio-stations.json; using empty store");
    } else {
        log::info("[radio] Loaded radio station store favorites="
                  + std::to_string(store_.favorites.size())
                  + " recent=" + std::to_string(store_.recent.size())
                  + " manual=" + std::to_string(store_.manual.size()));
    }
}

bool RadioSource::save_store_locked(std::string* error) const {
    std::string body = "{\"version\":1,\"lastStationId\":";
    append_json_string(body, store_.last_station_id);
    body += ",\"lastPlaying\":";
    body += store_.last_playing ? "true" : "false";
    body += ",\"lastManualUrl\":";
    append_json_string(body, store_.last_manual_url);
    body += ",\"lastManualName\":";
    append_json_string(body, store_.last_manual_name);
    body += ",\"favorites\":[";
    for (size_t i = 0; i < store_.favorites.size(); ++i) {
        if (i) body += ",";
        body += station_to_json(store_.favorites[i]);
    }
    body += "],\"recent\":[";
    for (size_t i = 0; i < store_.recent.size(); ++i) {
        if (i) body += ",";
        body += station_to_json(store_.recent[i]);
    }
    body += "],\"manual\":[";
    for (size_t i = 0; i < store_.manual.size(); ++i) {
        if (i) body += ",";
        body += station_to_json(store_.manual[i]);
    }
    body += "]}\n";
    return atomic_replace_file(store_path_, body, error);
}

bool RadioSource::play_station(Station station, std::string* error) {
    if (!running_.load(std::memory_order_acquire)) {
        if (error) *error = "radio source is not running";
        return false;
    }
    if (station.provider == "radiobrowser" && !station.stationuuid.empty()) {
        auto by_uuid = http_get_radio_browser("/json/stations/byuuid?uuids="
                                              + query_escape(station.stationuuid),
                                              5000);
        if (by_uuid.ok) {
            auto fresh = stations_from_radio_browser_json(by_uuid.body);
            if (!fresh.empty()) {
                fresh.front().added_at_ms = station.added_at_ms;
                station = fresh.front();
            }
        }
        auto click = http_get_radio_browser("/json/url/"
                                            + query_escape(station.stationuuid),
                                            5000);
        if (click.ok) {
            std::string clicked_url;
            if (extract_json_string_field(click.body, "url", clicked_url) &&
                is_http_url(clicked_url)) {
                station.resolved_url = clicked_url;
            }
        }
    }
    std::string resolved_error;
    std::string url = !station.resolved_url.empty()
        ? station.resolved_url
        : station.stream_url;
    url = resolve_stream_url(url, &resolved_error);
    if (url.empty()) {
        set_error(resolved_error.empty() ? "could not resolve station stream"
                                         : resolved_error);
        if (error) *error = last_error();
        return false;
    }
    station.resolved_url = url;
    station.last_played_at_ms = now_ms();
    if (station.id.empty()) {
        station.id = station.provider == "manual"
            ? make_manual_id(url)
            : station_id_from_uuid(station.stationuuid);
    }

    stop_metadata();
    stop_stream();
    auto artwork = download_artwork(station.favicon);
    bool using_default_artwork = artwork.empty();
    {
        std::lock_guard lock(mtx_);
        current_station_ = station;
        current_track_.uri = "radio:" + station.id;
        current_track_.title = station.name;
        current_track_.artist = station_artist(station);
        current_track_.album = station_album(station);
        current_track_.duration_ms = 0;
        current_track_.artwork_key = using_default_artwork
            ? std::string("online-radio")
            : station.favicon;
        current_track_.artwork_mime.clear();
        current_track_.artwork_bytes = std::move(artwork);
        current_track_.artwork_loading = false;
        error_.clear();
        store_.last_station_id = station.id;
        store_.last_playing = true;
        remember_recent_locked(station);
        save_store_locked(nullptr);
        start_stream_locked(station, url);
        start_metadata_locked(station, url);
    }
    log::info("[radio] now playing " + station.name + " url=" + url);
    return true;
}

bool RadioSource::start_stream_locked(const Station& station,
                                      const std::string& url) {
    stop_stream_.store(false, std::memory_order_release);
    playing_.store(false, std::memory_order_release);
    ++stream_generation_;
    stream_thread_ = std::thread(&RadioSource::stream_thread_fn, this,
                                 stream_generation_, station, url);
    return true;
}

void RadioSource::stop_stream() {
    stop_stream_.store(true, std::memory_order_release);
    if (stream_thread_.joinable()) stream_thread_.join();
    playing_.store(false, std::memory_order_release);
    if (clear_pcm_) clear_pcm_();
}

void RadioSource::start_metadata_locked(const Station& station,
                                        const std::string& url) {
    stop_metadata_.store(false, std::memory_order_release);
    metadata_thread_ = std::thread(&RadioSource::metadata_thread_fn, this,
                                   stream_generation_, station, url);
}

void RadioSource::stop_metadata() {
    stop_metadata_.store(true, std::memory_order_release);
    if (metadata_thread_.joinable()) metadata_thread_.join();
}

void RadioSource::stream_thread_fn(uint64_t generation,
                                   Station station,
                                   std::string url) {
    (void)generation;
    HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool should_uninit = SUCCEEDED(co_hr);
    if (FAILED(co_hr) && co_hr != RPC_E_CHANGED_MODE) {
        set_error("could not initialize COM for radio decoder");
        return;
    }
    if (!ensure_media_foundation_started()) {
        set_error("could not start Media Foundation");
        if (should_uninit) CoUninitialize();
        return;
    }

    IMFSourceReader* reader = nullptr;
    std::wstring wide_url = wide_from_utf8(url);
    HRESULT hr = MFCreateSourceReaderFromURL(wide_url.c_str(), nullptr, &reader);
    if (FAILED(hr) || !reader) {
        set_error("could not open radio stream");
        if (should_uninit) CoUninitialize();
        return;
    }

    IMFMediaType* type = nullptr;
    hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, static_cast<UINT32>(kFrameBytes));
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * static_cast<UINT32>(kFrameBytes));
    if (SUCCEEDED(hr)) {
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                         nullptr, type);
    }
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    release_com(type);
    if (FAILED(hr)) {
        release_com(reader);
        set_error("radio stream codec is not supported");
        if (should_uninit) CoUninitialize();
        return;
    }

    playing_.store(true, std::memory_order_release);
    set_error({});
    std::vector<int16_t> pending;
    size_t pending_pos = 0;
    while (running_.load(std::memory_order_acquire) &&
           !stop_stream_.load(std::memory_order_acquire)) {
        if (!pcm_enabled_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (free_bytes_ && free_bytes_() < kFrameBytes * 512) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (pending_pos < pending.size()) {
            size_t samples_left = pending.size() - pending_pos;
            size_t frames = samples_left / kChannels;
            if (frames > 0 && feed_pcm_) {
                float gain = volume_gain_.load(std::memory_order_acquire);
                if (feed_pcm_(pending.data() + pending_pos, frames, gain)) {
                    pending_pos += frames * kChannels;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            if (pending_pos >= pending.size()) {
                pending.clear();
                pending_pos = 0;
            }
            continue;
        }

        DWORD flags = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) {
            release_com(sample);
            set_error("radio stream read failed");
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release_com(sample);
            set_error("radio stream ended");
            break;
        }
        if (!sample) continue;

        IMFMediaBuffer* buffer = nullptr;
        hr = sample->ConvertToContiguousBuffer(&buffer);
        if (SUCCEEDED(hr) && buffer) {
            BYTE* data = nullptr;
            DWORD max_len = 0;
            DWORD len = 0;
            hr = buffer->Lock(&data, &max_len, &len);
            if (SUCCEEDED(hr) && data && len >= kFrameBytes) {
                len -= len % static_cast<DWORD>(kFrameBytes);
                size_t sample_count = len / sizeof(int16_t);
                pending.resize(sample_count);
                std::memcpy(pending.data(), data, len);
            }
            if (SUCCEEDED(hr)) buffer->Unlock();
        }
        release_com(buffer);
        release_com(sample);
    }

    playing_.store(false, std::memory_order_release);
    release_com(reader);
    if (should_uninit) CoUninitialize();
}

void RadioSource::metadata_thread_fn(uint64_t generation,
                                     Station station,
                                     std::string url) {
    HINTERNET session = WinHttpOpen(
        kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);

    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    DWORD status = 0;
    uint32_t metaint = 0;
    std::string current_url = std::move(url);
    for (uint32_t redirects = 0; redirects < 6; ++redirects) {
        UrlParts u;
        if (!parse_http_url(current_url, u)) {
            WinHttpCloseHandle(session);
            return;
        }
        connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
        if (!connect) {
            WinHttpCloseHandle(session);
            return;
        }

        DWORD flags = u.secure ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(
            connect, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return;
        }

        static constexpr wchar_t kIcyHeader[] = L"Icy-MetaData: 1\r\n";
        BOOL sent = WinHttpSendRequest(request, kIcyHeader,
                                       static_cast<DWORD>(-1),
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
        status = 0;
        DWORD status_size = sizeof(status);
        if (received) {
            WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                &status,
                                &status_size,
                                WINHTTP_NO_HEADER_INDEX);
        }

        if (received && (status == 301 || status == 302 || status == 303 ||
                         status == 307 || status == 308)) {
            std::wstring location;
            bool has_location =
                query_header_string(request, WINHTTP_QUERY_LOCATION, location);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            request = nullptr;
            connect = nullptr;
            if (!has_location) {
                WinHttpCloseHandle(session);
                return;
            }
            current_url = absolute_redirect_url(u, location);
            if (current_url.empty()) {
                WinHttpCloseHandle(session);
                return;
            }
            continue;
        }

        if (received && status >= 200 && status < 400 &&
            query_custom_u32_header(request, L"icy-metaint", metaint)) {
            break;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return;
    }
    if (!request || !connect || metaint == 0) {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return;
    }

    uint32_t audio_remaining = metaint;
    int meta_remaining = -1;
    std::string meta_buffer;
    std::string last_title;
    while (running_.load(std::memory_order_acquire) &&
           !stop_metadata_.load(std::memory_order_acquire)) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
            break;
        }
        available = std::min<DWORD>(available, 16 * 1024);
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);

        size_t i = 0;
        while (i < chunk.size()) {
            if (audio_remaining > 0) {
                size_t skip = std::min<size_t>(audio_remaining, chunk.size() - i);
                audio_remaining -= static_cast<uint32_t>(skip);
                i += skip;
                continue;
            }

            if (meta_remaining < 0) {
                meta_remaining = static_cast<unsigned char>(chunk[i++]) * 16;
                if (meta_remaining == 0) {
                    meta_remaining = -1;
                    audio_remaining = metaint;
                } else {
                    meta_buffer.clear();
                    meta_buffer.reserve(static_cast<size_t>(meta_remaining));
                }
                continue;
            }

            size_t take = std::min<size_t>(
                static_cast<size_t>(meta_remaining) - meta_buffer.size(),
                chunk.size() - i);
            meta_buffer.append(chunk.data() + i, take);
            i += take;

            if (meta_buffer.size() >= static_cast<size_t>(meta_remaining)) {
                std::string title = extract_icy_stream_title(meta_buffer);
                if (!title.empty() && title != last_title) {
                    last_title = title;
                    update_stream_title(generation, station, title);
                }
                meta_remaining = -1;
                audio_remaining = metaint;
                meta_buffer.clear();
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
}

void RadioSource::update_stream_title(uint64_t generation,
                                      const Station& station,
                                      const std::string& stream_title) {
    auto parsed = split_stream_title(stream_title);
    if (parsed.first.empty()) return;

    std::lock_guard lock(mtx_);
    if (generation != stream_generation_ || current_station_.id != station.id) {
        return;
    }
    current_track_.title = std::move(parsed.first);
    current_track_.artist = parsed.second.empty() ? station_artist(station)
                                                  : std::move(parsed.second);
    current_track_.album = station.name;
}

void RadioSource::set_error(const std::string& error) {
    std::lock_guard lock(mtx_);
    error_ = error;
    if (!error.empty()) log::warn("[radio] " + error);
}

void RadioSource::remember_recent_locked(const Station& station) {
    Station recent = station;
    recent.last_played_at_ms = now_ms();
    store_.recent.erase(
        std::remove_if(store_.recent.begin(), store_.recent.end(),
                       [&](const Station& item) { return item.id == recent.id; }),
        store_.recent.end());
    store_.recent.insert(store_.recent.begin(), recent);
    if (store_.recent.size() > kMaxRadioStoreItems) {
        store_.recent.resize(kMaxRadioStoreItems);
    }
}

std::vector<uint8_t> RadioSource::download_artwork(const std::string& url) const {
    if (!is_http_url(url)) return {};
    auto res = http_get_url(url, 4 * 1024 * 1024, 5000);
    if (!res.ok || res.body.empty()) return {};
    return std::vector<uint8_t>(res.body.begin(), res.body.end());
}

bool RadioSource::parse_station_json(std::string_view json, Station& out) {
    Station st;
    extract_json_string_field(json, "id", st.id);
    extract_json_string_field(json, "provider", st.provider);
    extract_json_string_field(json, "stationuuid", st.stationuuid);
    extract_json_string_field(json, "name", st.name);
    extract_json_string_field(json, "streamUrl", st.stream_url);
    extract_json_string_field(json, "resolvedUrl", st.resolved_url);
    extract_json_string_field(json, "homepage", st.homepage);
    extract_json_string_field(json, "favicon", st.favicon);
    extract_json_string_field(json, "tags", st.tags);
    extract_json_string_field(json, "countryCode", st.country_code);
    extract_json_string_field(json, "language", st.language);
    extract_json_string_field(json, "codec", st.codec);
    extract_json_u32_field(json, "bitrate", st.bitrate);
    extract_json_bool_field(json, "hls", st.hls);
    extract_json_i64_field(json, "addedAtMs", st.added_at_ms);
    extract_json_i64_field(json, "lastPlayedAtMs", st.last_played_at_ms);
    if (st.provider.empty()) {
        st.provider = st.stationuuid.empty() ? "manual" : "radiobrowser";
    }
    if (st.id.empty()) {
        if (st.provider == "radiobrowser") {
            st.id = station_id_from_uuid(st.stationuuid);
        } else if (!st.resolved_url.empty() || !st.stream_url.empty()) {
            st.id = make_manual_id(!st.resolved_url.empty()
                                       ? st.resolved_url
                                       : st.stream_url);
        }
    }
    if (st.name.empty() && !st.stream_url.empty()) st.name = st.stream_url;
    out = std::move(st);
    return !out.id.empty();
}

std::string RadioSource::station_to_json(const Station& station) {
    std::string out = "{\"id\":";
    append_json_string(out, station.id);
    out += ",\"provider\":";
    append_json_string(out, station.provider);
    out += ",\"stationuuid\":";
    append_json_string(out, station.stationuuid);
    out += ",\"name\":";
    append_json_string(out, station.name);
    out += ",\"streamUrl\":";
    append_json_string(out, station.stream_url);
    out += ",\"resolvedUrl\":";
    append_json_string(out, station.resolved_url);
    out += ",\"homepage\":";
    append_json_string(out, station.homepage);
    out += ",\"favicon\":";
    append_json_string(out, station.favicon);
    out += ",\"tags\":";
    append_json_string(out, station.tags);
    out += ",\"countryCode\":";
    append_json_string(out, station.country_code);
    out += ",\"language\":";
    append_json_string(out, station.language);
    out += ",\"codec\":";
    append_json_string(out, station.codec);
    out += ",\"bitrate\":" + std::to_string(station.bitrate);
    out += ",\"hls\":";
    out += station.hls ? "true" : "false";
    out += ",\"addedAtMs\":" + std::to_string(station.added_at_ms);
    out += ",\"lastPlayedAtMs\":" + std::to_string(station.last_played_at_ms);
    out += "}";
    return out;
}

std::string RadioSource::station_array_json(std::string_view key,
                                            const std::vector<Station>& stations) {
    std::string out = "{";
    append_json_string(out, key);
    out += ":[";
    for (size_t i = 0; i < stations.size(); ++i) {
        if (i) out += ",";
        out += station_to_json(stations[i]);
    }
    out += "]}";
    return out;
}

std::string RadioSource::make_manual_id(const std::string& url) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : url) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "manual:%016llx",
                  static_cast<unsigned long long>(hash));
    return buf;
}

} // namespace bridge
