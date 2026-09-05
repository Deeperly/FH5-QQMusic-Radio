// SSE store for /api/events. Falls back to /api/state polling if the
// EventSource disconnects. The backend BridgeStateStore pushes a fresh
// JSON snapshot whenever its revision counter advances.
//
// `bridgeOnline` reflects reachability of the bridge HTTP server. Goes
// false once we've had two consecutive failures (~2 s) — version.dll dies
// with the game process, so "offline" effectively means "game closed".

import { writable } from 'svelte/store';

const EMPTY = {
  game:    { id: 'unknown', attached: false, injector_ready: false },
  audio:   { active: false, r10_active: false, migrated: false },
  activeSource: 'spotify',
  spotify: { connected: false, blob_cached: false, playing: false,
             transferred_away: false },
  airplay: { available: true, running: false, connected: false, playing: false,
             deviceName: 'FH6 Radio', volumeDb: -15, volumePercent: 50,
             error: '' },
  sources: {
    spotify: { available: true, connected: false, playing: false },
    airplay: { available: true, connected: false, playing: false },
    local: { available: true, connected: false, playing: false },
    radio: { available: true, connected: false, playing: false },
    qqmusic: { available: true, connected: false, playing: false },
  },
  local: {
    available: true,
    ready: false,
    playing: false,
    musicDir: '',
    defaultMusicDir: '',
    recursive: true,
    shuffle: true,
    trackCount: 0,
    unsupportedCount: 0,
    position_ms: 0,
    supportedFormats: ['mp3', 'wav', 'flac'],
    error: '',
  },
  radio: {
    available: true,
    connected: false,
    playing: false,
    stationName: '',
    stationId: '',
    codec: '',
    bitrate: 0,
    error: '',
  },
  driving: {
    speedAvailable: false,
    speedMps: 0,
    nightRunnersVolumeFactor: 1,
    nightRunnersLazyCooldown: 0,
    nightRunnersSpeedProgress: 0,
    nightRunnersPeakMps: 0,
    nightRunnersBufferMps: 0,
    nightRunnersDriveState: 0,
    nonFreeroamVolumeActive: false,
  },
  options: {
    activeSource: 'spotify',
    locale: 'en',
    menuPlayback: 'pause',
    raceStartPlayback: 'next',
    raceStartRestartThresholdSeconds: 20,
    songStartOffsetEnabled: false,
    songStartOffsetSeconds: 30,
    volumeNormalization: 'on',
    quickStationSkip: false,
    equalizerEnabled: false,
    equalizerBands: [0, 0, 0, 0, 0],
    localMusicDir: '',
    qqMusicProcessName: 'QQMusic.exe',
    qqMusicExecutable: 'F:/QQMusic/QQMusic.exe',
    localRecursive: true,
    localShuffle: true,
    localVolume: 100,
    localTitleMetadataMode: 'metadata',
    localArtistMetadataMode: 'albumArtist',
    nightRunnersMode: false,
    nightRunnersStoppedVolumeDecrease: 50,
    nightRunnersMaxSpeedMph: 120,
    nightRunnersSpeedUnit: 'mph',
    nightRunnersCurveEnabled: false,
    nightRunnersCurveExponent: 2,
    nightRunnersLazyVolumeEnabled: false,
    nightRunnersLazyHoldSeconds: 2,
    nightRunnersLowCutEnabled: false,
    nightRunnersFrequencyCutMode: 'low',
    nightRunnersLowCutAmount: 50,
    nightRunnersLowCutFrequencyHz: 140,
    nightRunnersNonDrivingVolumeEnabled: true,
    nightRunnersNonDrivingVolume: 50,
    nightRunnersDynamicMode: false,
    nightRunnersDynamicThresholdMph: 40,
    nightRunnersDynamicBufferPercent: 15,
    nightRunnersDynamicBufferFillSeconds: 3,
    nightRunnersDynamicIncreaseSeconds: 1.5,
    nightRunnersDynamicDecreaseSeconds: 2.5,
    nightRunnersDynamicMaxDecayMphS: 0,
    radioLogoAlbumArtEnabled: true,
    radioLogoCustomGraphicEnabled: false,
    radioLogoSpotifyVariant: 'white',
    metadataTruncationEnabled: true,
    metadataTruncationLength: 30,
  },
  ui:      { credit_verified: true },
  track:   { title: '', artist: '' },
  errors:  [],
};

export const bridgeState  = writable(EMPTY);
export const bridgeOnline = writable(true);

let es = null;
let pollTimer = null;
let pollDelayMs = 1000;
let pollFailures = 0;
const FAIL_THRESHOLD = 2;

function markOnline() {
  pollFailures = 0;
  bridgeOnline.set(true);
}

function markFailure() {
  pollFailures += 1;
  if (pollFailures >= FAIL_THRESHOLD) {
    bridgeOnline.set(false);
    // Reset visible state so we don't keep showing stale "Radio Channel Ready"
    // after the bridge died.
    bridgeState.set(EMPTY);
  }
}

async function pollOnce() {
  try {
    const res = await fetch('/api/state', { cache: 'no-store' });
    if (!res.ok) throw new Error('http ' + res.status);
    const data = await res.json();
    bridgeState.set(data);
    markOnline();
  } catch {
    markFailure();
  }
}

function startPolling() {
  if (pollTimer) return;
  pollOnce();
  pollTimer = setInterval(pollOnce, pollDelayMs);
}

function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

function openSse() {
  try {
    es = new EventSource('/api/events');
  } catch {
    startPolling();
    return;
  }

  es.onopen = () => {
    stopPolling();
    markOnline();
  };

  es.onmessage = (ev) => {
    try {
      const data = JSON.parse(ev.data);
      bridgeState.set(data);
      markOnline();
    } catch { /* heartbeat or malformed — ignore */ }
  };

  // EventSource auto-reconnects; meanwhile fall back to /api/state poll so
  // we can detect actual unreachability (vs a transient disconnect).
  es.onerror = () => { startPolling(); };
}

export function init() {
  openSse();
}

export function destroy() {
  if (es) { es.close(); es = null; }
  stopPolling();
}
