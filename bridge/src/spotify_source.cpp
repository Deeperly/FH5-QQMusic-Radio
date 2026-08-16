#include "spotify_source.h"

#include "bridge_state.h"
#include <utility>

namespace bridge {

SpotifySource::SpotifySource(LibrespotPlayer& player) : player_(player) {}

const char* SpotifySource::id() const {
    return "spotify";
}

const char* SpotifySource::display_name() const {
    return "Spotify";
}

bool SpotifySource::is_connected() const {
    return player_.is_connected();
}

bool SpotifySource::is_playing() const {
    return player_.is_playing();
}

std::optional<uint32_t> SpotifySource::current_position_ms() const {
    if (!player_.is_connected()) return std::nullopt;
    return player_.current_position_ms();
}

SourceTrack SpotifySource::last_track() const {
    auto t = player_.last_track();
    SourceTrack out;
    out.uri = std::move(t.uri);
    out.title = std::move(t.title);
    out.artist = std::move(t.artist);
    out.album = std::move(t.album);
    out.duration_ms = t.duration_ms;
    out.artwork_key = std::move(t.artwork_key);
    out.artwork_mime = std::move(t.artwork_mime);
    out.artwork_bytes = std::move(t.artwork_bytes);
    out.artwork_loading = t.artwork_loading;
    return out;
}

void SpotifySource::pause_at_audio_boundary() {
    player_.pause_at_audio_boundary();
}

void SpotifySource::resume_rewound(uint32_t rewind_ms) {
    player_.resume_rewound(rewind_ms);
}

bool SpotifySource::restart_current_track() {
    return player_.seek(0);
}

bool SpotifySource::seek(uint32_t position_ms) {
    return player_.seek(position_ms);
}

bool SpotifySource::next_track() {
    return player_.next_track();
}

void SpotifySource::on_activated() {
    player_.start(advertise_for_selection_on_next_activation_);
    advertise_for_selection_on_next_activation_ = false;
}

void SpotifySource::on_deactivated() {
    advertise_for_selection_on_next_activation_ = true;
    player_.stop();
}

void SpotifySource::set_pcm_enabled(bool enabled) {
    player_.set_pcm_enabled(enabled);
}

void SpotifySource::set_volume_normalization(bool enabled) {
    player_.set_volume_normalization(enabled);
}

void SpotifySource::set_equalizer(bool enabled,
                                  const std::array<float, 5>& bands_db) {
    player_.set_equalizer(enabled, bands_db);
}

void SpotifySource::publish_state(BridgeStateStore& store) const {
    store.set_spotify_connection(player_.is_connected(),
                                 player_.blob_cached(),
                                 player_.spotify_device_id());
    store.set_spotify_playing(player_.is_playing());
}

} // namespace bridge
