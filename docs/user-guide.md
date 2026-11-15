# Tapeloop user guide

This guide is for the people testing the first alphas. It follows a match from
installing the plugin to putting a replay on air.

Tapeloop is in early development. The names, texts and defaults quoted here are
provisional and may change before the first stable release, and so may the way
settings and replay files are saved. Where a section says what a setting starts at,
that is the default of this version.

## Installing

Tapeloop needs OBS Studio 32.0 or newer on Windows 10 or 11, x64.

The Windows package is a zip with one folder, `tapeloop`. It holds `bin\64bit` (the
plugin) and `data` (texts and license notices). To install it, close OBS and copy the
`tapeloop` folder into `C:\ProgramData\obs-studio\plugins`, creating the `plugins`
folder if it is not there. Start OBS, open the Docks menu and tick Tapeloop. OBS adds
a new dock hidden and as a floating window, so tick it once and drag it into place.

A hardware encoder is recommended. NVIDIA NVENC is the only one tried so far. Intel
Quick Sync and AMD AMF can be used, but have not been tried. Without a usable
hardware encoder Tapeloop falls back to x264, which costs CPU time.

The macOS build has not been run in OBS on a Mac. It has no FFmpeg, so it cannot play
replays or export them to MP4.

## How it works

Each source you choose gets a buffer: a rolling, compressed copy of its last seconds,
encoded by a hardware encoder where one can take the source. The encoders you stream
and record with are never touched.

Capturing a replay keeps everything each buffer holds at that moment. The replay is
written to disk as it is captured, and the buffers keep recording. A replay has one
clip per source, so you can show the same moment from every angle.

## Choosing the sources to buffer

The dock lists the video sources of the current scene collection. Each row has
three columns, Source, Shown and Buffer.

- Tick a source's name to give it a buffer.
- Shown decides whether the source plays when the replay goes on air (see "The
  sequence").
- Buffer says what the buffer is doing: "Stopped", "Waiting for a picture"
  (display, window and game captures have no picture until something shows them),
  "Encoder failed", or the seconds and megabytes it holds, as "%1 s, %2 MB". A
  tooltip on the status says more when a buffer is not on the best path, for
  instance when it is encoded by x264.

Below the list are the settings every buffer shares:

| Setting | What it does |
|---|---|
| Buffer length | 10 to 300 seconds, 60 by default. |
| Resolution | The height replays are kept at: "Canvas" (the default), "Output (scaled)" or a fixed 360p, 480p, 720p, 1080p or 2160p. |
| Start with streaming or recording | On by default. The buffers run while streaming or recording runs. |
| Activate selected sources that are not on air | Off by default. Keeps sources active while they are buffered, for sources that only run while active. Its tooltip lists the sources it leaves out. |

Resolution gives a target height, never above the source's own height, and the
source keeps its shape. A 720p source set to 1080p stays 720p.

"Source settings..." (or Enter on a row) opens "Settings for" that source, where
"Own buffer length", "Own resolution" and "Own activation off air" replace the
shared setting for that source alone.

Changes reach a running buffer at its next start, except activation, "Memory for
buffers" and "Start with streaming or recording", which apply at once.

## Starting the buffers

With "Start with streaming or recording" on, the buffers start when you start
streaming or recording and stop when both have stopped. While either runs, the "Start
buffers" and "Stop buffers" button is disabled and the dock says "The buffers follow
streaming and recording while either runs." While neither runs, the button and the
hotkey "Tapeloop: Start or stop buffers" start and stop them by hand, for instance to
try them before the match.

With it off, only the button or the hotkey moves them, at any time.

Each start names a new broadcast, a folder for the replays captured until the next
start.

## Capturing a replay

Press "Capture replay" or the hotkey "Tapeloop: Capture replay". A new replay selects
itself in the list and is cued in the player.

Each source's clip reaches back as far as its own buffer holds, so the replay is as
long as the longest buffer. A source whose buffer holds nothing is left out.

Replays are written to a folder named after the scene collection and the start time
of the broadcast, inside a `Tapeloop` folder in the recording folder of OBS. Without
a recording folder in OBS the replay is not saved, it is listed as "not saved", and it
plays only while the buffers still hold it. Replays stay on disk across restarts.

After they stop, the buffers go on dropping their oldest part as if they were
running, so a replay captured just after the stream or recording ends still holds the
last moments, less the time since the stop. A buffer length after the stop they hold
nothing. Once a buffer starts again, it starts empty.

## The replay list

The list shows the replays of every broadcast, newest first, under a heading row for
each broadcast. The one selected is the one that goes on air next. A replay that
cannot play is listed as damaged and cannot be selected. While one is being written
the list says "saving".

The hotkeys "Tapeloop: Newer replay" and "Tapeloop: Older replay" move through
every replay, newest to oldest, whatever the filter shows, and skip damaged replays.
A replay the filter hides can be the current one: it is then not selected in the
list, but it is still the one that goes on air next.

## Tags

Tags help to find a replay later: type a name in the "Tag" field and press "Add tag"
(or Enter) to put it on the selected replay, which creates the tag if it is new.
"Remove tag" takes the tag typed in the field off the selected replay. Tags show
after the replay in the list, as `#name`.

A tag is trimmed of spaces and tabs at both ends, cannot be empty or hold control
characters, and is matched without regard to case for the letters A to Z: typing
"Goal" when "goal" exists uses "goal". Accented letters are matched as typed, so
"Ñuble" and "ñuble" are two tags. All the tags of one replay must fit in about 4 KB.

The filter above the list ("All replays" or a tag) shows only the replays that carry
the tag. "Delete tag" takes the filtered tag off every replay and deletes it, after
asking. The replays stay. Tags are saved with the replay.

## The player

The mini player has a position slider, a line of status ("Cued" or "On air", "playing"
or "paused", the speed and the position in seconds), and these buttons: "<< Hold",
"< Frame", "Play" (which reads "Pause" while playing), "Frame >", "Hold >>",
"Reverse", "Slower", "1x" and "Faster".

- The speeds are 0.25x, 0.5x, 0.75x, 1x and 2x. "Slower" and "Faster" move between
  them and "1x" returns to normal speed.
- "Reverse" turns the direction around, keeping the speed.
- "< Frame" and "Frame >" step one frame.
- "<< Hold" and "Hold >>" scrub while held. A scrub starts at 0.5x, speeds up after a
  quarter of a second and reaches 4x after a second and a half, and stops when
  released.

Off air the player shows the selected replay cued on its first shown source, and
the controls work on it, so you can find the moment before going on air. The replay
goes on air from where the player stands, playing forward at the speed it has, even
when the player was paused or in reverse.

"Mini player" puts the player "In the dock" or "In its own window" (shown with "Show
player"). "Player shows" picks "Controls and picture", "Controls only" or "Picture
only". Both are saved with the scene collection.

Playback reads from the buffer while it still holds the replay and from disk after.

## The sequence

When a replay goes on air, its sources play one after another, top of the list
first. The order is the order of the rows in the dock: drag a row to move it. Only
the sources with Shown ticked play, and a source with fewer than two frames in the
replay is left out.

Each source starts at its first frame at the speed the last one had. The next one
follows when a clip plays forward to its end, and after the last the replay goes back
to live. Changing the order or the Shown boxes while a replay plays affects what has
not played yet.

The order and the Shown boxes are not kept when OBS closes or the scene collection
changes. They start again from the dock list, in order of source name, without regard
to case for the letters A to Z (accented letters sort after Z), with every source
shown.

## Hotkeys

The hotkeys are set in OBS, in Settings, Hotkeys, under the names below. None has a
key by default, so none works until you give it one.

| Hotkey | What it does |
|---|---|
| Tapeloop: Start or stop buffers | Starts or stops the buffers, as the button does. |
| Tapeloop: Capture replay | Captures a replay. |
| Tapeloop: Put replay on air | Puts the current replay on air. |
| Tapeloop: Take replay off air | Takes the replay off air. |
| Tapeloop: Play or pause | Plays or pauses. |
| Tapeloop: Reverse direction | Reverses the direction. |
| Tapeloop: Next frame | Steps one frame forward. |
| Tapeloop: Previous frame | Steps one frame back. |
| Tapeloop: Faster | Moves to the next faster speed. |
| Tapeloop: Slower | Moves to the next slower speed. |
| Tapeloop: Normal speed | Plays at 1x. |
| Tapeloop: Scrub forward (hold) | Scrubs forward while the key is held. |
| Tapeloop: Scrub backward (hold) | Scrubs backward while the key is held. |
| Tapeloop: Skip to next source | Ends the part on air and goes to the next. |
| Tapeloop: Newer replay | Selects the next newer replay. |
| Tapeloop: Older replay | Selects the next older replay. |

Putting a replay on air and taking it off have no dock button, only a hotkey. The
player buttons work on the replay cued or on air, as the hotkeys do.

## Putting a replay on air

Press the hotkey "Tapeloop: Put replay on air". The current replay (see "The replay
list") goes over the program: your scenes are not touched. It goes out on output
channel 32, which draws above the program and above output channels 1 to 31, where the
frontend keeps its audio devices and the Downstream Keyer plugin puts its sources by
default. A source on a higher channel, such as a keyer moved above 32, draws over the
replay. What you stream and record shows it. The replay is scaled to the canvas and
keeps its shape: a source whose shape is not the canvas's, such as a 4:3 webcam on a
16:9 canvas, leaves the program visible at its sides or above and below. The same goes
for an intro or outro. Replays play without audio for now, and the program audio goes
on as it was.

Only one replay can be on air at a time. A replay does not go on air if another
source is already on the channel, if the replay cannot be read or is damaged, or if
none of its shown sources has anything to play. When another source holds the channel,
or the replay cannot be read or is damaged, the dock says why, for instance "Another
source is on the replay's channel, so the replay did not go on air." When no shown
source has anything to play, or a replay is already on air, nothing happens.

The replay goes back to live when its last source ends, or earlier with "Tapeloop:
Take replay off air", which goes through the outro when there is one. "Tapeloop: Skip
to next source" ends only the part on air.

When the buffers stop while a replay is on air, it is taken off as with "Tapeloop:
Take replay off air", through the outro when there is one.

The replay is taken off air at once, without an outro or a way back, in these cases:

- The program goes to black with Fade to Black, so the whole output is black.
- The scene collection changes. The sequence list is forgotten as well.
- Another source takes channel 32.
- OBS closes.

## Intro and outro

"On-air settings..." sets how a replay goes on air and back. It has two parts, each
with a kind ("None", "Video" or "Image"), a file ("Browse..." opens the file
dialog), and a transition.

- "Going on air" has an "Intro" and a transition from live into the intro, or into
  the replay when there is no intro.
- "Back to live" has an "Outro" and a transition from the outro, or from the replay
  when there is no outro, back to live.

A video plays to its end. An image shows for "Image time", 0.5 to 60 seconds, 3 by
default. The transition is "Default fade" or any transition you have in OBS, picked
by name, with a length of 50 to 20000 ms, 300 by default. Steps in between are cuts.
A picked transition that is no longer in OBS shows as "(not found)" in the list.

When an intro or outro file is missing, replays go on air without it and the dock says
so. A file that is there but cannot be played is not detected: a video leaves the
program showing for up to 5 seconds, before the replay or before going back to live,
and an image does the same for its time. Check a new file by putting a replay on air
before the match. These settings are saved with the scene collection.

## Export

Both buttons work on the selected replay, once it is saved on disk, and write to an
`Export` folder inside its broadcast folder. The dock shows the progress, "Exporting
%1: %2%", and then where the files are.

- "Export to MP4" writes each source to an MP4 file without encoding it again. It
  makes one file per source, and more for a source whose codec or picture size
  changed during the replay. The MP4 files have no audio. The button is not shown
  where the build has no FFmpeg.
- "Export replay file" writes the whole replay with its tags to one `.tplp` file,
  which plays without its broadcast folder.

There is no import control. A `.tplp` from another computer is listed when you place
it in a folder inside the `Tapeloop` folder of OBS's recording folder, for instance
`Tapeloop\Imported\`, and then start OBS. After you change the recording folder, its
replays are listed when the buffers next start.

## Settings and the RAM budget

"Advanced settings" shows four more settings:

| Setting | What it does |
|---|---|
| Replay encoder | "Automatic (recommended)" tries the encoders of the graphics card OBS renders on first, then those of other cards, then x264. Choosing one uses it even if it is not the fastest path, and falls back to the automatic order if it cannot start. |
| Allow encoders on another graphics card | On by default. Lets the automatic order use a card other than the one OBS renders on. |
| Encode replays in H.264 only | By default replays are HEVC where your card can encode it, which takes less memory and disk, and H.264 elsewhere. Ignored while an encoder is chosen. |
| Memory for buffers | How much memory all buffers may hold together. Automatic is a quarter of the computer's memory. |

If the selected sources need more memory than the budget for their lengths, every
buffer holds less than its length, the same share of it, and the dock says "The
selected sources need about %1 MiB for their buffer lengths, more than the %2 MiB the
buffers may use, so each buffer holds less time than its length." Shorter lengths,
fewer sources or more memory fix it. A budget above half the computer's memory shows a
warning, since OBS and other programs may run short.

Buffer settings and on-air settings are saved with the scene collection. Replays are
saved in the recording folder.

## Limits of this alpha

- Replays play without audio. This applies to the player, to going on air and to
  the MP4 files.
- No overlays on replays yet.
- No Stream Deck or MIDI controls. Use the OBS hotkeys.
- The order of the sources and the Shown boxes are not kept (see "The sequence").
- Putting on air and taking off air have no dock button.
- Only NVENC has been tried. Quick Sync and AMF can be used, but have not been
  tried.
- macOS has not been run in OBS on a Mac, and its build cannot play replays or export
  to MP4.

Please report what you find at https://github.com/Vraeh/tapeloop, with the OBS log if
you can.
