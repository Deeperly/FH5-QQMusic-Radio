# spotify-radio

An unofficial source-only Forza Horizon 5/6 mod that replaces the Streamer
Mode radio audio path with Spotify Connect, AirPlay, local files, or internet
radio. Playback is controlled from a LAN web interface.

The LAN control API has no authentication. Run it only on a trusted local
network and do not expose its port to the internet.

This repository is published as an archival, unmaintained release. It is not
affiliated with Microsoft, Playground Games, Spotify, or Apple. Game updates
can invalidate build-specific integration points. Use it at your own risk.

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
