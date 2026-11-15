# Troubleshooting

This page lists what the Tapeloop dock and the OBS log can tell you while you test an
alpha, what each message means and what to do about it. Tapeloop is not released and
everything here may change. So far only NVENC on an NVIDIA card has been tried, and
nothing has run in OBS on a Mac.

The [user guide](user-guide.md) explains how the plugin works and what each setting
does. This page links to it instead of repeating it.

## Where the logs are

OBS writes one log file for each session, and Tapeloop writes into it. Every line
Tapeloop writes starts with `[tapeloop]`.

- Help, then Log Files, then "View Current Log" opens the log of this session.
- Help, then Log Files, then "Show Log Files" opens the folder with the logs of the
  last ten or so sessions; OBS deletes older ones.
- The folder on Windows is `%APPDATA%\obs-studio\logs`.
- A portable OBS keeps it in `config\obs-studio\logs` inside its own folder, beside
  `bin`.
- Crash reports are in the `crashes` folder next to `logs`. Help, then Crash Reports,
  then "Show Crash Reports" opens it.

OBS starts a new log file each time it starts. If a problem happened in an earlier
session, take that session's file, not the newest one.

## What to attach to a bug report

Report at https://github.com/Vraeh/tapeloop and attach:

- the log file of the session where it happened, as a file;
- the steps that led to it, and what you expected;
- the OBS version, the Windows version and the graphics card with its driver
  version;
- the Tapeloop version, which the log gives in its first Tapeloop line (see
  "Loading").

Please do not use the upload buttons of OBS's log menu. Attach the file instead.

## Loading

- `[tapeloop] plugin loaded successfully (version %s)` appears once when OBS starts.
  If it is missing, the plugin did not load: check the install steps in the
  [user guide](user-guide.md#installing) and the log for lines about `tapeloop`.
- `[tapeloop] decoding with FFmpeg %s` follows it in builds that include FFmpeg.
  Without it the build has no decoder: replays show no picture in the player or on
  air, and MP4 export is not offered.
- `[tapeloop] could not start` (error): the plugin stopped while loading and gave
  up, so there is no dock. The code gives no remedy. Send the log.
- `[tapeloop] could not stop cleanly` (error): something failed while OBS closed the
  plugin. Nothing to do; send the log if OBS misbehaved around it.
- `[tapeloop] plugin unloaded` appears when OBS closes.

## Settings

- `[tapeloop] This scene collection holds settings of another version; changes made
  here are not saved`: the collection was saved by a version of Tapeloop this one does
  not know. Changes you make in the dock are not kept.
- `[tapeloop] This scene collection has settings of version %lld, which this version
  does not know; they are kept as they are and the defaults apply`: the same cause.
  The stored settings are left untouched and the defaults are used meanwhile.
- `[tapeloop] Found the saved source '%s' again by its name` is informational: a
  selected source whose identity changed was matched by its name.
- `[tapeloop] The selected source '%s' is not in this scene collection` is
  informational: a source you selected earlier was deleted, and no other source has
  its name. Select its replacement if you still want a buffer for it. Renaming a
  source does not cause this.
- `[tapeloop] Saving or loading the settings failed` (error): the settings of the
  scene collection were not saved or read. Send the log.

Dock texts:

- "This is more than half of this computer's memory: OBS and the other programs may
  run short of it." appears under "Memory for buffers" when the value is too high.
  Lower it. See "Settings and the RAM budget" in the
  [user guide](user-guide.md#settings-and-the-ram-budget).
- "The selected sources need about %1 MiB for their buffer lengths, more than the %2
  MiB the buffers may use, so each buffer holds less time than its length. Shorter
  lengths, fewer sources or more memory for buffers in the advanced settings make
  room." The dock itself gives the remedy. The tooltip "This buffer holds less than
  its length: the buffers together need more memory than they may use, which the
  advanced settings set. The seconds it shows are the ones it holds." marks the
  buffers that hold less.

## Buffers and encoders

The dock shows one status for each selected source: "Stopped", "%1 s, %2 MB" while it
runs, "Waiting for a picture" and "Encoder failed".

Waiting for a picture:

- Tooltip: "This source has no picture until something shows it, as display, window
  and game captures do. Tapeloop shows it on its own and starts its buffer as soon as
  it has one." Log: `[tapeloop] '%s' has no size yet; showing it and waiting for a
  picture`. Nothing to do. If it stays, the source has no picture; check the source
  in OBS.

Encoder failed:

- "Encoder failed" in the status, and `[tapeloop] %s failed while capturing '%s',
  which tries H.264 first from its next start` in the log when the encoder was HEVC.
  The tooltip says: "This source's HEVC encoder failed while it ran. When its buffer
  starts again, it tries the H.264 encoder of the same graphics card first, and goes
  on doing so until OBS closes. A buffer starts again when it is started, or when its
  source is unticked and ticked." After that, the source shows "This source's HEVC
  encoder failed earlier, so until OBS closes it tries the H.264 encoder of the same
  graphics card first." For any other encoder, start the buffers again or untick and
  tick the source: it tries the same encoders again. If it fails again, send the log.
  The status can also mean the buffer could not store what the encoder gave it, which
  the log may not mention.

Slower paths. These work but cost more CPU time or memory bandwidth. The dock shows
an information icon with a tooltip, and the log has a warning:

- "This source is encoded on the CPU by x264, because no hardware encoder could take
  it. Its replays work, but it costs CPU time: it is not the optimal path." Log: `'%s'
  is encoded by %s on the CPU because no hardware encoder could take it. Replays
  work, but this costs CPU time: it is not the optimal path`.
- "This source is encoded on the CPU by x264, the replay encoder chosen in the
  advanced settings. ..." Log: `'%s' is encoded by %s on the CPU, the encoder chosen
  for replays. ...`. You chose it. Set "Replay encoder" back to "Automatic
  (recommended)", then stop and start the buffers, or untick and tick the source: a
  running buffer keeps its encoder until its next start.
- "This source's encoder takes every frame read back through memory instead of OBS's
  textures. ..." Log: `'%s' is encoded by %s, which takes every frame read back
  through memory instead of OBS's textures. Replays work, but this costs CPU time
  and memory bandwidth: it is not the optimal path`.
- "This source's encoder runs on another graphics card than the one OBS renders on,
  so every frame is read back through memory for it. ..." Log: `'%s' is encoded by
  %s, which runs on another graphics card than the one OBS renders on, ...`.
  "Allow encoders on another graphics card" in the advanced settings controls whether
  the automatic order may use such an encoder. Change it, then stop and start the
  buffers, or untick and tick the source: a running buffer keeps its encoder until its
  next start.
- "This source's encoder cannot take OBS's textures, so every frame is read back
  through memory for it. ..." Log: `'%s' is encoded by %s, which cannot take OBS's
  textures, so every frame is read back through memory for it. ...`.
- "OBS cannot hand this source's encoder its frames as textures with this graphics
  card or renderer, so every frame is read back through memory for it. ..." Log:
  `'%s' is encoded by %s, but OBS cannot hand it textures with this graphics card or
  renderer, ...`.
- "This encoder is the one chosen in the advanced settings; Automatic tries the
  encoders of the graphics card OBS renders on first." You chose a path that is not
  the fastest.

For all of these the code offers no further remedy than "Automatic" and the settings
named above. If you expected a hardware encoder to be used, send the log: it names
the encoder in the `Capturing` line below.

Choosing an encoder:

- `[tapeloop] Capturing '%s' at %ux%u with %s` is informational and names the
  encoder of each source.
- `[tapeloop] %s could not encode '%s'%s%s` is informational: that encoder failed
  to start and the next one in the order is tried. The reason follows when OBS gave
  one.
- "The replay encoder chosen in the advanced settings could not start for this
  source, or is not available on this computer, so it is encoded by the next one in
  the automatic order. The OBS log says why." Log: `%s, the encoder chosen for
  replays, could not start for '%s', which %s encodes instead, the next encoder in
  the automatic order` or `%s, the encoder chosen for replays, is not available, so
  %s encodes '%s' instead, the next encoder in the automatic order`. Pick another
  encoder or "Automatic", then stop and start the buffers, or untick and tick the
  source: a running buffer keeps its encoder until its next start.
- `[tapeloop] No encoder could start for '%s'` and `[tapeloop] No encoder can
  capture '%s'`: nothing could encode the source, so it has no buffer. Send the log.
  `No encoder can capture` means no usable encoder was found at all; for `No encoder
  could start`, the `could not encode` lines above it name the encoders that failed to
  start. An encoder OBS could not create is skipped without one.

Other reasons a buffer does not start:

- `[tapeloop] '%s' is %ux%u, larger than the %u pixels a capture allows`: the
  source is too big for a buffer.
- `[tapeloop] '%s' is %ux%u, too small to encode at height %u`: the source itself is
  too small or too narrow to encode (under 2 pixels on a side at the replay's height).
  Replays are never scaled up, so a higher Resolution helps only a source many times
  taller than it is wide; check the source's size and crop.
- `[tapeloop] OBS has no video, not capturing '%s'`: OBS had no video to take the
  picture from. The code gives no remedy; send the log.
- `[tapeloop] Could not create a view of '%s' at %ux%u`: the code gives no remedy.
  Send the log.
- `[tapeloop] Out of memory while starting the capture of '%s'` (error): the
  computer ran out of memory. The code gives no remedy; see "Memory for buffers"
  in the [user guide](user-guide.md#settings-and-the-ram-budget) and send the log.
- `[tapeloop] A captured source changed size, restarting its capture` is
  informational.
- `[tapeloop] Still waiting for the encoder to stop after %lld s` repeats every five
  seconds while a buffer stops. If it goes on, OBS may look stuck: send the log.

A source that restarts when it becomes active:

- Tooltip: "This source restarts, unpauses or refreshes when it becomes active, so
  Tapeloop does not keep it active while it is off air: ..." It is explained in the
  user guide under "Choosing the sources to buffer" and "Activate selected sources
  that are not on air". Nothing is wrong.

## Capture and saving

- `[tapeloop] Captured replay %llu from %zu sources, %zu of them with nothing in
  range` appears when you capture. The last number counts sources with no footage
  in the range.
- `[tapeloop] Replays of these buffers go to '%s'` names the broadcast folder.
- `[tapeloop] Replay %llu saved as '%s': %llu bytes written, %zu GOPs new, %zu on
  disk already` confirms the replay is on disk.
- "%1, %2 sources, not saved" in the list with the tooltip "This replay could not be
  saved on disk; the OBS log says why. It plays only while the buffers still hold
  it." The log line is one of:
  - `Replay %llu is not saved: OBS has no recording folder`. OBS has no recording
    path to put the replay in; set one in OBS.
  - `Replay %llu could not be saved: it could not be handed to the writer`. The code
    gives no remedy. Send the log.
  - `Replay %llu could not be saved: %s`. The reason follows; it often names the
    folder or the disk. If it does not explain itself, send the log.
- `[tapeloop] The disk falls behind: the writer has %zu replays, tag edits or scans
  to do, and every replay among them keeps its footage in memory`: the disk
  is writing slower than replays are captured, and the unsaved ones stay in memory
  meanwhile. The code gives no remedy; send the log if it persists.
- `[tapeloop] Replays are written at normal I/O priority: the system refused a lower
  one`: replays still save, written at the normal priority.

## Replay list and files

- `[tapeloop] Found %zu replays on disk` appears after the folder is read.
- `[tapeloop] The replays on disk could not be read: %s`: the folder could not be
  scanned. The reason follows; send the log if it does not explain itself.
- `[tapeloop] Reading the replays on disk: %s`: one file or folder of the scan gave an
  error, and the scan went on.
- `[tapeloop] The replay '%s' is damaged` and, in the list, "%1, damaged" with the
  tooltip "The file of this replay is damaged, or a part of it is gone, so it cannot
  play." It cannot be repaired from the dock. Check that the whole broadcast folder
  is still there.
- `[tapeloop] Deleted '%s', which a crash left half written` is informational.
- `[tapeloop] The tags of '%s' could not be saved: %s`: the tag edit did not reach
  the disk. Check the reason that follows.

A `.tplp` file is listed only when it sits in a folder inside the `Tapeloop` folder,
such as `Tapeloop\Imported\`, after OBS starts or after the buffers next start with a
new recording folder; see "Export" in the [user guide](user-guide.md#export).

## Any time

- `[tapeloop] A hotkey failed` (error), `[tapeloop] A task on the UI thread failed`
  (error), `[tapeloop] Handling a frontend event failed` (error): something went wrong
  inside Tapeloop and was caught, and these can come from any area. Note what you
  pressed and send the log.

## Playback

- `[tapeloop] A replay player cannot show pictures decoded on the GPU, so it decodes
  them in memory from now on. Replays work, but this costs CPU time: it is not the
  optimal path`. From then on replays decode on the CPU, in the player and on
  air, until OBS restarts.
- `[tapeloop] This graphics card or OBS's renderer gives no shared NV12 textures, so
  replay pictures are copied through memory. Replays work, but this costs CPU time
  and memory bandwidth: it is not the optimal path`.
- `[tapeloop] Shared NV12 textures for replay pictures could not be made, so pictures
  are copied through memory until they can be`.
- `[tapeloop] A replay picture could not be handed over on the GPU: %s %s`, and the
  same with `, as it did %llu more times since this was last said`: the log repeats
  it at a limited rate. If playback stutters or freezes, send the log.
- `[tapeloop] The picture effect does not compile: %s` (error): the picture effect of
  the replay player could not be built. Send the log with your graphics card and
  driver.
- `[tapeloop] Going on to the next source failed` (error): something went wrong
  inside Tapeloop and was caught. Note what you pressed and send the log.

## On air

Dock messages:

- "Another source is on the replay's channel, so the replay did not go on air."
  Log: `Another source is on the replay's channel, so replay %llu cannot go on air`.
  Something else uses the output channel the replay needs. See "Putting a replay on
  air" in the [user guide](user-guide.md#putting-a-replay-on-air).
- "That replay cannot be read, so it did not go on air." Log: `Replay %llu cannot be
  read, so it cannot go on air`. Check the replay in the list; its files may be gone.
- "That replay is damaged, so it did not go on air." See "Replay list and files".
- "The intro is missing or cannot be opened, so replays go on air without it: %1" and
  the same for the outro. The path follows. Check the file in the on-air settings; see
  "Intro and outro" in the [user guide](user-guide.md#intro-and-outro).

Log lines:

- `[tapeloop] Another source took the replay's channel, so the replay is off air` is
  informational.
- `[tapeloop] OBS has no transition named %s, so the replay fades`: the transition
  you chose in the on-air settings is gone from OBS. The settings list it as "%1 (not
  found)".
- `[tapeloop] OBS has no %s, so the replay cuts`: OBS could not make the transition.
- `[tapeloop] A transition is not ready yet, so it may run short this once` is a
  one-off.
- `[tapeloop] OBS has no %s, so the replay goes on air without %s` and
  `[tapeloop] %s is not there, so the replay goes on air without %s`: the intro or
  outro could not be made, or its file is missing.
- `[tapeloop] Following the replay on air failed` (error) and
  `[tapeloop] Letting go of the replay failed as OBS exits` (error): send the log.

## Export

- "Exporting %1: %2%" and "Exporting %1: %2%, %3 more waiting" are progress, and
  "Exported %1 to %2" says where the files are.
- "Could not export %1: %2". The reason follows. The log has the same: `[tapeloop]
  The export of '%s' failed: %s`. A build without FFmpeg shows no Export to MP4
  button; see "Loading". Otherwise the code gives no remedy: send the log.
- `[tapeloop] The export of '%s' was not started, OBS is closing`: the export waited
  while you closed OBS. Start it again next session.
- `[tapeloop] Exporting '%s' to MP4` or `as a replay file`, and `Exported '%s' to 1
  file in '%s'` or `to %zu files in '%s'`, are informational.

The export buttons stay greyed out for a replay that is saving, not saved or damaged.
A saved replay whose files cannot be read fails with "Could not export %1: %2". See
"Capture and saving" and "Replay list and files", and also "Export" in the
[user guide](user-guide.md#export).
