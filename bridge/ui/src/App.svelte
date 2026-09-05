<script>
  import { onMount, onDestroy } from 'svelte';
  import { fly } from 'svelte/transition';
  import { bridgeState, bridgeOnline, init, destroy } from './stores/state.js';

  let s        = $derived($bridgeState);
  let online   = $derived($bridgeOnline);
  let game     = $derived(s.game);
  let gameTitle = $derived(
    online && game?.id === 'fh5' ? 'Forza Horizon 5'
      : online && game?.id === 'fh6' ? 'Forza Horizon 6'
        : 'Forza Horizon'
  );
  let audio    = $derived(s.audio);
  let spotify  = $derived(s.spotify);
  let airplay  = $derived(s.airplay ?? {
    available: true,
    running: false,
    connected: false,
    playing: false,
    deviceName: 'FH6 Radio',
    volumeDb: -15,
    volumePercent: 50,
    error: '',
  });
  let options  = $derived(s.options ?? {
    activeSource: 'spotify',
    locale: 'en',
    menuPlayback: 'pause',
    raceStartPlayback: 'next',
    raceStartRestartThresholdSeconds: 20,
    volumeNormalization: 'on',
    quickStationSkip: false,
    equalizerEnabled: false,
    equalizerBands: [0, 0, 0, 0, 0],
    localMusicDir: '',
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
    nightRunnersLowCutEnabled: false,
    nightRunnersFrequencyCutMode: 'low',
    nightRunnersLowCutAmount: 50,
    nightRunnersLowCutFrequencyHz: 140,
    nightRunnersNonDrivingVolumeEnabled: true,
    nightRunnersNonDrivingVolume: 50,
    radioLogoAlbumArtEnabled: true,
    radioLogoCustomGraphicEnabled: false,
    radioLogoSpotifyVariant: 'white',
    metadataTruncationEnabled: true,
    metadataTruncationLength: 30,
  });
  let optimisticSource = $state(null);
  let activeSource = $derived(optimisticSource ?? s.activeSource ?? 'spotify');

  // Spotify OAuth ("Log in with Spotify") flow state.
  let oauthBusy = $state(false);        // /oauth/start request in flight
  let oauthError = $state('');
  let local    = $derived(s.local ?? {
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
  });
  let radio    = $derived(s.radio ?? {
    available: true,
    connected: false,
    playing: false,
    stationName: '',
    stationId: '',
    codec: '',
    bitrate: 0,
    error: '',
  });
  let driving = $derived(s.driving ?? {
    speedAvailable: false,
    speedMps: 0,
    nightRunnersVolumeFactor: 1,
    nonFreeroamVolumeActive: false,
  });
  let ui       = $derived(s.ui ?? { credit_verified: true });
  let track    = $derived(s.track);
  let errors   = $derived(s.errors ?? []);
  let optionsOpen = $state(false);
  let savingOptions = $state(false);
  let optionsError = $state('');
  let pendingLocale = $state(null);
  let pendingMenuPlayback = $state(null);
  let pendingRaceStartPlayback = $state(null);
  let pendingRaceStartRestartThreshold = $state(null);
  let pendingSongStartOffsetEnabled = $state(null);
  let pendingSongStartOffsetSeconds = $state(null);
  let pendingVolumeNormalization = $state(null);
  let pendingQuickStationSkip = $state(null);
  let pendingEqualizerEnabled = $state(null);
  let pendingEqualizerBands = $state(null);
  let pendingLocalVolume = $state(null);
  let pendingNightRunnersMode = $state(null);
  let pendingNightRunnersStoppedVolumeDecrease = $state(null);
  let pendingNightRunnersMaxSpeedMph = $state(null);
  let pendingNightRunnersSpeedUnit = $state(null);
  let pendingNightRunnersCurveEnabled = $state(null);
  let pendingNightRunnersCurveExponent = $state(null);
  let pendingNightRunnersLazyVolumeEnabled = $state(null);
  let pendingNightRunnersLazyHoldSeconds = $state(null);
  let pendingNightRunnersLowCutEnabled = $state(null);
  let pendingNightRunnersFrequencyCutMode = $state(null);
  let pendingNightRunnersLowCutAmount = $state(null);
  let pendingNightRunnersLowCutFrequencyHz = $state(null);
  let pendingNightRunnersNonDrivingVolumeEnabled = $state(null);
  let pendingNightRunnersNonDrivingVolume = $state(null);
  let pendingNightRunnersDynamicMode = $state(null);
  let pendingNightRunnersDynamicThresholdMph = $state(null);
  let pendingNightRunnersDynamicBufferPercent = $state(null);
  let pendingNightRunnersDynamicBufferFillSeconds = $state(null);
  let pendingNightRunnersDynamicIncreaseSeconds = $state(null);
  let pendingNightRunnersDynamicDecreaseSeconds = $state(null);
  let pendingNightRunnersDynamicMaxDecayMphS = $state(null);
  let pendingRadioLogoAlbumArtEnabled = $state(null);
  let pendingRadioLogoCustomGraphicEnabled = $state(null);
  let pendingRadioLogoSpotifyVariant = $state(null);
  let pendingMetadataTruncationEnabled = $state(null);
  let pendingMetadataTruncationLength = $state(null);
  let pendingLocalTitleMetadataMode = $state(null);
  let pendingLocalArtistMetadataMode = $state(null);
  let localVolumeOpen = $state(false);
  let radioVolumeOpen = $state(false);
  let sourceMenuOpen = $state(false);
  let sourceSwitching = $state(false);
  let localPathInput = $state('');
  let localRecursiveInput = $state(true);
  let localShuffleInput = $state(true);
  let localPanelOpen = $state(false);
  let localPanelEmptyDismissed = $state(false);
  let localEmptyAutoOpenKey = $state('');
  let localPanelTab = $state('browser');
  let localLibrary = $state([]);
  let localManualQueue = $state([]);
  let localAutoplayQueue = $state([]);
  let localPanelLoading = $state(false);
  let localPanelError = $state('');
  let localQueueFeedbackKey = $state('');
  let localQueueFeedbackTimer = null;
  let radioPanelOpen = $state(false);
  let radioPanelTab = $state('discover');
  let radioSearchInput = $state('');
  let radioStations = $state([]);
  let radioSuggestions = $state([]);
  let radioFavorites = $state([]);
  let radioLoading = $state(false);
  let radioError = $state('');
  let radioManualUrl = $state('');
  let radioManualName = $state('');
  let radioManualStations = $state([]);
  let radioManualPrefilled = false;
  let radioSearchTimer = null;
  let radioPanelAutoOpenDismissed = $state(false);
  let savingLocal = $state(false);
  let localError = $state('');
  let errorsCollapsed = $state(true);
  let errorsHeight = $state(0);
  let markedErrorIndex = $state(null);
  let copiedErrorIndex = $state(null);
  let copiedErrorTimer = null;
  let creditIntegrityWarning = $state(false);
  let releaseVersion = $state('');
  let releaseDiag = $state(false);
  let bridgeCreditIntegrityWarning = $derived(ui.credit_verified === false);

  const defaultLocaleId = 'en';
  const expectedCreditText = 'Made by Big John';
  const expectedCreditHref = 'https://ko-fi.com/big_john';
  const defaultTexts = {
    'document.title': 'Spotify Radio - FH6 Status',
    'aria.version': 'Version {version}',
    'aria.audioSource': 'Audio source',
    'aria.localFiles': 'Local files',
    'aria.onlineRadio': 'Online radio',
    'aria.radioBrowser': 'Radio station browser',
    'aria.radioPanelTabs': 'Radio panel tabs',
    'aria.playStation': 'Play {station}',
    'aria.stopStation': 'Stop {station}',
    'aria.removeSavedStation': 'Remove saved station {station}',
    'aria.favoriteStation': 'Favorite {station}',
    'aria.unfavoriteStation': 'Remove {station} from favorites',
    'aria.scanFolder': 'Scan folder',
    'aria.chooseFolder': 'Choose folder',
    'aria.localFileBrowser': 'Local file browser',
    'aria.addFolderToQueue': 'Add {folder} to queue',
    'aria.playTrack': 'Play {title}',
    'aria.addTrackToQueue': 'Add {title} to queue',
    'aria.localQueue': 'Local queue',
    'aria.removeTrack': 'Remove {title}',
    'aria.localPanelTabs': 'Local panel tabs',
    'aria.connectAirPlay': 'Connect AirPlay device',
    'aria.airplayPlaying': 'AirPlay playing',
    'aria.airplayPaused': 'AirPlay paused',
    'aria.localFilesPlaying': 'Local files playing',
    'aria.localFilesPaused': 'Local files paused',
    'aria.chooseLocalMusicFolder': 'Choose local music folder',
    'aria.seek': 'Seek',
    'aria.localFilesVolume': 'Local files volume',
    'aria.previous': 'Previous',
    'aria.pause': 'Pause',
    'aria.play': 'Play',
    'aria.next': 'Next',
    'aria.localFilesFolder': 'Local files folder',
    'aria.includeSubfolders': 'Include subfolders',
    'aria.shuffleLocalFiles': 'Shuffle local files',
    'aria.connectSpotify': 'Connect Spotify device',
    'aria.spotifyPlaying': 'Spotify playing',
    'aria.spotifyPaused': 'Spotify paused',
    'aria.playingElsewhere': 'Playing on another device',
    'aria.drivingSpeed': 'Driving speed',
    'aria.joinDiscord': 'Join Discord',
    'aria.options': 'Options',
    'aria.menuPlaybackBehavior': 'Menu playback behavior',
    'aria.raceStartBehavior': 'Race start playback behavior',
    'aria.raceRestartThreshold': 'Race start restart threshold',
    'aria.stationSwitchingBehavior': 'Quick station switching behavior',
    'aria.trackVolumeBehavior': 'Track volume behavior',
    'aria.radioLogoBehavior': 'Radio logo behavior',
    'aria.metadataDisplay': 'Metadata display',
    'aria.metadataTruncationLength': 'Metadata truncation length',
    'aria.localTitleMetadata': 'Local file title metadata',
    'aria.localArtistMetadata': 'Local file artist metadata',
    'aria.nightRunnersMode': 'Night Runners Mode',
    'aria.stoppedReduction': 'Stopped volume reduction',
    'aria.fullVolumeSpeed': 'Full volume speed',
    'aria.speedUnit': 'Speed unit',
    'aria.curveStrength': 'Speed curve strength',
    'aria.lazyHold': 'Lazy volume hold time',
    'aria.lazyCooldown': 'Lazy volume drop countdown',
    'aria.frequencyCutMode': 'Frequency cut target',
    'aria.lowCutAmount': 'Frequency cut amount',
    'aria.lowCutFrequency': 'Frequency cut frequency',
    'aria.menuVolume': 'Garage and race menu volume',
    'aria.equalizer': '5-band equalizer',
    'aria.closeOptions': 'Close options',
    'aria.copyError': 'Copy error message',
    'source.spotify': 'Spotify',
    'source.airplay': 'AirPlay',
    'source.local': 'Local Files',
    'source.radio': 'Online Radio',
    'source.qqmusic': 'QQ Music',
    'source.vanilla': 'Vanilla Streamer Mode',
    'source.vanillaActive': "Game's own radio",
    'source.connected': 'Connected',
    'source.discoverable': 'Discoverable',
    'source.inactive': 'Inactive',
    'source.chooseFolder': 'Choose a folder',
    'count.track.one': '{count} track',
    'count.track.other': '{count} tracks',
    'status.gameClosed': 'Game Closed',
    'status.preparingGame': 'Preparing Game',
    'status.gameReady': 'Game Ready',
    'status.preparingRadio': 'Preparing Radio',
    'status.channelNotSelected': 'Channel Not Selected',
    'local.noFolderSelected': 'No folder selected',
    'local.subfolders': 'Subfolders',
    'local.shuffle': 'Shuffle',
    'local.loadingFiles': 'Loading files',
    'local.noCompatibleFiles': 'No compatible files detected ({formats})',
    'local.musicFolder': 'Music folder',
    'local.addFolderToQueue': 'Add folder to queue',
    'local.playNow': 'Play now',
    'local.addToQueue': 'Add to queue',
    'local.remove': 'Remove',
    'local.autoplayQueue': 'Autoplay queue',
    'local.files': 'Files',
    'local.queue': 'Queue',
    'local.chooseMusicFolder': 'Choose a music folder',
    'radio.discover': 'Discover',
    'radio.favorites': 'Favorites',
    'radio.recent': 'Recent',
    'radio.manual': 'URL',
    'radio.search': 'Search stations',
    'radio.searchPlaceholder': 'Station, genre, city',
    'radio.loadingStations': 'Loading stations',
    'radio.noStations': 'No stations found',
    'radio.topStations': 'Top stations',
    'radio.playNow': 'Play station',
    'radio.stop': 'Stop station',
    'radio.savedUrls': 'Saved stations',
    'radio.removeSaved': 'Remove saved station',
    'radio.addFavorite': 'Add favorite',
    'radio.removeFavorite': 'Remove favorite',
    'radio.live': 'Live',
    'radio.manualUrl': 'Stream URL',
    'radio.manualName': 'Display name',
    'radio.playUrl': 'Play URL',
    'radio.waiting': 'Choose a station',
    'prompt.selectAirPlay': 'Select "{device}"',
    'prompt.fromAirPlay': 'from AirPlay or Control Center',
    'prompt.waitingAirPlay': 'Waiting for AirPlay',
    'prompt.connectSpotify': 'Connect to device "FH6 Radio"',
    'prompt.inSpotifyApp': 'in your Spotify app',
    'prompt.playingElsewhere': 'Playing on another device',
    'prompt.selectSpotify': 'select FH6 Radio in Spotify',
    'oauth.or': 'or',
    'oauth.login': 'Log in with Spotify',
    'oauth.opening': 'Opening Spotify…',
    'oauth.failed': 'Login failed — try again',
    'aria.loginSpotify': 'Log in with Spotify',
    'footer.creditWarning': 'Unverified UI credit',
    'options.language': 'Language',
    'options.menuPlayback': 'Menu playback',
    'options.pauseMenus': 'Pause in menus',
    'options.pauseMenusDesc': 'Match vanilla radio behavior.',
    'options.silentMenus': 'Keep playing silently',
    'options.silentMenusDesc': 'Mute game output while the source advances.',
    'options.raceStart': 'Race start',
    'options.nextSong': 'Play next song',
    'options.nextSongDesc': 'Advance when a race begins.',
    'options.restartSong': 'Restart current song',
    'options.restartSongDesc': 'Begin races from the track start.',
    'options.keepPlaying': 'Keep playing',
    'options.keepPlayingDesc': 'Leave the source at the current position.',
    'options.smartRace': 'Restart or skip',
    'options.smartRaceDesc': 'Restart early songs, otherwise skip.',
    'options.smartRaceAirplayDesc': 'Restart only after the song has played long enough.',
    'options.raceRestartThreshold': 'Restart under',
    'options.raceRestartAfterThreshold': 'Restart after',
    'options.songStartOffset': 'Race song start',
    'options.songStartOffsetDesc': 'When a race starts a song, begin it at a set time instead of the beginning.',
    'options.songStartOffsetAirplay': 'Not available on AirPlay — the sender controls the position.',
    'options.songStartOffsetTime': 'Start point',
    'options.stationSwitching': 'Station switching',
    'options.normalSwitching': 'Normal switching',
    'options.normalSwitchingDesc': 'Keep the current song when returning.',
    'options.skipSong': 'Skip Song',
    'options.skipSongDesc': 'Return within 1s to play the next song.',
    'options.trackVolume': 'Track volume',
    'options.normalizeVolume': 'Normalize volume',
    'options.normalizeVolumeDesc': 'Keep songs closer in loudness.',
    'options.originalVolume': 'Original track volume',
    'options.originalVolumeDesc': "Use each track's own loudness.",
    'options.drivingVolume': 'Driving volume',
    'options.nightRunnersMode': 'Night Runners Mode',
    'options.nightRunnersDesc': 'Increased volume while driving fast.',
    'options.on': 'On',
    'options.off': 'Off',
    'options.stoppedReduction': 'Stopped reduction',
    'options.dynamicMode': 'Dynamic volume',
    'options.dynamicModeDesc': 'Scale volume to speed in real time: it swells as you accelerate, holds while you keep pace, and eases off as you slow.',
    'options.dynamicThreshold': 'Speed threshold',
    'options.dynamicBuffer': 'Coast buffer',
    'options.dynamicBufferFill': 'Buffer fill time',
    'options.dynamicFadeIn': 'Fade-in time',
    'options.dynamicFadeOut': 'Fade-out time',
    'options.dynamicMaxDecay': 'Max speed ease-down',
    'options.fullVolumeSpeed': 'Full volume speed',
    'options.speedCurve': 'Speed curve',
    'options.speedCurveDesc': 'Keep low-speed changes quieter, then rise faster near the limit.',
    'options.curveStrength': 'Curve strength',
    'options.lazyVolume': 'Lazy volume drop',
    'options.lazyVolumeDesc': 'Hold volume after a speed peak, then ease it down only if you stay slower.',
    'options.lazyHold': 'Hold time',
    'options.lazyCooldown': 'Drop countdown',
    'options.lowFrequencyCut': 'Frequency cut',
    'options.lowFrequencyCutDesc': 'Cut low or high frequencies at low speed and ease them back in as speed rises.',
    'options.lowEndCut': 'Low end',
    'options.highEndCut': 'High end',
    'options.cutAmount': 'Cut amount',
    'options.frequencyCut': 'Frequency cut',
    'options.garageRaceMenus': 'Garage and race menus',
    'options.garageRaceMenusDesc': 'Use a fixed volume in garage and race menus.',
    'options.menuVolume': 'Menu volume',
    'options.equalizer': 'Equalizer',
    'options.customTone': 'Custom tone',
    'options.customToneDesc': 'Shape playback with a local 5-band EQ.',
    'options.radioLogo': 'Radio logo',
    'options.albumArtLogo': 'Album covers',
    'options.albumArtLogoDesc': 'Use track artwork as the in-game station logo when available.',
    'options.customLogo': 'Custom graphic',
    'options.customLogoDesc': 'Uses spotify-radio/logos/custom.png when album artwork is unavailable or disabled.',
    'options.customLogoHelp': 'Place a square PNG at spotify-radio/logos/custom.png. Restart the game after changing the file.',
    'options.spotifyLogoColor': 'Spotify logo',
    'options.spotifyLogoWhite': 'White',
    'options.spotifyLogoWhiteDesc': 'Use the white Spotify default logo.',
    'options.spotifyLogoColored': 'Color',
    'options.spotifyLogoColoredDesc': 'Use spotify-radio/logos/spotify-color.png when present.',
    'options.metadataDisplay': 'Metadata display',
    'options.truncateMetadata': 'Shorten title and artist',
    'options.truncateMetadataDesc': 'Limit long track and artist text in the Web UI and in-game radio card.',
    'options.truncateMetadataLength': 'Max characters',
    'options.localTitleMetadata': 'Local title',
    'options.localTitleTrackTitle': 'Track title',
    'options.localTitleTrackTitleDesc': 'Use embedded title metadata, then fall back to the file name.',
    'options.localTitleFilename': 'File name',
    'options.localTitleFilenameDesc': 'Use the local file name without its extension.',
    'options.localArtistMetadata': 'Local artist line',
    'options.localArtistAlbumArtist': 'Album artist + artist',
    'options.localArtistAlbumArtistDesc': 'Use album artist and contributing artist, then fall back to album or folder.',
    'options.localArtistFolder': 'Folder',
    'options.localArtistFolderDesc': 'Use the parent folder, then fall back to embedded artist metadata.',
    'options.localArtistAlbum': 'Album',
    'options.localArtistAlbumDesc': 'Use embedded album metadata, then fall back to artist or folder.',
    'options.reset': 'Reset',
    'error.saveOptions': 'Could not save options',
    'error.saveVolume': 'Could not save volume',
    'error.loadLocalFiles': 'Could not load local files',
    'error.switchSource': 'Could not switch source',
    'error.scanFolder': 'Could not scan that folder',
    'error.openFolderPicker': 'Could not open the folder picker',
    'error.localCommand': 'Local playback command failed',
    'error.loadRadioStations': 'Could not load radio stations',
    'error.radioCommand': 'Radio command failed',
    'error.favoriteStation': 'Could not update favorite',
    'error.playFile': 'Could not play that file',
    'error.addFile': 'Could not add that file',
    'error.addFolder': 'Could not add that folder',
    'error.removeItem': 'Could not remove that item',
    'errors.one': '1 error',
    'errors.other': '{count} errors',
    'hint.audioKey': 'Spotify servers refused to play audio for this Spotify Connect track. The song title and artist can still appear correctly in game, but playback will not work while Spotify denies the audio key. This is usually account-side, especially with trial Premium or very new Premium accounts. Try a different Spotify account, end the trial Premium period, or switch to another playback mode from the top-left dashboard selector.',
    'hint.spotifyNetwork': 'Cannot reach Spotify Access Point servers (ap-*.spotify.com, TCP 4070 / 443). This is network-side, not the mod. Common causes: firewall or antivirus blocking outbound traffic (whitelist forzahorizon6.exe), VPN or TLS-inspecting proxy mangling the handshake (disable VPN or split-tunnel *.spotify.com), country / ISP-level Spotify block (test on a mobile hotspot to confirm), or DNS failure on apresolve.spotify.com (try a public resolver like 1.1.1.1 or 8.8.8.8). If the official Spotify desktop app on the same machine cannot stream either, it is your network, not the mod.',
  };
  let localeTexts = $state(defaultTexts);
  let availableLocales = $state([
    { id: 'en', name: 'English' },
    { id: 'de', name: 'Deutsch' },
  ]);
  let localeLoadToken = 0;
  let activeLocale = $derived(pendingLocale ?? options.locale ?? defaultLocaleId);
  const eqBands = [
    { label: '60', unit: 'Hz' },
    { label: '250', unit: 'Hz' },
    { label: '1k', unit: 'Hz' },
    { label: '4k', unit: 'Hz' },
    { label: '12k', unit: 'Hz' },
  ];
  const localVolumeMax = 300;
  const nightSpeedMphMin = 10;
  const nightSpeedMphMax = 250;
  const nightSpeedMphDefault = 120;
  const nightCurveExponentMin = 1;
  const nightCurveExponentMax = 4;
  const nightCurveExponentDefault = 2;
  const nightLazyHoldMin = 0.5;
  const nightLazyHoldMax = 10;
  const nightLazyHoldDefault = 2;
  const nightDynThresholdMphMin = 5;
  const nightDynThresholdMphMax = 250;
  const nightDynThresholdMphDefault = 40;
  const nightDynBufferPercentDefault = 15;
  const nightDynBufferPercentMax = 50;
  const nightDynBufferFillMin = 0.5;
  const nightDynBufferFillMax = 15;
  const nightDynBufferFillDefault = 3;
  const nightDynRampMin = 0.2;
  const nightDynRampMax = 15;
  const nightDynIncreaseDefault = 1.5;
  const nightDynDecreaseDefault = 2.5;
  const nightDynMaxDecayMphMax = 30;
  const nightDynMaxDecayDefault = 0;
  const metadataTruncationMin = 10;
  const metadataTruncationMax = 50;
  const metadataTruncationDefault = 30;
  const nightFrequencyCutRanges = {
    low: { min: 40, max: 320, default: 140 },
    high: { min: 1200, max: 12000, default: 5000 },
  };
  const mphToKmh = 1.609344;
  const mpsToMph = 2.2369362921;

  function t(key, params = {}) {
    const raw = localeTexts[key] ?? defaultTexts[key] ?? key;
    return String(raw).replace(/\{([A-Za-z0-9_]+)\}/g, (_, name) => (
      params[name] === undefined || params[name] === null ? '' : String(params[name])
    ));
  }

  function tracksLabel(count) {
    const safe = Math.max(0, Number(count) || 0);
    return t(safe === 1 ? 'count.track.one' : 'count.track.other', { count: safe });
  }

  function toggleLabel(value) {
    return value ? t('options.on') : t('options.off');
  }

  function clampEq(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return 0;
    return Math.max(-6, Math.min(6, Math.round(n * 2) / 2));
  }

  function clampRaceThreshold(value, fallback = 20) {
    const n = Math.round(Number(value));
    if (!Number.isFinite(n)) return fallback;
    return Math.max(5, Math.min(60, n));
  }

  function clampSongStartOffset(value, fallback = 30) {
    const n = Math.round(Number(value));
    if (!Number.isFinite(n)) return fallback;
    return Math.max(3, Math.min(240, n));
  }

  function normalizedEqBands(value) {
    const source = Array.isArray(value) ? value : [];
    return eqBands.map((_, index) => clampEq(source[index] ?? 0));
  }

  function clampLocalVolume(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return 100;
    return Math.max(0, Math.min(localVolumeMax, Math.round(n)));
  }

  function clampPercent(value, fallback = 50) {
    const n = Number(value);
    if (!Number.isFinite(n)) return fallback;
    return Math.max(0, Math.min(100, Math.round(n)));
  }

  function clampNightSpeedMph(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightSpeedMphDefault;
    return Math.max(nightSpeedMphMin, Math.min(nightSpeedMphMax, Math.round(n)));
  }

  function clampNightCurveExponent(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightCurveExponentDefault;
    return Math.max(nightCurveExponentMin, Math.min(nightCurveExponentMax, Math.round(n * 10) / 10));
  }

  function clampNightLazyHoldSeconds(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightLazyHoldDefault;
    return Math.max(nightLazyHoldMin, Math.min(nightLazyHoldMax, Math.round(n * 2) / 2));
  }

  function clampNightDynThresholdMph(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightDynThresholdMphDefault;
    return Math.max(nightDynThresholdMphMin,
                    Math.min(nightDynThresholdMphMax, Math.round(n)));
  }

  function clampNightDynBufferFill(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightDynBufferFillDefault;
    return Math.max(nightDynBufferFillMin,
                    Math.min(nightDynBufferFillMax, Math.round(n * 2) / 2));
  }

  function clampNightDynBufferPercent(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightDynBufferPercentDefault;
    return Math.max(0, Math.min(nightDynBufferPercentMax, Math.round(n)));
  }

  function clampNightDynRamp(value, fallback) {
    const n = Number(value);
    if (!Number.isFinite(n)) return fallback;
    return Math.max(nightDynRampMin,
                    Math.min(nightDynRampMax, Math.round(n * 10) / 10));
  }

  function clampNightDynMaxDecay(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightDynMaxDecayDefault;
    return Math.max(0, Math.min(nightDynMaxDecayMphMax, Math.round(n)));
  }

  function clampMetadataTruncationLength(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return metadataTruncationDefault;
    return Math.max(metadataTruncationMin,
                    Math.min(metadataTruncationMax, Math.round(n)));
  }

  function normalizeLocalTitleMetadataMode(value) {
    return value === 'filename' ? 'filename' : 'metadata';
  }

  function normalizeLocalArtistMetadataMode(value) {
    return value === 'folder' || value === 'album' ? value : 'albumArtist';
  }

  function normalizeFrequencyCutMode(value) {
    return value === 'high' ? 'high' : 'low';
  }

  function frequencyCutRange(mode) {
    return nightFrequencyCutRanges[normalizeFrequencyCutMode(mode)];
  }

  function clampNightLowCutFrequency(value, mode = 'low') {
    const range = frequencyCutRange(mode);
    const n = Number(value);
    if (!Number.isFinite(n)) return range.default;
    return Math.max(range.min, Math.min(range.max, Math.round(n)));
  }

  function frequencyCutSliderFromHz(value, mode) {
    const range = frequencyCutRange(mode);
    const hz = clampNightLowCutFrequency(value, mode);
    return Math.round(((hz - range.min) / (range.max - range.min)) * 100);
  }

  function frequencyCutHzFromSlider(value, mode) {
    const range = frequencyCutRange(mode);
    const n = Number(value);
    const pos = Number.isFinite(n) ? Math.max(0, Math.min(100, n)) / 100 : 0.5;
    return clampNightLowCutFrequency(range.min + (range.max - range.min) * pos, mode);
  }

  function remapFrequencyCutHz(value, fromMode, toMode) {
    const slider = frequencyCutSliderFromHz(value, fromMode);
    return frequencyCutHzFromSlider(slider, toMode);
  }

  function formatFrequency(value) {
    const hz = Math.max(0, Math.round(Number(value) || 0));
    if (hz >= 1000) {
      const khz = hz / 1000;
      return `${khz >= 10 ? khz.toFixed(0) : khz.toFixed(1)} kHz`;
    }
    return `${hz} Hz`;
  }

  function normalizeSpeedUnit(value) {
    return value === 'kmh' ? 'kmh' : 'mph';
  }

  function speedUnitLabel(unit) {
    return normalizeSpeedUnit(unit) === 'kmh' ? 'km/h' : 'mph';
  }

  function speedDisplayFromMph(mph, unit) {
    const safeMph = clampNightSpeedMph(mph);
    return normalizeSpeedUnit(unit) === 'kmh'
      ? Math.round(safeMph * mphToKmh)
      : safeMph;
  }

  function speedMphFromDisplay(value, unit) {
    const n = Number(value);
    if (!Number.isFinite(n)) return nightSpeedMphDefault;
    const mph = normalizeSpeedUnit(unit) === 'kmh' ? n / mphToKmh : n;
    return clampNightSpeedMph(mph);
  }

  function curvePath(exponent) {
    const exp = clampNightCurveExponent(exponent);
    const width = 220;
    const height = 96;
    const pad = 12;
    const points = [];
    for (let i = 0; i <= 28; i += 1) {
      const x = i / 28;
      const y = Math.pow(x, exp);
      points.push(`${pad + x * (width - pad * 2)},${height - pad - y * (height - pad * 2)}`);
    }
    return `M ${points.join(' L ')}`;
  }

  function lowCutPath(amount, curveEnabled, exponent) {
    const intensity = clampPercent(amount) / 100;
    const exp = clampNightCurveExponent(exponent);
    const width = 220;
    const height = 96;
    const pad = 12;
    const points = [];
    for (let i = 0; i <= 28; i += 1) {
      const x = i / 28;
      const volumeProgress = curveEnabled ? Math.pow(x, exp) : x;
      const y = intensity * (1 - volumeProgress);
      points.push(`${pad + x * (width - pad * 2)},${height - pad - y * (height - pad * 2)}`);
    }
    return `M ${points.join(' L ')}`;
  }

  // Known error patterns get an inline hint so users can self-triage.
  // Hint = null for unrecognised lines; the raw log line still renders.
  function trackErrorDetail(line) {
    const marker = 'TrackError detail=';
    const at = line.indexOf(marker);
    if (at < 0) return '';
    return line.slice(at + marker.length)
      .replace(/^(metadata_unavailable|unplayable|audio_source|cdn|decode|output|invalid_uri):\s*/, '');
  }

  function hintFor(line) {
    if (line.includes('TrackError') &&
        trackErrorDetail(line).includes('audio key request failed')) {
      return t('hint.audioKey');
    }
    if (line.includes('all APs failed handshake')) {
      return t('hint.spotifyNetwork');
    }
    return null;
  }
  // Attach the hint only to the FIRST line carrying a given hint, so a
  // repeated error (e.g. WARN + ERROR for the same handshake failure)
  // does not stack the same paragraph twice.
  let annotatedErrors = $derived((() => {
    const seen = new Set();
    return errors.map(line => {
      const hint = hintFor(line);
      if (hint && !seen.has(hint)) { seen.add(hint); return { line, hint }; }
      return { line, hint: null };
    });
  })());
  let errorOffset = $derived(annotatedErrors.length > 0 ? errorsHeight : 0);

  // The dashboard is served over plain HTTP on the LAN, which is not a secure
  // context, so navigator.clipboard is usually unavailable on phones. Fall back
  // to a temporary textarea + execCommand within the click gesture.
  async function copyText(text) {
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
        return true;
      }
    } catch { /* fall through to legacy path */ }
    try {
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.top = '-1000px';
      ta.style.opacity = '0';
      ta.style.userSelect = 'text';
      ta.style.webkitUserSelect = 'text';
      document.body.appendChild(ta);
      ta.focus();
      ta.select();
      const ok = document.execCommand('copy');
      document.body.removeChild(ta);
      return ok;
    } catch {
      return false;
    }
  }

  async function copyError(index, text) {
    // Don't hijack a manual text selection the user made inside the errors.
    const sel = typeof window !== 'undefined' ? window.getSelection?.() : null;
    if (sel && sel.toString().trim().length > 0) return;
    markedErrorIndex = index;
    const ok = await copyText(text);
    if (ok) {
      copiedErrorIndex = index;
      clearTimeout(copiedErrorTimer);
      copiedErrorTimer = setTimeout(() => { copiedErrorIndex = null; }, 1500);
    }
  }

  // ── Single status label (priority order) ──
  //   0. bridge offline                            → Game Closed         (grey, power_off)
  //   1. game not ready                              → Preparing Game        (amber, hourglass)
  //   2. game ready, audio idle                      → Game Ready            (green, check)
  //   3. game ready, audio active but R10 untuned    → Channel Not Selected  (amber, radio)
  //   4. R10 tuned, still waiting for routing         → Preparing Radio      (amber, hourglass)
  //   5. game ready, routed off master, R10 tuned     → Radio Channel Ready  (green, check)
  let status = $derived((() => {
    if (!online)
      return { text: t('status.gameClosed'),          tone: 'idle', icon: 'power_settings_new' };
    if (!(game.attached && game.injector_ready))
      return { text: t('status.preparingGame'),       tone: 'warn', icon: 'hourglass_top' };
    if (!audio.active)
      return { text: t('status.gameReady'),           tone: 'ok',   icon: 'check_circle' };
    if (audio.migrated && audio.r10_active)
      return { text: '',                     tone: 'ok',   icon: 'check_circle' };
    if (audio.r10_active)
      return { text: t('status.preparingRadio'),      tone: 'warn', icon: 'hourglass_top' };
    return   { text: t('status.channelNotSelected'), tone: 'warn', icon: 'radio' };
  })());

  let hasTrack = $derived(Boolean(track.title?.trim()));
  let localReady = $derived(Boolean(local.ready && local.trackCount > 0));
  let localPath = $derived(local.musicDir ?? options.localMusicDir ?? '');
  let localRecursive = $derived(Boolean(local.recursive ?? options.localRecursive ?? true));
  let localShuffle = $derived(Boolean(local.shuffle ?? options.localShuffle ?? true));
  let localSupportedFormats = $derived((local.supportedFormats ?? ['mp3', 'wav', 'flac']).join(', ').toUpperCase());
  let localDurationMs = $derived(Number(track.duration_ms ?? 0));
  let localPositionMs = $derived(Number(local.position_ms ?? 0));
  let localProgress = $derived(
    localDurationMs > 0
      ? Math.min(100, Math.max(0, (localPositionMs / localDurationMs) * 100))
      : 0
  );
  let localFolderGroups = $derived((() => {
    const groups = new Map();
    for (const track of localLibrary) {
      const folder = localRecursive ? (track.folder || t('local.musicFolder')) : t('local.musicFolder');
      if (!groups.has(folder)) groups.set(folder, []);
      groups.get(folder).push(track);
    }
    return Array.from(groups, ([folder, tracks]) => ({ folder, tracks }));
  })());
  let localPanelVisibleError = $derived((() => {
    const message = localPanelError || localError || local.error || '';
    return isLocalEmptyLibraryError(message) ? '' : message;
  })());
  let radioPanelVisibleError = $derived(radioError || radio.error || '');
  let radioStationMeta = $derived((() => {
    const bits = [];
    if (radio.codec) bits.push(String(radio.codec).toUpperCase());
    if (Number(radio.bitrate) > 0) bits.push(`${radio.bitrate} kbps`);
    return bits.join(' ');
  })());
  let activeSourceCanSeek = $derived(activeSource !== 'airplay' && activeSource !== 'radio');
  let activeSourceCanRestart = $derived(activeSource !== 'radio');
  let activeSourceCanSmartRace = $derived(activeSource !== 'radio');
  let showSourceLabel = $derived(online === true);
  let showOptionsLabel = $derived(Boolean(
    online === true &&
    game?.attached === true &&
    game?.injector_ready === true &&
    // Vanilla Streamer Mode is the game's own radio, not our DSP, so none of our
    // audio/behavior options apply — hide the panel + button (same as offline).
    activeSource !== 'vanilla'
  ));
  let menuPlayback = $derived(pendingMenuPlayback ?? options.menuPlayback ?? 'pause');
  let raceStartPlayback = $derived(
    pendingRaceStartPlayback ?? options.raceStartPlayback ?? 'next'
  );
  let raceStartRestartThreshold = $derived(
    pendingRaceStartRestartThreshold
      ?? clampRaceThreshold(options.raceStartRestartThresholdSeconds, 20)
  );
  let songStartOffsetEnabled = $derived(
    pendingSongStartOffsetEnabled ?? Boolean(options.songStartOffsetEnabled)
  );
  let songStartOffsetSeconds = $derived(
    pendingSongStartOffsetSeconds
      ?? clampSongStartOffset(options.songStartOffsetSeconds, 30)
  );
  let volumeNormalization = $derived(
    pendingVolumeNormalization ?? options.volumeNormalization ?? 'on'
  );
  let quickStationSkip = $derived(
    pendingQuickStationSkip ?? Boolean(options.quickStationSkip)
  );
  let equalizerEnabled = $derived(
    pendingEqualizerEnabled ?? Boolean(options.equalizerEnabled)
  );
  let equalizerBands = $derived(
    pendingEqualizerBands ?? normalizedEqBands(options.equalizerBands)
  );
  let localVolume = $derived(
    pendingLocalVolume ?? clampLocalVolume(options.localVolume ?? 100)
  );
  let localVolumeIcon = $derived(
    localVolume <= 0 ? 'volume_off' : localVolume < 100 ? 'volume_down' : 'volume_up'
  );
  let nightRunnersMode = $derived(
    pendingNightRunnersMode ?? Boolean(options.nightRunnersMode)
  );
  let nightRunnersStoppedVolumeDecrease = $derived(
    pendingNightRunnersStoppedVolumeDecrease
      ?? clampPercent(options.nightRunnersStoppedVolumeDecrease ?? 50)
  );
  let nightRunnersMaxSpeedMph = $derived(
    pendingNightRunnersMaxSpeedMph
      ?? clampNightSpeedMph(options.nightRunnersMaxSpeedMph ?? nightSpeedMphDefault)
  );
  let nightRunnersSpeedUnit = $derived(
    pendingNightRunnersSpeedUnit ?? normalizeSpeedUnit(options.nightRunnersSpeedUnit)
  );
  let nightRunnersCurveEnabled = $derived(
    pendingNightRunnersCurveEnabled ?? Boolean(options.nightRunnersCurveEnabled)
  );
  let nightRunnersCurveExponent = $derived(
    pendingNightRunnersCurveExponent
      ?? clampNightCurveExponent(options.nightRunnersCurveExponent ?? nightCurveExponentDefault)
  );
  let nightRunnersLazyVolumeEnabled = $derived(
    pendingNightRunnersLazyVolumeEnabled ?? Boolean(options.nightRunnersLazyVolumeEnabled)
  );
  let nightRunnersLazyHoldSeconds = $derived(
    pendingNightRunnersLazyHoldSeconds
      ?? clampNightLazyHoldSeconds(options.nightRunnersLazyHoldSeconds ?? nightLazyHoldDefault)
  );
  let nightRunnersLowCutEnabled = $derived(
    pendingNightRunnersLowCutEnabled ?? Boolean(options.nightRunnersLowCutEnabled)
  );
  let nightRunnersFrequencyCutMode = $derived(
    pendingNightRunnersFrequencyCutMode
      ?? normalizeFrequencyCutMode(options.nightRunnersFrequencyCutMode)
  );
  let nightRunnersLowCutAmount = $derived(
    pendingNightRunnersLowCutAmount
      ?? clampPercent(options.nightRunnersLowCutAmount ?? 50)
  );
  let nightRunnersLowCutFrequencyHz = $derived(
    pendingNightRunnersLowCutFrequencyHz
      ?? clampNightLowCutFrequency(
        options.nightRunnersLowCutFrequencyHz,
        nightRunnersFrequencyCutMode
      )
  );
  let nightRunnersFrequencyCutSlider = $derived(
    frequencyCutSliderFromHz(
      nightRunnersLowCutFrequencyHz,
      nightRunnersFrequencyCutMode
    )
  );
  let nightRunnersNonDrivingVolumeEnabled = $derived(
    pendingNightRunnersNonDrivingVolumeEnabled
      ?? Boolean(options.nightRunnersNonDrivingVolumeEnabled ?? true)
  );
  let nightRunnersNonDrivingVolume = $derived(
    pendingNightRunnersNonDrivingVolume
      ?? clampPercent(options.nightRunnersNonDrivingVolume ?? 50)
  );
  let nightRunnersDynamicMode = $derived(
    pendingNightRunnersDynamicMode ?? Boolean(options.nightRunnersDynamicMode)
  );
  let nightRunnersDynamicThresholdMph = $derived(
    pendingNightRunnersDynamicThresholdMph
      ?? clampNightDynThresholdMph(
        options.nightRunnersDynamicThresholdMph ?? nightDynThresholdMphDefault)
  );
  let nightRunnersDynamicBufferPercent = $derived(
    pendingNightRunnersDynamicBufferPercent
      ?? clampNightDynBufferPercent(options.nightRunnersDynamicBufferPercent
                      ?? nightDynBufferPercentDefault)
  );
  let nightRunnersDynamicBufferFillSeconds = $derived(
    pendingNightRunnersDynamicBufferFillSeconds
      ?? clampNightDynBufferFill(
        options.nightRunnersDynamicBufferFillSeconds ?? nightDynBufferFillDefault)
  );
  let nightRunnersDynamicIncreaseSeconds = $derived(
    pendingNightRunnersDynamicIncreaseSeconds
      ?? clampNightDynRamp(
        options.nightRunnersDynamicIncreaseSeconds, nightDynIncreaseDefault)
  );
  let nightRunnersDynamicDecreaseSeconds = $derived(
    pendingNightRunnersDynamicDecreaseSeconds
      ?? clampNightDynRamp(
        options.nightRunnersDynamicDecreaseSeconds, nightDynDecreaseDefault)
  );
  let nightRunnersDynamicMaxDecayMphS = $derived(
    pendingNightRunnersDynamicMaxDecayMphS
      ?? clampNightDynMaxDecay(options.nightRunnersDynamicMaxDecayMphS
                               ?? nightDynMaxDecayDefault)
  );
  let radioLogoAlbumArtEnabled = $derived(
    pendingRadioLogoAlbumArtEnabled ?? Boolean(options.radioLogoAlbumArtEnabled ?? true)
  );
  let radioLogoCustomGraphicEnabled = $derived(
    pendingRadioLogoCustomGraphicEnabled ?? Boolean(options.radioLogoCustomGraphicEnabled)
  );
  let radioLogoSpotifyVariant = $derived(
    pendingRadioLogoSpotifyVariant ?? (options.radioLogoSpotifyVariant === 'color' ? 'color' : 'white')
  );
  let metadataTruncationEnabled = $derived(
    pendingMetadataTruncationEnabled ??
      Boolean(options.metadataTruncationEnabled ?? true)
  );
  let metadataTruncationLength = $derived(
    pendingMetadataTruncationLength
      ?? clampMetadataTruncationLength(
        options.metadataTruncationLength ?? metadataTruncationDefault)
  );
  let localTitleMetadataMode = $derived(
    pendingLocalTitleMetadataMode
      ?? normalizeLocalTitleMetadataMode(options.localTitleMetadataMode)
  );
  let localArtistMetadataMode = $derived(
    pendingLocalArtistMetadataMode
      ?? normalizeLocalArtistMetadataMode(options.localArtistMetadataMode)
  );
  let nightRunnersCurvePath = $derived(curvePath(nightRunnersCurveExponent));
  let nightRunnersLowCutPath = $derived(
    lowCutPath(nightRunnersLowCutAmount, nightRunnersCurveEnabled, nightRunnersCurveExponent)
  );
  let nightSpeedDisplayMin = $derived(speedDisplayFromMph(nightSpeedMphMin, nightRunnersSpeedUnit));
  let nightSpeedDisplayMax = $derived(speedDisplayFromMph(nightSpeedMphMax, nightRunnersSpeedUnit));
  let nightSpeedDisplayValue = $derived(speedDisplayFromMph(nightRunnersMaxSpeedMph, nightRunnersSpeedUnit));
  let drivingSpeedMph = $derived(Math.max(0, Number(driving.speedMps ?? 0) * mpsToMph));
  let drivingSpeedProgress = $derived(
    nightRunnersMaxSpeedMph > 0
      ? Math.max(0, Math.min(1, drivingSpeedMph / nightRunnersMaxSpeedMph))
      : 0
  );
  // The bridge publishes the effective (lazy-held) speed progress, so the
  // graph markers reflect the held position when lazy volume is active and
  // track live speed otherwise.
  let speedMarkerProgress = $derived(
    driving.nightRunnersSpeedProgress != null
      ? Math.max(0, Math.min(1, Number(driving.nightRunnersSpeedProgress)))
      : drivingSpeedProgress
  );
  let speedMarkerTranslate = $derived(speedMarkerProgress * 196);
  let showSpeedGraphMarker = $derived(
    nightRunnersMode &&
    driving.speedAvailable &&
    !Boolean(driving.nonFreeroamVolumeActive)
  );
  let drivingSpeedDisplay = $derived(
    normalizeSpeedUnit(nightRunnersSpeedUnit) === 'kmh'
      ? Math.round(drivingSpeedMph * mphToKmh)
      : Math.round(drivingSpeedMph)
  );
  let drivingVolumeFill = $derived(
    (() => {
      const factor = Math.max(0, Math.min(1, Number(driving.nightRunnersVolumeFactor ?? 1)));
      const stoppedFactor = 1 - nightRunnersStoppedVolumeDecrease / 100;
      const range = 1 - stoppedFactor;
      if (range <= 0.001) return 100;
      return Math.max(0, Math.min(100, ((factor - stoppedFactor) / range) * 100));
    })()
  );
  let drivingSpeedLabel = $derived(
    `${driving.speedAvailable ? drivingSpeedDisplay : '--'} ${speedUnitLabel(nightRunnersSpeedUnit)}`
  );
  let showDrivingIndicator = $derived(
    nightRunnersMode &&
    driving.speedAvailable &&
    drivingSpeedDisplay > 0 &&
    !Boolean(driving.nonFreeroamVolumeActive)
  );
  // ── Dynamic-mode speed-state bar (bar #2) ──
  // Speed axis: threshold anchored at the midpoint (axis max = 2x threshold);
  // when live speed exceeds 2x threshold the axis grows to keep the head and
  // peak on-bar (the threshold separator then drifts left of centre).
  let dynPeakMph = $derived(Math.max(0, Number(driving.nightRunnersPeakMps ?? 0) * mpsToMph));
  let dynBufferMph = $derived(Math.max(0, Number(driving.nightRunnersBufferMps ?? 0) * mpsToMph));
  let dynAxisMaxMph = $derived(
    Math.max(
      nightRunnersDynamicThresholdMph * 2,
      drivingSpeedMph,
      dynPeakMph,
      1
    )
  );
  let dynThresholdPos = $derived(
    Math.max(0, Math.min(100, (nightRunnersDynamicThresholdMph / dynAxisMaxMph) * 100))
  );
  let dynSpeedPos = $derived(
    Math.max(0, Math.min(100, (drivingSpeedMph / dynAxisMaxMph) * 100))
  );
  let dynPeakPos = $derived(
    Math.max(0, Math.min(100, (dynPeakMph / dynAxisMaxMph) * 100))
  );
  let dynBufferLeftPos = $derived(
    Math.max(0, Math.min(100, ((dynPeakMph - dynBufferMph) / dynAxisMaxMph) * 100))
  );
  let dynBufferWidth = $derived(Math.max(0, dynPeakPos - dynBufferLeftPos));
  let dynStateClass = $derived(
    ['below', 'accel', 'hold', 'fade'][Number(driving.nightRunnersDriveState ?? 0)] ?? 'below'
  );
  let showDynamicBar = $derived(
    nightRunnersMode &&
    nightRunnersDynamicMode &&
    driving.speedAvailable &&
    !Boolean(driving.nonFreeroamVolumeActive)
  );
  let nightDynThresholdIsKmh = $derived(normalizeSpeedUnit(nightRunnersSpeedUnit) === 'kmh');
  let nightDynThresholdDisplayMin = $derived(
    nightDynThresholdIsKmh ? Math.round(nightDynThresholdMphMin * mphToKmh) : nightDynThresholdMphMin
  );
  let nightDynThresholdDisplayMax = $derived(
    nightDynThresholdIsKmh ? Math.round(nightDynThresholdMphMax * mphToKmh) : nightDynThresholdMphMax
  );
  let nightDynThresholdDisplayValue = $derived(
    nightDynThresholdIsKmh
      ? Math.round(nightRunnersDynamicThresholdMph * mphToKmh)
      : nightRunnersDynamicThresholdMph
  );
  // Max-decay rate slider, displayed in the selected speed unit per second.
  let nightDynMaxDecayDisplayMax = $derived(
    nightDynThresholdIsKmh
      ? Math.round(nightDynMaxDecayMphMax * mphToKmh)
      : nightDynMaxDecayMphMax
  );
  let nightDynMaxDecayDisplayValue = $derived(
    nightDynThresholdIsKmh
      ? Math.round(nightRunnersDynamicMaxDecayMphS * mphToKmh)
      : nightRunnersDynamicMaxDecayMphS
  );
  let lazyCooldownFill = $derived(
    Math.max(0, Math.min(100, Number(driving.nightRunnersLazyCooldown ?? 0) * 100))
  );
  let showLazyCooldown = $derived(
    nightRunnersMode &&
    nightRunnersLazyVolumeEnabled &&
    driving.speedAvailable &&
    !Boolean(driving.nonFreeroamVolumeActive)
  );

  // ── Spotify icon state ──
  //   'eq'        — animated equalizer (playing)
  //   'pause'     — paused
  //   'away'      — transferred to other device
  //   'connect'   — waiting for the user to pick this Connect device
  let spotifyIcon = $derived(
    !spotify.connected         ? 'connect'
    : spotify.transferred_away ? 'away'
    : spotify.playing && hasTrack ? 'eq'
    : hasTrack                 ? 'pause'
    : 'connect'
  );
  // Authenticated = logged in now OR a reusable credential exists on disk.
  // The OAuth button only makes sense before either is true (spotifyIcon can be
  // 'connect' even when connected-but-idle, so it can't gate the button alone).
  let spotifyAuthed = $derived(!!(spotify.connected || spotify.blob_cached));
  let airplayIcon = $derived(
    !airplay.running || !airplay.connected ? 'connect'
    : airplay.playing && hasTrack ? 'eq'
    : hasTrack ? 'pause'
    : 'connect'
  );

  function isLocalEmptyLibraryError(message) {
    return typeof message === 'string'
        && /^No supported .* files found$/i.test(message.trim());
  }

  function safeLocaleId(value) {
    const id = String(value || defaultLocaleId);
    return /^[A-Za-z0-9_-]{1,48}$/.test(id) ? id : defaultLocaleId;
  }

  function normalizeLocaleData(data) {
    const source = data?.strings && typeof data.strings === 'object'
      ? data.strings
      : data;
    const strings = {};
    if (source && typeof source === 'object') {
      for (const [key, value] of Object.entries(source)) {
        if (typeof value === 'string') strings[key] = value;
      }
    }
    return strings;
  }

  async function loadAvailableLocales() {
    try {
      const data = await getJson('/api/locales');
      if (Array.isArray(data?.locales) && data.locales.length > 0) {
        availableLocales = data.locales
          .filter(locale => safeLocaleId(locale?.id) === locale?.id)
          .map(locale => ({
            id: locale.id,
            name: String(locale.name || locale.id),
          }));
      }
    } catch {
      // Keep bundled UI fallbacks if the editable locale folder is missing.
    }
  }

  async function loadLocale(id) {
    const localeId = safeLocaleId(id);
    const token = ++localeLoadToken;
    try {
      const data = await getJson(`/api/locales/${encodeURIComponent(localeId)}`);
      if (token !== localeLoadToken) return;
      localeTexts = { ...defaultTexts, ...normalizeLocaleData(data) };
    } catch {
      if (localeId !== defaultLocaleId) {
        await loadLocale(defaultLocaleId);
        return;
      }
      if (token === localeLoadToken) localeTexts = defaultTexts;
    }
  }

  onMount(() => {
    const cleanupState = init();
    loadAvailableLocales();

    fetch('/release.json', { cache: 'no-store' })
      .then(res => res.ok ? res.json() : null)
      .then(data => {
        if (data?.version) {
          releaseVersion = String(data.version);
          releaseDiag = data.diag === true;
        }
      })
      .catch(() => {});

    const verifyCredit = () => {
      const credit = document.querySelector('.credit-text');
      const text = credit?.textContent?.replace(/\s+/g, ' ').trim() ?? '';
      const href = credit instanceof HTMLAnchorElement
        ? credit.href.replace(/\/$/, '')
        : '';
      creditIntegrityWarning =
        text !== expectedCreditText || href !== expectedCreditHref;
    };

    verifyCredit();
    const observer = new MutationObserver(verifyCredit);
    observer.observe(document.body, {
      childList: true,
      subtree: true,
      characterData: true,
      attributes: true,
      attributeFilter: ['href', 'class'],
    });

    return () => {
      observer.disconnect();
      if (typeof cleanupState === 'function') cleanupState();
    };
  });
  onDestroy(() => {
    destroy();
    if (localQueueFeedbackTimer) clearTimeout(localQueueFeedbackTimer);
    if (radioSearchTimer) clearTimeout(radioSearchTimer);
  });

  $effect(() => {
    if (!showOptionsLabel) optionsOpen = false;
  });
  $effect(() => {
    loadLocale(activeLocale);
  });
  $effect(() => {
    document.title = t('document.title');
  });
  $effect(() => {
    if (annotatedErrors.length === 0) errorsCollapsed = true;
  });
  $effect(() => {
    const next = localPath || '';
    const focused = document.activeElement?.id;
    if (!savingLocal && focused !== 'local-path' && focused !== 'local-path-edit') {
      localPathInput = next;
    }
    if (!savingLocal) {
      localRecursiveInput = localRecursive;
      localShuffleInput = localShuffle;
    }
  });
  $effect(() => {
    if (!showSourceLabel) sourceMenuOpen = false;
  });
  $effect(() => {
    if (optimisticSource && s.activeSource === optimisticSource) {
      optimisticSource = null;
    }
  });
  $effect(() => {
    const nextEmptyKey = `${activeSource}|${localPath}|${local.trackCount}|${local.error || ''}`;
    if (activeSource === 'local' && !localReady && nextEmptyKey !== localEmptyAutoOpenKey) {
      localEmptyAutoOpenKey = nextEmptyKey;
      localPanelEmptyDismissed = false;
    }
  });
  $effect(() => {
    if (activeSource !== 'local') {
      localVolumeOpen = false;
      localPanelOpen = false;
      localPanelEmptyDismissed = false;
    } else if (sourceMenuOpen) {
      localPanelOpen = false;
    } else if (localReady) {
      localPanelEmptyDismissed = false;
    } else if (!localPanelOpen && !localPanelEmptyDismissed) {
      localPanelOpen = true;
      setTimeout(refreshLocalPanel, 0);
    }
  });
  $effect(() => {
    if (activeSource !== 'radio') {
      radioVolumeOpen = false;
      radioPanelOpen = false;
      radioPanelAutoOpenDismissed = false;
    } else if (sourceMenuOpen) {
      radioPanelOpen = false;
    } else if (!radio.connected && !radioPanelOpen && !radioPanelAutoOpenDismissed) {
      radioPanelOpen = true;
      setTimeout(refreshRadioPanel, 0);
    }
  });

  function buildOptionsPayload(overrides = {}) {
    return {
      menuPlayback,
      locale: activeLocale,
      raceStartPlayback,
      raceStartRestartThresholdSeconds: raceStartRestartThreshold,
      songStartOffsetEnabled,
      songStartOffsetSeconds,
      volumeNormalization,
      quickStationSkip,
      equalizerEnabled,
      equalizerBands,
      localMusicDir: localPath,
      localRecursive,
      localShuffle,
      localVolume,
      nightRunnersMode,
      nightRunnersStoppedVolumeDecrease,
      nightRunnersMaxSpeedMph,
      nightRunnersSpeedUnit,
      nightRunnersCurveEnabled,
      nightRunnersCurveExponent,
      nightRunnersLazyVolumeEnabled,
      nightRunnersLazyHoldSeconds,
      nightRunnersLowCutEnabled,
      nightRunnersFrequencyCutMode,
      nightRunnersLowCutAmount,
      nightRunnersLowCutFrequencyHz,
      nightRunnersNonDrivingVolumeEnabled,
      nightRunnersNonDrivingVolume,
      nightRunnersDynamicMode,
      nightRunnersDynamicThresholdMph,
      nightRunnersDynamicBufferPercent,
      nightRunnersDynamicBufferFillSeconds,
      nightRunnersDynamicIncreaseSeconds,
      nightRunnersDynamicDecreaseSeconds,
      nightRunnersDynamicMaxDecayMphS,
      radioLogoAlbumArtEnabled,
      radioLogoCustomGraphicEnabled,
      radioLogoSpotifyVariant,
      metadataTruncationEnabled,
      metadataTruncationLength,
      localTitleMetadataMode,
      localArtistMetadataMode,
      ...overrides,
    };
  }

  async function postOptions(payload) {
    const res = await fetch('/api/options', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!res.ok) throw new Error('save failed');
    const data = await res.json();
    bridgeState.update(current => ({ ...current, options: data }));
  }

  async function setLocale(value) {
    const next = safeLocaleId(value);
    if (next === activeLocale || savingOptions) return;
    pendingLocale = next;
    savingOptions = true;
    optionsError = '';
    try {
      await loadLocale(next);
      await postOptions(buildOptionsPayload({ locale: next }));
      pendingLocale = null;
    } catch {
      pendingLocale = null;
      optionsError = t('error.saveOptions');
      loadLocale(activeLocale);
    } finally {
      savingOptions = false;
    }
  }

  async function setMenuPlayback(value) {
    if (value === menuPlayback || savingOptions) return;
    pendingMenuPlayback = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ menuPlayback: value }));
      pendingMenuPlayback = null;
    } catch {
      pendingMenuPlayback = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setRaceStartPlayback(value) {
    if (value === raceStartPlayback || savingOptions) return;
    pendingRaceStartPlayback = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ raceStartPlayback: value }));
      pendingRaceStartPlayback = null;
    } catch {
      pendingRaceStartPlayback = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewRaceStartRestartThreshold(value) {
    pendingRaceStartRestartThreshold = clampRaceThreshold(value);
  }

  async function commitRaceStartRestartThreshold() {
    if (savingOptions) return;
    const next = clampRaceThreshold(raceStartRestartThreshold);
    pendingRaceStartRestartThreshold = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ raceStartRestartThresholdSeconds: next }));
      pendingRaceStartRestartThreshold = null;
    } catch {
      pendingRaceStartRestartThreshold = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setSongStartOffsetEnabled(value) {
    if (value === songStartOffsetEnabled || savingOptions) return;
    pendingSongStartOffsetEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ songStartOffsetEnabled: value }));
      pendingSongStartOffsetEnabled = null;
    } catch {
      pendingSongStartOffsetEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewSongStartOffsetSeconds(value) {
    pendingSongStartOffsetSeconds = clampSongStartOffset(value);
  }

  async function commitSongStartOffsetSeconds() {
    if (savingOptions) return;
    const next = clampSongStartOffset(songStartOffsetSeconds);
    pendingSongStartOffsetSeconds = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ songStartOffsetSeconds: next }));
      pendingSongStartOffsetSeconds = null;
    } catch {
      pendingSongStartOffsetSeconds = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setVolumeNormalization(value) {
    if (value === volumeNormalization || savingOptions) return;
    pendingVolumeNormalization = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ volumeNormalization: value }));
      pendingVolumeNormalization = null;
    } catch {
      pendingVolumeNormalization = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setQuickStationSkip(value) {
    if (value === quickStationSkip || savingOptions) return;
    pendingQuickStationSkip = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ quickStationSkip: value }));
      pendingQuickStationSkip = null;
    } catch {
      pendingQuickStationSkip = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersMode(value) {
    if (value === nightRunnersMode || savingOptions) return;
    pendingNightRunnersMode = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersMode: value }));
      pendingNightRunnersMode = null;
    } catch {
      pendingNightRunnersMode = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersStoppedDecrease(value) {
    pendingNightRunnersStoppedVolumeDecrease = clampPercent(value);
  }

  async function commitNightRunnersStoppedDecrease() {
    if (savingOptions) return;
    const next = clampPercent(nightRunnersStoppedVolumeDecrease);
    pendingNightRunnersStoppedVolumeDecrease = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersStoppedVolumeDecrease: next }));
      pendingNightRunnersStoppedVolumeDecrease = null;
    } catch {
      pendingNightRunnersStoppedVolumeDecrease = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersMaxSpeed(value) {
    pendingNightRunnersMaxSpeedMph = speedMphFromDisplay(value, nightRunnersSpeedUnit);
  }

  async function commitNightRunnersMaxSpeed() {
    if (savingOptions) return;
    const next = clampNightSpeedMph(nightRunnersMaxSpeedMph);
    pendingNightRunnersMaxSpeedMph = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersMaxSpeedMph: next }));
      pendingNightRunnersMaxSpeedMph = null;
    } catch {
      pendingNightRunnersMaxSpeedMph = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersSpeedUnit(value) {
    const next = normalizeSpeedUnit(value);
    if (next === nightRunnersSpeedUnit || savingOptions) return;
    pendingNightRunnersSpeedUnit = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersSpeedUnit: next }));
      pendingNightRunnersSpeedUnit = null;
    } catch {
      pendingNightRunnersSpeedUnit = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersCurveEnabled(value) {
    if (value === nightRunnersCurveEnabled || savingOptions) return;
    pendingNightRunnersCurveEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersCurveEnabled: value }));
      pendingNightRunnersCurveEnabled = null;
    } catch {
      pendingNightRunnersCurveEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersCurveExponent(value) {
    pendingNightRunnersCurveExponent = clampNightCurveExponent(value);
  }

  async function commitNightRunnersCurveExponent() {
    if (savingOptions) return;
    const next = clampNightCurveExponent(nightRunnersCurveExponent);
    pendingNightRunnersCurveExponent = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersCurveExponent: next }));
      pendingNightRunnersCurveExponent = null;
    } catch {
      pendingNightRunnersCurveExponent = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersLazyVolumeEnabled(value) {
    if (value === nightRunnersLazyVolumeEnabled || savingOptions) return;
    pendingNightRunnersLazyVolumeEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersLazyVolumeEnabled: value }));
      pendingNightRunnersLazyVolumeEnabled = null;
    } catch {
      pendingNightRunnersLazyVolumeEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersLazyHoldSeconds(value) {
    pendingNightRunnersLazyHoldSeconds = clampNightLazyHoldSeconds(value);
  }

  async function commitNightRunnersLazyHoldSeconds() {
    if (savingOptions) return;
    const next = clampNightLazyHoldSeconds(nightRunnersLazyHoldSeconds);
    pendingNightRunnersLazyHoldSeconds = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersLazyHoldSeconds: next }));
      pendingNightRunnersLazyHoldSeconds = null;
    } catch {
      pendingNightRunnersLazyHoldSeconds = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersLowCutEnabled(value) {
    if (value === nightRunnersLowCutEnabled || savingOptions) return;
    pendingNightRunnersLowCutEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersLowCutEnabled: value }));
      pendingNightRunnersLowCutEnabled = null;
    } catch {
      pendingNightRunnersLowCutEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersFrequencyCutMode(value) {
    const nextMode = normalizeFrequencyCutMode(value);
    if (nextMode === nightRunnersFrequencyCutMode || savingOptions) return;
    const currentMode = nightRunnersFrequencyCutMode;
    const nextFrequency = remapFrequencyCutHz(
      nightRunnersLowCutFrequencyHz,
      currentMode,
      nextMode
    );
    pendingNightRunnersFrequencyCutMode = nextMode;
    pendingNightRunnersLowCutFrequencyHz = nextFrequency;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({
        nightRunnersFrequencyCutMode: nextMode,
        nightRunnersLowCutFrequencyHz: nextFrequency,
      }));
      pendingNightRunnersFrequencyCutMode = null;
      pendingNightRunnersLowCutFrequencyHz = null;
    } catch {
      pendingNightRunnersFrequencyCutMode = null;
      pendingNightRunnersLowCutFrequencyHz = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersLowCutAmount(value) {
    pendingNightRunnersLowCutAmount = clampPercent(value);
  }

  async function commitNightRunnersLowCutAmount() {
    if (savingOptions) return;
    const next = clampPercent(nightRunnersLowCutAmount);
    pendingNightRunnersLowCutAmount = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersLowCutAmount: next }));
      pendingNightRunnersLowCutAmount = null;
    } catch {
      pendingNightRunnersLowCutAmount = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersLowCutFrequency(value) {
    pendingNightRunnersLowCutFrequencyHz =
      frequencyCutHzFromSlider(value, nightRunnersFrequencyCutMode);
  }

  async function commitNightRunnersLowCutFrequency() {
    if (savingOptions) return;
    const next = clampNightLowCutFrequency(
      nightRunnersLowCutFrequencyHz,
      nightRunnersFrequencyCutMode
    );
    pendingNightRunnersLowCutFrequencyHz = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersLowCutFrequencyHz: next }));
      pendingNightRunnersLowCutFrequencyHz = null;
    } catch {
      pendingNightRunnersLowCutFrequencyHz = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersNonDrivingVolumeEnabled(value) {
    if (value === nightRunnersNonDrivingVolumeEnabled || savingOptions) return;
    pendingNightRunnersNonDrivingVolumeEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersNonDrivingVolumeEnabled: value }));
      pendingNightRunnersNonDrivingVolumeEnabled = null;
    } catch {
      pendingNightRunnersNonDrivingVolumeEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersNonDrivingVolume(value) {
    pendingNightRunnersNonDrivingVolume = clampPercent(value);
  }

  async function commitNightRunnersNonDrivingVolume() {
    if (savingOptions) return;
    const next = clampPercent(nightRunnersNonDrivingVolume);
    pendingNightRunnersNonDrivingVolume = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersNonDrivingVolume: next }));
      pendingNightRunnersNonDrivingVolume = null;
    } catch {
      pendingNightRunnersNonDrivingVolume = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setNightRunnersDynamicMode(value) {
    if (value === nightRunnersDynamicMode || savingOptions) return;
    pendingNightRunnersDynamicMode = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicMode: value }));
      pendingNightRunnersDynamicMode = null;
    } catch {
      pendingNightRunnersDynamicMode = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicThreshold(value) {
    const mph = nightDynThresholdIsKmh ? Number(value) / mphToKmh : Number(value);
    pendingNightRunnersDynamicThresholdMph = clampNightDynThresholdMph(mph);
  }

  async function commitNightRunnersDynamicThreshold() {
    if (savingOptions) return;
    const next = clampNightDynThresholdMph(nightRunnersDynamicThresholdMph);
    pendingNightRunnersDynamicThresholdMph = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicThresholdMph: next }));
      pendingNightRunnersDynamicThresholdMph = null;
    } catch {
      pendingNightRunnersDynamicThresholdMph = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicBuffer(value) {
    pendingNightRunnersDynamicBufferPercent = clampNightDynBufferPercent(value);
  }

  async function commitNightRunnersDynamicBuffer() {
    if (savingOptions) return;
    const next = clampNightDynBufferPercent(nightRunnersDynamicBufferPercent);
    pendingNightRunnersDynamicBufferPercent = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicBufferPercent: next }));
      pendingNightRunnersDynamicBufferPercent = null;
    } catch {
      pendingNightRunnersDynamicBufferPercent = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicBufferFill(value) {
    pendingNightRunnersDynamicBufferFillSeconds = clampNightDynBufferFill(value);
  }

  async function commitNightRunnersDynamicBufferFill() {
    if (savingOptions) return;
    const next = clampNightDynBufferFill(nightRunnersDynamicBufferFillSeconds);
    pendingNightRunnersDynamicBufferFillSeconds = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicBufferFillSeconds: next }));
      pendingNightRunnersDynamicBufferFillSeconds = null;
    } catch {
      pendingNightRunnersDynamicBufferFillSeconds = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicIncrease(value) {
    pendingNightRunnersDynamicIncreaseSeconds =
      clampNightDynRamp(value, nightDynIncreaseDefault);
  }

  async function commitNightRunnersDynamicIncrease() {
    if (savingOptions) return;
    const next = clampNightDynRamp(nightRunnersDynamicIncreaseSeconds, nightDynIncreaseDefault);
    pendingNightRunnersDynamicIncreaseSeconds = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicIncreaseSeconds: next }));
      pendingNightRunnersDynamicIncreaseSeconds = null;
    } catch {
      pendingNightRunnersDynamicIncreaseSeconds = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicDecrease(value) {
    pendingNightRunnersDynamicDecreaseSeconds =
      clampNightDynRamp(value, nightDynDecreaseDefault);
  }

  async function commitNightRunnersDynamicDecrease() {
    if (savingOptions) return;
    const next = clampNightDynRamp(nightRunnersDynamicDecreaseSeconds, nightDynDecreaseDefault);
    pendingNightRunnersDynamicDecreaseSeconds = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicDecreaseSeconds: next }));
      pendingNightRunnersDynamicDecreaseSeconds = null;
    } catch {
      pendingNightRunnersDynamicDecreaseSeconds = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewNightRunnersDynamicMaxDecay(value) {
    const mph = nightDynThresholdIsKmh ? Number(value) / mphToKmh : Number(value);
    pendingNightRunnersDynamicMaxDecayMphS = clampNightDynMaxDecay(mph);
  }

  async function commitNightRunnersDynamicMaxDecay() {
    if (savingOptions) return;
    const next = clampNightDynMaxDecay(nightRunnersDynamicMaxDecayMphS);
    pendingNightRunnersDynamicMaxDecayMphS = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ nightRunnersDynamicMaxDecayMphS: next }));
      pendingNightRunnersDynamicMaxDecayMphS = null;
    } catch {
      pendingNightRunnersDynamicMaxDecayMphS = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setRadioLogoAlbumArtEnabled(value) {
    if (value === radioLogoAlbumArtEnabled || savingOptions) return;
    pendingRadioLogoAlbumArtEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ radioLogoAlbumArtEnabled: value }));
      pendingRadioLogoAlbumArtEnabled = null;
    } catch {
      pendingRadioLogoAlbumArtEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setRadioLogoCustomGraphicEnabled(value) {
    if (value === radioLogoCustomGraphicEnabled || savingOptions) return;
    pendingRadioLogoCustomGraphicEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ radioLogoCustomGraphicEnabled: value }));
      pendingRadioLogoCustomGraphicEnabled = null;
    } catch {
      pendingRadioLogoCustomGraphicEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setRadioLogoSpotifyVariant(value) {
    const next = value === 'color' ? 'color' : 'white';
    if (next === radioLogoSpotifyVariant || savingOptions) return;
    pendingRadioLogoSpotifyVariant = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ radioLogoSpotifyVariant: next }));
      pendingRadioLogoSpotifyVariant = null;
    } catch {
      pendingRadioLogoSpotifyVariant = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setMetadataTruncationEnabled(value) {
    if (value === metadataTruncationEnabled || savingOptions) return;
    pendingMetadataTruncationEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ metadataTruncationEnabled: value }));
      pendingMetadataTruncationEnabled = null;
    } catch {
      pendingMetadataTruncationEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewMetadataTruncationLength(value) {
    pendingMetadataTruncationLength = clampMetadataTruncationLength(value);
  }

  async function commitMetadataTruncationLength() {
    const next = clampMetadataTruncationLength(metadataTruncationLength);
    if (next === clampMetadataTruncationLength(options.metadataTruncationLength
                                              ?? metadataTruncationDefault) ||
        savingOptions) {
      pendingMetadataTruncationLength = null;
      return;
    }
    pendingMetadataTruncationLength = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ metadataTruncationLength: next }));
      pendingMetadataTruncationLength = null;
    } catch {
      pendingMetadataTruncationLength = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setLocalTitleMetadataMode(value) {
    const next = normalizeLocalTitleMetadataMode(value);
    if (next === localTitleMetadataMode || savingOptions) return;
    pendingLocalTitleMetadataMode = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ localTitleMetadataMode: next }));
      pendingLocalTitleMetadataMode = null;
    } catch {
      pendingLocalTitleMetadataMode = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setLocalArtistMetadataMode(value) {
    const next = normalizeLocalArtistMetadataMode(value);
    if (next === localArtistMetadataMode || savingOptions) return;
    pendingLocalArtistMetadataMode = next;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ localArtistMetadataMode: next }));
      pendingLocalArtistMetadataMode = null;
    } catch {
      pendingLocalArtistMetadataMode = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function setEqualizerEnabled(value) {
    if (value === equalizerEnabled || savingOptions) return;
    pendingEqualizerEnabled = value;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ equalizerEnabled: value }));
      pendingEqualizerEnabled = null;
    } catch {
      pendingEqualizerEnabled = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewEqualizerBand(index, value) {
    const next = [...equalizerBands];
    next[index] = clampEq(value);
    pendingEqualizerBands = next;
  }

  async function commitEqualizerBands() {
    if (savingOptions) return;
    const next = normalizedEqBands(equalizerBands);
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ equalizerBands: next }));
      pendingEqualizerBands = null;
    } catch {
      pendingEqualizerBands = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  async function resetEqualizer() {
    if (savingOptions) return;
    const flat = [0, 0, 0, 0, 0];
    pendingEqualizerBands = flat;
    savingOptions = true;
    optionsError = '';
    try {
      await postOptions(buildOptionsPayload({ equalizerBands: flat }));
      pendingEqualizerBands = null;
    } catch {
      pendingEqualizerBands = null;
      optionsError = t('error.saveOptions');
    } finally {
      savingOptions = false;
    }
  }

  function previewLocalVolume(value) {
    pendingLocalVolume = clampLocalVolume(value);
  }

  async function commitLocalVolume() {
    if (savingOptions) return;
    const next = clampLocalVolume(localVolume);
    pendingLocalVolume = next;
    savingOptions = true;
    localError = '';
    try {
      await postOptions(buildOptionsPayload({ localVolume: next }));
      pendingLocalVolume = null;
    } catch {
      pendingLocalVolume = null;
      localError = t('error.saveVolume');
    } finally {
      savingOptions = false;
    }
  }

  async function postJson(url, payload = {}, timeoutMs = 0) {
    const controller = timeoutMs > 0 ? new AbortController() : null;
    const timer = controller
      ? setTimeout(() => controller.abort(), timeoutMs)
      : null;
    let res;
    try {
      res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        signal: controller?.signal,
      });
    } finally {
      if (timer) clearTimeout(timer);
    }
    if (!res.ok) throw new Error('request failed');
    return await res.json().catch(() => ({}));
  }

  async function postJsonReliable(url, payload = {}) {
    const res = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (!res.ok) throw new Error('request failed');
    return await res.json().catch(() => ({}));
  }

  async function getJson(url) {
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error('request failed');
    return await res.json().catch(() => ({}));
  }

  // Start the Spotify OAuth login: ask the bridge for the authorize URL and
  // open it. On this PC the loopback redirect auto-completes; on a phone the
  // user pastes the code into the follow-up field. Success flips
  // spotify.connected via SSE, which unmounts this whole prompt.
  async function startSpotifyOAuth() {
    if (oauthBusy) return;
    oauthError = '';
    oauthBusy = true;
    try {
      const res = await fetch('/api/spotify/oauth/start', { method: 'POST' });
      if (!res.ok) throw new Error('http ' + res.status);
      const data = await res.json();
      if (!data.authUrl) throw new Error('no url');
      window.open(data.authUrl, '_blank', 'noopener');
    } catch {
      oauthError = t('oauth.failed');
    } finally {
      oauthBusy = false;
    }
  }

  async function refreshLocalPanel() {
    if (localPanelLoading) return;
    localPanelLoading = true;
    localPanelError = '';
    try {
      const [library, queue] = await Promise.all([
        getJson('/api/source/local/library'),
        getJson('/api/source/local/queue'),
      ]);
      localLibrary = Array.isArray(library?.tracks) ? library.tracks : [];
      localManualQueue = Array.isArray(queue?.manual) ? queue.manual : [];
      localAutoplayQueue = Array.isArray(queue?.autoplay) ? queue.autoplay : [];
    } catch {
      localPanelError = t('error.loadLocalFiles');
    } finally {
      localPanelLoading = false;
    }
  }

  function applyQueueResponse(data) {
    if (Array.isArray(data?.manual)) localManualQueue = data.manual;
    if (Array.isArray(data?.autoplay)) localAutoplayQueue = data.autoplay;
  }

  function markLocalQueueAdded(key) {
    localQueueFeedbackKey = key;
    if (localQueueFeedbackTimer) clearTimeout(localQueueFeedbackTimer);
    localQueueFeedbackTimer = setTimeout(() => {
      if (localQueueFeedbackKey === key) localQueueFeedbackKey = '';
      localQueueFeedbackTimer = null;
    }, 900);
  }

  function isCurrentLocalTrack(item) {
    if (!item?.path || activeSource !== 'local') return false;
    return track?.uri === `local:${item.path}`;
  }

  function toggleSourceMenu() {
    const nextOpen = !sourceMenuOpen;
    sourceMenuOpen = nextOpen;
    if (nextOpen) {
      localPanelOpen = false;
      localVolumeOpen = false;
      radioVolumeOpen = false;
      radioPanelOpen = false;
    }
  }

  function closeSourceMenuOnOutsideClick(event) {
    const target = event.target;
    if (!(target instanceof Element)) return;
    if (sourceMenuOpen &&
        !target.closest('.source-label') &&
        !target.closest('.source-menu')) {
      sourceMenuOpen = false;
    }
    if (localVolumeOpen && !target.closest('.local-volume-control')) {
      localVolumeOpen = false;
    }
    if (radioVolumeOpen && !target.closest('.radio-volume-control')) {
      radioVolumeOpen = false;
    }
  }

  function toggleLocalPanel() {
    localPathInput = localPath;
    localRecursiveInput = localRecursive;
    localShuffleInput = localShuffle;
    const nextOpen = !localPanelOpen;
    localPanelOpen = nextOpen;
    if (nextOpen) sourceMenuOpen = false;
    localPanelEmptyDismissed = !nextOpen && !localReady;
    if (localPanelOpen) refreshLocalPanel();
  }

  function toggleRadioPanel() {
    const nextOpen = !radioPanelOpen;
    radioPanelOpen = nextOpen;
    if (!nextOpen) radioPanelAutoOpenDismissed = true;
    if (nextOpen) {
      sourceMenuOpen = false;
      radioPanelAutoOpenDismissed = false;
      refreshRadioPanel();
    }
  }

  async function switchSource(source) {
    sourceMenuOpen = false;
    if (sourceSwitching || source === activeSource) {
      return;
    }
    const previousSource = activeSource;
    optimisticSource = source;
    sourceSwitching = true;
    localError = '';
    try {
      await postJsonReliable('/api/source/switch', { source });
    } catch (error) {
      optimisticSource = previousSource;
      localError = t('error.switchSource');
    } finally {
      sourceSwitching = false;
    }
  }

  async function loadRadioManual() {
    try {
      const data = await getJson('/api/source/radio/manual');
      radioManualStations = Array.isArray(data?.stations) ? data.stations : [];
      if (!radioManualPrefilled && !radioManualUrl.trim() && !radioManualName.trim()) {
        radioManualPrefilled = true;
        radioManualUrl = typeof data?.lastUrl === 'string' ? data.lastUrl : '';
        radioManualName = typeof data?.lastName === 'string' ? data.lastName : '';
      }
    } catch {
      // Manual list is non-critical; keep whatever is shown.
    }
  }

  async function refreshRadioPanel() {
    if (radioLoading) return;
    radioLoading = true;
    radioError = '';
    try {
      const [favorites, suggestions] = await Promise.all([
        getJson('/api/source/radio/favorites'),
        getJson('/api/source/radio/suggestions?kind=top&limit=24'),
        loadRadioManual(),
      ]);
      radioFavorites = Array.isArray(favorites?.stations) ? favorites.stations : [];
      radioSuggestions = Array.isArray(suggestions?.stations) ? suggestions.stations : [];
      if (radioPanelTab === 'discover' && !radioSearchInput.trim()) {
        radioStations = radioSuggestions;
      }
      if (radioPanelTab === 'favorites') radioStations = radioFavorites;
      if (suggestions?.error) radioError = suggestions.error;
    } catch {
      radioError = t('error.loadRadioStations');
    } finally {
      radioLoading = false;
    }
  }

  async function searchRadioStations() {
    if (radioLoading) return;
    const q = radioSearchInput.trim();
    if (!q) {
      radioStations = radioSuggestions;
      return;
    }
    radioLoading = true;
    radioError = '';
    try {
      const data = await getJson(`/api/source/radio/search?q=${encodeURIComponent(q)}&limit=30`);
      radioStations = Array.isArray(data?.stations) ? data.stations : [];
      if (data?.error) radioError = data.error;
    } catch {
      radioError = t('error.loadRadioStations');
    } finally {
      radioLoading = false;
    }
  }

  function scheduleRadioSearch(value) {
    radioSearchInput = value;
    radioPanelTab = 'discover';
    if (radioSearchTimer) clearTimeout(radioSearchTimer);
    radioSearchTimer = setTimeout(() => {
      radioSearchTimer = null;
      searchRadioStations();
    }, 280);
  }

  function radioStationSubtitle(station) {
    const parts = [];
    if (station?.countryCode) parts.push(station.countryCode);
    if (station?.language) parts.push(station.language);
    if (station?.codec) parts.push(String(station.codec).toUpperCase());
    if (Number(station?.bitrate) > 0) parts.push(`${station.bitrate} kbps`);
    return parts.join(' · ');
  }

  function radioStationTags(station) {
    const tags = String(station?.tags || '').split(',').map(v => v.trim()).filter(Boolean);
    return tags.slice(0, 3).join(' · ');
  }

  function isRadioFavorite(station) {
    return radioFavorites.some(item => item.id === station?.id);
  }

  function isCurrentRadioStation(station) {
    return Boolean(station?.id && radio.stationId && station.id === radio.stationId);
  }

  async function playRadioStation(station) {
    radioError = '';
    try {
      await postJsonReliable('/api/source/radio/play', { station });
      await refreshRadioPanel();
    } catch {
      radioError = t('error.radioCommand');
    }
  }

  async function radioAction(action) {
    radioError = '';
    try {
      const data = await postJsonReliable(`/api/source/radio/${action}`);
      if (data?.error) radioError = data.error;
    } catch {
      radioError = t('error.radioCommand');
    }
  }

  async function toggleRadioFavorite(station) {
    radioError = '';
    try {
      if (isRadioFavorite(station)) {
        const data = await fetch(`/api/source/radio/favorites/${station.id}`, {
          method: 'DELETE',
        });
        if (!data.ok) throw new Error('request failed');
        const json = await data.json().catch(() => ({}));
        radioFavorites = Array.isArray(json?.stations) ? json.stations : [];
      } else {
        const data = await postJsonReliable('/api/source/radio/favorites', station);
        radioFavorites = Array.isArray(data?.stations) ? data.stations : [];
      }
      if (radioPanelTab === 'favorites') radioStations = radioFavorites;
    } catch {
      radioError = t('error.favoriteStation');
    }
  }

  async function playManualRadio() {
    const url = radioManualUrl.trim();
    if (!url) return;
    radioError = '';
    try {
      await postJsonReliable('/api/source/radio/play-url', {
        url,
        name: radioManualName.trim(),
      });
      await refreshRadioPanel();
    } catch {
      radioError = t('error.radioCommand');
    }
  }

  async function removeManualStation(station) {
    if (!station?.id) return;
    radioError = '';
    try {
      const res = await fetch(`/api/source/radio/manual/${encodeURIComponent(station.id)}`, {
        method: 'DELETE',
      });
      if (!res.ok) throw new Error('request failed');
      const json = await res.json().catch(() => ({}));
      radioManualStations = Array.isArray(json?.stations) ? json.stations : [];
    } catch {
      radioError = t('error.radioCommand');
    }
  }

  async function stopRadioPlayback() {
    radioError = '';
    try {
      const data = await postJsonReliable('/api/source/radio/stop');
      if (data?.error) radioError = data.error;
    } catch {
      radioError = t('error.radioCommand');
    }
  }

  async function saveLocalLibrary(overrides = {}) {
    if (savingLocal) return;
    savingLocal = true;
    localError = '';
    const payload = {
      musicDir: localPathInput.trim(),
      recursive: localRecursiveInput,
      shuffle: localShuffleInput,
      ...overrides,
    };
    try {
      const data = await postJsonReliable('/api/source/local/rescan', payload);
      if (data?.error) localError = data.error;
      await refreshLocalPanel();
    } catch {
      localError = t('error.scanFolder');
    } finally {
      savingLocal = false;
    }
  }

  async function browseLocalFolder() {
    if (savingLocal) return;
    savingLocal = true;
    localError = '';
    try {
      const data = await postJsonReliable('/api/source/local/browse', {
        initialDir: localPathInput || localPath || local.defaultMusicDir || '',
      });
      if (data?.cancelled) return;
      if (data?.musicDir) localPathInput = data.musicDir;
      if (data?.error) localError = data.error;
      await refreshLocalPanel();
    } catch {
      localError = t('error.openFolderPicker');
    } finally {
      savingLocal = false;
    }
  }

  async function localAction(action, payload = {}) {
    if (savingLocal || !localReady) return;
    localError = '';
    try {
      const data = await postJsonReliable(`/api/source/local/${action}`, payload);
      if (data?.error) localError = data.error;
      if (action === 'next' || action === 'previous' || action === 'play') {
        await refreshLocalPanel();
      }
    } catch {
      localError = t('error.localCommand');
    }
  }

  async function playLocalTrack(index) {
    localError = '';
    try {
      const data = await postJsonReliable('/api/source/local/play-index', { index });
      applyQueueResponse(data);
    } catch {
      localError = t('error.playFile');
    }
  }

  async function queueLocalTrack(index) {
    localPanelError = '';
    try {
      const data = await postJsonReliable('/api/source/local/queue-add', { index });
      applyQueueResponse(data);
      markLocalQueueAdded(`track:${index}`);
    } catch {
      localPanelError = t('error.addFile');
    }
  }

  async function queueLocalFolder(folder) {
    localPanelError = '';
    try {
      const data = await postJsonReliable('/api/source/local/queue-folder', { folder });
      applyQueueResponse(data);
      markLocalQueueAdded(`folder:${folder}`);
    } catch {
      localPanelError = t('error.addFolder');
    }
  }

  async function removeLocalQueueEntry(id) {
    localPanelError = '';
    try {
      const data = await postJsonReliable('/api/source/local/queue-remove', { id });
      applyQueueResponse(data);
    } catch {
      localPanelError = t('error.removeItem');
    }
  }

  function formatTime(ms) {
    const total = Math.max(0, Math.floor(Number(ms || 0) / 1000));
    const minutes = Math.floor(total / 60);
    const seconds = String(total % 60).padStart(2, '0');
    return `${minutes}:${seconds}`;
  }

</script>

<svelte:window onpointerdown={closeSourceMenuOnOutsideClick} />

<div class="app" style={`--error-offset: ${errorOffset}px`}>
  <div class="backdrop-car" aria-hidden="true"></div>
  {#if releaseVersion}
    <div class="version-label" aria-label={t('aria.version', { version: releaseVersion })}>
      v{releaseVersion}{releaseDiag ? '-diag' : ''}
    </div>
  {/if}
  <button class="source-label" class:visible={showSourceLabel}
          class:opening={sourceMenuOpen}
          type="button" disabled={!showSourceLabel}
          aria-label={t('aria.audioSource')}
          onclick={toggleSourceMenu}>
    {#if activeSource === 'vanilla'}
      <span class="material-icon source-local-icon" aria-hidden="true">graphic_eq</span>
    {:else if activeSource === 'local'}
      <span class="material-icon source-local-icon" aria-hidden="true">folder</span>
    {:else if activeSource === 'radio'}
      <span class="material-icon source-local-icon" aria-hidden="true">radio</span>
    {:else if activeSource === 'qqmusic'}
      <span class="material-icon source-local-icon" aria-hidden="true">music_note</span>
    {:else if activeSource === 'airplay'}
      <svg class="source-airplay-icon" viewBox="0 0 24 24" aria-hidden="true">
        <rect x="4" y="5" width="16" height="10" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/>
        <path d="M8.5 20h7L12 15.8 8.5 20z" fill="currentColor"/>
      </svg>
    {:else}
      <svg class="source-spotify-icon" viewBox="0 0 496 512" aria-hidden="true">
        <path fill="currentColor" d="M248 8C111.1 8 0 119.1 0 256s111.1 248 248 248 248-111.1 248-248S384.9 8 248 8zm114.6 365.9c-4.2 6.8-13 8.9-19.8 4.7-54.3-33.2-122.7-40.7-203.2-22.3-7.8 1.8-15.5-3.1-17.3-10.8-1.8-7.8 3.1-15.5 10.8-17.3 88.1-20.1 163.8-11.3 224.8 26 6.8 4.2 8.9 13.1 4.7 19.7zm30.6-68.1c-5.3 8.5-16.5 11.1-25 5.8-62.2-38.2-157-49.3-230.6-27-9.6 2.9-19.7-2.5-22.6-12.1-2.9-9.6 2.5-19.7 12.1-22.6 84.1-25.5 188.6-13.1 260.3 30.9 8.5 5.3 11.1 16.5 5.8 25zm2.6-70.9c-74.6-44.3-197.6-48.4-268.8-26.8-11.5 3.5-23.7-3-27.2-14.5-3.5-11.5 3-23.7 14.5-27.2 81.8-24.8 218-19.9 303.8 31 10.4 6.2 13.8 19.6 7.6 30-6.1 10.3-19.6 13.7-29.9 7.5z"/>
      </svg>
    {/if}
    <span class="material-icon source-arrow" aria-hidden="true">{sourceMenuOpen ? 'chevron_left' : 'chevron_right'}</span>
  </button>
  {#if sourceMenuOpen && showSourceLabel}
    <aside class="source-menu" transition:fly={{ x: -18, duration: 140 }} aria-label={t('aria.audioSource')}>
      <button type="button" class="source-vanilla" class:active={activeSource === 'vanilla'}
              disabled={sourceSwitching}
              onclick={() => switchSource('vanilla')}>
        <span class="material-icon" aria-hidden="true">graphic_eq</span>
        <span>
          <strong>{t('source.vanilla')}</strong>
          <small>{activeSource === 'vanilla' ? t('source.vanillaActive') : t('source.inactive')}</small>
        </span>
      </button>
      <button type="button" class:active={activeSource === 'spotify'}
              disabled={sourceSwitching}
              onclick={() => switchSource('spotify')}>
        <svg class="source-spotify-icon" viewBox="0 0 496 512" aria-hidden="true">
          <path fill="currentColor" d="M248 8C111.1 8 0 119.1 0 256s111.1 248 248 248 248-111.1 248-248S384.9 8 248 8zm114.6 365.9c-4.2 6.8-13 8.9-19.8 4.7-54.3-33.2-122.7-40.7-203.2-22.3-7.8 1.8-15.5-3.1-17.3-10.8-1.8-7.8 3.1-15.5 10.8-17.3 88.1-20.1 163.8-11.3 224.8 26 6.8 4.2 8.9 13.1 4.7 19.7zm30.6-68.1c-5.3 8.5-16.5 11.1-25 5.8-62.2-38.2-157-49.3-230.6-27-9.6 2.9-19.7-2.5-22.6-12.1-2.9-9.6 2.5-19.7 12.1-22.6 84.1-25.5 188.6-13.1 260.3 30.9 8.5 5.3 11.1 16.5 5.8 25zm2.6-70.9c-74.6-44.3-197.6-48.4-268.8-26.8-11.5 3.5-23.7-3-27.2-14.5-3.5-11.5 3-23.7 14.5-27.2 81.8-24.8 218-19.9 303.8 31 10.4 6.2 13.8 19.6 7.6 30-6.1 10.3-19.6 13.7-29.9 7.5z"/>
        </svg>
        <span>
          <strong>{t('source.spotify')}</strong>
          <small>{activeSource === 'spotify'
            ? (spotify.connected ? t('source.connected') : t('source.discoverable'))
            : t('source.inactive')}</small>
        </span>
      </button>
      <button type="button" class:active={activeSource === 'airplay'}
              disabled={sourceSwitching}
              onclick={() => switchSource('airplay')}>
        <svg class="source-airplay-icon" viewBox="0 0 24 24" aria-hidden="true">
          <rect x="4" y="5" width="16" height="10" rx="2" fill="none" stroke="currentColor" stroke-width="1.8"/>
          <path d="M8.5 20h7L12 15.8 8.5 20z" fill="currentColor"/>
        </svg>
        <span>
          <strong>{t('source.airplay')}</strong>
          <small>{activeSource === 'airplay'
            ? (airplay.connected ? t('source.connected') : t('source.discoverable'))
            : t('source.inactive')}</small>
        </span>
      </button>
      <button type="button" class:active={activeSource === 'radio'}
              disabled={sourceSwitching}
              onclick={() => switchSource('radio')}>
        <span class="material-icon" aria-hidden="true">radio</span>
        <span>
          <strong>{t('source.radio')}</strong>
          <small>{activeSource === 'radio'
            ? (radio.connected ? t('source.connected') : t('radio.waiting'))
            : t('source.inactive')}</small>
        </span>
      </button>
      <button type="button" class:active={activeSource === 'local'}
              disabled={sourceSwitching}
              onclick={() => switchSource('local')}>
        <span class="material-icon" aria-hidden="true">folder</span>
        <span>
          <strong>{t('source.local')}</strong>
          <small>{local.trackCount > 0 ? tracksLabel(local.trackCount) : t('source.chooseFolder')}</small>
        </span>
      </button>
      <button type="button" class:active={activeSource === 'qqmusic'}
              disabled={sourceSwitching}
              onclick={() => switchSource('qqmusic')}>
        <span class="material-icon" aria-hidden="true">music_note</span>
        <span>
          <strong>{t('source.qqmusic')}</strong>
          <small>{activeSource === 'qqmusic'
            ? ($bridgeState.sources.qqmusic?.connected
                ? t('source.connected')
                : t('source.discoverable'))
            : t('source.inactive')}</small>
        </span>
      </button>
    </aside>
  {/if}
  {#if status.text}
    <div class="status-line {status.tone}">
      <span class="status-text">{status.text}</span>
    </div>
  {/if}
  {#if localPanelOpen && activeSource === 'local'}
    <aside class="local-panel" transition:fly={{ x: -28, duration: 160 }} aria-label={t('aria.localFiles')}>
      <div class="options-group local-panel-head">
        <div class="folder-picker">
          <div class="folder-path">
            <strong>{localPathInput || t('local.noFolderSelected')}</strong>
          </div>
          <button class="folder-scan" type="button" aria-label={t('aria.scanFolder')}
                  title={t('aria.scanFolder')}
                  disabled={savingLocal || !localPathInput.trim()}
                  onclick={() => saveLocalLibrary()}>
            <span class="material-icon" aria-hidden="true">refresh</span>
          </button>
          <button type="button" aria-label={t('aria.chooseFolder')}
                  disabled={savingLocal} onclick={browseLocalFolder}>
            <span class="material-icon" aria-hidden="true">folder_open</span>
          </button>
        </div>
        <div class="local-panel-toggles">
          <button type="button" class:active={localRecursiveInput}
                  disabled={savingLocal || !localPathInput.trim()}
                  onclick={() => {
                    localRecursiveInput = !localRecursiveInput;
                    saveLocalLibrary({ recursive: localRecursiveInput });
                  }}>
            <span class="material-icon" aria-hidden="true">account_tree</span>
            <span>{t('local.subfolders')}</span>
          </button>
          <button type="button" class:active={localShuffleInput}
                  disabled={savingLocal || !localPathInput.trim()}
                  onclick={() => {
                    localShuffleInput = !localShuffleInput;
                    saveLocalLibrary({ shuffle: localShuffleInput });
                  }}>
            <span class="material-icon" aria-hidden="true">shuffle</span>
            <span>{t('local.shuffle')}</span>
          </button>
        </div>
        {#if localPanelVisibleError}
          <div class="local-error">{localPanelVisibleError}</div>
        {/if}
      </div>

      <div class="local-panel-scroll-frame">
        <div class="local-panel-scroll">
          {#if localPanelTab === 'browser'}
            <div class="local-panel-list" aria-label={t('aria.localFileBrowser')}>
              {#if localPanelLoading}
                <div class="local-empty">{t('local.loadingFiles')}</div>
              {:else if localFolderGroups.length === 0}
                <div class="local-empty">{t('local.noCompatibleFiles', { formats: localSupportedFormats })}</div>
              {:else}
                {#each localFolderGroups as group}
                  <section class="local-folder-group">
                    <div class="local-folder-head">
                      <span>{group.folder}</span>
                      {#if localRecursive && group.folder !== t('local.musicFolder')}
                        <button type="button" title={t('local.addFolderToQueue')}
                                class:queued={localQueueFeedbackKey === `folder:${group.folder}`}
                                disabled={localQueueFeedbackKey === `folder:${group.folder}`}
                                aria-label={t('aria.addFolderToQueue', { folder: group.folder })}
                                onclick={() => queueLocalFolder(group.folder)}>
                          <span class="material-icon" aria-hidden="true">
                            {localQueueFeedbackKey === `folder:${group.folder}` ? 'check' : 'playlist_add'}
                          </span>
                        </button>
                      {/if}
                    </div>
                    {#each group.tracks as item}
                      <div class="local-track-row" class:playing={isCurrentLocalTrack(item)}>
                        <div class="local-track-text">
                          <strong>{item.title}</strong>
                          <span>{item.folder || localPathInput || t('local.musicFolder')}</span>
                        </div>
                        <div class="local-track-actions">
                          <button type="button" title={t('local.playNow')}
                                  aria-label={t('aria.playTrack', { title: item.title })}
                                  onclick={() => playLocalTrack(item.index)}>
                            <span class="material-icon" aria-hidden="true">play_arrow</span>
                          </button>
                          <button type="button" title={t('local.addToQueue')}
                                  class:queued={localQueueFeedbackKey === `track:${item.index}`}
                                  disabled={localQueueFeedbackKey === `track:${item.index}`}
                                  aria-label={t('aria.addTrackToQueue', { title: item.title })}
                                  onclick={() => queueLocalTrack(item.index)}>
                            <span class="material-icon" aria-hidden="true">
                              {localQueueFeedbackKey === `track:${item.index}` ? 'check' : 'playlist_add'}
                            </span>
                          </button>
                        </div>
                      </div>
                    {/each}
                  </section>
                {/each}
              {/if}
            </div>
          {:else}
            <div class="local-panel-list" aria-label={t('aria.localQueue')}>
              {#if localManualQueue.length > 0}
                <section class="local-folder-group">
                  {#each localManualQueue as entry}
                    <div class="local-track-row">
                      <div class="local-track-text">
                        <strong>{entry.track.title}</strong>
                        <span>{entry.track.folder || t('local.musicFolder')}</span>
                      </div>
                      <div class="local-track-actions">
                        <button type="button" title={t('local.remove')}
                                aria-label={t('aria.removeTrack', { title: entry.track.title })}
                                onclick={() => removeLocalQueueEntry(entry.id)}>
                          <span class="material-icon" aria-hidden="true">close</span>
                        </button>
                      </div>
                    </div>
                  {/each}
                </section>
              {/if}
              {#if localAutoplayQueue.length > 0}
                <section class="local-folder-group">
                  <div class="local-folder-head"><span>{t('local.autoplayQueue')}</span></div>
                  {#each localAutoplayQueue as item}
                    <div class="local-track-row autoplay">
                      <div class="local-track-text">
                        <strong>{item.title}</strong>
                        <span>{item.folder || t('local.musicFolder')}</span>
                      </div>
                    </div>
                  {/each}
                </section>
              {/if}
            </div>
          {/if}
        </div>
      </div>
      <div class="local-panel-tabs" aria-label={t('aria.localPanelTabs')}>
        <button type="button" class:active={localPanelTab === 'browser'}
                onclick={() => localPanelTab = 'browser'}>
          <span class="material-icon" aria-hidden="true">library_music</span>
          <span>{t('local.files')}</span>
        </button>
        <button type="button" class:active={localPanelTab === 'queue'}
                onclick={() => {
                  localPanelTab = 'queue';
                  refreshLocalPanel();
                }}>
          <span class="material-icon" aria-hidden="true">queue_music</span>
          <span>{t('local.queue')}</span>
        </button>
      </div>
    </aside>
  {/if}
  {#if radioPanelOpen && activeSource === 'radio'}
    <aside class="local-panel radio-panel" transition:fly={{ x: -28, duration: 160 }} aria-label={t('aria.onlineRadio')}>
      {#snippet radioStationRow(station, removable = false)}
        <div class="local-track-row radio-station-row" class:playing={isCurrentRadioStation(station)}>
          <div class="radio-station-logo">
            {#if station.favicon}
              <img src={station.favicon} alt="" loading="lazy"
                   onerror={(event) => {
                     event.currentTarget.style.display = 'none';
                     const fallback = event.currentTarget.nextElementSibling;
                     if (fallback instanceof HTMLElement) fallback.hidden = false;
                   }} />
              <span class="material-icon" aria-hidden="true" hidden>radio</span>
            {:else}
              <span class="material-icon" aria-hidden="true">radio</span>
            {/if}
          </div>
          <div class="local-track-text">
            <strong>{station.name}</strong>
            <span>{radioStationSubtitle(station) || radioStationTags(station) || t('radio.live')}</span>
          </div>
          <div class="local-track-actions">
            {#if isCurrentRadioStation(station) && radio.playing}
              <button type="button" title={t('radio.stop')}
                      aria-label={t('aria.stopStation', { station: station.name })}
                      onclick={stopRadioPlayback}>
                <span class="material-icon" aria-hidden="true">stop</span>
              </button>
            {:else}
              <button type="button" title={t('radio.playNow')}
                      aria-label={t('aria.playStation', { station: station.name })}
                      onclick={() => playRadioStation(station)}>
                <span class="material-icon" aria-hidden="true">play_arrow</span>
              </button>
            {/if}
            <button type="button"
                    title={isRadioFavorite(station) ? t('radio.removeFavorite') : t('radio.addFavorite')}
                    aria-label={isRadioFavorite(station)
                      ? t('aria.unfavoriteStation', { station: station.name })
                      : t('aria.favoriteStation', { station: station.name })}
                    class:queued={isRadioFavorite(station)}
                    onclick={() => toggleRadioFavorite(station)}>
              <span class="material-icon" aria-hidden="true">
                {isRadioFavorite(station) ? 'favorite' : 'favorite_border'}
              </span>
            </button>
            {#if removable}
              <button type="button"
                      title={t('radio.removeSaved')}
                      aria-label={t('aria.removeSavedStation', { station: station.name })}
                      onclick={() => removeManualStation(station)}>
                <span class="material-icon" aria-hidden="true">delete</span>
              </button>
            {/if}
          </div>
        </div>
      {/snippet}
      <div class="radio-panel-search">
        <div class="folder-picker radio-search">
          <div class="folder-path">
            <input type="search"
                   value={radioSearchInput}
                   placeholder={t('radio.searchPlaceholder')}
                   aria-label={t('radio.search')}
                   oninput={(event) => scheduleRadioSearch(event.currentTarget.value)} />
          </div>
          <button class="folder-scan" type="button" aria-label={t('radio.search')}
                  title={t('radio.search')}
                  disabled={radioLoading}
                  onclick={searchRadioStations}>
            <span class="material-icon" aria-hidden="true">search</span>
          </button>
          <button type="button" aria-label={t('radio.topStations')}
                  disabled={radioLoading}
                  onclick={refreshRadioPanel}>
            <span class="material-icon" aria-hidden="true">refresh</span>
          </button>
        </div>
        {#if radioPanelVisibleError}
          <div class="local-error">{radioPanelVisibleError}</div>
        {/if}
      </div>
      <div class="local-panel-scroll">
        {#if radioPanelTab === 'manual'}
          <div class="options-group radio-manual-form">
            <label class="radio-manual-field">
              <span>{t('radio.manualUrl')}</span>
              <input type="url" bind:value={radioManualUrl} placeholder="https://..." />
            </label>
            <label class="radio-manual-field">
              <span>{t('radio.manualName')}</span>
              <input type="text" bind:value={radioManualName} />
            </label>
            <button class="folder-scan radio-manual-play" type="button"
                    disabled={radioLoading || !radioManualUrl.trim()}
                    onclick={playManualRadio}>
              <span class="material-icon" aria-hidden="true">play_arrow</span>
              <span>{t('radio.playUrl')}</span>
            </button>
          </div>
          {#if radioManualStations.length > 0}
            <div class="local-panel-list" aria-label={t('aria.radioBrowser')}>
              <section class="local-folder-group">
                <div class="local-folder-head"><span>{t('radio.savedUrls')}</span></div>
                {#each radioManualStations as station}
                  {@render radioStationRow(station, true)}
                {/each}
              </section>
            </div>
          {/if}
        {:else}
          <div class="local-panel-list" aria-label={t('aria.radioBrowser')}>
            {#if radioLoading}
              <div class="local-empty">{t('radio.loadingStations')}</div>
            {:else if (radioPanelTab === 'favorites' ? radioFavorites : radioStations).length === 0}
              <div class="local-empty">{t('radio.noStations')}</div>
            {:else}
              <section class="local-folder-group">
                {#if radioPanelTab === 'discover' && !radioSearchInput.trim()}
                  <div class="local-folder-head"><span>{t('radio.topStations')}</span></div>
                {/if}
                {#each (radioPanelTab === 'favorites' ? radioFavorites : radioStations) as station}
                  {@render radioStationRow(station)}
                {/each}
              </section>
            {/if}
          </div>
        {/if}
      </div>
      <div class="local-panel-tabs" aria-label={t('aria.radioPanelTabs')}>
        <button type="button" class:active={radioPanelTab === 'discover'}
                title={t('radio.discover')}
                aria-label={t('radio.discover')}
                onclick={() => {
                  radioPanelTab = 'discover';
                  radioStations = radioSearchInput.trim() ? radioStations : radioSuggestions;
                  refreshRadioPanel();
                }}>
          <span class="material-icon" aria-hidden="true">travel_explore</span>
        </button>
        <button type="button" class:active={radioPanelTab === 'favorites'}
                title={t('radio.favorites')}
                aria-label={t('radio.favorites')}
                onclick={() => {
                  radioPanelTab = 'favorites';
                  radioStations = radioFavorites;
                  refreshRadioPanel();
                }}>
          <span class="material-icon" aria-hidden="true">favorite</span>
        </button>
        <button type="button" class:active={radioPanelTab === 'recent'}
                title={t('radio.recent')}
                aria-label={t('radio.recent')}
                onclick={async () => {
                  radioPanelTab = 'recent';
                  radioLoading = true;
                  radioError = '';
                  try {
                    const data = await getJson('/api/source/radio/suggestions?kind=recent&limit=24');
                    radioStations = Array.isArray(data?.stations) ? data.stations : [];
                    if (data?.error) radioError = data.error;
                  } catch {
                    radioError = t('error.loadRadioStations');
                  } finally {
                    radioLoading = false;
                  }
                }}>
          <span class="material-icon" aria-hidden="true">history</span>
        </button>
        <button type="button" class:active={radioPanelTab === 'manual'}
                title={t('radio.manual')}
                aria-label={t('radio.manual')}
                onclick={() => {
                  radioPanelTab = 'manual';
                  loadRadioManual();
                }}>
          <span class="material-icon" aria-hidden="true">link</span>
        </button>
      </div>
    </aside>
  {/if}
  <main class="content">
    <div class="stack">
      {#if activeSource === 'vanilla'}
        <div class="hero">
          <span class="brand-logo game-title">{gameTitle}</span>
        </div>
      {:else if activeSource !== 'local'}
        <div class="hero">
          <span class="brand-logo game-title">{gameTitle}</span>
          <span class="link-icon material-icon">link</span>
          {#if activeSource === 'airplay'}
            <svg class="brand-logo airplay-logo" viewBox="0 0 72 72" aria-label="AirPlay">
              <rect x="10" y="12" width="52" height="34" rx="6" fill="none" stroke="currentColor" stroke-width="5"/>
              <path d="M22 62h28L36 45 22 62z" fill="currentColor"/>
            </svg>
          {:else if activeSource === 'radio'}
            <span class="brand-logo radio-logo material-icon" aria-label={t('source.radio')}>radio</span>
          {:else}
            <svg class="brand-logo spotify-logo" viewBox="0 0 496 512" xmlns="http://www.w3.org/2000/svg" aria-label="Spotify">
              <path fill="#1DB954" d="M248 8C111.1 8 0 119.1 0 256s111.1 248 248 248 248-111.1 248-248S384.9 8 248 8z"/>
              <path fill="#191414" d="M406.6 231.1c-5.2 0-8.4-1.3-12.9-3.9-71.2-42.5-198.5-52.7-280.9-29.7-3.6 1-8.1 2.6-12.9 2.6-13.2 0-23.3-10.3-23.3-23.6 0-13.6 8.4-21.3 17.4-23.9 35.2-10.3 74.6-15.2 117.5-15.2 73 0 149.5 15.2 205.4 47.8 7.8 4.5 12.9 10.7 12.9 22.6 0 13.6-11 23.3-23.2 23.3zm-31 76.2c-5.2 0-8.7-2.3-12.3-4.2-62.5-37-155.7-51.9-238.6-29.4-4.8 1.3-7.4 2.6-11.9 2.6-10.7 0-19.4-8.7-19.4-19.4s5.2-17.8 15.5-20.7c27.8-7.8 56.2-13.6 97.8-13.6 64.9 0 127.6 16.1 177 45.5 8.1 4.8 11.3 11 11.3 19.7-.1 10.8-8.5 19.5-19.4 19.5zm-26.9 65.6c-4.2 0-6.8-1.3-10.7-3.6-62.4-37.6-135-39.2-206.7-24.5-3.9 1-9.4 2.6-11.9 2.6-9.7 0-15.8-7.7-15.8-15.8 0-10.3 6.1-15.2 13.6-16.8 81.9-18.1 165.6-16.5 237 26.2 6.1 3.9 9.7 7.4 9.7 16.5s-7.1 15.4-15.2 15.4z"/>
            </svg>
          {/if}
        </div>
      {/if}

      <!-- Active source widget: keep one compact center surface. -->
      {#if !online}
        <!-- intentionally empty -->
      {:else if activeSource === 'vanilla'}
        <!-- Vanilla Streamer Mode: no playback info, just the Forza logo above. -->
      {:else if activeSource === 'airplay'}
        {#if airplayIcon === 'connect'}
          <div class="np np-prompt">
            <div class="np-icon">
              <span class="material-icon icon-away" aria-label={t('aria.connectAirPlay')}>airplay</span>
            </div>
            <div class="np-text">
              <div class="np-title prompt-title">{t('prompt.selectAirPlay', { device: airplay.deviceName || 'FH6 Radio' })}</div>
              <div class="np-artist">{t('prompt.fromAirPlay')}</div>
            </div>
          </div>
        {:else}
          <div class="np airplay-widget">
            <div class="np-icon">
              {#if airplayIcon === 'eq'}
                <div class="eq" aria-label={t('aria.airplayPlaying')}>
                  <span class="bar b1"></span>
                  <span class="bar b2"></span>
                  <span class="bar b3"></span>
                </div>
              {:else}
                <span class="material-icon icon-pause" aria-label={t('aria.airplayPaused')}>pause_circle</span>
              {/if}
            </div>
            <div class="np-text">
              <div class="np-title">{track.title || t('source.airplay')}</div>
              <div class="np-artist">{track.artist || (airplay.connected ? t('source.connected') : t('prompt.waitingAirPlay'))}</div>
            </div>
          </div>
        {/if}
      {:else if activeSource === 'local'}
        <div class="local-widget">
          <div class="local-player">
            <div class="np">
              <div class="np-icon">
                {#if localReady && local.playing}
                  <div class="eq" aria-label={t('aria.localFilesPlaying')}>
                    <span class="bar b1"></span>
                    <span class="bar b2"></span>
                    <span class="bar b3"></span>
                  </div>
                {:else if localReady}
                  <span class="material-icon icon-pause" aria-label={t('aria.localFilesPaused')}>pause_circle</span>
                {:else}
                  <span class="material-icon icon-away icon-folder" aria-label={t('aria.chooseLocalMusicFolder')}>folder_open</span>
                {/if}
              </div>
              <div class="np-text">
                <div class="np-title">{track.title || t('source.local')}</div>
                <div class="np-artist">
                  {track.artist || (localReady ? tracksLabel(local.trackCount) : t('local.chooseMusicFolder'))}
                </div>
              </div>
            </div>

            <div class="local-progress" class:disabled={!localReady || localDurationMs <= 0}>
              <span>{formatTime(localReady ? localPositionMs : 0)}</span>
              <button type="button" class="progress-track" aria-label={t('aria.seek')}
                      disabled={!localReady || localDurationMs <= 0}
                      onclick={(event) => {
                        if (!localReady || localDurationMs <= 0) return;
                        const rect = event.currentTarget.getBoundingClientRect();
                        const pct = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
                        localAction('seek', { positionMs: Math.round(localDurationMs * pct) });
                      }}>
                <span style={`width: ${localReady ? localProgress : 0}%`}></span>
              </button>
              <span>{formatTime(localReady ? localDurationMs : 0)}</span>
            </div>

            <div class="transport">
              <button type="button" aria-label={t('aria.previous')} disabled={!localReady || savingLocal}
                      onclick={() => localAction('previous')}>
                <span class="material-icon" aria-hidden="true">skip_previous</span>
              </button>
              <button type="button" class="play-button" aria-label={local.playing ? t('aria.pause') : t('aria.play')}
                      disabled={!localReady || savingLocal}
                      onclick={() => localAction(local.playing ? 'pause' : 'play')}>
                <span class="material-icon" aria-hidden="true">{local.playing ? 'pause' : 'play_arrow'}</span>
              </button>
              <button type="button" aria-label={t('aria.next')} disabled={!localReady || savingLocal}
                      onclick={() => localAction('next')}>
                <span class="material-icon" aria-hidden="true">skip_next</span>
              </button>
            </div>

            <div class="local-secondary">
              <button type="button" class:active={localPanelOpen}
                      aria-label={t('aria.localFilesFolder')}
                      onclick={toggleLocalPanel}>
                <span class="material-icon" aria-hidden="true">folder_open</span>
              </button>
              <button type="button" class:active={localRecursiveInput}
                      aria-label={t('aria.includeSubfolders')}
                      disabled={savingLocal || !localPathInput.trim()}
                      onclick={() => {
                        localRecursiveInput = !localRecursiveInput;
                        saveLocalLibrary({ recursive: localRecursiveInput });
                      }}>
                <span class="material-icon" aria-hidden="true">account_tree</span>
              </button>
              <button type="button" class:active={localShuffleInput}
                      aria-label={t('aria.shuffleLocalFiles')}
                      disabled={savingLocal || !localPathInput.trim()}
                      onclick={() => {
                        localShuffleInput = !localShuffleInput;
                        saveLocalLibrary({ shuffle: localShuffleInput });
                      }}>
                <span class="material-icon" aria-hidden="true">shuffle</span>
              </button>
              <div class="volume-control local-volume-control" class:open={localVolumeOpen}>
                {#if localVolumeOpen}
                  <div class="volume-popover">
                    <input class="volume-slider-vertical"
                           type="range" min="0" max={localVolumeMax} step="1"
                           value={localVolume}
                           disabled={savingOptions}
                           aria-label={t('aria.localFilesVolume')}
                           oninput={(event) => previewLocalVolume(event.currentTarget.value)}
                           onchange={commitLocalVolume} />
                    <span>{localVolume}%</span>
                  </div>
                {/if}
                <button type="button"
                        class="volume-button"
                        aria-label={t('aria.localFilesVolume')}
                        aria-expanded={localVolumeOpen}
                        disabled={savingOptions}
                        onclick={() => localVolumeOpen = !localVolumeOpen}>
                  <span class="material-icon" aria-hidden="true">{localVolumeIcon}</span>
                </button>
              </div>
            </div>
          </div>
        </div>
      {:else if activeSource === 'radio'}
        <div class="local-widget radio-widget">
          <div class="local-player">
            <div class="np">
              <div class="np-icon">
                {#if radio.playing}
                  <div class="eq" aria-label={t('radio.live')}>
                    <span class="bar b1"></span>
                    <span class="bar b2"></span>
                    <span class="bar b3"></span>
                  </div>
                {:else}
                  <span class="material-icon icon-away" aria-label={t('aria.onlineRadio')}>radio</span>
                {/if}
              </div>
              <div class="np-text">
                <div class="np-title">{radio.stationName || t('source.radio')}</div>
              </div>
            </div>
            {#if radioPanelVisibleError}
              <div class="local-error">{radioPanelVisibleError}</div>
            {/if}
            <div class="local-secondary">
              <button type="button" class:active={radioPanelOpen}
                      aria-label={t('aria.onlineRadio')}
                      onclick={toggleRadioPanel}>
                <span class="material-icon" aria-hidden="true">travel_explore</span>
              </button>
              <div class="volume-control radio-volume-control" class:open={radioVolumeOpen}>
                {#if radioVolumeOpen}
                  <div class="volume-popover">
                    <input class="volume-slider-vertical"
                           type="range" min="0" max={localVolumeMax} step="1"
                           value={localVolume}
                           disabled={savingOptions}
                           aria-label={t('options.trackVolume')}
                           oninput={(event) => previewLocalVolume(event.currentTarget.value)}
                           onchange={commitLocalVolume} />
                    <span>{localVolume}%</span>
                  </div>
                {/if}
                <button type="button"
                        class="volume-button"
                        aria-label={t('options.trackVolume')}
                        aria-expanded={radioVolumeOpen}
                        disabled={savingOptions}
                        onclick={() => radioVolumeOpen = !radioVolumeOpen}>
                  <span class="material-icon" aria-hidden="true">{localVolumeIcon}</span>
                </button>
              </div>
            </div>
          </div>
        </div>
      {:else}
        {#if spotifyIcon === 'connect'}
          <div class="np np-prompt">
            <div class="np-icon">
              <span class="material-icon icon-away" aria-label={t('aria.connectSpotify')}>devices_other</span>
            </div>
            <div class="np-text">
              <div class="np-title prompt-title">{t('prompt.connectSpotify')}</div>
              <div class="np-artist">{t('prompt.inSpotifyApp')}</div>
            </div>
          </div>
          {#if !spotifyAuthed}
            <div class="spotify-oauth">
              <div class="oauth-or">{t('oauth.or')}</div>
              <button type="button" class="oauth-btn"
                      aria-label={t('aria.loginSpotify')}
                      onclick={startSpotifyOAuth} disabled={oauthBusy}>
                <span class="material-icon" aria-hidden="true">login</span>
                <span>{oauthBusy ? t('oauth.opening') : t('oauth.login')}</span>
              </button>
              {#if oauthError}<div class="oauth-error">{oauthError}</div>{/if}
            </div>
          {/if}
        {:else}
          <div class="np">
            <div class="np-icon">
              {#if spotifyIcon === 'eq'}
                <div class="eq" aria-label={t('aria.spotifyPlaying')}>
                  <span class="bar b1"></span>
                  <span class="bar b2"></span>
                  <span class="bar b3"></span>
                </div>
              {:else if spotifyIcon === 'pause'}
                <span class="material-icon icon-pause" aria-label={t('aria.spotifyPaused')}>pause_circle</span>
              {:else}
                <span class="material-icon icon-away" aria-label={t('aria.playingElsewhere')}>devices_other</span>
              {/if}
            </div>
            {#if track.title}
              <div class="np-text">
                <div class="np-title">{track.title}</div>
                {#if track.artist}<div class="np-artist">{track.artist}</div>{/if}
              </div>
            {:else if spotifyIcon === 'away'}
              <div class="np-text">
                <div class="np-title prompt-title">{t('prompt.playingElsewhere')}</div>
                <div class="np-artist">{t('prompt.selectSpotify')}</div>
              </div>
            {/if}
          </div>
        {/if}
      {/if}
    </div>
    {#if showDrivingIndicator}
      <div class="speed-indicator" class:has-dynamic={showDynamicBar}
           aria-label={t('aria.drivingSpeed')}>
        <div class="speed-readout">{drivingSpeedLabel}</div>
        <div class="speed-bar" aria-hidden="true">
          <span style={`width: ${drivingVolumeFill}%`}></span>
        </div>
        {#if showDynamicBar}
          <div class={`speed-state-bar ${dynStateClass}`} aria-hidden="true">
            <div class="speed-state-track">
              <span class="speed-state-fill" style={`width: ${dynSpeedPos}%`}></span>
              <span class="speed-state-buffer"
                    style={`left: ${dynBufferLeftPos}%; width: ${dynBufferWidth}%`}></span>
            </div>
            <span class="speed-state-peak" style={`left: ${dynPeakPos}%`}></span>
            <span class="speed-state-threshold" style={`left: ${dynThresholdPos}%`}></span>
          </div>
        {/if}
      </div>
    {/if}
  </main>

  <footer class="credit">
    <a class="discord-link" href="https://discord.gg/NZT9uNdYbM" target="_blank"
       rel="noopener noreferrer" aria-label={t('aria.joinDiscord')}>
      <svg class="discord-icon" viewBox="0 0 64 48" aria-hidden="true">
        <path d="M40.575 0c-.619 1.099-1.174 2.235-1.68 3.397a48.87 48.87 0 0 0-14.497 0A34.128 34.128 0 0 0 22.719 0c-4.509.77-8.903 2.122-13.071 4.028C1.39 16.265-.846 28.186.266 39.943c4.836 3.574 10.254 6.302 16.025 8.044a34.455 34.455 0 0 0 3.435-5.531 31.244 31.244 0 0 1-5.405-2.576c.454-.328.897-.669 1.326-.998 10.14 4.774 21.885 4.774 32.038 0 .429.354.871.695 1.326.998a31.167 31.167 0 0 1-5.418 2.589A34.48 34.48 0 0 0 47.028 48c5.771-1.743 11.189-4.458 16.025-8.032 1.314-13.638-2.247-25.458-9.408-35.927C49.491 2.134 45.096.783 40.588.025L40.575 0ZM21.14 32.707c-3.119 0-5.708-2.828-5.708-6.327 0-3.498 2.488-6.339 5.696-6.339 3.207 0 5.758 2.854 5.707 6.339-.05 3.486-2.513 6.327-5.695 6.327Zm21.039 0c-3.132 0-5.696-2.828-5.696-6.327 0-3.498 2.488-6.339 5.696-6.339 3.207 0 5.758 2.854 5.695 6.339-.051 3.486-2.513 6.327-5.695 6.327Z"/>
      </svg>
    </a>
    <a class="credit-text" href="https://ko-fi.com/big_john" target="_blank" rel="noopener noreferrer">
      Made by Big John
    </a>
    {#if creditIntegrityWarning || bridgeCreditIntegrityWarning}
      <div class="credit-warning">{t('footer.creditWarning')}</div>
    {/if}
  </footer>

  <button class="options-label" class:visible={showOptionsLabel} class:opening={optionsOpen}
          type="button" disabled={!showOptionsLabel || optionsOpen} aria-label={t('aria.options')}
          onclick={() => optionsOpen = true}>
    <span class="material-icon" aria-hidden="true">settings</span>
  </button>

  {#if optionsOpen && showOptionsLabel}
    <aside class="options-panel" transition:fly={{ x: 28, duration: 160 }} aria-label={t('aria.options')}>
      <div class="options-scroll">
        <div class="options-group">
          <label class="locale-row">
            <span class="options-title">{t('options.language')}</span>
            <select value={activeLocale}
                    disabled={savingOptions}
                    onchange={(event) => setLocale(event.currentTarget.value)}>
              {#each availableLocales as locale}
                <option value={locale.id}>{locale.name}</option>
              {/each}
            </select>
          </label>
        </div>
        {#if activeSource !== 'radio'}
        <div class="options-group">
          <div class="options-title">{t('options.menuPlayback')}</div>
          <div class="segmented" aria-label={t('aria.menuPlaybackBehavior')}>
            <button class:active={menuPlayback === 'pause'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setMenuPlayback('pause')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">pause_circle</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.pauseMenus')}</span>
                <span class="choice-desc">{t('options.pauseMenusDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
            <button class:active={menuPlayback === 'silent'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setMenuPlayback('silent')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">volume_off</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.silentMenus')}</span>
                <span class="choice-desc">{t('options.silentMenusDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
          </div>
          {#if optionsError}
            <div class="options-error">{optionsError}</div>
          {/if}
        </div>
        <div class="options-group">
          <div class="options-title">{t('options.raceStart')}</div>
          <div class="segmented" aria-label={t('aria.raceStartBehavior')}>
            <button class:active={raceStartPlayback === 'next'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setRaceStartPlayback('next')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">skip_next</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.nextSong')}</span>
                <span class="choice-desc">{t('options.nextSongDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
            {#if activeSourceCanRestart}
              <button class:active={raceStartPlayback === 'restart'} type="button"
                      aria-disabled={savingOptions}
                      onclick={() => setRaceStartPlayback('restart')}>
                <span class="choice-glass" aria-hidden="true"></span>
                <span class="choice-icon material-icon" aria-hidden="true">replay</span>
                <span class="choice-text">
                  <span class="choice-label">{t('options.restartSong')}</span>
                  <span class="choice-desc">{t('options.restartSongDesc')}</span>
                </span>
                <span class="choice-dot" aria-hidden="true"></span>
              </button>
            {/if}
            <button class:active={raceStartPlayback === 'ignore'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setRaceStartPlayback('ignore')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">play_circle</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.keepPlaying')}</span>
                <span class="choice-desc">{t('options.keepPlayingDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
            {#if activeSourceCanSmartRace}
              <button class:active={raceStartPlayback === 'smart'} type="button"
                      aria-disabled={savingOptions}
                      onclick={() => setRaceStartPlayback('smart')}>
                <span class="choice-glass" aria-hidden="true"></span>
                <span class="choice-icon material-icon" aria-hidden="true">auto_mode</span>
                <span class="choice-text">
                  <span class="choice-label">{t('options.smartRace')}</span>
                  <span class="choice-desc">
                    {activeSource === 'airplay'
                      ? t('options.smartRaceAirplayDesc')
                      : t('options.smartRaceDesc')}
                  </span>
                </span>
                <span class="choice-dot" aria-hidden="true"></span>
              </button>
            {/if}
          </div>
          {#if raceStartPlayback === 'smart' && activeSourceCanSmartRace}
            <div class="curve-editor">
              <label class="night-row">
                <span class="night-label">
                  {activeSource === 'airplay'
                    ? t('options.raceRestartAfterThreshold')
                    : t('options.raceRestartThreshold')}
                </span>
                <input type="range"
                       min="5"
                       max="60"
                       step="1"
                       value={raceStartRestartThreshold}
                       disabled={savingOptions}
                       aria-label={t('aria.raceRestartThreshold')}
                       oninput={(event) => previewRaceStartRestartThreshold(event.currentTarget.value)}
                       onchange={commitRaceStartRestartThreshold} />
                <span class="night-value">{raceStartRestartThreshold}s</span>
              </label>
            </div>
          {/if}
        </div>
        <div class="options-group">
          <div class="options-title">{t('options.songStartOffset')}</div>
          <div class="night-panel" aria-label={t('options.songStartOffset')}>
            <span class="choice-glass" aria-hidden="true"></span>
            <div class="night-panel-head">
              <span class="choice-icon material-icon" aria-hidden="true">av_timer</span>
              <div class="choice-text">
                <span class="choice-label">{t('options.songStartOffset')}</span>
                <span class="choice-desc">
                  {!activeSourceCanSeek
                    ? t('options.songStartOffsetAirplay')
                    : t('options.songStartOffsetDesc')}
                </span>
              </div>
              <button class="eq-toggle" class:active={songStartOffsetEnabled} type="button"
                      aria-pressed={songStartOffsetEnabled}
                      aria-disabled={savingOptions || !activeSourceCanSeek}
                      disabled={!activeSourceCanSeek}
                      onclick={() => setSongStartOffsetEnabled(!songStartOffsetEnabled)}>
                {toggleLabel(songStartOffsetEnabled)}
              </button>
            </div>
            {#if songStartOffsetEnabled && activeSourceCanSeek}
              <div class="night-controls">
                <label class="night-row">
                  <span class="night-label">{t('options.songStartOffsetTime')}</span>
                  <input type="range" min="3" max="240" step="1"
                         value={songStartOffsetSeconds}
                         disabled={savingOptions}
                         aria-label={t('options.songStartOffsetTime')}
                         oninput={(event) => previewSongStartOffsetSeconds(event.currentTarget.value)}
                         onchange={commitSongStartOffsetSeconds} />
                  <span class="night-value">{songStartOffsetSeconds}s</span>
                </label>
              </div>
            {/if}
          </div>
        </div>
        <div class="options-group">
          <div class="options-title">{t('options.stationSwitching')}</div>
          <div class="segmented" aria-label={t('aria.stationSwitchingBehavior')}>
            <button class:active={!quickStationSkip} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setQuickStationSkip(false)}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">radio</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.normalSwitching')}</span>
                <span class="choice-desc">{t('options.normalSwitchingDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
            <button class:active={quickStationSkip} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setQuickStationSkip(true)}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">skip_next</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.skipSong')}</span>
                <span class="choice-desc">{t('options.skipSongDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
          </div>
        </div>
        {/if}
        <div class="options-group">
          <div class="options-title">{t('options.radioLogo')}</div>
          <div class="logo-panel" aria-label={t('aria.radioLogoBehavior')}>
            <div class="logo-toggle-row">
              <span class="choice-icon material-icon" aria-hidden="true">album</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.albumArtLogo')}</span>
                <span class="choice-desc">{t('options.albumArtLogoDesc')}</span>
              </span>
              <button class="logo-toggle" class:active={radioLogoAlbumArtEnabled} type="button"
                      aria-pressed={radioLogoAlbumArtEnabled}
                      aria-disabled={savingOptions}
                      onclick={() => setRadioLogoAlbumArtEnabled(!radioLogoAlbumArtEnabled)}>
                {toggleLabel(radioLogoAlbumArtEnabled)}
              </button>
            </div>
            <div class="logo-toggle-row">
              <span class="choice-icon material-icon" aria-hidden="true">image</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.customLogo')}</span>
                <span class="choice-desc">{t('options.customLogoDesc')}</span>
                <span class="choice-help">{t('options.customLogoHelp')}</span>
              </span>
              <button class="logo-toggle" class:active={radioLogoCustomGraphicEnabled} type="button"
                      aria-pressed={radioLogoCustomGraphicEnabled}
                      aria-disabled={savingOptions}
                      onclick={() => setRadioLogoCustomGraphicEnabled(!radioLogoCustomGraphicEnabled)}>
                {toggleLabel(radioLogoCustomGraphicEnabled)}
              </button>
            </div>
            <div class="logo-variant-head">
              <span class="choice-icon material-icon" aria-hidden="true">palette</span>
              <span class="choice-label">{t('options.spotifyLogoColor')}</span>
            </div>
            <div class="segmented logo-variant" aria-label={t('options.spotifyLogoColor')}>
              <button class:active={radioLogoSpotifyVariant === 'white'} type="button"
                      aria-disabled={savingOptions}
                      onclick={() => setRadioLogoSpotifyVariant('white')}>
                <span class="choice-glass" aria-hidden="true"></span>
                <span class="choice-icon material-icon" aria-hidden="true">radio_button_unchecked</span>
                <span class="choice-text">
                  <span class="choice-label">{t('options.spotifyLogoWhite')}</span>
                  <span class="choice-desc">{t('options.spotifyLogoWhiteDesc')}</span>
                </span>
                <span class="choice-dot" aria-hidden="true"></span>
              </button>
              <button class:active={radioLogoSpotifyVariant === 'color'} type="button"
                      aria-disabled={savingOptions}
                      onclick={() => setRadioLogoSpotifyVariant('color')}>
                <span class="choice-glass" aria-hidden="true"></span>
                <span class="choice-icon material-icon" aria-hidden="true">palette</span>
                <span class="choice-text">
                  <span class="choice-label">{t('options.spotifyLogoColored')}</span>
                  <span class="choice-desc">{t('options.spotifyLogoColoredDesc')}</span>
                </span>
                <span class="choice-dot" aria-hidden="true"></span>
              </button>
            </div>
          </div>
        </div>
        <div class="options-group">
          <div class="options-title">{t('options.metadataDisplay')}</div>
          <div class="logo-panel" aria-label={t('aria.metadataDisplay')}>
            <div class="logo-toggle-row">
              <span class="choice-icon material-icon" aria-hidden="true">short_text</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.truncateMetadata')}</span>
                <span class="choice-desc">{t('options.truncateMetadataDesc')}</span>
              </span>
              <button class="logo-toggle" class:active={metadataTruncationEnabled} type="button"
                      aria-pressed={metadataTruncationEnabled}
                      aria-disabled={savingOptions}
                      onclick={() => setMetadataTruncationEnabled(!metadataTruncationEnabled)}>
                {toggleLabel(metadataTruncationEnabled)}
              </button>
            </div>
            <label class="night-row metadata-length-row"
                   class:disabled={!metadataTruncationEnabled}>
              <span class="night-label">{t('options.truncateMetadataLength')}</span>
              <input type="range"
                     min={metadataTruncationMin}
                     max={metadataTruncationMax}
                     step="1"
                     value={metadataTruncationLength}
                     disabled={!metadataTruncationEnabled || savingOptions}
                     aria-label={t('aria.metadataTruncationLength')}
                     oninput={(event) => previewMetadataTruncationLength(event.currentTarget.value)}
                     onchange={commitMetadataTruncationLength} />
              <span class="night-value">{metadataTruncationLength}</span>
            </label>
            {#if activeSource === 'local'}
              <div class="logo-variant-head">
                <span class="choice-icon material-icon" aria-hidden="true">title</span>
                <span class="choice-label">{t('options.localTitleMetadata')}</span>
              </div>
              <div class="segmented logo-variant" aria-label={t('aria.localTitleMetadata')}>
                <button class:active={localTitleMetadataMode === 'metadata'} type="button"
                        aria-disabled={savingOptions}
                        onclick={() => setLocalTitleMetadataMode('metadata')}>
                  <span class="choice-glass" aria-hidden="true"></span>
                  <span class="choice-icon material-icon" aria-hidden="true">music_note</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.localTitleTrackTitle')}</span>
                    <span class="choice-desc">{t('options.localTitleTrackTitleDesc')}</span>
                  </span>
                  <span class="choice-dot" aria-hidden="true"></span>
                </button>
                <button class:active={localTitleMetadataMode === 'filename'} type="button"
                        aria-disabled={savingOptions}
                        onclick={() => setLocalTitleMetadataMode('filename')}>
                  <span class="choice-glass" aria-hidden="true"></span>
                  <span class="choice-icon material-icon" aria-hidden="true">description</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.localTitleFilename')}</span>
                    <span class="choice-desc">{t('options.localTitleFilenameDesc')}</span>
                  </span>
                  <span class="choice-dot" aria-hidden="true"></span>
                </button>
              </div>

              <div class="logo-variant-head">
                <span class="choice-icon material-icon" aria-hidden="true">person</span>
                <span class="choice-label">{t('options.localArtistMetadata')}</span>
              </div>
              <div class="segmented logo-variant local-artist-variant"
                   aria-label={t('aria.localArtistMetadata')}>
                <button class:active={localArtistMetadataMode === 'albumArtist'} type="button"
                        aria-disabled={savingOptions}
                        onclick={() => setLocalArtistMetadataMode('albumArtist')}>
                  <span class="choice-glass" aria-hidden="true"></span>
                  <span class="choice-icon material-icon" aria-hidden="true">groups</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.localArtistAlbumArtist')}</span>
                    <span class="choice-desc">{t('options.localArtistAlbumArtistDesc')}</span>
                  </span>
                  <span class="choice-dot" aria-hidden="true"></span>
                </button>
                <button class:active={localArtistMetadataMode === 'folder'} type="button"
                        aria-disabled={savingOptions}
                        onclick={() => setLocalArtistMetadataMode('folder')}>
                  <span class="choice-glass" aria-hidden="true"></span>
                  <span class="choice-icon material-icon" aria-hidden="true">folder</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.localArtistFolder')}</span>
                    <span class="choice-desc">{t('options.localArtistFolderDesc')}</span>
                  </span>
                  <span class="choice-dot" aria-hidden="true"></span>
                </button>
                <button class:active={localArtistMetadataMode === 'album'} type="button"
                        aria-disabled={savingOptions}
                        onclick={() => setLocalArtistMetadataMode('album')}>
                  <span class="choice-glass" aria-hidden="true"></span>
                  <span class="choice-icon material-icon" aria-hidden="true">album</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.localArtistAlbum')}</span>
                    <span class="choice-desc">{t('options.localArtistAlbumDesc')}</span>
                  </span>
                  <span class="choice-dot" aria-hidden="true"></span>
                </button>
              </div>
            {/if}
          </div>
        </div>
        {#if activeSource !== 'radio'}
        <div class="options-group">
          <div class="options-title">{t('options.trackVolume')}</div>
          <div class="segmented" aria-label={t('aria.trackVolumeBehavior')}>
            <button class:active={volumeNormalization === 'on'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setVolumeNormalization('on')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">equalizer</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.normalizeVolume')}</span>
                <span class="choice-desc">{t('options.normalizeVolumeDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
            <button class:active={volumeNormalization === 'off'} type="button"
                    aria-disabled={savingOptions}
                    onclick={() => setVolumeNormalization('off')}>
              <span class="choice-glass" aria-hidden="true"></span>
              <span class="choice-icon material-icon" aria-hidden="true">album</span>
              <span class="choice-text">
                <span class="choice-label">{t('options.originalVolume')}</span>
                <span class="choice-desc">{t('options.originalVolumeDesc')}</span>
              </span>
              <span class="choice-dot" aria-hidden="true"></span>
            </button>
          </div>
        </div>
        {/if}
        <div class="options-group">
          <div class="options-title">{t('options.drivingVolume')}</div>
          <div class="night-panel" aria-label={t('aria.nightRunnersMode')}>
            <span class="choice-glass" aria-hidden="true"></span>
            <div class="night-panel-head">
              <span class="choice-icon material-icon" aria-hidden="true">speed</span>
              <div class="choice-text">
                <span class="choice-label">{t('options.nightRunnersMode')}</span>
                <span class="choice-desc">{t('options.nightRunnersDesc')}</span>
              </div>
              <button class="eq-toggle" class:active={nightRunnersMode} type="button"
                      aria-pressed={nightRunnersMode}
                      aria-disabled={savingOptions}
                      onclick={() => setNightRunnersMode(!nightRunnersMode)}>
                {toggleLabel(nightRunnersMode)}
              </button>
            </div>
            {#if nightRunnersMode}
              <div class="night-controls">
                <label class="night-row">
                  <span class="night-label">{t('options.stoppedReduction')}</span>
                  <input type="range" min="0" max="100" step="1"
                         value={nightRunnersStoppedVolumeDecrease}
                         disabled={savingOptions}
                         aria-label={t('aria.stoppedReduction')}
                         oninput={(event) => previewNightRunnersStoppedDecrease(event.currentTarget.value)}
                         onchange={commitNightRunnersStoppedDecrease} />
                  <span class="night-value">{nightRunnersStoppedVolumeDecrease}%</span>
                </label>
                <div class="curve-toggle-row">
                  <span class="choice-icon material-icon" aria-hidden="true">auto_graph</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.dynamicMode')}</span>
                    <span class="choice-desc">{t('options.dynamicModeDesc')}</span>
                  </span>
                  <button class="eq-toggle" class:active={nightRunnersDynamicMode} type="button"
                          aria-pressed={nightRunnersDynamicMode}
                          aria-disabled={savingOptions}
                          onclick={() => setNightRunnersDynamicMode(!nightRunnersDynamicMode)}>
                    {toggleLabel(nightRunnersDynamicMode)}
                  </button>
                </div>
                {#if nightRunnersDynamicMode}
                  <div class="curve-editor">
                    <div class="night-row speed-row">
                      <span class="night-label">{t('options.dynamicThreshold')}</span>
                      <input type="range"
                             min={nightDynThresholdDisplayMin}
                             max={nightDynThresholdDisplayMax}
                             step="1"
                             value={nightDynThresholdDisplayValue}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicThreshold')}
                             oninput={(event) => previewNightRunnersDynamicThreshold(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicThreshold} />
                      <span class="night-value">
                        {nightDynThresholdDisplayValue} {speedUnitLabel(nightRunnersSpeedUnit)}
                      </span>
                      <div class="unit-toggle" aria-label={t('aria.speedUnit')}>
                        <button type="button" class:active={nightRunnersSpeedUnit === 'mph'}
                                aria-disabled={savingOptions}
                                onclick={() => setNightRunnersSpeedUnit('mph')}>mph</button>
                        <button type="button" class:active={nightRunnersSpeedUnit === 'kmh'}
                                aria-disabled={savingOptions}
                                onclick={() => setNightRunnersSpeedUnit('kmh')}>km/h</button>
                      </div>
                    </div>
                    <label class="night-row">
                      <span class="night-label">{t('options.dynamicBuffer')}</span>
                      <input type="range" min="0" max="50" step="1"
                             value={nightRunnersDynamicBufferPercent}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicBuffer')}
                             oninput={(event) => previewNightRunnersDynamicBuffer(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicBuffer} />
                      <span class="night-value">{nightRunnersDynamicBufferPercent}%</span>
                    </label>
                    <label class="night-row">
                      <span class="night-label">{t('options.dynamicBufferFill')}</span>
                      <input type="range"
                             min={nightDynBufferFillMin}
                             max={nightDynBufferFillMax}
                             step="0.5"
                             value={nightRunnersDynamicBufferFillSeconds}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicBufferFill')}
                             oninput={(event) => previewNightRunnersDynamicBufferFill(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicBufferFill} />
                      <span class="night-value">{nightRunnersDynamicBufferFillSeconds.toFixed(1)}s</span>
                    </label>
                    <label class="night-row">
                      <span class="night-label">{t('options.dynamicFadeIn')}</span>
                      <input type="range"
                             min={nightDynRampMin}
                             max={nightDynRampMax}
                             step="0.1"
                             value={nightRunnersDynamicIncreaseSeconds}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicFadeIn')}
                             oninput={(event) => previewNightRunnersDynamicIncrease(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicIncrease} />
                      <span class="night-value">{nightRunnersDynamicIncreaseSeconds.toFixed(1)}s</span>
                    </label>
                    <label class="night-row">
                      <span class="night-label">{t('options.dynamicFadeOut')}</span>
                      <input type="range"
                             min={nightDynRampMin}
                             max={nightDynRampMax}
                             step="0.1"
                             value={nightRunnersDynamicDecreaseSeconds}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicFadeOut')}
                             oninput={(event) => previewNightRunnersDynamicDecrease(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicDecrease} />
                      <span class="night-value">{nightRunnersDynamicDecreaseSeconds.toFixed(1)}s</span>
                    </label>
                    <label class="night-row">
                      <span class="night-label">{t('options.dynamicMaxDecay')}</span>
                      <input type="range" min="0" max={nightDynMaxDecayDisplayMax} step="1"
                             value={nightDynMaxDecayDisplayValue}
                             disabled={savingOptions}
                             aria-label={t('options.dynamicMaxDecay')}
                             oninput={(event) => previewNightRunnersDynamicMaxDecay(event.currentTarget.value)}
                             onchange={commitNightRunnersDynamicMaxDecay} />
                      <span class="night-value">
                        {nightRunnersDynamicMaxDecayMphS === 0
                          ? t('options.off')
                          : `${nightDynMaxDecayDisplayValue} ${speedUnitLabel(nightRunnersSpeedUnit)}/s`}
                      </span>
                    </label>
                  </div>
                {/if}
                {#if !nightRunnersDynamicMode}
                <div class="night-row speed-row">
                  <span class="night-label">{t('options.fullVolumeSpeed')}</span>
                  <input type="range"
                         min={nightSpeedDisplayMin}
                         max={nightSpeedDisplayMax}
                         step="1"
                         value={nightSpeedDisplayValue}
                         disabled={savingOptions}
                         aria-label={t('aria.fullVolumeSpeed')}
                         oninput={(event) => previewNightRunnersMaxSpeed(event.currentTarget.value)}
                         onchange={commitNightRunnersMaxSpeed} />
                  <span class="night-value">
                    {nightSpeedDisplayValue} {speedUnitLabel(nightRunnersSpeedUnit)}
                  </span>
                  <div class="unit-toggle" aria-label={t('aria.speedUnit')}>
                    <button type="button" class:active={nightRunnersSpeedUnit === 'mph'}
                            aria-disabled={savingOptions}
                            onclick={() => setNightRunnersSpeedUnit('mph')}>mph</button>
                    <button type="button" class:active={nightRunnersSpeedUnit === 'kmh'}
                            aria-disabled={savingOptions}
                            onclick={() => setNightRunnersSpeedUnit('kmh')}>km/h</button>
                  </div>
                </div>
                <div class="curve-toggle-row">
                  <span class="choice-icon material-icon" aria-hidden="true">timeline</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.speedCurve')}</span>
                    <span class="choice-desc">{t('options.speedCurveDesc')}</span>
                  </span>
                  <button class="eq-toggle" class:active={nightRunnersCurveEnabled} type="button"
                          aria-pressed={nightRunnersCurveEnabled}
                          aria-disabled={savingOptions}
                          onclick={() => setNightRunnersCurveEnabled(!nightRunnersCurveEnabled)}>
                    {toggleLabel(nightRunnersCurveEnabled)}
                  </button>
                </div>
                {#if nightRunnersCurveEnabled}
                  <div class="curve-editor">
                    <div class="curve-graph" aria-hidden="true">
                      <svg viewBox="0 0 220 96" preserveAspectRatio="none">
                        <path class="curve-grid" d="M 12 24 H 208 M 12 48 H 208 M 12 72 H 208 M 61 12 V 84 M 110 12 V 84 M 159 12 V 84" />
                        <path class="curve-linear" d="M 12 84 L 208 12" />
                        <path class="curve-line" d={nightRunnersCurvePath} />
                        {#if showSpeedGraphMarker}
                          <g class="curve-marker-group" transform={`translate(${speedMarkerTranslate} 0)`}>
                            <line class="curve-marker" x1="12" x2="12" y1="10" y2="86" />
                            <circle class="curve-marker-dot" cx="12" cy="84" r="3.2" />
                          </g>
                        {/if}
                      </svg>
                    </div>
                    <label class="night-row curve-row">
                      <span class="night-label">{t('options.curveStrength')}</span>
                      <input type="range"
                             min={nightCurveExponentMin}
                             max={nightCurveExponentMax}
                             step="0.1"
                             value={nightRunnersCurveExponent}
                             disabled={savingOptions}
                             aria-label={t('aria.curveStrength')}
                             oninput={(event) => previewNightRunnersCurveExponent(event.currentTarget.value)}
                             onchange={commitNightRunnersCurveExponent} />
                      <span class="night-value">{nightRunnersCurveExponent.toFixed(1)}x</span>
                    </label>
                  </div>
                {/if}
                {/if}
                <div class="curve-toggle-row">
                  <span class="choice-icon material-icon" aria-hidden="true">filter_alt</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.lowFrequencyCut')}</span>
                    <span class="choice-desc">{t('options.lowFrequencyCutDesc')}</span>
                  </span>
                  <button class="eq-toggle" class:active={nightRunnersLowCutEnabled} type="button"
                          aria-pressed={nightRunnersLowCutEnabled}
                          aria-disabled={savingOptions}
                          onclick={() => setNightRunnersLowCutEnabled(!nightRunnersLowCutEnabled)}>
                    {toggleLabel(nightRunnersLowCutEnabled)}
                  </button>
                </div>
                {#if nightRunnersLowCutEnabled}
                  <div class="curve-editor">
                    <div class="frequency-cut-mode" aria-label={t('aria.frequencyCutMode')}>
                      <button type="button"
                              class:active={nightRunnersFrequencyCutMode === 'low'}
                              aria-disabled={savingOptions}
                              onclick={() => setNightRunnersFrequencyCutMode('low')}>
                        {t('options.lowEndCut')}
                      </button>
                      <button type="button"
                              class:active={nightRunnersFrequencyCutMode === 'high'}
                              aria-disabled={savingOptions}
                              onclick={() => setNightRunnersFrequencyCutMode('high')}>
                        {t('options.highEndCut')}
                      </button>
                    </div>
                    <div class="curve-graph low-cut-graph" aria-hidden="true">
                      <svg viewBox="0 0 220 96" preserveAspectRatio="none">
                        <path class="curve-grid" d="M 12 24 H 208 M 12 48 H 208 M 12 72 H 208 M 61 12 V 84 M 110 12 V 84 M 159 12 V 84" />
                        <path class="curve-linear" d="M 12 84 L 208 84" />
                        <path class="curve-line low-cut-line" d={nightRunnersLowCutPath} />
                        {#if showSpeedGraphMarker}
                          <g class="curve-marker-group" transform={`translate(${speedMarkerTranslate} 0)`}>
                            <line class="curve-marker" x1="12" x2="12" y1="10" y2="86" />
                            <circle class="curve-marker-dot" cx="12" cy="84" r="3.2" />
                          </g>
                        {/if}
                      </svg>
                    </div>
                    <label class="night-row">
                      <span class="night-label">{t('options.cutAmount')}</span>
                      <input type="range" min="0" max="100" step="1"
                             value={nightRunnersLowCutAmount}
                             disabled={savingOptions}
                             aria-label={t('aria.lowCutAmount')}
                             oninput={(event) => previewNightRunnersLowCutAmount(event.currentTarget.value)}
                             onchange={commitNightRunnersLowCutAmount} />
                      <span class="night-value">{nightRunnersLowCutAmount}%</span>
                    </label>
                    <label class="night-row">
                      <span class="night-label">{t('options.frequencyCut')}</span>
                      <input type="range"
                             min="0"
                             max="100"
                             step="1"
                             value={nightRunnersFrequencyCutSlider}
                             disabled={savingOptions}
                             aria-label={t('aria.lowCutFrequency')}
                             oninput={(event) => previewNightRunnersLowCutFrequency(event.currentTarget.value)}
                             onchange={commitNightRunnersLowCutFrequency} />
                      <span class="night-value">{formatFrequency(nightRunnersLowCutFrequencyHz)}</span>
                    </label>
                  </div>
                {/if}
                {#if !nightRunnersDynamicMode}
                <div class="curve-toggle-row">
                  <span class="choice-icon material-icon" aria-hidden="true">hourglass_top</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.lazyVolume')}</span>
                    <span class="choice-desc">{t('options.lazyVolumeDesc')}</span>
                  </span>
                  <button class="eq-toggle" class:active={nightRunnersLazyVolumeEnabled} type="button"
                          aria-pressed={nightRunnersLazyVolumeEnabled}
                          aria-disabled={savingOptions}
                          onclick={() => setNightRunnersLazyVolumeEnabled(!nightRunnersLazyVolumeEnabled)}>
                    {toggleLabel(nightRunnersLazyVolumeEnabled)}
                  </button>
                </div>
                {#if nightRunnersLazyVolumeEnabled}
                  <div class="curve-editor">
                    <label class="night-row">
                      <span class="night-label">{t('options.lazyHold')}</span>
                      <input type="range"
                             min={nightLazyHoldMin}
                             max={nightLazyHoldMax}
                             step="0.5"
                             value={nightRunnersLazyHoldSeconds}
                             disabled={savingOptions}
                             aria-label={t('aria.lazyHold')}
                             oninput={(event) => previewNightRunnersLazyHoldSeconds(event.currentTarget.value)}
                             onchange={commitNightRunnersLazyHoldSeconds} />
                      <span class="night-value">{nightRunnersLazyHoldSeconds.toFixed(1)}s</span>
                    </label>
                    <div class="night-row cooldown-row">
                      <span class="night-label">{t('options.lazyCooldown')}</span>
                      <div class="cooldown-bar" aria-label={t('aria.lazyCooldown')}>
                        <span style={`width: ${showLazyCooldown ? lazyCooldownFill : 0}%`}></span>
                      </div>
                    </div>
                  </div>
                {/if}
                {/if}
                <div class="curve-toggle-row">
                  <span class="choice-icon material-icon" aria-hidden="true">garage</span>
                  <span class="choice-text">
                    <span class="choice-label">{t('options.garageRaceMenus')}</span>
                    <span class="choice-desc">{t('options.garageRaceMenusDesc')}</span>
                  </span>
                  <button class="eq-toggle" class:active={nightRunnersNonDrivingVolumeEnabled} type="button"
                          aria-pressed={nightRunnersNonDrivingVolumeEnabled}
                          aria-disabled={savingOptions}
                          onclick={() => setNightRunnersNonDrivingVolumeEnabled(!nightRunnersNonDrivingVolumeEnabled)}>
                    {toggleLabel(nightRunnersNonDrivingVolumeEnabled)}
                  </button>
                </div>
                {#if nightRunnersNonDrivingVolumeEnabled}
                  <label class="night-row">
                    <span class="night-label">{t('options.menuVolume')}</span>
                    <input type="range" min="0" max="100" step="1"
                           value={nightRunnersNonDrivingVolume}
                           disabled={savingOptions}
                           aria-label={t('aria.menuVolume')}
                           oninput={(event) => previewNightRunnersNonDrivingVolume(event.currentTarget.value)}
                           onchange={commitNightRunnersNonDrivingVolume} />
                    <span class="night-value">{nightRunnersNonDrivingVolume}%</span>
                  </label>
                {/if}
              </div>
            {/if}
          </div>
        </div>
        {#if activeSource !== 'radio'}
        <div class="options-group">
          <div class="options-title">{t('options.equalizer')}</div>
          <div class="eq-panel" aria-label={t('aria.equalizer')}>
            <span class="choice-glass" aria-hidden="true"></span>
            <div class="eq-panel-head">
              <span class="choice-icon material-icon" aria-hidden="true">graphic_eq</span>
              <div class="choice-text">
                <span class="choice-label">{t('options.customTone')}</span>
                <span class="choice-desc">{t('options.customToneDesc')}</span>
              </div>
              <button class="eq-toggle" class:active={equalizerEnabled} type="button"
                      aria-pressed={equalizerEnabled}
                      aria-disabled={savingOptions}
                      onclick={() => setEqualizerEnabled(!equalizerEnabled)}>
                {toggleLabel(equalizerEnabled)}
              </button>
            </div>
            <div class="eq-sliders" class:disabled={!equalizerEnabled}>
              {#each eqBands as band, index}
                <label class="eq-row">
                  <span class="eq-band-label">{band.label}<small>{band.unit}</small></span>
                  <input type="range" min="-6" max="6" step="0.5"
                         value={equalizerBands[index]}
                         disabled={!equalizerEnabled}
                         aria-label={`${band.label} ${band.unit}`}
                         oninput={(event) => previewEqualizerBand(index, event.currentTarget.value)}
                         onchange={commitEqualizerBands} />
                  <span class="eq-value">{equalizerBands[index] > 0 ? '+' : ''}{equalizerBands[index].toFixed(1)}</span>
                </label>
              {/each}
            </div>
            <button class="eq-reset" type="button" disabled={savingOptions}
                    onclick={resetEqualizer}>{t('options.reset')}</button>
          </div>
        </div>
        {/if}
      </div>
      <button class="options-close material-icon" type="button" aria-label={t('aria.closeOptions')}
              onclick={() => optionsOpen = false}>close</button>
    </aside>
  {/if}

  {#if annotatedErrors.length > 0}
    <aside class="errors" class:collapsed={errorsCollapsed} bind:clientHeight={errorsHeight}>
      <button class="errors-toggle" type="button"
              aria-expanded={!errorsCollapsed}
              onclick={() => errorsCollapsed = !errorsCollapsed}>
        {#if errorsCollapsed}
          <span>{t(annotatedErrors.length === 1 ? 'errors.one' : 'errors.other', { count: annotatedErrors.length })}</span>
          <span class="material-icon errors-toggle-icon" aria-hidden="true">keyboard_arrow_up</span>
        {:else}
          <span class="material-icon errors-toggle-icon" aria-hidden="true">keyboard_arrow_down</span>
        {/if}
      </button>
      {#if !errorsCollapsed}
        <div class="errors-body">
          {#each annotatedErrors as e, i}
            <div class="err-item" class:marked={markedErrorIndex === i} class:copied={copiedErrorIndex === i}>
              <div class="err-line" role="button" tabindex="0"
                   title={t('aria.copyError')}
                   aria-label={t('aria.copyError')}
                   onclick={() => copyError(i, e.line)}
                   onkeydown={(ev) => { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); copyError(i, e.line); } }}>
                <span class="err">{e.line}</span>
                <span class="err-copy material-icon" aria-hidden="true">{copiedErrorIndex === i ? 'check' : 'content_copy'}</span>
              </div>
              {#if e.hint}
                <div class="err-hint">{e.hint}</div>
              {/if}
            </div>
          {/each}
        </div>
      {/if}
    </aside>
  {/if}
</div>

<style>
  .app {
    display: flex;
    flex-direction: column;
    position: relative;
    min-height: 100dvh;
    background: var(--bg);
    overflow: hidden;
  }

  .backdrop-car {
    position: fixed;
    inset: 0;
    z-index: 0;
    pointer-events: none;
    background-image: url('/car-silhouette.webp');
    background-repeat: no-repeat;
    background-position: calc(50% + 3px) calc(50% + clamp(0rem, 2vh, 1.5rem));
    background-size: min(1220px, 150vw) auto;
    opacity: 0.68;
  }

  .version-label {
    position: fixed;
    top: 1rem;
    right: 1rem;
    z-index: 20;
    font-size: 0.78rem;
    font-weight: 500;
    line-height: 1.2;
    color: var(--text-secondary);
    opacity: 0.42;
    letter-spacing: 0;
    pointer-events: none;
    user-select: none;
  }

  .source-label {
    position: fixed;
    top: 1rem;
    left: 1rem;
    z-index: 24;
    min-width: 4.35rem;
    height: 2.6rem;
    padding: 0 0.55rem 0 0.65rem;
    gap: 0.34rem;
    border: 1px solid rgba(255,255,255,0.12);
    border-radius: 0.5rem;
    background: rgba(16,16,16,0.58);
    box-shadow:
      0 1px 2px rgba(0,0,0,0.22),
      inset 0 1px 0 rgba(255,255,255,0.07);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    cursor: pointer;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    pointer-events: none;
    visibility: hidden;
    opacity: 0;
    transform: translateX(-10px);
  }
  .source-label.visible {
    pointer-events: auto;
    visibility: visible;
    opacity: 0.78;
    transform: translateX(0);
  }
  .source-label .material-icon {
    font-size: 1.54rem;
  }
  .source-local-icon {
    color: var(--text-secondary);
  }
  .source-spotify-icon {
    width: 1.43rem;
    height: 1.43rem;
    display: block;
    color: currentColor;
    opacity: 0.95;
  }
  .source-airplay-icon {
    width: 1.51rem;
    height: 1.51rem;
    display: block;
    color: currentColor;
    opacity: 0.95;
  }
  .source-arrow {
    color: var(--text-secondary);
    opacity: 0.9;
  }
  .source-label.visible:hover,
  .source-label.visible:focus-visible,
  .source-label.opening {
    color: var(--text-primary);
    opacity: 0.95;
    border-color: rgba(255,255,255,0.2);
    background: rgba(28,28,28,0.72);
  }
  .source-menu {
    position: fixed;
    top: 4rem;
    left: 1rem;
    z-index: 35;
    width: min(270px, calc(100vw - 2rem));
    display: grid;
    gap: 0.45rem;
  }
  .source-menu button {
    min-height: 58px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.72);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    color: var(--text-secondary);
    font: inherit;
    cursor: pointer;
    display: grid;
    grid-template-columns: 32px minmax(0, 1fr);
    align-items: center;
    gap: 0.65rem;
    padding: 0.65rem 0.8rem;
    text-align: left;
    overflow: hidden;
  }
  .source-menu .material-icon,
  .source-menu .source-spotify-icon,
  .source-menu .source-airplay-icon {
    width: 28px;
    height: 28px;
    display: grid;
    place-items: center;
    color: currentColor;
    font-size: 24px;
    line-height: 1;
    overflow: hidden;
  }
  .source-menu button:hover,
  .source-menu button:focus-visible,
  .source-menu button.active {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(32, 32, 32, 0.78);
  }
  .source-menu strong,
  .source-menu small {
    display: block;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .source-menu strong {
    font-size: 0.88rem;
    font-weight: 600;
  }
  .source-menu small {
    margin-top: 0.12rem;
    font-size: 0.72rem;
    color: var(--text-secondary);
  }

  .folder-picker,
  .transport button,
  .local-secondary button,
  .volume-popover,
  .locale-row select,
  .local-panel-toggles button,
  .local-panel-tabs button,
  .local-track-actions button,
  .local-folder-head button,
  .local-track-row,
  .radio-station-logo,
  .radio-search input,
  .radio-manual-field input,
  .radio-manual-play,
  .logo-toggle-row,
  .metadata-length-row,
  .unit-toggle,
  .frequency-cut-mode,
  .curve-graph {
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
  }

  .content {
    flex: 1;
    display: flex;
    position: relative;
    z-index: 1;
    align-items: center;
    justify-content: center;
    padding: 1.5rem 1rem;
    max-width: 600px;
    width: 100%;
    margin: 0 auto;
  }

  .stack {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 1.25rem;
    width: 100%;
  }

  /* ── Hero (logos) ── */
  .hero {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.75rem;
  }
  .brand-logo  { height: auto; object-fit: contain; }
  .game-title {
    max-width: min(140px, 30vw);
    color: var(--text-primary);
    font-size: min(1rem, 4vw);
    font-weight: 700;
    line-height: 1.05;
    text-align: center;
  }
  .spotify-logo{ width: min(52px, 12vw); }
  .airplay-logo {
    width: min(58px, 13vw);
    color: var(--text-primary);
  }
  .radio-logo {
    width: min(58px, 13vw);
    height: min(58px, 13vw);
    display: grid;
    place-items: center;
    color: var(--text-primary);
    font-size: min(56px, 12vw);
  }
  .link-icon {
    font-size: 1.5rem;
    color: var(--text-secondary);
    opacity: 0.5;
  }

  /* ── Status line ── */
  .status-line {
    position: fixed;
    top: 1rem;
    left: 50%;
    z-index: 22;
    transform: translateX(-50%);
    display: flex;
    align-items: center;
    justify-content: center;
    width: max-content;
    max-width: min(56vw, 420px);
    font-size: 1rem;
    font-weight: 500;
    line-height: 1;
    pointer-events: none;
    white-space: nowrap;
  }
  .status-line.ok    { color: var(--accent); }
  .status-line.warn  { color: #f5a623; }
  .status-line.idle  { color: var(--text-secondary); opacity: 0.7; }

  /* ── Now-playing widget (icon + title together) ── */
  .np {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.75rem;
    width: fit-content;
    max-width: min(92vw, 420px);
    margin-top: 1.25rem;
    margin-left: auto;
    margin-right: auto;
  }
  .np-prompt {
    max-width: min(92vw, 420px);
  }
  /* Fixed-size icon slot so equalizer and material icon align identically */
  .np-icon {
    width: 36px;
    height: 36px;
    display: flex;
    align-items: center;
    justify-content: center;
    flex-shrink: 0;
  }
  .np-icon .material-icon {
    font-size: 32px;
    line-height: 1;
    display: block;
  }
  .icon-pause { color: var(--text-secondary); }
  .icon-away  { color: #f5a623; }
  .icon-folder { color: var(--text-secondary); }

  .np-text {
    display: flex;
    flex-direction: column;
    align-items: flex-start;
    justify-content: center;
    flex: 0 1 auto;
    min-width: 0;
    text-align: left;
  }
  .np-title {
    font-size: 1.05rem;
    font-weight: 600;
    color: var(--text-primary);
    line-height: 1.2;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 100%;
  }
  .prompt-title {
    color: #f5a623;
  }
  .np-artist {
    font-size: 0.85rem;
    color: var(--text-secondary);
    line-height: 1.2;
    margin-top: 0.15rem;
  }

  /* "Log in with Spotify" — alternative to Zeroconf pairing. Kept compact so it
     clears the absolutely-positioned driving speed bars below the now-playing
     card (.speed-indicator sits at a fixed top offset). */
  .spotify-oauth {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.4rem;
    margin-top: 0.55rem;
    width: 100%;
    max-width: min(92%, 420px);
    margin-left: auto;
    margin-right: auto;
  }
  .oauth-or {
    font-size: 0.72rem;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--text-secondary);
    opacity: 0.7;
  }
  .oauth-btn {
    display: inline-flex;
    align-items: center;
    gap: 0.4rem;
    padding: 0.4rem 1rem;
    border: none;
    border-radius: 999px;
    background: #1DB954;
    color: #06320f;
    font-size: 0.88rem;
    font-weight: 700;
    cursor: pointer;
    transition: filter 0.15s ease, transform 0.05s ease;
  }
  .oauth-btn:hover:not(:disabled) { filter: brightness(1.08); }
  .oauth-btn:active:not(:disabled) { transform: scale(0.97); }
  .oauth-btn:disabled { opacity: 0.6; cursor: default; }
  .oauth-btn .material-icon { font-size: 18px; line-height: 1; }
  .oauth-error {
    font-size: 0.8rem;
    color: #e2554b;
    text-align: center;
  }

  .local-widget {
    position: relative;
    width: min(92vw, 460px);
    display: grid;
    place-items: center;
  }
  .airplay-widget {
    width: min(92vw, 420px);
  }
  .local-player {
    position: relative;
    width: 100%;
    display: grid;
    gap: 0.85rem;
    margin-top: 1rem;
  }
  .local-note,
  .local-error {
    font-size: 0.74rem;
    line-height: 1.35;
    color: var(--text-secondary);
  }
  .folder-picker {
    min-height: 58px;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.58);
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto auto;
    align-items: center;
    gap: 0.45rem;
    padding: 0.65rem;
  }
  .folder-path {
    min-width: 0;
    position: relative;
    overflow: hidden;
    padding-right: 1.15rem;
  }
  .folder-path::after {
    content: '';
    position: absolute;
    top: 0;
    right: 0;
    bottom: 0;
    width: 2.5rem;
    pointer-events: none;
    background: linear-gradient(90deg, rgba(18, 18, 18, 0), rgba(18, 18, 18, 0.86));
  }
  .folder-path strong {
    display: block;
    min-width: 0;
    overflow: hidden;
    white-space: nowrap;
  }
  .folder-path strong {
    font-size: 0.82rem;
    font-weight: 500;
    color: var(--text-primary);
  }
  .folder-picker button {
    width: 34px;
    min-height: 34px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 6px;
    background: rgba(255, 255, 255, 0.04);
    color: var(--text-secondary);
    cursor: pointer;
    font: inherit;
    font-size: 0.78rem;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.3rem;
    padding: 0;
  }
  .folder-picker button:hover,
  .folder-picker button:focus-visible {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(255, 255, 255, 0.08);
  }
  .folder-picker button:disabled {
    cursor: default;
    opacity: 0.5;
  }
  .transport,
  .local-secondary {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.55rem;
    flex-wrap: wrap;
  }
  .transport button,
  .local-secondary button {
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.52);
    color: var(--text-secondary);
    cursor: pointer;
    font: inherit;
    min-height: 38px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.35rem;
    padding: 0 0.7rem;
  }
  .transport button:hover,
  .transport button:focus-visible,
  .local-secondary button:hover,
  .local-secondary button:focus-visible,
  .local-secondary button.active {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(32, 32, 32, 0.72);
  }
  .transport button:disabled,
  .local-secondary button:disabled {
    cursor: default;
    opacity: 0.45;
  }
  .transport button {
    width: 42px;
    height: 42px;
    padding: 0;
    border-radius: 999px;
  }
  .transport {
    position: relative;
    min-height: 52px;
  }
  .transport .play-button {
    width: 52px;
    height: 52px;
    color: var(--text-primary);
  }
  .volume-control {
    position: relative;
    width: 34px;
    height: 34px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    flex: 0 0 auto;
  }
  .local-secondary .volume-control .volume-button {
    width: 34px;
    min-height: 34px;
    padding: 0;
    border-color: transparent;
    background: transparent;
  }
  .volume-control.open .volume-button {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(32, 32, 32, 0.72);
  }
  .volume-popover {
    position: absolute;
    left: 50%;
    bottom: calc(100% + 0.7rem);
    transform: translateX(-50%);
    z-index: 8;
    display: grid;
    grid-template-rows: 136px auto;
    justify-items: center;
    gap: 0.55rem;
    padding: 0.85rem 0.5rem 0.65rem;
    min-width: 58px;
    border: 1px solid rgba(255, 255, 255, 0.16);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.96);
    box-shadow: 0 18px 40px rgba(0, 0, 0, 0.42);
    color: var(--text-secondary);
    font-size: 0.72rem;
    font-variant-numeric: tabular-nums;
  }
  .volume-popover::after {
    content: '';
    position: absolute;
    left: 50%;
    bottom: -6px;
    width: 10px;
    height: 10px;
    transform: translateX(-50%) rotate(45deg);
    background: rgba(18, 18, 18, 0.96);
    border-right: 1px solid rgba(255, 255, 255, 0.16);
    border-bottom: 1px solid rgba(255, 255, 255, 0.16);
  }
  .volume-slider-vertical {
    width: 28px;
    height: 136px;
    writing-mode: vertical-lr;
    direction: rtl;
    accent-color: var(--text-primary);
    cursor: pointer;
  }
  .volume-slider-vertical:disabled {
    cursor: default;
    opacity: 0.55;
  }
  .local-secondary {
    margin-top: -0.1rem;
  }
  .local-secondary button {
    width: 34px;
    min-height: 34px;
    padding: 0;
    background: transparent;
    border-color: transparent;
  }
  .local-secondary button:hover,
  .local-secondary button:focus-visible,
  .local-secondary button.active {
    background: rgba(255, 255, 255, 0.06);
  }
  .local-progress {
    display: grid;
    grid-template-columns: 44px minmax(0, 1fr) 44px;
    align-items: center;
    gap: 0.6rem;
    color: var(--text-secondary);
    font-size: 0.72rem;
    font-variant-numeric: tabular-nums;
  }
  .local-progress > span:first-child {
    text-align: left;
  }
  .local-progress > span:last-child {
    text-align: right;
  }
  .local-progress.disabled {
    opacity: 0.55;
  }
  .speed-indicator {
    position: absolute;
    left: 50%;
    top: calc(50% + 126px);
    z-index: 2;
    transform: translateX(-50%);
    width: min(92vw, 460px);
    height: 48px;
    display: grid;
    grid-template-rows: auto 6px;
    justify-items: center;
    align-content: center;
    gap: 0.34rem;
    color: var(--text-primary);
    font-variant-numeric: tabular-nums;
    pointer-events: none;
  }
  .speed-indicator.unavailable {
    opacity: 0.68;
  }
  .speed-bar {
    position: relative;
    width: calc(100% - 88px);
    height: 6px;
    overflow: hidden;
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.12);
    box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.05);
  }
  .speed-bar span {
    display: block;
    height: 100%;
    min-width: 0;
    border-radius: inherit;
    background: linear-gradient(90deg, rgba(29, 185, 84, 0.55), rgba(255, 255, 255, 0.88));
    transition: width 460ms cubic-bezier(0.16, 1, 0.3, 1);
    will-change: width;
  }
  .speed-readout {
    position: relative;
    z-index: 1;
    color: var(--text-primary);
    text-align: center;
    font-size: 1.05rem;
    font-weight: 700;
    line-height: 1;
    text-shadow: 0 2px 12px rgba(0, 0, 0, 0.65);
  }
  .speed-indicator.has-dynamic {
    height: 58px;
    grid-template-rows: auto 6px 6px;
  }
  /* Bar #2 — dynamic-mode speed/volume-influence state bar.
     The bar itself does NOT clip, so the peak/threshold markers can protrude
     and glow; the rounded fill + buffer band are clipped by an inner track. */
  .speed-state-bar {
    position: relative;
    width: calc(100% - 88px);
    height: 6px;
  }
  .speed-state-track {
    position: absolute;
    inset: 0;
    overflow: hidden;
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.1);
    box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.06);
  }
  /* Speed fill — tinted by the current volume-influence state. */
  .speed-state-fill {
    position: absolute;
    left: 0;
    top: 0;
    height: 100%;
    min-width: 0;
    border-radius: inherit;
    background: linear-gradient(90deg,
      rgba(120, 120, 120, 0.34), rgba(190, 190, 190, 0.46));
    transition: width 220ms linear, background 240ms ease;
    will-change: width;
  }
  /* Increasing — green, kept translucent so the volume bar stays the focus. */
  .speed-state-bar.accel .speed-state-fill {
    background: linear-gradient(90deg,
      rgba(29, 185, 84, 0.38), rgba(120, 255, 170, 0.58));
  }
  .speed-state-bar.hold .speed-state-fill {
    background: linear-gradient(90deg,
      rgba(214, 170, 40, 0.5), rgba(255, 214, 92, 0.92));
  }
  .speed-state-bar.fade .speed-state-fill {
    background: linear-gradient(90deg,
      rgba(150, 60, 50, 0.5), rgba(232, 110, 92, 0.88));
  }
  /* Decreasing — translucent gray so the yellow/orange buffer band stays
     visible and the volume bar keeps the focus. */
  .speed-state-bar.below .speed-state-fill {
    background: linear-gradient(90deg,
      rgba(64, 64, 72, 0.36), rgba(112, 112, 124, 0.5));
  }
  /* Buffer band — the yellow tolerance window [peak-buffer .. peak]. */
  .speed-state-buffer {
    position: absolute;
    top: 0;
    height: 100%;
    min-width: 0;
    background: rgba(255, 209, 71, 0.42);
    border-left: 1px solid rgba(255, 224, 130, 0.7);
    transition: left 220ms linear, width 220ms linear;
    pointer-events: none;
  }
  /* Trailing-peak marker (right edge of the buffer band). */
  .speed-state-peak {
    position: absolute;
    top: -1px;
    width: 2px;
    height: calc(100% + 2px);
    margin-left: -1px;
    background: rgba(255, 255, 255, 0.85);
    transition: left 220ms linear;
    pointer-events: none;
  }
  /* Threshold separator — min speed for the buffered swell. */
  .speed-state-threshold {
    position: absolute;
    top: -2px;
    width: 2px;
    height: calc(100% + 4px);
    margin-left: -1px;
    background: rgba(120, 200, 255, 0.95);
    box-shadow: 0 0 6px rgba(120, 200, 255, 0.6);
    transition: left 220ms linear;
    pointer-events: none;
  }
  .progress-track {
    position: relative;
    height: 18px;
    border: 0;
    background: transparent;
    cursor: pointer;
  }
  .progress-track:disabled {
    cursor: default;
  }
  .progress-track::before {
    content: '';
    position: absolute;
    left: 0;
    right: 0;
    top: 8px;
    height: 2px;
    border-radius: 2px;
    background: rgba(255, 255, 255, 0.18);
  }
  .progress-track span {
    position: absolute;
    left: 0;
    top: 8px;
    height: 2px;
    border-radius: 2px;
    background: var(--text-primary);
  }
  .local-note {
    text-align: center;
  }
  .local-error {
    color: #f5a623;
    text-align: center;
  }

  /* ── Animated equalizer ── */
  .eq {
    display: flex;
    align-items: flex-end;
    gap: 4px;
    height: 28px;
  }
  .eq .bar {
    display: block;
    width: 5px;
    border-radius: 2px;
    background: var(--accent);
    transform-origin: bottom center;
    animation: eq-bounce 0.9s ease-in-out infinite;
  }
  .eq .b1 { animation-delay: 0s;     height: 60%; }
  .eq .b2 { animation-delay: 0.18s;  height: 100%; }
  .eq .b3 { animation-delay: 0.36s;  height: 70%; }
  @keyframes eq-bounce {
    0%, 100% { transform: scaleY(0.35); }
    50%      { transform: scaleY(1.0);  }
  }

  /* ── Footer ── */
  .credit {
    position: relative;
    z-index: 1;
    padding: 1rem;
    text-align: center;
  }
  .credit-text,
  .discord-link {
    display: inline-flex;
    align-items: center;
    justify-content: center;
  }
  .credit-text,
  .discord-link,
  .source-label,
  .options-label {
    appearance: none;
    -webkit-appearance: none;
    font-family: var(--font-sans);
    font-size: 0.78rem;
    font-weight: 400;
    line-height: 1.2;
    letter-spacing: 0;
    color: var(--text-secondary);
    opacity: 0.35;
    text-transform: none;
    text-decoration: none;
    transition: color 140ms ease, opacity 140ms ease, transform 160ms ease;
  }
  .credit-text:hover,
  .credit-text:focus-visible,
  .discord-link:hover,
  .discord-link:focus-visible {
    color: var(--text-primary);
    opacity: 0.62;
  }
  .credit-text:hover,
  .credit-text:focus-visible,
  .discord-link:hover,
  .discord-link:focus-visible {
    text-decoration: none;
  }
  .discord-icon {
    width: 1.63rem;
    height: 1.24rem;
    display: block;
    fill: currentColor;
  }
  .discord-link {
    position: fixed;
    left: 1rem;
    bottom: calc(1rem + var(--error-offset, 0px));
    width: 2.34rem;
    height: 2.34rem;
    z-index: 20;
  }
  .credit-warning {
    margin-top: 0.25rem;
    font-size: 0.68rem;
    line-height: 1.2;
    color: #f5a623;
    opacity: 0.7;
  }

  /* ── Options ── */
  .options-label {
    position: fixed;
    right: 1rem;
    bottom: calc(1rem + var(--error-offset, 0px));
    z-index: 20;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 8px;
    background: rgba(16, 16, 16, 0.42);
    box-shadow:
      0 1px 2px rgba(0, 0, 0, 0.22),
      inset 0 1px 0 rgba(255, 255, 255, 0.06);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    cursor: pointer;
    width: 2.34rem;
    height: 2.34rem;
    padding: 0;
    margin: 0;
    align-items: center;
    justify-content: center;
    text-align: center;
    pointer-events: none;
    visibility: hidden;
    opacity: 0;
    transform: translateX(10px);
  }
  .options-label.visible {
    display: inline-flex;
    pointer-events: auto;
    visibility: visible;
    opacity: 0.35;
    transform: translateX(0);
  }
  .options-label .material-icon {
    font-size: 1.63rem;
  }
  .options-label.visible:hover,
  .options-label.visible:focus-visible {
    color: var(--text-primary);
    opacity: 0.78;
    border-color: rgba(255, 255, 255, 0.18);
    background: rgba(28, 28, 28, 0.58);
  }
  .options-label.opening {
    pointer-events: none;
    opacity: 0;
    transform: translateX(34px);
  }
  .options-panel,
  .local-panel {
    position: fixed;
    bottom: calc(1rem + var(--error-offset, 0px));
    z-index: 30;
    max-height: calc(100dvh - 4.35rem - var(--error-offset, 0px));
    overflow: hidden;
    padding: 0;
    border: 0;
    border-radius: 8px;
    background: transparent;
    box-shadow: none;
    display: grid;
    grid-template-rows: minmax(0, 1fr) auto;
    gap: 0.75rem;
    --scroll-fade-size: 26px;
    --panel-footer-size: calc(38px + 0.75rem);
  }
  .options-panel::before,
  .options-panel::after,
  .local-panel-scroll-frame::before,
  .local-panel-scroll-frame::after {
    content: '';
    position: absolute;
    left: 0;
    right: 0;
    z-index: 2;
    height: var(--scroll-fade-size);
    pointer-events: none;
  }
  .options-panel::before,
  .local-panel-scroll-frame::before {
    top: 0;
    background: linear-gradient(to bottom, rgba(10, 10, 10, 0.92), rgba(10, 10, 10, 0));
  }
  .options-panel::after {
    bottom: var(--panel-footer-size);
    background: linear-gradient(to top, rgba(10, 10, 10, 0.92), rgba(10, 10, 10, 0));
  }
  .local-panel-scroll-frame::after {
    bottom: 0;
    background: linear-gradient(to top, rgba(10, 10, 10, 0.92), rgba(10, 10, 10, 0));
  }
  .options-panel {
    right: 1.5rem;
    width: min(430px, calc(100vw - 3rem));
    --panel-footer-size: calc(28px + 0.75rem);
  }
  .options-panel,
  .options-panel *,
  .options-panel *::before,
  .options-panel *::after {
    box-sizing: border-box;
  }
  .local-panel {
    width: min(340px, calc(100vw - 3rem));
  }
  .local-panel {
    left: 1.5rem;
    top: 4.35rem;
    bottom: calc(3.55rem + var(--error-offset, 0px));
    height: auto;
    max-height: none;
    grid-template-rows: auto minmax(0, 1fr) auto;
  }
  .radio-panel {
    grid-template-rows: auto minmax(0, 1fr) auto;
  }
  .local-panel-head {
    position: relative;
    z-index: 3;
    margin-top: 0;
    padding-right: 0.4rem;
  }
  .radio-panel-search {
    position: relative;
    z-index: 3;
    padding-right: 0.4rem;
  }
  .local-panel-scroll-frame {
    position: relative;
    z-index: 1;
    min-height: 0;
    overflow: hidden;
  }
  .options-scroll,
  .local-panel-scroll {
    position: relative;
    z-index: 1;
    min-height: 0;
    overflow-y: auto;
    scrollbar-gutter: stable;
  }
  .local-panel-scroll {
    height: 100%;
  }
  .options-scroll {
    padding: var(--scroll-fade-size) 0.65rem;
  }
  .local-panel-scroll {
    padding: var(--scroll-fade-size) 0.4rem var(--scroll-fade-size) 0;
  }
  .local-panel-scroll {
    display: grid;
    align-content: start;
    gap: 1rem;
  }
  .options-close {
    justify-self: end;
    flex: 0 0 auto;
    width: 28px;
    height: 28px;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--text-secondary);
    cursor: pointer;
    display: grid;
    place-items: center;
    font-size: 20px;
  }
  .options-close:hover,
  .options-close:focus-visible {
    background: rgba(255, 255, 255, 0.06);
    color: var(--text-primary);
  }
  .options-group {
    margin-top: 1rem;
  }
  .options-group:first-child {
    margin-top: 0;
  }
  .options-title {
    font-size: 0.78rem;
    font-weight: 500;
    color: var(--text-secondary);
    margin-bottom: 0.45rem;
  }
  .locale-row {
    display: grid;
    gap: 0.45rem;
  }
  .locale-row select {
    width: 100%;
    min-height: 38px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.72);
    color: var(--text-primary);
    font: inherit;
    font-size: 0.82rem;
    padding: 0 0.65rem;
  }
  .locale-row select:disabled {
    opacity: 0.55;
  }
  .local-panel-toggles {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem;
    margin-top: 0.5rem;
  }
  .local-panel-toggles button,
  .local-panel-tabs button,
  .local-track-actions button,
  .local-folder-head button {
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.42);
    color: var(--text-secondary);
    cursor: pointer;
    font: inherit;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.35rem;
  }
  .local-panel-toggles button {
    min-height: 42px;
    font-size: 0.78rem;
  }
  .local-panel-toggles button:hover,
  .local-panel-toggles button:focus-visible,
  .local-panel-toggles button.active,
  .local-panel-tabs button:hover,
  .local-panel-tabs button:focus-visible,
  .local-panel-tabs button.active,
  .local-track-actions button:hover,
  .local-track-actions button:focus-visible,
  .local-folder-head button:hover,
  .local-folder-head button:focus-visible {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(32, 32, 32, 0.72);
  }
  .local-track-actions button:disabled,
  .local-folder-head button:disabled {
    cursor: default;
  }
  .local-track-actions button.queued,
  .local-track-actions button.queued:disabled,
  .local-folder-head button.queued,
  .local-folder-head button.queued:disabled {
    color: #39d979;
    border-color: rgba(57, 217, 121, 0.45);
    background: rgba(57, 217, 121, 0.14);
    cursor: default;
    opacity: 1;
  }
  .local-panel-toggles button:disabled {
    cursor: default;
    opacity: 0.45;
  }
  .local-panel-list {
    display: grid;
    gap: 0.8rem;
  }
  .local-folder-group {
    display: grid;
    gap: 0.42rem;
  }
  .local-folder-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 0.6rem;
    color: var(--text-secondary);
    font-size: 0.74rem;
    font-weight: 600;
    min-width: 0;
  }
  .local-folder-head > span {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .local-folder-head button,
  .local-track-actions button {
    width: 30px;
    height: 30px;
    padding: 0;
    flex: 0 0 auto;
  }
  .local-folder-head .material-icon,
  .local-track-actions .material-icon {
    font-size: 19px;
  }
  .local-track-row {
    min-height: 48px;
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    align-items: center;
    gap: 0.55rem;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.42);
    padding: 0.48rem 0.52rem 0.48rem 0.68rem;
  }
  .local-track-row.autoplay {
    opacity: 0.72;
  }
  .radio-station-row {
    grid-template-columns: 42px minmax(0, 1fr) auto;
    padding-left: 0.52rem;
  }
  .local-track-row.playing {
    border-color: rgba(57, 217, 121, 0.46);
    background: rgba(57, 217, 121, 0.12);
  }
  .local-track-row.playing .local-track-text strong {
    color: var(--text-primary);
  }
  .radio-station-logo {
    width: 42px;
    height: 42px;
    border-radius: 8px;
    overflow: hidden;
    display: grid;
    place-items: center;
    background: rgba(255, 255, 255, 0.10);
    color: var(--text-secondary);
  }
  .radio-station-logo img {
    width: 100%;
    height: 100%;
    object-fit: cover;
  }
  .radio-search .folder-path {
    padding: 0;
    background: transparent;
  }
  .radio-search input,
  .radio-manual-field input {
    width: 100%;
    min-width: 0;
    border: 0;
    outline: 0;
    border-radius: 6px;
    min-height: 42px;
    padding: 0 0.72rem;
    background: rgba(255, 255, 255, 0.10);
    color: var(--text-primary);
    font: inherit;
  }
  .radio-search input::placeholder,
  .radio-manual-field input::placeholder {
    color: rgba(255, 255, 255, 0.48);
  }
  .radio-search input::-webkit-search-cancel-button,
  .radio-search input::-webkit-search-decoration {
    -webkit-appearance: none;
    appearance: none;
    display: none;
  }
  .radio-manual-field {
    display: grid;
    gap: 0.48rem;
    color: var(--text-secondary);
    font-size: 0.74rem;
  }
  .radio-manual-form {
    display: grid;
    gap: 0.75rem;
  }
  .radio-manual-form.options-group {
    margin-top: 0;
  }
  .radio-manual-play {
    width: 100%;
    min-height: 42px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.52);
    color: var(--text-secondary);
    cursor: pointer;
    font: inherit;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 0.4rem;
  }
  .radio-manual-play:hover,
  .radio-manual-play:focus-visible {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.22);
    background: rgba(32, 32, 32, 0.72);
  }
  .radio-manual-play:disabled {
    cursor: default;
    opacity: 0.45;
  }
  .local-track-text {
    display: grid;
    gap: 0.12rem;
    min-width: 0;
  }
  .local-track-text strong,
  .local-track-text span {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .local-track-text strong {
    color: var(--text-primary);
    font-size: 0.82rem;
    font-weight: 600;
  }
  .local-track-text span,
  .local-empty {
    color: var(--text-secondary);
    font-size: 0.72rem;
  }
  .local-empty,
  .local-error {
    padding-top: 0.28rem;
  }
  .local-track-actions {
    display: flex;
    align-items: center;
    gap: 0.35rem;
  }
  .local-panel-tabs {
    position: relative;
    z-index: 3;
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.5rem;
  }
  .radio-panel .local-panel-tabs {
    grid-template-columns: repeat(4, minmax(0, 1fr));
  }
  .radio-panel .local-panel-tabs button {
    padding: 0;
    gap: 0;
  }
  .radio-panel .local-panel-tabs .material-icon {
    font-size: 22px;
  }
  .local-panel-tabs button {
    min-height: 38px;
    font-size: 0.78rem;
  }
  .segmented {
    display: grid;
    gap: 0.5rem;
  }
  .segmented button {
    min-height: 74px;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: transparent;
    color: var(--text-secondary);
    font: inherit;
    text-align: left;
    padding: 0.75rem 2.75rem 0.75rem 0.85rem;
    cursor: pointer;
    position: relative;
    overflow: hidden;
    display: flex;
    align-items: center;
    gap: 0.75rem;
  }
  .choice-glass {
    position: absolute;
    inset: 0;
    z-index: 0;
    border-radius: inherit;
    background: rgba(18, 18, 18, 0.42);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
    pointer-events: none;
  }
  .segmented button:hover,
  .segmented button:focus-visible {
    border-color: rgba(255, 255, 255, 0.16);
    color: var(--text-primary);
  }
  .segmented button:hover .choice-glass,
  .segmented button:focus-visible .choice-glass {
    background: rgba(24, 24, 24, 0.5);
  }
  .segmented button.active {
    border-color: rgba(255, 255, 255, 0.22);
    color: var(--text-primary);
  }
  .segmented button.active .choice-glass {
    background: rgba(32, 32, 32, 0.58);
  }
  .choice-icon {
    position: relative;
    z-index: 1;
    flex: 0 0 auto;
    width: 28px;
    height: 28px;
    display: grid;
    place-items: center;
    border-radius: 999px;
    color: var(--text-secondary);
    font-size: 22px;
    background: rgba(255, 255, 255, 0.04);
  }
  .segmented button:hover .choice-icon,
  .segmented button:focus-visible .choice-icon,
  .segmented button.active .choice-icon {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.08);
  }
  .choice-text {
    display: grid;
    position: relative;
    z-index: 1;
    gap: 0.25rem;
    min-width: 0;
  }
  .choice-label {
    font-size: 0.9rem;
    font-weight: 600;
    line-height: 1.2;
    color: var(--text-primary);
  }
  .choice-desc {
    font-size: 0.78rem;
    font-weight: 400;
    line-height: 1.35;
    color: var(--text-secondary);
  }
  .choice-help {
    font-size: 0.72rem;
    font-weight: 400;
    line-height: 1.35;
    color: rgba(255, 255, 255, 0.48);
  }
  .choice-dot {
    position: absolute;
    z-index: 1;
    right: 0.9rem;
    top: 50%;
    width: 18px;
    height: 18px;
    border-radius: 999px;
    border: 1px solid rgba(255, 255, 255, 0.18);
    transform: translateY(-50%);
  }
  .segmented button.active .choice-dot {
    border: 5px solid var(--text-primary);
  }
  .segmented button[aria-disabled='true'] {
    cursor: default;
  }
  .options-error {
    margin-top: 0.55rem;
    font-size: 0.72rem;
    color: var(--danger);
  }
  .eq-panel,
  .night-panel {
    position: relative;
    overflow: hidden;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: transparent;
    padding: 0.85rem;
    color: var(--text-secondary);
  }
  .eq-panel > .choice-glass,
  .night-panel > .choice-glass {
    background: rgba(18, 18, 18, 0.42);
    backdrop-filter: blur(14px);
    -webkit-backdrop-filter: blur(14px);
  }
  .logo-panel {
    display: grid;
    gap: 0.62rem;
  }
  .logo-toggle-row {
    min-height: 58px;
    display: grid;
    grid-template-columns: 28px minmax(0, 1fr) 58px;
    align-items: center;
    gap: 0.68rem;
    padding: 0.62rem 0.7rem;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.42);
  }
  .logo-toggle-row .choice-text {
    gap: 0.18rem;
  }
  .logo-toggle,
  .logo-variant button {
    cursor: pointer;
  }
  .logo-toggle {
    width: 58px;
    min-width: 58px;
    height: 32px;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.04);
    color: var(--text-secondary);
    font: inherit;
    font-size: 0.72rem;
    font-weight: 700;
    padding: 0 0.45rem;
  }
  .logo-toggle:hover,
  .logo-toggle:focus-visible,
  .logo-toggle.active {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.24);
    background: rgba(255, 255, 255, 0.08);
  }
  .logo-variant-head {
    display: grid;
    grid-template-columns: 28px minmax(0, 1fr);
    align-items: center;
    gap: 0.68rem;
    padding: 0.08rem 0.7rem 0;
  }
  .logo-variant-head .choice-icon {
    width: 28px;
    height: 28px;
  }
  .logo-variant {
    grid-template-columns: 1fr 1fr;
  }
  .local-artist-variant {
    grid-template-columns: 1fr;
  }
  .logo-variant button {
    min-height: 62px;
    padding: 0.62rem 2.3rem 0.62rem 0.68rem;
    gap: 0.6rem;
  }
  .logo-variant .choice-label {
    font-size: 0.82rem;
  }
  .logo-variant .choice-desc {
    font-size: 0.72rem;
  }
  .eq-panel-head,
  .night-panel-head {
    position: relative;
    z-index: 1;
    display: grid;
    grid-template-columns: 28px minmax(0, 1fr) auto;
    align-items: center;
    gap: 0.75rem;
  }
  .eq-toggle {
    min-width: 44px;
    height: 28px;
    border: 1px solid rgba(255, 255, 255, 0.14);
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.04);
    color: var(--text-secondary);
    font: inherit;
    font-size: 0.72rem;
    font-weight: 600;
    cursor: pointer;
  }
  .eq-toggle:hover,
  .eq-toggle:focus-visible,
  .eq-toggle.active {
    color: var(--text-primary);
    border-color: rgba(255, 255, 255, 0.24);
    background: rgba(255, 255, 255, 0.08);
  }
  .night-controls {
    position: relative;
    z-index: 1;
    display: grid;
    gap: 0.75rem;
    margin-top: 0.9rem;
  }
  .night-row {
    display: grid;
    grid-template-columns: 84px minmax(150px, 1fr) 72px;
    align-items: center;
    column-gap: 0.7rem;
    row-gap: 0.42rem;
    min-height: 30px;
  }
  .night-label {
    min-width: 0;
    color: var(--text-primary);
    font-size: 0.74rem;
    font-weight: 600;
    line-height: 1.15;
  }
  .night-value {
    color: var(--text-secondary);
    font-size: 0.72rem;
    text-align: right;
    white-space: nowrap;
    font-variant-numeric: tabular-nums;
  }
  .night-row input[type='range'] {
    width: 100%;
    height: 18px;
    accent-color: var(--text-primary);
    cursor: pointer;
  }
  .night-row input[type='range']:disabled {
    cursor: default;
    opacity: 0.55;
  }
  .metadata-length-row {
    grid-template-columns: 112px minmax(120px, 1fr) 36px;
    padding: 0.62rem 0.7rem;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background: rgba(18, 18, 18, 0.42);
  }
  .metadata-length-row.disabled {
    opacity: 0.62;
  }
  .cooldown-row {
    min-height: 24px;
  }
  .cooldown-bar {
    position: relative;
    height: 6px;
    width: 100%;
    overflow: hidden;
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.12);
    box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.05);
  }
  .cooldown-bar span {
    display: block;
    height: 100%;
    min-width: 0;
    border-radius: inherit;
    background: linear-gradient(90deg, rgba(29, 185, 84, 0.55), rgba(255, 255, 255, 0.88));
    transition: width 120ms linear;
    will-change: width;
  }
  .unit-toggle {
    grid-column: 2 / 4;
    justify-self: end;
    display: inline-flex;
    gap: 0.25rem;
    padding: 0.15rem;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.04);
  }
  .unit-toggle button {
    min-width: 42px;
    height: 24px;
    border: 0;
    border-radius: 999px;
    background: transparent;
    color: var(--text-secondary);
    font: inherit;
    font-size: 0.68rem;
    font-weight: 600;
    cursor: pointer;
    padding: 0 0.45rem;
  }
  .unit-toggle button:hover,
  .unit-toggle button:focus-visible,
  .unit-toggle button.active {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.10);
  }
  .unit-toggle button[aria-disabled='true'] {
    cursor: default;
  }
  .frequency-cut-mode {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.35rem;
    padding: 0.18rem;
    border: 1px solid rgba(255, 255, 255, 0.10);
    border-radius: 8px;
    background: rgba(255, 255, 255, 0.04);
  }
  .frequency-cut-mode button {
    min-height: 30px;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--text-secondary);
    font: inherit;
    font-size: 0.72rem;
    font-weight: 600;
    cursor: pointer;
    padding: 0 0.55rem;
  }
  .frequency-cut-mode button:hover,
  .frequency-cut-mode button:focus-visible,
  .frequency-cut-mode button.active {
    color: var(--text-primary);
    background: rgba(255, 255, 255, 0.10);
  }
  .frequency-cut-mode button[aria-disabled='true'] {
    cursor: default;
  }
  .curve-toggle-row {
    display: grid;
    grid-template-columns: 28px minmax(0, 1fr) auto;
    align-items: center;
    gap: 0.75rem;
    min-height: 34px;
    padding-top: 0.1rem;
  }
  .curve-toggle-row .choice-icon {
    width: 28px;
    height: 28px;
  }
  .curve-editor {
    display: grid;
    gap: 0.7rem;
  }
  .curve-graph {
    height: 104px;
    overflow: hidden;
    border: 1px solid rgba(255, 255, 255, 0.12);
    border-radius: 8px;
    background:
      radial-gradient(circle at 88% 20%, rgba(255, 255, 255, 0.08), transparent 32%),
      rgba(255, 255, 255, 0.035);
    box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.05);
  }
  .curve-graph svg {
    width: 100%;
    height: 100%;
    display: block;
  }
  .curve-grid,
  .curve-linear,
  .curve-line,
  .curve-marker,
  .curve-marker-dot {
    fill: none;
    vector-effect: non-scaling-stroke;
  }
  .curve-marker-group {
    transition: transform 280ms cubic-bezier(0.16, 1, 0.3, 1);
    will-change: transform;
  }
  .curve-grid {
    stroke: rgba(255, 255, 255, 0.08);
    stroke-width: 1;
  }
  .curve-linear {
    stroke: rgba(255, 255, 255, 0.18);
    stroke-width: 1.5;
    stroke-dasharray: 5 6;
  }
  .curve-line {
    stroke: rgba(255, 255, 255, 0.92);
    stroke-width: 3;
    stroke-linecap: round;
    stroke-linejoin: round;
    filter: drop-shadow(0 3px 10px rgba(29, 185, 84, 0.32));
    transition: d 140ms ease-out;
  }
  .low-cut-line {
    stroke: rgba(255, 255, 255, 0.86);
    filter: drop-shadow(0 3px 10px rgba(86, 185, 255, 0.24));
  }
  .curve-marker {
    stroke: rgba(255, 255, 255, 0.72);
    stroke-width: 1.5;
    stroke-dasharray: 4 5;
  }
  .curve-marker-dot {
    fill: var(--text-primary);
    stroke: rgba(0, 0, 0, 0.45);
    stroke-width: 1;
    filter: drop-shadow(0 2px 6px rgba(255, 255, 255, 0.22));
  }
  .curve-row {
    grid-template-columns: 92px minmax(150px, 1fr) 52px;
  }
  .eq-sliders {
    position: relative;
    z-index: 1;
    display: grid;
    gap: 0.55rem;
    margin-top: 0.85rem;
  }
  .eq-sliders.disabled {
    opacity: 0.46;
  }
  .eq-row {
    display: grid;
    grid-template-columns: 46px minmax(0, 1fr) 44px;
    align-items: center;
    gap: 0.65rem;
    min-height: 30px;
  }
  .eq-band-label {
    display: flex;
    align-items: baseline;
    gap: 0.15rem;
    font-size: 0.78rem;
    font-weight: 600;
    color: var(--text-primary);
    line-height: 1;
  }
  .eq-band-label small {
    font-size: 0.62rem;
    font-weight: 500;
    color: var(--text-secondary);
  }
  .eq-value {
    font-size: 0.72rem;
    color: var(--text-secondary);
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .eq-row input[type='range'] {
    width: 100%;
    height: 18px;
    accent-color: var(--text-primary);
    cursor: pointer;
  }
  .eq-row input[type='range']:disabled {
    cursor: default;
  }
  .eq-reset {
    position: relative;
    z-index: 1;
    justify-self: start;
    margin-top: 0.7rem;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--text-secondary);
    font: inherit;
    font-size: 0.74rem;
    font-weight: 600;
    padding: 0.25rem 0;
    cursor: pointer;
  }
  .eq-reset:hover,
  .eq-reset:focus-visible {
    color: var(--text-primary);
  }
  .eq-reset:disabled {
    cursor: default;
    opacity: 0.5;
  }

  /* ── Errors ── */
  .errors {
    position: relative;
    z-index: 40;
    border-top: 1px solid var(--border);
    background: rgba(231, 76, 60, 0.07);
    padding: 0;
    max-height: 30vh;
    overflow: hidden;
    flex-shrink: 0;
  }
  .errors-toggle {
    width: 100%;
    min-height: 34px;
    border: 0;
    background: transparent;
    color: #e74c3c;
    font: inherit;
    font-size: 0.72rem;
    cursor: pointer;
    padding: 0.35rem 1rem;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 0.25rem;
    opacity: 0.8;
  }
  .errors-toggle:hover,
  .errors-toggle:focus-visible {
    color: var(--text-primary);
    opacity: 1;
  }
  .errors-toggle-icon {
    font-size: 1rem;
    line-height: 1;
  }
  .errors-body {
    max-height: calc(30vh - 34px);
    overflow-y: auto;
    padding: 0 1rem 0.6rem;
  }
  .err-item {
    border-radius: 7px;
  }
  .err-item.marked {
    background: rgba(231, 76, 60, 0.12);
    box-shadow: inset 0 0 0 1px rgba(231, 76, 60, 0.28);
  }
  .err-line {
    display: flex;
    align-items: flex-start;
    gap: 0.4rem;
    cursor: pointer;
    border-radius: 7px;
    padding: 0.15rem 0.35rem;
    -webkit-tap-highlight-color: transparent;
  }
  .err-line:hover,
  .err-line:focus-visible {
    background: rgba(255, 255, 255, 0.06);
    outline: none;
  }
  .err {
    flex: 1 1 auto;
    min-width: 0;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    font-size: 0.72rem;
    color: #e74c3c;
    line-height: 1.45;
    word-break: break-word;
    user-select: text;
    -webkit-user-select: text;
  }
  .err-copy {
    flex: 0 0 auto;
    font-size: 0.95rem;
    line-height: 1.45;
    color: var(--text-secondary);
    opacity: 0.45;
    transition: opacity 140ms ease, color 140ms ease;
  }
  .err-line:hover .err-copy,
  .err-line:focus-visible .err-copy {
    opacity: 0.9;
  }
  .err-item.copied .err-copy {
    color: #1db954;
    opacity: 1;
  }
  .err-hint {
    font-size: 0.72rem;
    color: var(--text-secondary);
    line-height: 1.45;
    padding: 0.15rem 0.35rem 0.5rem 0.6rem;
    border-left: 2px solid rgba(231, 76, 60, 0.35);
    margin: 0.1rem 0 0 0.45rem;
    user-select: text;
    -webkit-user-select: text;
  }
</style>
