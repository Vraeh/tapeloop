# Tapeloop

Instant replay for OBS Studio. Tapeloop keeps a rolling buffer of the sources you
choose, so when something happens you can cut back to it from any angle, at any speed.

It is being built for live amateur sports production, where a dedicated replay server
is out of budget and the existing OBS options either fill the RAM with raw frames or
are not stable enough to trust during a match.

## Status

Early development. Nothing is usable yet and there are no releases.

## Goals for the first release

- Replay buffers for any video source, encoded on the GPU and kept in memory, never
  written to disk.
- Buffer length and replay resolution set by the user.
- Playback controls: speed changes while the replay is running, reverse, frame
  stepping and hold-to-scrub.
- A list of marked moments to choose from.
- Audio modes: replay audio only, replay audio over the program, or program only.
- Keyboard hotkeys. Stream Deck and MIDI controllers come later.

## Requirements

- OBS Studio 32.0 or newer.
- Windows 10 or 11, x64. macOS on Apple Silicon is planned.
- An NVIDIA GPU with NVENC. Intel Quick Sync and AMD AMF are planned.

## Building

Tapeloop uses the build system of the
[OBS plugin template](https://github.com/obsproject/obs-plugintemplate). The first
configure run downloads the OBS sources and prebuilt dependencies listed in
`buildspec.json` into `.deps/`.

Windows, with Visual Studio 2022 and CMake 3.28 or newer:

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

macOS, with Xcode 16 and CMake 3.28 or newer:

```
cmake --preset macos
cmake --build --preset macos
```

See [CONTRIBUTING.md](CONTRIBUTING.md) before sending changes.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE).

The Windows and Linux builds include [FFmpeg](https://ffmpeg.org) 8.1.3, linked
statically for decoding, under the LGPL 2.1 or later. Its notice, with the source and the
exact configure options, and its license are installed with the plugin's data
(`FFmpeg-NOTICE.txt`, `FFmpeg-LICENSE.txt`); `cmake/ffmpeg/BuildFFmpeg.cmake` builds it.
