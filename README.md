# FH5 QQ Music Radio

An unofficial source-only Forza Horizon 5/6 mod that replaces the Streamer
Mode radio audio path with QQ Music, Spotify Connect, AirPlay, local files, or
internet radio. Playback is controlled from a LAN web interface.

This snapshot adds a QQ Music process-loopback source. It captures QQ Music at
48 kHz stereo float32, injects it into the native FMOD radio path, follows the
game's radio effects and volume, switches QQ Music tracks when the in-game
station changes, and shows the current QQ Music title and artist in the radio
UI.

The LAN control API has no authentication. Run it only on a trusted local
network and do not expose its port to the internet.

This repository is published as an archival, unmaintained release. It is not
affiliated with Microsoft, Playground Games, Spotify, or Apple. Game updates
can invalidate build-specific integration points. Use it at your own risk.

## QQ Music setup

QQ Music integration is experimental and was validated with a specific Forza
Horizon 5 build. Game updates can break its memory/FMOD integration.

Requirements:

- Windows 10/11
- Forza Horizon 5
- QQ Music for Windows
- Steam Streaming Speakers virtual output device
- Python 3.10+ with `winappaudiorouter` and `pycaw`

Steps:

1. Build `bridge/bin/version.dll` using the instructions below.
2. Copy it into your Forza Horizon 5 game folder.
3. Start the game and open `http://127.0.0.1:8103`.
4. Set `activeSource` to `qqmusic`.
5. While FH5 is running, route only QQ Music to Steam Streaming Speakers:

```powershell
python -m pip install winappaudiorouter pycaw
@'
import winappaudiorouter as router
print(router.set_app_output_device(
    process_name="QQMusic.exe", device="Steam Streaming Speakers"))
'@ | python -
```

`bridge/tools/qqmusic_router.py` can run in the background and switch QQ Music
between Steam Streaming Speakers while FH5 is running and the system default
device when FH5 exits. It writes its log to
`%LOCALAPPDATA%\FH5-QQMusic-Radio\router.log` by default; override that path
with `FH5_QQMUSIC_ROUTER_LOG`.

## Build

Requirements: Windows 10/11, Visual Studio 2022 with the v143 C++ toolset,
Windows SDK, Node.js, and clean Release/x64 builds of:

- [librespotclib](https://github.com/matkhl/librespotclib)
- [airplayclib](https://github.com/matkhl/airplayclib)

The mod links both libraries statically. Build all three projects with the v143
toolset and the static multithreaded runtime (`/MT`); mixing CRT variants is not
supported.

The easiest manual Visual Studio setup is:

1. Open and build `librespotclib.sln` in `Release|x64`.
2. Open and build `airplayclib.sln` in `Release|x64`.
3. Copy the resulting public headers and static libraries into this repository:

```text
librespotclib/include/librespotc/librespotc.h -> bridge/vendor/librespotc/librespotc.h
librespotclib/bin/librespotc.lib               -> bridge/vendor/lib/librespotc.lib
airplayclib/include/airplayc/airplayc.h        -> bridge/vendor/airplayc/airplayc.h
airplayclib/bin/airplayc.lib                   -> bridge/vendor/lib/airplayc.lib
```

Those destination directories are present in a clean checkout and already
configured as include and library search paths. The copied headers and binaries
remain ignored by Git.

If all three repositories are cloned side by side under the same parent
directory, copying is unnecessary: the project also searches the two library
repositories' `include` and `bin` directories by default. Arbitrary layouts can
use `LibrespotcRoot` and `AirplaycRoot` overrides.

```powershell
cd bridge/ui
npm ci
npm run build

cd ../..
msbuild bridge/bridge.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:LibrespotcRoot=C:\path\to\librespotclib `
  /p:AirplaycRoot=C:\path\to\airplayclib
```

Each overridden root must contain the library's public `include` directory and
a Release/x64 static library in `bin` (`librespotc.lib` or `airplayc.lib`). For
nonstandard output directories, also set `LibrespotcLibDir` or
`AirplaycLibDir`.

The DLL is written to `bridge/bin/version.dll`. No compiled dependencies or
game files are included in this source repository.

## Status

The final public snapshot was build-checked from a clean repository. Runtime
compatibility still depends on the game build and must be verified manually.
Issues and pull requests may not receive a response.

## License

GPL-3.0-only. See `LICENSE` and `THIRD_PARTY_NOTICES.md` for contributor and
third-party terms.

Copyright (c) 2026 matkhl.
