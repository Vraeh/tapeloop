# Tapeloop

Instant replay for OBS Studio. Tapeloop keeps a rolling buffer of the sources you
choose, so when something happens you can cut back to it from any angle, at any speed.

It is being built for live amateur sports production, where a dedicated replay server
is out of budget and the existing OBS options either fill the RAM with raw frames or
are not stable enough to trust during a match.

## Status

Early development. There are no releases yet. The first test releases will be alphas,
and everything here is provisional until then: settings, names and file formats can
still change.

## What works in the development branch

- A buffer for each source you choose, encoded by a hardware encoder where one can
  take the source and by x264 on the CPU otherwise. Buffers hold 10 to 300 seconds
  (60 by default), at the canvas resolution, the output resolution or a fixed height,
  with their own length and resolution per source if you want. The buffers can follow
  streaming and recording, and a memory limit (a quarter of the computer's memory by
  default) shortens them evenly if the selected sources need more.
- Encoder choice: automatic, which tries the encoders of the graphics card OBS renders
  on first, or one picked in the advanced settings. The code knows NVENC, Quick Sync,
  AMF, Apple VideoToolbox and x264. Replays are encoded in HEVC where the card can and
  in H.264 elsewhere, or in H.264 only if you choose. The encoders you stream and
  record with are never touched.
- Capture replay: a hotkey or a button keeps everything each buffer holds as a replay.
  Replays are written to disk as they are captured, in a Tapeloop folder inside the
  OBS recording folder, and stay across restarts. The buffers keep recording.
- A list of replays, newest first, with tags you can filter by.
- A player with play and pause, reverse, speed from 0.05x to 4x, frame stepping and
  hold-to-scrub, in the dock or in its own window. Playback reads from the buffer
  while it still holds the replay and from disk afterwards.
- A sequence: the sources shown in the replay play one after another, in the order
  you set.
- On air: a replay goes on air over the program and back to live, with a transition,
  and an optional intro and outro (video or image) set in the on-air settings.
- Export: each source of a replay to an MP4 file without encoding it again, or the
  whole replay to a single `.tplp` file that plays without its folder.
- Hotkeys, set in the OBS hotkey settings, for starting and stopping the buffers,
  capturing, going on and off air, play and pause, reverse, frames, speed, scrubbing,
  skipping to the next source and moving between replays.

## Not there yet

- Replay audio. Replays play without sound for now.
- Overlays on replays.
- Stream Deck and MIDI controls.
- On macOS the player draws nothing and MP4 export is not offered, because that build
  has no FFmpeg yet. Replays are still captured and saved there.

## Requirements

- OBS Studio 32.0 or newer.
- Windows 10 or 11, x64. macOS on Apple Silicon is planned.
- A hardware encoder is recommended: NVIDIA NVENC, Intel Quick Sync or AMD AMF.
  Without one Tapeloop falls back to x264, which costs CPU time.

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

The Windows and Linux builds include [FFmpeg](https://ffmpeg.org) 8.1, linked
statically for decoding and for exporting replays to MP4, under the LGPL 2.1 or later.
Its notice, with the exact version, the source and the configure options, its license,
FFmpeg's own account of its licensing, the notices of the few files under other licenses
and the Independent JPEG Group's READMEs are installed with the plugin's data
(`FFmpeg-NOTICE.txt`, `FFmpeg-LICENSE.txt`, `FFmpeg-LICENSE.md`, `FFmpeg-THIRD-PARTY.txt`,
`FFmpeg-IJG-README-6b.txt`, `FFmpeg-IJG-README-4.txt`); `cmake/ffmpeg/BuildFFmpeg.cmake`
builds it. Every release carries the exact FFmpeg source it was built from.
