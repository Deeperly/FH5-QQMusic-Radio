// SigscanResolver — see sigscan.h.

#include "sigscan.h"
#include "game_profile.h"
#include "log_file.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bridge {

namespace {

std::string hex(uintptr_t v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)v);
    return buf;
}

struct SectionRange { uintptr_t start = 0, end = 0; };

struct PdataEntry {
    uint32_t start_rva = 0;
    uint32_t end_rva   = 0;
    // Primary function-start RVA. For non-chunked functions this equals
    // start_rva. For functions MSVC split across multiple .pdata entries
    // (try/catch, cold/hot blocks, etc.), the secondary chunks' UWI has
    // the UNW_FLAG_CHAININFO bit set with a trailing RUNTIME_FUNCTION
    // pointing back at the primary entry; this field resolves the chain.
    uint32_t fn_start  = 0;
};

class Context {
public:
    uintptr_t base = 0;
    size_t    size = 0;
    SectionRange text;
    SectionRange rdata;
    std::vector<PdataEntry> pdata;

    bool init(uintptr_t b, size_t s);

    // Find every full-string match of `needle` in .rdata. A match must be
    // preceded by a null byte (or the section start) and followed by a
    // null byte — prevents partial matches inside longer strings like
    // "System::createSoundEx".
    std::vector<uintptr_t> find_strings(const char* needle) const;

    // Scan .text for the byte pattern. Mask bytes are 'x' for literal
    // match, '?' for wildcard.
    std::vector<uintptr_t> find_pattern(const uint8_t* pat,
                                        const uint8_t* mask,
                                        size_t len) const;

    // Walk .text for every `lea r, [rip+disp32]` whose absolute target
    // matches `target`.
    std::vector<uintptr_t> find_leas_to(uintptr_t target) const;

    // .pdata-backed containing-function lookup. Every x64 PE registers
    // every non-leaf function's bounds in RUNTIME_FUNCTION records, so this
    // is exact for the wrappers we care about. Returns 0 if not found.
    uintptr_t containing_fn(uintptr_t code_addr) const;
};

bool Context::init(uintptr_t b, size_t s) {
    base = b;
    size = s;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), ".text", 8) == 0) {
            text.start = base + sec[i].VirtualAddress;
            text.end   = text.start + sec[i].Misc.VirtualSize;
        } else if (strncmp(reinterpret_cast<const char*>(sec[i].Name), ".rdata", 8) == 0) {
            rdata.start = base + sec[i].VirtualAddress;
            rdata.end   = rdata.start + sec[i].Misc.VirtualSize;
        }
    }
    if (!text.start || !rdata.start) return false;

    const auto& exc =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exc.VirtualAddress || exc.Size < sizeof(RUNTIME_FUNCTION)) {
        return false;
    }
    const auto* rf =
        reinterpret_cast<const RUNTIME_FUNCTION*>(base + exc.VirtualAddress);
    size_t n = exc.Size / sizeof(RUNTIME_FUNCTION);

    // Walk UNWIND_INFO CHAININFO chains so each pdata entry maps to its
    // primary function start, not a cold chunk start. Chains are short
    // (typically depth 1), so we cap iterations defensively.
    auto resolve_primary = [&](uint32_t begin_rva, uint32_t uwi_rva) -> uint32_t {
        for (int hops = 0; hops < 16; ++hops) {
            if (uwi_rva == 0 || uwi_rva + 4 > size) return begin_rva;
            const auto* uwi = reinterpret_cast<const uint8_t*>(base + uwi_rva);
            uint8_t b0   = uwi[0];
            uint8_t flags = static_cast<uint8_t>((b0 >> 3) & 0x1Fu);
            if (!(flags & 0x04)) return begin_rva;  // UNW_FLAG_CHAININFO clear
            uint8_t cnt = uwi[2];
            size_t code_bytes = (static_cast<size_t>(cnt) * 2 + 3) & ~size_t(3);
            size_t chain_off  = 4 + code_bytes;
            if (uwi_rva + chain_off + sizeof(RUNTIME_FUNCTION) > size) return begin_rva;
            const auto* chained =
                reinterpret_cast<const RUNTIME_FUNCTION*>(uwi + chain_off);
            begin_rva = chained->BeginAddress;
            uwi_rva   = chained->UnwindInfoAddress;
        }
        return begin_rva;
    };

    pdata.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (rf[i].BeginAddress == 0 && rf[i].EndAddress == 0) continue;
        PdataEntry e;
        e.start_rva = rf[i].BeginAddress;
        e.end_rva   = rf[i].EndAddress;
        e.fn_start  = resolve_primary(rf[i].BeginAddress, rf[i].UnwindInfoAddress);
        pdata.push_back(e);
    }
    std::sort(pdata.begin(), pdata.end(),
              [](const PdataEntry& a, const PdataEntry& b) {
                  return a.start_rva < b.start_rva;
              });
    return true;
}

std::vector<uintptr_t> Context::find_strings(const char* needle) const {
    std::vector<uintptr_t> out;
    size_t nlen = strlen(needle);
    if (nlen == 0) return out;
    const auto* start = reinterpret_cast<const uint8_t*>(rdata.start);
    const auto* end   = reinterpret_cast<const uint8_t*>(rdata.end);
    if (end - start <= static_cast<ptrdiff_t>(nlen + 1)) return out;
    for (const auto* p = start; p + nlen + 1 < end; ++p) {
        if (memcmp(p, needle, nlen) != 0) continue;
        if (p[nlen] != 0) continue;  // require trailing null
        if (p > start && *(p - 1) != 0) continue;  // require leading boundary
        out.push_back(reinterpret_cast<uintptr_t>(p));
    }
    return out;
}

std::vector<uintptr_t> Context::find_pattern(const uint8_t* pat,
                                              const uint8_t* mask,
                                              size_t len) const {
    std::vector<uintptr_t> out;
    if (len == 0) return out;
    if (text.start + len > text.end) return out;
    const auto* start = reinterpret_cast<const uint8_t*>(text.start);
    const auto* end   = reinterpret_cast<const uint8_t*>(text.end) - len;
    // Mild Boyer-Moore-ish skip: index first non-wildcard byte for quick reject.
    size_t first_fixed = 0;
    while (first_fixed < len && mask[first_fixed] == '?') ++first_fixed;
    if (first_fixed == len) return out;  // all wildcards — reject

    for (const auto* p = start; p <= end; ++p) {
        if (p[first_fixed] != pat[first_fixed]) continue;
        bool ok = true;
        for (size_t i = 0; i < len; ++i) {
            if (mask[i] == '?') continue;
            if (p[i] != pat[i]) { ok = false; break; }
        }
        if (ok) out.push_back(reinterpret_cast<uintptr_t>(p));
    }
    return out;
}

std::vector<uintptr_t> Context::find_leas_to(uintptr_t target) const {
    std::vector<uintptr_t> out;
    if (text.start + 7 > text.end) return out;
    const auto* start = reinterpret_cast<const uint8_t*>(text.start);
    const auto* end   = reinterpret_cast<const uint8_t*>(text.end) - 7;
    for (const auto* p = start; p < end; ++p) {
        // REX prefix for `lea r64, [rip+disp32]`. W=1 is mandatory; R=any
        // (extends the destination reg field to r8-r15). X and B bits are
        // unused for RIP-rel addressing. FMOD log helpers receive the
        // __FUNCTION__ string in the 4th arg (r9), encoded with REX.WR
        // (0x4C), so we MUST accept both 0x48 and 0x4C — earlier scan
        // missed every wrapper because `lea r9, [rip+disp]` uses 0x4C.
        if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D) continue;
        // ModR/M for `lea r, [rip+disp32]`: mod=00, rm=101, reg=any.
        const uint8_t modrm = p[2];
        if ((modrm & 0xC7) != 0x05) continue;
        int32_t disp = 0;
        std::memcpy(&disp, p + 3, sizeof(disp));
        uintptr_t next = reinterpret_cast<uintptr_t>(p) + 7;
        if (next + static_cast<intptr_t>(disp) == target) {
            out.push_back(reinterpret_cast<uintptr_t>(p));
        }
    }
    return out;
}

uintptr_t Context::containing_fn(uintptr_t code_addr) const {
    if (code_addr < base) return 0;
    uint32_t rva = static_cast<uint32_t>(code_addr - base);
    // Largest entry with start_rva <= rva.
    auto it = std::upper_bound(pdata.begin(), pdata.end(), rva,
                               [](uint32_t v, const PdataEntry& e) {
                                   return v < e.start_rva;
                               });
    if (it == pdata.begin()) return 0;
    --it;
    if (rva >= it->end_rva) return 0;
    return base + it->fn_start;
}

// Parses a pattern string like "48 8B ?? ?? 90 C3" into (bytes, mask).
// Wildcards must be exactly `??`; whitespace between bytes is optional.
struct ParsedPattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
};

ParsedPattern parse_pattern(const char* pat) {
    ParsedPattern p;
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (*pat) {
        while (*pat == ' ' || *pat == '\t' || *pat == '\n') ++pat;
        if (!*pat) break;
        if (pat[0] == '?' && pat[1] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back('?');
            pat += 2;
        } else {
            int hi = hexval(pat[0]);
            int lo = hexval(pat[1]);
            if (hi < 0 || lo < 0) break;
            p.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            p.mask.push_back('x');
            pat += 2;
        }
    }
    return p;
}

std::vector<ParsedPattern> parse_pattern_alternatives(const char* pats) {
    std::vector<ParsedPattern> out;
    if (!pats || !*pats) return out;
    std::string s(pats);
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find('|', start);
        std::string part = s.substr(start, end == std::string::npos
                                           ? std::string::npos
                                           : end - start);
        out.push_back(parse_pattern(part.c_str()));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

bool prologue_matches(const Context& ctx, uintptr_t fn,
                      const ParsedPattern& p) {
    if (fn < ctx.text.start || fn + p.bytes.size() > ctx.text.end) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(fn);
    for (size_t i = 0; i < p.bytes.size(); ++i) {
        if (p.mask[i] == '?') continue;
        if (b[i] != p.bytes[i]) return false;
    }
    return true;
}

bool prologue_matches_any(const Context& ctx, uintptr_t fn,
                          const std::vector<ParsedPattern>& patterns) {
    for (const auto& p : patterns) {
        if (prologue_matches(ctx, fn, p)) return true;
    }
    return false;
}

// Resolve an FMOD public wrapper via its `__FUNCTION__` string.
// `prologue_pat` confirms the function entry against a fingerprint; it
// MUST be provided when the string has multiple .rdata copies (e.g.
// ChannelControl::setVolume) or multiple consumers (e.g. Bus::*).
bool resolve_string_anchor(const Context& ctx, const char* anchor_str,
                           const char* prologue_pat,
                           uintptr_t& out, const char* name, bool quiet) {
    if (out) return true;  // already resolved on a prior pass
    auto strs = ctx.find_strings(anchor_str);
    if (strs.empty()) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name
                  + " — anchor string \"" + anchor_str
                  + "\" not present in .rdata");
        return false;
    }
    std::vector<uintptr_t> leas;
    for (uintptr_t s : strs) {
        auto v = ctx.find_leas_to(s);
        leas.insert(leas.end(), v.begin(), v.end());
    }
    if (leas.empty()) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name + " — no LEA targets \""
                  + anchor_str + "\" (" + std::to_string(strs.size())
                  + " string copies)");
        return false;
    }
    std::vector<uintptr_t> fns;
    for (uintptr_t lea : leas) {
        uintptr_t fn = ctx.containing_fn(lea);
        if (!fn) continue;
        if (std::find(fns.begin(), fns.end(), fn) == fns.end()) {
            fns.push_back(fn);
        }
    }
    if (fns.empty()) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name
                  + " — no .pdata-registered function contains any matched LEA");
        return false;
    }
    if (prologue_pat && *prologue_pat) {
        auto patterns = parse_pattern_alternatives(prologue_pat);
        std::vector<uintptr_t> filtered;
        for (uintptr_t fn : fns) {
            if (prologue_matches_any(ctx, fn, patterns)) filtered.push_back(fn);
        }
        fns.swap(filtered);
    }
    if (fns.size() != 1) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name + " — anchor \""
                  + anchor_str + "\" resolved to " + std::to_string(fns.size())
                  + " candidate fn(s) after prologue filter");
        return false;
    }
    out = fns[0] - ctx.base;
    log::info(std::string("[sigscan] ") + name + "  RVA=" + hex(out)
              + " (anchor \"" + anchor_str + "\")");
    return true;
}

bool resolve_direct(const Context& ctx, const char* pat_str,
                    uintptr_t& out, const char* name, bool quiet) {
    if (out) return true;
    auto p = parse_pattern(pat_str);
    auto hits = ctx.find_pattern(p.bytes.data(), p.mask.data(), p.bytes.size());
    if (hits.size() != 1) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name + " — pattern matched "
                  + std::to_string(hits.size()) + " hit(s) in .text");
        return false;
    }
    out = hits[0] - ctx.base;
    log::info(std::string("[sigscan] ") + name + "  RVA=" + hex(out)
              + " (direct pattern)");
    return true;
}

bool resolve_rip32(const Context& ctx, const char* pat_str, int operand_offset,
                   uintptr_t& out, const char* name, bool quiet) {
    if (out) return true;
    auto p = parse_pattern(pat_str);
    if (operand_offset < 0 ||
        static_cast<size_t>(operand_offset) + 4 > p.bytes.size()) {
        log::error(std::string("[sigscan] FAIL ") + name
                   + " — operand_offset out of pattern bounds");
        return false;
    }
    auto hits = ctx.find_pattern(p.bytes.data(), p.mask.data(), p.bytes.size());
    if (hits.size() != 1) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name + " — pattern matched "
                  + std::to_string(hits.size()) + " hit(s) in .text");
        return false;
    }
    uintptr_t match = hits[0];
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(match + operand_offset),
                sizeof(disp));
    uintptr_t target = match + operand_offset + 4 + static_cast<intptr_t>(disp);
    if (target < ctx.base || target >= ctx.base + ctx.size) {
        if (!quiet) log::warn(std::string("[sigscan] FAIL ") + name
                  + " — decoded target " + hex(target) + " outside image");
        return false;
    }
    out = target - ctx.base;
    log::info(std::string("[sigscan] ") + name + "  RVA=" + hex(out)
              + " (Rip32 anchor at " + hex(match - ctx.base) + ")");
    return true;
}

bool heapish_user_pointer(uintptr_t v) {
    return v >= 0x10000000000ull && v < 0x800000000000ull;
}

bool readable_qword(uintptr_t addr, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool resolve_logo_handle_arrays_by_shape(const Context& ctx,
                                         uintptr_t& base_array_out,
                                         uintptr_t& stride_array_out,
                                         bool quiet) {
    if (base_array_out && stride_array_out) return true;

    constexpr uintptr_t kStrideArrayDelta = 0x70;
    constexpr uintptr_t kExpectedStride = 0x20;
    std::vector<uintptr_t> matches;

    auto scan_range = [&](SectionRange range) {
        if (!range.start || range.end <= range.start + kStrideArrayDelta + 0x18) {
            return;
        }
        for (uintptr_t p = (range.start + 7) & ~uintptr_t(7);
             p + kStrideArrayDelta + 0x18 <= range.end; p += 8) {
            uintptr_t base1 = 0;
            uintptr_t base2 = 0;
            uintptr_t stride1 = 0;
            uintptr_t stride2 = 0;
            if (!readable_qword(p + 0x08, base1) ||
                !readable_qword(p + 0x10, base2) ||
                !readable_qword(p + kStrideArrayDelta + 0x08, stride1) ||
                !readable_qword(p + kStrideArrayDelta + 0x10, stride2)) {
                continue;
            }
            if (heapish_user_pointer(base1) &&
                heapish_user_pointer(base2) &&
                stride1 == kExpectedStride &&
                stride2 == kExpectedStride) {
                matches.push_back(p);
            }
        }
    };

    scan_range(ctx.rdata);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ctx.base);
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(ctx.base + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (strncmp(reinterpret_cast<const char*>(sec[i].Name), ".data", 8) == 0) {
            scan_range(SectionRange{
                ctx.base + sec[i].VirtualAddress,
                ctx.base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize});
        }
    }

    std::sort(matches.begin(), matches.end());
    std::vector<uintptr_t> clusters;
    for (uintptr_t match : matches) {
        if (clusters.empty() || match - clusters.back() > 0x20) {
            clusters.push_back(match);
        }
    }

    if (clusters.size() == 1) {
        base_array_out = clusters[0] - ctx.base;
        stride_array_out = base_array_out + kStrideArrayDelta;
        log::info(std::string("[sigscan] logo_handle_arrays  RVA=")
                  + hex(base_array_out) + "/" + hex(stride_array_out)
                  + " (shape)");
        return true;
    }

    if (!quiet) {
        log::info(std::string("[sigscan] logo_handle_arrays unavailable — shape matches=")
                  + std::to_string(matches.size())
                  + " clusters=" + std::to_string(clusters.size()));
    }
    return false;
}

} // namespace

bool resolve_signatures(uintptr_t module_base, size_t module_size,
                        const GameProfile& gp,
                        ResolvedRvas& out, bool quiet) {
    Context ctx;
    if (!ctx.init(module_base, module_size)) {
        if (!quiet) log::error("[sigscan] init failed — could not parse PE headers / .pdata");
        return false;
    }
    if (!quiet) {
        log::info("[sigscan] init ok — .text=" + hex(ctx.text.start) + ".." +
                  hex(ctx.text.end) + " .rdata=" + hex(ctx.rdata.start) + ".." +
                  hex(ctx.rdata.end) + " pdata_entries=" +
                  std::to_string(ctx.pdata.size()));
    }

    const SigProfile& s = gp.sig;
    if (!quiet) {
        log::info(std::string("[sigscan] profile=") + gp.name);
    }

    // FMOD Core DSP wrappers (string-anchored; prologue is a tie-breaker).
    resolve_string_anchor(ctx, s.createDSP_anchor, s.createDSP_prologue,
                          out.systemCreateDSP, "systemCreateDSP", quiet);
    resolve_string_anchor(ctx, s.dspRelease_anchor, s.dspRelease_prologue,
                          out.dspRelease, "dspRelease", quiet);
    resolve_string_anchor(ctx, s.addDSP_anchor, s.addDSP_prologue,
                          out.channelControlAddDSP, "channelControlAddDSP", quiet);
    resolve_string_anchor(ctx, s.removeDSP_anchor, s.removeDSP_prologue,
                          out.channelControlRemoveDSP, "channelControlRemoveDSP",
                          quiet);

    // FMOD internal handle resolver + paired unlock (direct byte patterns; no
    // __FUNCTION__ anchor). The resolver covers the handle-decoder math
    // (shift-by-17 / mask 0xFFF / movzx). The unlock is a tiny lock-release
    // stub keyed on the FMOD internal-state lock offset (game-specific).
    if (s.fmod_handle_resolver) {
        resolve_direct(ctx, s.fmod_handle_resolver, out.fmod_handle_resolver,
                       "fmod_handle_resolver", quiet);
    }
    if (s.fmod_handle_unlock) {
        resolve_direct(ctx, s.fmod_handle_unlock, out.fmod_handle_unlock,
                       "fmod_handle_unlock", quiet);
    }

    // RadioState singleton (rip32 anchor). The station gate walks
    // singleton -> +player -> +station -> +name to detect Streamer Mode.
    if (s.radio_state_singleton) {
        resolve_rip32(ctx, s.radio_state_singleton,
                      s.radio_state_singleton_operand, out.radio_state_singleton,
                      "radio_state_singleton", quiet);
    }

    // RadioState station setter by name — optional startup self-nudge, kept out
    // of all_ok(). Null when the game has no portable anchor (FH5 hash-id path).
    if (s.radio_set_station_by_name) {
        resolve_direct(ctx, s.radio_set_station_by_name,
                       out.radio_set_station_by_name, "radio_set_station_by_name",
                       quiet);
    }

    // Runtime radio-logo resource path. Optional for core audio and kept out of
    // all_ok(); a null pattern leaves the RVA 0 so the logo override fails open.
    if (s.logo_create_2d_raw_texture) {
        resolve_direct(ctx, s.logo_create_2d_raw_texture,
                       out.logo_create_2d_raw_texture,
                       "logo_create_2d_raw_texture", quiet);
    }
    if (s.logo_texture_system) {
        resolve_rip32(ctx, s.logo_texture_system, s.logo_texture_system_operand,
                      out.logo_texture_system, "logo_texture_system", quiet);
    }
    if (s.logo_request_manager) {
        resolve_rip32(ctx, s.logo_request_manager, s.logo_request_manager_operand,
                      out.logo_request_manager, "logo_request_manager", quiet);
    }
    if (s.logo_resource_registry) {
        resolve_rip32(ctx, s.logo_resource_registry,
                      s.logo_resource_registry_operand, out.logo_resource_registry,
                      "logo_resource_registry", quiet);
    }
    // Stock-resource in-place BC7 upload producer. The validation globals
    // (resource table / refcount / wrapper vtable) are not sig-resolved here —
    // they use build constants with fail-closed validation. The upload FUNCTION
    // is uniquely sig-resolved, so it is never called on a non-matching build.
    if (s.logo_inplace_upload) {
        resolve_direct(ctx, s.logo_inplace_upload, out.logo_inplace_upload,
                       "logo_inplace_upload", quiet);
    }

    // FMOD ChannelControl mute/volume levers + DSP-chain inspectors (runtime-mute
    // path). Each anchor is a single unique .rdata copy, so no prologue
    // tie-breaker is needed. Optional — kept out of all_ok().
    if (s.setVolume_anchor) {
        resolve_string_anchor(ctx, s.setVolume_anchor, "",
                              out.channelControlSetVolume,
                              "channelControlSetVolume", quiet);
    }
    if (s.setMute_anchor) {
        resolve_string_anchor(ctx, s.setMute_anchor, "",
                              out.channelControlSetMute,
                              "channelControlSetMute", quiet);
    }
    if (s.getNumDSPs_anchor) {
        resolve_string_anchor(ctx, s.getNumDSPs_anchor, "",
                              out.channelControlGetNumDSPs,
                              "channelControlGetNumDSPs", quiet);
    }
    if (s.getDSP_anchor) {
        resolve_string_anchor(ctx, s.getDSP_anchor, "",
                              out.channelControlGetDSP,
                              "channelControlGetDSP", quiet);
    }

    // The custom-art alias path needs the family handle-table base/stride
    // arrays (family-1 raw texture handle -> family-2 UI descriptor handle).
    // Resolve by structure — game-agnostic shape scan. Only meaningful when the
    // logo core resolved, so skip it when this game has no logo patterns.
    if (s.logo_create_2d_raw_texture) {
        resolve_logo_handle_arrays_by_shape(ctx, out.logo_handle_base_array,
                                            out.logo_handle_stride_array, quiet);
    }

    // Known-build layout fixups (.text VirtualSize keyed). The generic logo
    // pattern can select a neighboring helper on a specific shipped build; pin
    // the proven value. A 0 field in the override means "leave whatever
    // resolved". Listed per-game in the profile.
    if (ctx.text.end > ctx.text.start) {
        const size_t text_size = ctx.text.end - ctx.text.start;
        for (size_t i = 0; i < s.build_override_count; ++i) {
            const auto& bo = s.build_overrides[i];
            if (bo.text_size != text_size) continue;
            if (bo.logo_create_2d_raw_texture &&
                out.logo_create_2d_raw_texture != bo.logo_create_2d_raw_texture) {
                out.logo_create_2d_raw_texture = bo.logo_create_2d_raw_texture;
                log::info("[sigscan] logo_create_2d_raw_texture  RVA="
                          + hex(out.logo_create_2d_raw_texture)
                          + " (known build layout)");
            }
            if (bo.logo_handle_base_array && !out.logo_handle_base_array &&
                !out.logo_handle_stride_array) {
                out.logo_handle_base_array = bo.logo_handle_base_array;
                out.logo_handle_stride_array = bo.logo_handle_stride_array;
                log::info("[sigscan] logo_handle_arrays  RVA="
                          + hex(out.logo_handle_base_array) + "/"
                          + hex(out.logo_handle_stride_array)
                          + " (known build layout)");
            }
        }
    }

    return out.all_ok();
}

} // namespace bridge
