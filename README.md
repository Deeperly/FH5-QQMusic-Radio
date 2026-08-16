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
supported. The recommended setup is to build both library repositories from
source and pass their repository roots to MSBuild. If all three repositories
are cloned side by side under the same parent directory, these are also the
project defaults and the two root overrides can be omitted.

```powershell
cd bridge/ui
npm ci
npm run build

cd ../..
msbuild bridge/bridge.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:LibrespotcRoot=C:\path\to\librespotclib `
  /p:AirplaycRoot=C:\path\to\airplayclib
```

Each root must contain the library's public `include` directory and a
Release/x64 static library in `bin` (`librespotc.lib` or `airplayc.lib`).

For a local vendored layout, copy only locally built headers and libraries to:

```text
bridge/vendor/librespotc/include/librespotc/librespotc.h
bridge/vendor/librespotc/lib/librespotc.lib
bridge/vendor/airplayc/include/airplayc/airplayc.h
bridge/vendor/airplayc/lib/airplayc.lib
```

Then build with `LibrespotcRoot` and `AirplaycRoot` pointing to those two vendor
directories and set `LibrespotcLibDir` and `AirplaycLibDir` to their respective
`lib` directories:

```powershell
msbuild bridge/bridge.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:LibrespotcRoot=C:\path\to\spotify-radio\bridge\vendor\librespotc `
  /p:LibrespotcLibDir=C:\path\to\spotify-radio\bridge\vendor\librespotc\lib `
  /p:AirplaycRoot=C:\path\to\spotify-radio\bridge\vendor\airplayc `
  /p:AirplaycLibDir=C:\path\to\spotify-radio\bridge\vendor\airplayc\lib
```

These vendor directories are ignored by Git: prebuilt
libraries are compiler-configuration-specific and are not part of this
source-only repository.

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
