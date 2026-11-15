# Architecture

This page is for people who want to change Tapeloop's code. It describes how the
plugin is split into modules, which thread runs what, how pictures travel from a
source to the replay buffer, to disk and back on air, and how the code is tested.

What the plugin does from the user's side is in the [user guide](user-guide.md), the
warnings it gives are in [troubleshooting](troubleshooting.md), and the bytes of the
replay files are in the [file format page](file-format.md). This page does not repeat
them.

## Modules

The sources live in `src`, one folder per module:

| Module | Folder | What it is | Depends on |
|---|---|---|---|
| core | `src/core` | Buffers, the replay library, the replay format, the store, export, the decode planner, the playhead, the encoder policy and the sequence state | The standard library only |
| decode | `src/decode` | The FFmpeg decoder and the MP4 writer | core, FFmpeg |
| obs | `src/obs` | The glue to libobs: captures, the replay source and its player, the composition on air, hotkeys, the frontend events | core, libobs, the interfaces of decode and ui |
| ui | `src/ui` | The Qt dock and its dialogs | core, Qt |
| plugin | `src/plugin-main.cpp` | The module entry | everything above |

core, decode and obs build as static libraries (`tapeloop-core`, `tapeloop-decode` and
`tapeloop-obs`). The plugin target compiles `src/plugin-main.cpp`, the ui sources and
`src/obs/FrontendBridge.cpp` itself, and links the three libraries. FrontendBridge is
the one obs file that needs the frontend API, which is why it is not in
`tapeloop-obs`.

The rules between them:

- core includes nothing from libobs, FFmpeg or Qt. That is what lets it be tested,
  fuzzed and measured as a plain C++ library.
- Only decode includes FFmpeg, always through `decode/FFmpegHeaders.hpp`. A configure
  check fails the build when a file includes an FFmpeg header without it.
- `tapeloop-obs` uses decode only through its header-only interfaces,
  `decode/Picture.hpp` and `decode/PictureDecoder.hpp`. The plugin entry creates the
  FFmpeg decoder and FrontendBridge the MP4 writer.
- ui never includes libobs. It reaches OBS through two interfaces it declares,
  `ui::DockBackend` and `ui::PictureSurface`, which obs implements
  (`ManagerDockBackend` and `SourceDisplay`) and the dock tests replace.

`obs_module_load` registers the capture output and the replay source type, creates the
FrontendBridge and adds the dock. The FrontendBridge owns the CaptureManager, the
SequencePlayer and the hotkeys, and answers OBS's frontend events.

No exception crosses into libobs. Every callback the plugin registers is `noexcept`
and catches what it calls, and the module exports, which `obs-module.h` declares
without `noexcept`, catch everything themselves.

FFmpeg is built and linked statically on Windows and Linux; CONTRIBUTING.md says how.
The macOS build has no FFmpeg, so it has no decoder and no MP4 export: its replay
sources draw nothing.

## Threads

### UI thread

OBS's main thread, where Qt runs. Almost everything that changes state runs here:

- the dock and its dialogs;
- every call into CaptureManager, which owns the captures, the buffers' lifecycle, the
  replay library, the store and the exporter;
- SequencePlayer and AirComposition, which own the replay source, the private
  transitions, the intro and outro, and the output channel;
- the frontend events, the save callback and the hotkey actions.

ReplayLibrary is not thread-safe and is only touched here. Work reaches the UI thread
from other threads through `obs_queue_task(OBS_TASK_UI, ...)`. Each such task carries
a `weak_ptr` to an `alive` token of the object it is for, and does nothing once that
object is gone.

Capture calls are never made from inside a render callback or with the graphics
context entered. SourceCapture's `stop()` waits, polling, until libobs has stopped its
output.

### Graphics thread

libobs's video thread renders every frame and runs the tick callbacks. The plugin runs
on it:

- the replay source's `video_tick` and `video_render`, which call the player's
  `tick()` and `render()` and draw the picture with the plugin's own effect,
  converting it to RGB;
- the dock's player picture, an `obs_display` that draws the replay source;
- three tick callbacks. The FrontendBridge's queues a poll to the UI thread once a
  second, which restarts captures that failed or whose source changed size, drops
  removed sources, collects the store's results and lets the library forget what the
  buffers no longer hold. The SequencePlayer's watches for the clip on air to reach
  its end and hands that to the UI thread. The AirComposition's, while it is on the
  channel, hands what its signals noted and the deadline of a part to the UI thread.

Decoding never runs here: a frame takes as long as it takes on the player's own
thread, and the picture on screen follows when it is ready.

### Encoder threads

Each buffered source has its own encoder, fed from an `obs_view` of its own. libobs
encodes on its threads and calls the capture output's packet callback on the encoder's
thread. The callback copies the packet into the source's SourceBuffer, because the
packet's data belongs to the encoder and is only valid during the call. On an encoder
error, or a packet that cannot be stored, it sets an atomic flag and stops the output;
the UI thread's poll sees the flag and restarts or reports.

SourceBuffer is the one structure shared with the encoder thread. A mutex guards its
ring of GOPs. Readers cut clips that share the GOPs (`shared_ptr<const Gop>`), so
eviction never invalidates a clip.

### The store's thread

ReplayStore runs every write to disk and every folder scan on one thread of its own at
low I/O priority, one job at a time in the order they were asked for. The UI thread
asks for jobs and collects their results with `poll()`. ReplayWriter is used only on
this thread.

### Each player's decoding thread

Every ReplayPlayer starts a decoding thread of its own. The decoder factory runs on
it, so whatever device the decoder opens is opened there, and the decode planner and
the GopReader that reads GOPs from disk belong to it. Controls and `load()` may come
from any thread: they go into a fixed array of commands under a mutex and take effect
at the next tick. Pictures in memory come back through three buffers (one being
filled, one being drawn, one ready); pictures on the GPU go through GpuPictures.

### The export thread

ReplayExporter runs exports on a thread of its own at low I/O priority, one at a time.
Requests and progress cross under a mutex, so it is safe to call from any thread.

### libobs hotkey and signal threads

libobs calls a hotkey with its hotkey lock held, on OBS's UI thread. The hotkey
handler does not act there: it queues a task to the graphics queue, which queues the
action to the UI queue, so the action runs on a later pass of the UI thread after the
lock is released, in the order the presses came.

The AirComposition listens to the `channel_change`, `media_started`, `media_ended` and
`transition_stop` signals, which come on whatever thread libobs raises them. Their
handlers only note what they saw under a mutex and queue a task to the UI thread. They
make no libobs call while that mutex is held, because `channel_change` comes with
libobs's channel lock held. A source's `remove` signal sets an atomic flag that the
next poll acts on.

The plugin runs no code on libobs's audio thread: the capture views mix no audio and
the plugin registers no audio processing.

## Buffers and storage

### Capturing a source

A SourceCapture renders one source on an `obs_view` of its own and feeds an encoder
from that view. The encoder is the first that starts among the candidates
EncoderPolicy ranks: the texture encoders of the vendor whose adapter OBS renders on
come first, then the other hardware encoders, then x264. The capture's output, an
encoded video output the plugin registers at load, gives the SourceBuffer the codec's
configuration when it starts, then every packet.

A GopBuilder groups packets into GOPs, each starting at a keyframe. A finished Gop is
immutable and carries its codec, H.264 or HEVC, and a shared copy of the codec
configuration. The SourceBuffer keeps a ring of them, bounded by a time window and a
byte limit, and evicts at keyframes; the newest GOP stays even when it alone is over
the limit. Once warm the builder does not allocate.

### The memory budget

All buffers share one budget, by default a quarter of physical memory, which the user
can set in MiB. When what the buffers need for their windows exceeds the budget, each
gets a share in proportion to its need. The user guide has the details under [Settings
and the RAM budget](user-guide.md#settings-and-the-ram-budget).

### Capturing a replay

A capture cuts a clip from every buffer, from now back to the longest window that
holds anything, and adds it to the ReplayLibrary. The library is only an index: the
pictures stay in the buffers, shared with the clips, until they are on disk.

The store then writes the capture in the background. ReplayWriter appends the GOPs not
stored yet to the source's open segment, flushes, writes the replay's manifest under a
temporary name and renames it. A GOP already stored by an earlier capture is not
written again. The capture tells the library where each GOP is on disk before it lets
go of the GOPs, so a replay played meanwhile always finds every GOP in the buffer or
in a file. Once written, a replay holds no picture in memory. The file layout is in
[the file format page](file-format.md).

### Reading back

The player sees a replay's source as a GopSource: the frames' times are known without
reading anything, and a GOP is read when decoding reaches it, so a long replay is
never all in memory. StoredGops takes a GOP from the buffer while it is still there,
and otherwise reads it from its segment through a GopReader of its own, which keeps
the GOPs read last in a cache of 64 MiB. A GopReader is not thread-safe: the planner
reads through it on the player's decoding thread, and the exporter reads through one
of its own on the export thread.

## Decoding and the player

### The playhead

Playhead is deterministic and never reads a clock: it is moved by the elapsed time it
is given. Speeds are in thousandths of real time, from 50 to 4000, negative for
reverse. Stepping pauses and moves by whole frames. Scrubbing runs at half speed for
the first 250 ms it is held, then speeds up to four times real time at 1500 ms. The
speed presets that the faster and slower controls move between are 250, 500, 750, 1000
and 2000.

### The decode planner

DecodePlanner decides what to feed a FrameDecoder so that the frame the playhead is on
is ready. Decoding starts at the keyframe of that frame's GOP and goes no further than
needed. The decoded frames of the last GOPs are kept, by default two GOPs within
512 MiB, so stepping back costs nothing while they fit. Playing in reverse decodes the
previous GOP ahead of time. A GOP is read from its source when a pass over it starts.

Runs, the stretches of a buffer from one start of its encoder, are told apart by their
codec and configuration bytes. A new run reopens the decoder. When a decoder reports
that it lost its device, the planner lets go of every frame and opens it again. Once
warm, the planner allocates nothing unless a GOP is longer than any before it. It runs
on the caller's thread, which in the plugin is the player's decoding thread.

### Decoders

`FrameDecoder` in core is the interface the planner drives: open, send, receive,
flush, reset, release and close, all `noexcept`, returning a status that tells a
missing frame, invalid data, an unsupported stream, memory running out and a lost
device apart. `decode::PictureDecoder` adds the picture of a decoded frame: I420 or
NV12 in memory, or an NV12 Direct3D 11 texture. One decoder belongs to one replay
player and is used from one thread.

FFmpegDecoder is the implementation. Given the LUID of a graphics adapter, it decodes
with D3D11VA on its own device on that adapter, and copies each frame it holds into a
texture of its own. Without one, on other platforms, or for a stream that adapter
cannot decode, FFmpeg decodes on the CPU. It uses slice threads, never frame threads.

### The GPU path and its fallback

On Windows, the plugin reads the adapter OBS renders on at load time and the replay
source's decoders decode there. Their pictures go to SharedPictures: two NV12 textures
shared between the decoder's device and OBS's, with a keyed mutex. The decoding thread
publishes into them, and the graphics thread latches the newest and draws it. When OBS
rebuilds its device, the player hands the frame on screen over again.

If the shared textures cannot be prepared for the decoder's pictures, the player makes
a new decoder that gives pictures in memory, picks the clip up at the frame the
playhead is on, logs the change once and does not try the GPU again. On other
platforms there is no GpuPictures, and decoders give pictures in memory from the
start.

## On air

A replay goes on air over the program, on output channel 32, without touching the
user's scenes. Channels draw in order over the program on channel 0. The frontend
keeps channels 1 to 6 for its audio devices, and the downstream keyer plugin starts at
7.

SequencePlayer owns a private replay source and plays one replay through it. Its order
comes from ReplayPlayerState in core: the intro if there is one, then every shown
source that has a clip, in the order of the sequence, then the outro, then live. The
next part starts once the clip played forward to its end. Each source starts from the
first frame of its clip at the speed the one before had. With nothing on air, the
current replay is cued: the replay source holds its first clip paused on its first
frame, for the dock's player and the controls, and nothing goes on the channel.

AirComposition puts the parts on the channel. The first part enters through the
"Going on air" transition when nothing was on air, each later part cuts from the one
before, and the way back to live goes through the "Back to live" transition. The
transitions are private copies of the ones the user picked, made ahead of their first
use. The intro and outro are private media or image sources. If another source takes
the channel, the composition leaves it and goes off air.

What the sequence answers, all on the UI thread:

| Event | What happens |
|---|---|
| Scene collection changing, scripting shutdown | The sequence closes, taking any replay off air at once, before the frontend empties the channels |
| Scene collection cleanup | The sequence closes, every capture stops and its buffer goes, and the on-air settings are applied again |
| Scene collection changed | The sequence opens and cues the current replay |
| Program transition stopped | If the program is now black, the replay is taken off air at once, without an outro or a way back |
| The buffers stopped | The replay goes back to live the way the user takes it off |
| Exit | The composition releases the sources it made, which belong to other modules, then the manager ends |

Of the other frontend events, streaming and recording starting and stopping go to the
CaptureManager, which can start and stop the buffers with them. The end of OBS's
loading loads the hotkeys' bindings from the profile and the replay library, and a
profile change loads the bindings again.

## Tests

| Suite | Folder | What it covers | Runs in CI as |
|---|---|---|---|
| Core tests | `tests/core` | The code in `src/core`, with a fake decoder and a synthetic encoder | `linux-gcc`, `linux-clang-asan`, `linux-clang-tsan`, `windows-msvc`, `macos` |
| Fuzzers | `tests/fuzz` | The source buffer, the playhead, the decode planner, the sequence state, the replay format, the parameter set filter, and decoding and exporting damaged replays | `linux-clang-fuzz`, a minute each |
| Coverage | `tests/core` | Line coverage of `src/core`, which must stay at 90% or above | `linux-clang-coverage` |
| libobs harness | `tests/obs` | Captures, decoding, the player, the picture renderer, the displays, the dock backend, the hotkeys and the manager, against a real libobs | `linux-obs` (with ASan and UBSan) and `windows-obs` |
| Dock tests | `tests/ui` | The dock, its dialogs and the player picture, with fake backends | `linux-ui` |
| clang-tidy | | `src/core`, `src/decode` and `src/obs` with `.clang-tidy`; the tests and fuzzers for braces only | the Analyze workflow |

[CONTRIBUTING.md](../CONTRIBUTING.md#tests) gives the commands for the core presets,
the fuzzers, the libobs harness on Linux, coverage and clang-tidy over the core. What
it does not cover follows.

Each test suite is a CMake project of its own with its own presets, and builds into
`build_tests/<preset>` at the top of the checkout. The core tests build `src/core`
with counters and a file hook that the tests read, so their library is not the one the
plugin links. The fuzz preset is the only core preset that builds FFmpeg and
`src/decode`, for the decode and export fuzzers.

The dock tests need Qt 6 Widgets (`qt6-base-dev` on Ubuntu) and no display, since they
set Qt's offscreen platform themselves:

```
cd tests/ui
cmake --workflow --preset linux-ui
```

The libobs harness on Windows builds against the libobs in `.deps`, which the plugin's
configure builds with its Direct3D 11 renderer and obs-x264 when asked. It needs the
MSYS2 tools CONTRIBUTING.md lists for FFmpeg:

```
cmake --preset windows-ci-x64 -DTAPELOOP_OBS_TEST_RUNTIME=ON
cd tests/obs
cmake --preset windows-obs
cmake --build --preset windows-obs
ctest --preset windows-obs
```

clang-tidy also reads `src/decode` and `src/obs`, from the compile database of the
`linux-obs-tidy` preset, which compiles FrontendBridge as well. The database lists
only the files Linux compiles. With libobs built as CONTRIBUTING.md says:

```
cd tests/obs
export TAPELOOP_LIBOBS_PREFIX=$HOME/libobs
cmake --preset linux-obs-tidy
cd ../..
jq -r '.[].file' build_tests/linux-obs-tidy/compile_commands.json |
  grep -E '/src/(decode|obs)/' | sort -u |
  xargs clang-tidy -p build_tests/linux-obs-tidy
```
