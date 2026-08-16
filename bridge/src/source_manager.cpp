#include "source_manager.h"

#include "bridge_state.h"
#include "options.h"
#include "log_file.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace bridge {

void SourceManager::set_state_store(BridgeStateStore* store) {
    std::lock_guard lock(mtx_);
    state_store_ = store;
    publish_active_source_locked();
}

void SourceManager::register_source(AudioSource& source) {
    std::lock_guard lock(mtx_);
    auto existing = std::find(sources_.begin(), sources_.end(), &source);
    if (existing == sources_.end()) sources_.push_back(&source);
    if (!fallback_source_ || std::string_view(source.id()) == "spotify") {
        fallback_source_ = &source;
    }
    if (!active_source_) active_source_ = fallback_source_;
    source.set_pcm_enabled(active_source_ == &source);
    publish_active_source_locked();
}

void SourceManager::set_spotify_source(AudioSource& source) {
    register_source(source);
}

void SourceManager::set_local_source(AudioSource& source) {
    register_source(source);
}

void SourceManager::set_clear_pcm_callback(std::function<void()> clear_pcm) {
    std::lock_guard lock(mtx_);
    clear_pcm_ = std::move(clear_pcm);
}

const AudioSource* SourceManager::active_source() const {
    std::lock_guard lock(mtx_);
    return control_source_locked();
}

const char* SourceManager::active_source_id() const {
    std::lock_guard lock(mtx_);
    auto* source = control_source_locked();
    return source ? source->id() : "none";
}

AudioSource* SourceManager::find_source(std::string_view source_id) const {
    std::lock_guard lock(mtx_);
    return source_by_id_locked(source_id);
}

bool SourceManager::switch_to(std::string_view source_id) {
    std::lock_guard switch_lock(switch_mtx_);
    AudioSource* previous = nullptr;
    AudioSource* next = nullptr;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        next = source_by_id_locked(source_id);
        if (!next) {
#if defined(SPOTIFY_RADIO_DIAG)
            log::warn("[skip-diag] source switch rejected target="
                      + std::string(source_id));
#endif
            return false;
        }
        previous = control_source_locked();
        if (previous == next) {
            next->set_pcm_enabled(true);
            publish_active_source_locked();
#if defined(SPOTIFY_RADIO_DIAG)
            log::info("[skip-diag] source switch noop active="
                      + std::string(next->id()));
#endif
            return true;
        }
        active_source_ = next;
        clear_pcm = clear_pcm_;
        publish_active_source_locked();
    }

#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source switch previous="
              + std::string(previous ? previous->id() : "none")
              + " next=" + std::string(next->id()));
#endif
    if (previous) {
        previous->set_pcm_enabled(false);
        previous->on_deactivated();
        previous->pause_at_audio_boundary();
    }
    if (clear_pcm) clear_pcm();
    next->on_activated();
    next->set_pcm_enabled(true);
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source switch complete active="
              + std::string(next->id()));
#endif
    return true;
}

bool SourceManager::activate_current_source() {
    std::lock_guard switch_lock(switch_mtx_);
    AudioSource* source = nullptr;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
        if (!source) return false;
        source->set_pcm_enabled(true);
        publish_active_source_locked();
    }
    source->on_activated();
    return true;
}

bool SourceManager::is_playing() const {
    std::lock_guard lock(mtx_);
    auto* source = control_source_locked();
    return source && source->is_playing();
}

SourceTrack SourceManager::last_track() const {
    std::lock_guard lock(mtx_);
    auto* source = control_source_locked();
    return source ? source->last_track() : SourceTrack{};
}

void SourceManager::pause_at_audio_boundary() {
    AudioSource* source = nullptr;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
    }
    if (source) source->pause_at_audio_boundary();
}

void SourceManager::resume_rewound(uint32_t rewind_ms) {
    AudioSource* source = nullptr;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
    }
    if (source) source->resume_rewound(rewind_ms);
}

bool SourceManager::restart_current_track() {
    AudioSource* source = nullptr;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
        clear_pcm = clear_pcm_;
    }
    bool ok = source ? source->restart_current_track() : false;
    if (ok && clear_pcm) clear_pcm();
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source restart active="
              + std::string(source ? source->id() : "none")
              + " ok=" + std::to_string(ok));
#endif
    return ok;
}

bool SourceManager::next_track() {
    AudioSource* source = nullptr;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
        clear_pcm = clear_pcm_;
    }
    bool ok = source ? source->next_track() : false;
    if (ok && clear_pcm) clear_pcm();
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source next active="
              + std::string(source ? source->id() : "none")
              + " ok=" + std::to_string(ok));
#endif
    return ok;
}

bool SourceManager::previous_track() {
    AudioSource* source = nullptr;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
        clear_pcm = clear_pcm_;
    }
    bool ok = source ? source->previous_track() : false;
    if (ok && clear_pcm) clear_pcm();
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source previous active="
              + std::string(source ? source->id() : "none")
              + " ok=" + std::to_string(ok));
#endif
    return ok;
}

std::optional<uint32_t> SourceManager::current_position_ms() const {
    AudioSource* source = nullptr;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
    }
    return source ? source->current_position_ms() : std::nullopt;
}

bool SourceManager::seek(uint32_t position_ms) {
    AudioSource* source = nullptr;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        source = control_source_locked();
        clear_pcm = clear_pcm_;
    }
    bool ok = source ? source->seek(position_ms) : false;
    if (ok && clear_pcm) clear_pcm();
#if defined(SPOTIFY_RADIO_DIAG)
    log::info("[skip-diag] source seek active="
              + std::string(source ? source->id() : "none")
              + " target_ms=" + std::to_string(position_ms)
              + " ok=" + std::to_string(ok));
#endif
    return ok;
}

void SourceManager::request_race_start_offset(uint32_t offset_ms,
                                              const std::string& baseline_uri) {
    std::lock_guard lock(mtx_);
    pending_race_offset_ms_ = offset_ms;
    pending_race_offset_base_uri_ = baseline_uri;
    pending_race_offset_polls_ = 200; // ~10 s at the 50 ms pump cadence
}

void SourceManager::service_race_start_offset() {
    AudioSource* source = nullptr;
    uint32_t offset_ms = 0;
    std::string base_uri;
    std::function<void()> clear_pcm;
    {
        std::lock_guard lock(mtx_);
        if (pending_race_offset_ms_ == 0) return;
        if (--pending_race_offset_polls_ <= 0) {
            pending_race_offset_ms_ = 0;
            return;
        }
        source = control_source_locked();
        offset_ms = pending_race_offset_ms_;
        base_uri = pending_race_offset_base_uri_;
        clear_pcm = clear_pcm_;
    }
    auto disarm = [this]() {
        std::lock_guard lock(mtx_);
        pending_race_offset_ms_ = 0;
    };
    if (!source || std::string_view(source->id()) == "airplay" ||
        std::string_view(source->id()) == "radio") {
        disarm();
        return;
    }
    SourceTrack t = source->last_track();
    // Wait until the skipped-to track has actually loaded and started playing.
    if (t.uri.empty() || t.uri == base_uri || !source->is_playing()) return;
    if (t.duration_ms != 0 && offset_ms >= t.duration_ms) {
        disarm(); // shorter than the offset -> leave it at the start
        return;
    }
    if (source->seek(offset_ms)) {
        if (clear_pcm) clear_pcm();
        disarm();
    }
}

void SourceManager::set_volume_normalization(bool enabled) {
    std::vector<AudioSource*> sources;
    {
        std::lock_guard lock(mtx_);
        sources = sources_;
    }
    for (auto* source : sources) source->set_volume_normalization(enabled);
}

void SourceManager::set_equalizer(bool enabled,
                                  const std::array<float, 5>& bands_db) {
    std::vector<AudioSource*> sources;
    {
        std::lock_guard lock(mtx_);
        sources = sources_;
    }
    for (auto* source : sources) source->set_equalizer(enabled, bands_db);
}

void SourceManager::set_metadata_truncation(bool enabled,
                                            uint32_t max_chars) {
    std::lock_guard lock(mtx_);
    metadata_truncation_enabled_ = enabled;
    metadata_truncation_length_ =
        sanitize_metadata_truncation_length(max_chars);
}

void SourceManager::publish_source_states() {
    BridgeStateStore* store = nullptr;
    AudioSource* active = nullptr;
    std::vector<AudioSource*> sources;
    bool truncate_metadata = false;
    uint32_t truncate_length = 20;
    {
        std::lock_guard lock(mtx_);
        store = state_store_;
        active = control_source_locked();
        sources = sources_;
        truncate_metadata = metadata_truncation_enabled_;
        truncate_length = metadata_truncation_length_;
        publish_active_source_locked();
    }
    if (!store) return;
    std::vector<BridgeState::SourceState> published_sources;
    published_sources.reserve(sources.size());
    for (auto* source : sources) source->publish_state(*store);
    for (auto* source : sources) {
        if (!source) continue;
        published_sources.push_back({
            source->id(),
            source->display_name(),
            true,
            source->is_connected(),
            source->is_playing(),
            source->capabilities()
        });
    }
    store->set_sources(published_sources);
    if (active) {
        auto track = active->last_track();
        store->set_track(
            track.uri,
            truncate_metadata_text(track.title, truncate_metadata,
                                   truncate_length),
            truncate_metadata_text(track.artist, truncate_metadata,
                                   truncate_length),
            track.album,
            track.duration_ms);
    }
}

AudioSource* SourceManager::control_source_locked() const {
    return active_source_ ? active_source_ : fallback_source_;
}

AudioSource* SourceManager::source_by_id_locked(std::string_view source_id) const {
    for (auto* source : sources_) {
        if (source && source_id == source->id()) return source;
    }
    return nullptr;
}

void SourceManager::publish_active_source_locked() {
    if (state_store_) {
        auto* source = control_source_locked();
        state_store_->set_active_source(source ? source->id() : "spotify");
    }
}

} // namespace bridge
