# Replay file format

Tapeloop stores replays in two kinds of files: segments (`.tpls`), which hold the
encoded video of one source, and manifests (`.tplp`), which describe one replay and
point into the segments. An exported replay is a self-contained `.tplp` that holds its
own video. This page describes these files as the code in this repository reads and
writes them.

Tapeloop is not released. The format can change before the first stable release, and
nothing here promises that a later version reads files written by this one. Every file
carries a major and a minor version number (see [Versioning](#versioning)); a change
of format shows there first.

## Where the files are

Replays go to a `Tapeloop` folder inside OBS's recording folder. Each broadcast gets a
folder of its own there, named `<scene collection> YYYY-MM-DD HH-MM` from the local
time at which the buffers started.

```
Tapeloop/
  <scene collection> 2026-11-15 18-30/
    2026-11-15 18-42-07.tplp          manifest of one replay
    2026-11-15 18-42-07 (2).tplp      a second replay captured in the same second
    data/
      <source key>-000001.tpls        segments of one source
      <source key>-000002.tpls
    Export/
      2026-11-15 18-42-07.tplp        a self-contained replay
      2026-11-15 18-42-07 - Cam 1.mp4
```

- A manifest is named `YYYY-MM-DD HH-MM-SS.tplp` from the local time of the capture.
  When that name is taken, ` (2)`, ` (3)` and so on are added, up to 1000 tries.
- A segment is named `<source key>-<sequence>.tpls`. The source key is the OBS UUID of
  the source. The sequence counts from 1 for each source and is written with at least
  six digits.
- Exports go to `Export`. A self-contained replay takes the name of the manifest it
  came from, with ` (2)` and so on when the name is taken.
- Every name is made safe for Windows, macOS and Linux before it is used: characters
  one of them refuses, control characters and bytes that are not UTF-8 become `_`, as
  does a leading dot; trailing spaces and dots are dropped; and the name is cut to 120
  bytes at a character boundary before the extension is added (for a segment, the key
  part, with the sequence and extension after it; for an MP4 export, before ` (2)`
  and so on). A segment's path is built from the safe form of the key, so a key
  cannot name a file outside `data`.

### Temporary names and the scan

A manifest is written as `<name>.tplp.part`, flushed to the disk and then renamed to
`<name>.tplp`. An export writes `<file name>.tapeloop-part` and renames it when it is
complete. A file with its final name is therefore always whole.

When OBS has finished loading, and when the recording folder changes, Tapeloop scans
the `Tapeloop` folder. For each folder directly inside it, in name order, the scan:

- deletes the `*.tplp.part` files a crash left half written;
- reads every `*.tplp` that is a regular file, and lists it as a
  replay. A file that does not decode, or that refers to a segment that is not a
  regular file, is listed as damaged;
- deletes the `*.tapeloop-part` files in `Export` that no export of this OBS session
  is writing.

The scan lists no replay from files directly in `Tapeloop`, inside `data` or inside
`Export`. A self-contained replay is listed when it is placed in a folder directly
inside `Tapeloop`.

### How a capture is written

For each source, the GOPs that no segment of the broadcast folder holds yet are
appended to the source's open segment. A GOP is known by its run key, its first `pts`,
its first time and its packet count (see [GOP chunk](#gop-chunk)), so a GOP that
several replays use is stored once. The packet count is part of it because a GOP still
being encoded can be captured before it is complete. Every segment written to is
flushed, and only then is the manifest written. A crash while a capture is written
loses that capture; bytes appended to a segment that no manifest refers to are never
read.

A segment is only appended to by the OBS session that created it. A segment rolls over
before a chunk would take it past 1 GiB; a chunk larger than that gets a segment of
its own. When a write fails, the later captures of the same sources start new
segments.

## Common rules

- Every integer is little-endian. Signed fields are two's complement.
- Times are 64-bit signed nanoseconds. Packet times, frame times, `in`, `out`,
  `start`, `end` and the capture's clock time are on the OBS clock the packets are
  stamped with. The capture's UTC time is nanoseconds since the Unix epoch.
- Codec numbers: 1 is H.264, 2 is HEVC.
- Chunks start at offsets that are multiples of 4096 from the start of the file, and
  are followed by zero bytes up to the next multiple of 4096. The index chunk of a
  manifest is the exception: the footer follows it directly.
- A four-character code is stored as its four ASCII characters in order, so `RUN `
  appears in the file as the bytes `52 55 4E 20`.
- Every checksum is CRC-32C (Castagnoli): reflected polynomial `0x82F63B78`, initial
  value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The CRC-32C of the ASCII bytes
  `123456789` is `0xE3069283`. When a CRC covers two ranges, it is computed over the
  first and continued over the second, as if they were one run of bytes.
- Fields shown as zero below are written as zero bytes and not read.

## Segments (.tpls)

A segment is a 4096-byte header followed by chunks.

### Segment header

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic: `54 50 4C 53 0D 0A 1A 0A` (`TPLS`, CR, LF, `0x1A`, LF) |
| 8 | 2 | Major version, 1 |
| 10 | 2 | Minor version, 0 |
| 12 | 4 | Alignment, 4096 |
| 16 | 4 | Sequence of this segment |
| 20 | 2 | Source key size in bytes |
| 22 | 2 | Source name size in bytes |
| 24 | 16 | Broadcast id: random bytes, drawn again each time OBS starts writing to a broadcast folder |
| 40 | 16 | Zero |
| 56 | 4 | CRC-32C of bytes 0 to 55, continued over bytes 60 to the end of the name |
| 60 | 4 | Zero |
| 64 | | Source key, then source name, as UTF-8 bytes without terminators |

The rest of the 4096 bytes is zero. The key and the name together must fit in the 4032
bytes after offset 64; the writer cuts the name to 1024 bytes.

Tapeloop reaches each GOP through the offset a manifest gives. The header, like the
RUN chunks, lets a tool make sense of a segment without a manifest.

### Chunk header

Every chunk, in segments and in manifests, starts with this 32-byte header.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Type: `RUN `, `GOP ` or `INDX` |
| 4 | 4 | Header size, 32 |
| 8 | 8 | Payload size in bytes, without the header and the padding |
| 16 | 4 | Written as `0xFFFFFFFF`; not read |
| 20 | 4 | Written as the chunk's number in its segment, from 0, and as 0 in manifests and self-contained replays; not read |
| 24 | 4 | CRC-32C of the payload |
| 28 | 4 | CRC-32C of bytes 0 to 27 of this header |

The payload follows the header. A manifest records the header CRC of every GOP chunk
it uses, so that a read can tell that the chunk at an offset is the one it meant.

### RUN chunk

A run is a stretch of GOPs from one encoder with one codec configuration and one frame
duration. A RUN chunk describes the run of the GOP chunks that follow it, and comes
before the first GOP of each run in a segment.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Codec number |
| 4 | 4 | Configuration format, 1: the bytes as the encoder gave them (Annex B for H.264 and HEVC) |
| 8 | 8 | Frame duration in nanoseconds |
| 16 | 4 | Run key |
| 20 | 4 | Configuration size in bytes |
| 24 | | Configuration bytes |

The run key is the CRC-32C of one byte holding the codec number, continued over the
configuration bytes. It is the same for every GOP of a run. A run with no
configuration has size 0, and its key is the CRC-32C of the codec byte alone.

The plugin does not read RUN chunks; the manifest's index carries the same description
of each run.

### GOP chunk

A GOP chunk holds one group of pictures: a keyframe and the packets that follow it.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Packet count |
| 4 | 4 | Run key |
| 8 | 8 | Data size: the bytes of every packet together |
| 16 | 8 | `pts` of the first packet |
| 24 | 8 | Time of the first packet |
| 32 | 32 per packet | Packet entries |
| | | Packet data, back to back in entry order |

Each packet entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `pts`, as the encoder gave it |
| 8 | 8 | `dts`, as the encoder gave it |
| 16 | 8 | Time in nanoseconds |
| 24 | 4 | Size in bytes |
| 28 | 4 | Flags: bit 0 set for a keyframe |

The payload size is `32 + 32 * count + data size`. The first `pts`, the first time and
the packet count of the head, with the run key, are what the writer knows a GOP by;
readers take the packets from the entries and do not compare the head with them.

## Manifests (.tplp)

A manifest is 12288 bytes of head (a 64-byte header, then tag slot A at 4096 and tag
slot B at 8192), then the index chunk at offset 12288, then a 64-byte footer that ends
the file.

| Offset | Size | Part |
|---|---|---|
| 0 | 64 | Header |
| 64 | 4032 | Zero |
| 4096 | 4096 | Tag slot A |
| 8192 | 4096 | Tag slot B |
| 12288 | 32 + payload | Index chunk |
| file size - 64 | 64 | Footer |

### Header

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic: `54 50 4C 50 0D 0A 1A 0A` (`TPLP`, CR, LF, `0x1A`, LF) |
| 8 | 2 | Major version, 1 |
| 10 | 2 | Minor version, 0 |
| 12 | 4 | Header size, 64 |
| 16 | 4 | Flags: bit 0 set when the GOPs are in segments, clear in a self-contained replay |
| 20 | 4 | Alignment, 4096 |
| 24 | 16 | Replay id: random bytes |
| 40 | 8 | Capture time, Unix nanoseconds (UTC) |
| 48 | 8 | Capture time on the OBS clock |
| 56 | 4 | Tag slot size, 4096 |
| 60 | 4 | CRC-32C of bytes 0 to 59 |

### Tag slots

The tags live in two slots of 4096 bytes so that an edit can be written in place
without a moment when the file holds no valid tags.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `TPMD` |
| 4 | 4 | Payload size, at most 4064 |
| 8 | 8 | Generation |
| 16 | 4 | CRC-32C of the payload |
| 20 | 8 | Zero |
| 28 | 4 | CRC-32C of bytes 0 to 27 of the slot |
| 32 | | Payload: tag records, one after the other |

Each tag record is an 8-byte head and its bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Record type, 1 for a tag |
| 2 | 2 | Zero |
| 4 | 4 | Length in bytes |
| 8 | | The tag |

The tags are the bytes of type 1 records in order. Records of any other type are
skipped. A record that runs past the end of the payload makes the whole slot invalid.

A new manifest has the same tags in both slots, at generation 1. A reader takes the
tags of the slot that decodes with the higher generation, slot A on a tie, the only
slot that decodes when one does not, and no tags when neither does. A damaged tag slot
does not make the replay damaged.

An edit rewrites one slot in place and flushes it: a slot that does not decode, or
else the slot with the lower generation, slot B on a tie. The new generation is one
more than the highest intact one, so a write cut short leaves the other slot with the
tags from before the edit. When the newer slot already holds the highest possible
generation, which only a made-up file can, the edit writes generation 1 to the older
slot, flushes it, then writes the same bytes to the newer one.

### Index chunk

The index chunk has the chunk header above with type `INDX`. Its payload is a 40-byte
head, then five areas one after the other: the source table, the run table, the GOP
table, the frame times and the blob.

Index head:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Source count |
| 4 | 4 | Run count, over all sources |
| 8 | 4 | GOP count, over all sources |
| 12 | 4 | Frame count, over all sources |
| 16 | 4 | Blob size in bytes |
| 20 | 4 | Zero |
| 24 | 8 | Start of the range the capture asked for |
| 32 | 8 | End of that range |

The source table starts at 40, the run table at `40 + 48 * sources`, the GOP table at
`run table + 32 * runs`, the frame times at `GOP table + 40 * GOPs`, the blob at
`frame times + 8 * frames`, and the payload ends where the blob does.

Source entry, 48 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | First run of this source in the run table |
| 4 | 4 | Run count |
| 8 | 4 | First GOP in the GOP table |
| 12 | 4 | GOP count |
| 16 | 8 | In: time of the first frame of the clip |
| 24 | 8 | Out: time of the last frame of the clip |
| 32 | 4 | Offset of the source key in the blob; the name follows the key |
| 36 | 2 | Key size |
| 38 | 2 | Name size |
| 40 | 4 | First frame in the frame times |
| 44 | 4 | Frame count |

Frames of the source's GOPs before `in` or after `out` are there only so that the
frames of the clip can be decoded.

Run entry, 32 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Codec number |
| 4 | 4 | Configuration format, 1 |
| 8 | 8 | Frame duration in nanoseconds |
| 16 | 4 | Run key |
| 20 | 4 | Offset of the configuration in the blob |
| 24 | 4 | Configuration size |
| 28 | 4 | Zero |

GOP entry, 40 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Sequence of the segment that holds it, or `0xFFFFFFFF` for the file itself |
| 4 | 4 | Packet count |
| 8 | 8 | Offset of the GOP chunk in that file |
| 16 | 8 | Size of the GOP chunk: header and payload, without padding |
| 24 | 4 | Header CRC of the GOP chunk, as at its offset 28 |
| 28 | 4 | Run, an index into this source's runs |
| 32 | 8 | Zero |

The frame times are 8 bytes each: the time of every packet of every GOP of a source,
in GOP order, then those of the next source. Replays have no B-frames, so decode order
is presentation order.

The blob holds, for each source in order, its key, its name and then the configuration
of each of its runs in order, with nothing between them.

### Footer

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic `TPLPTAIL` |
| 8 | 2 | Major version, 1 |
| 10 | 2 | Minor version, 0 |
| 12 | 4 | Zero |
| 16 | 8 | Offset of the index chunk |
| 24 | 8 | Size of the index chunk, header included |
| 32 | 16 | Replay id, the same as in the header |
| 48 | 8 | File size |
| 56 | 4 | Zero |
| 60 | 4 | CRC-32C of bytes 0 to 59 |

A reader finds the index from the footer, so it reads the head, the index and the
footer, and never the GOP chunks between them in a self-contained replay.

## Self-contained replays

An exported replay has the manifest's layout with bit 0 of the header flags clear, and
its own GOP chunks between the tag slots and the index:

| Offset | Part |
|---|---|
| 0 | Header and both tag slots, as in a manifest |
| 12288 | For each source in order, its GOP chunks in order, with a RUN chunk before each GOP whose run differs from that of the GOP before it in the source |
| index offset | Index chunk |
| file size - 64 | Footer |

Every GOP entry has segment `0xFFFFFFFF` and an offset into the file itself. The
export gives the copy a new replay id and the tags of the replay it came from, both
slots at generation 1. Neither kind of replay may mix the two: in a manifest no GOP
entry may be `0xFFFFFFFF`, and in a self-contained replay every one must be.

## Versioning

The major and minor versions are at bytes 8 and 10 of the segment header, the manifest
header and the footer. This version writes 1.0.

- A manifest header or footer of another major version is refused, and the replay is
  listed as damaged.
- The minor version is not checked. Tag records of an unknown type, which a newer
  minor version could add, are skipped.
- A manifest with a flag bit other than bit 0 set is refused, since an unknown flag
  could change where the GOPs are.
- The header size, the alignment and the tag slot size of a manifest, the chunk header
  size and the configuration format must have exactly the values above, or the file
  is refused.

## Checksums

| CRC-32C | Covers | Stored at |
|---|---|---|
| Segment header | Bytes 0 to 55 and 60 to the end of the name | Segment offset 56 |
| Chunk header | Bytes 0 to 27 of the chunk header | Chunk offset 28 |
| Chunk payload | The payload, without padding | Chunk offset 24 |
| Manifest header | Bytes 0 to 59 | Manifest offset 60 |
| Tag slot header | Bytes 0 to 27 of the slot | Slot offset 28 |
| Tag slot payload | The tag records | Slot offset 16 |
| Footer | Bytes 0 to 59 of the footer | Footer offset 60 |

The GOP entries of the index hold the header CRC of each GOP chunk. Since that header
holds the payload's CRC, a manifest names the exact chunk it uses, and a segment
written again with other GOPs at the same offsets does not pass for the old one.

Padding, the zero bytes after the segment header's strings and bytes 64 to 4095 of a
manifest are not covered.

CRC-32C detects accidental damage: a torn write, a bad sector, a truncated copy. It is
not authentication. Anyone can change a file and compute its CRCs again, so a file
whose CRCs all match can still hold anything. The readers check every field as if
there were no CRCs; the CRCs only make most damage fail early.

## Limits

What the readers enforce:

| Limit | Value |
|---|---|
| Index chunk read from a manifest | At most 256 MiB; a larger one is not read |
| Counts and blob size | 32-bit fields; the payload size must equal the size the counts give, exactly |
| Key and name sizes in the index | 16-bit fields, 65535 bytes each |
| Tag slot payload | At most 4064 bytes |
| Frame duration | More than 0, at most 2147483647 seconds |
| `dts` and `pts` of a packet | `dts` not above `pts`, and `pts - dts` at most 16 |
| Packet size | 32-bit field; the sizes must add up to the data size exactly |
| Packets, runs and GOPs per source | At least one each; at least one packet per GOP |
| Span of a source's frame times | Must fit a signed 64-bit count of nanoseconds |
| Decoded picture | At most 35389440 pixels (as many as 8192 x 4320), counted on the coded size, by the decoder, not the format |

What they do not enforce:

- The format does not store a picture size; it is in the bitstream and the
  configuration, which go to the decoder as they are. The decoder refuses pictures
  above the limit above.
- The start and end of the index and the capture times are not checked. The replay id
  is checked only in that the footer's must equal the header's.
- An index with no sources decodes. The writer never makes one: a capture needs a
  source with frames.

The writer's own limits: a segment rolls over at 1 GiB, a source name is cut to 1024
bytes, the key and the name of a segment header fit its 4032 bytes, tags must fit one
slot, and every count and the blob must fit 32 bits.

## Reading untrusted files

A `.tplp` or `.tpls` can come from anywhere: a self-contained replay is made to be
copied to another computer, and anyone can edit a broadcast folder. Tapeloop treats
every replay file as untrusted input, and so should any other program that reads one.

### What Tapeloop checks

Files:

- A named pipe or a similar device at a replay's path is refused at once instead of
  waiting for a writer. The type comes from the opened
  file, not from the path when it was listed.

The footer, before anything else is read:

- magic, major version and CRC; its file size field equals the real size; the file is
  at least 12352 bytes;
- the index offset is a multiple of 4096, at least 12288 and not past the footer, and
  the index chunk ends exactly where the footer starts;
- the index chunk is at most 256 MiB.

The header and the index:

- magic, major version, header size, flags, alignment, tag slot size and CRC; the
  footer's replay id equals the header's; in a manifest, the index is at 12288;
- the index chunk's type, header size, both CRCs, and a payload that fills the chunk;
- the payload size equals what the counts give, so no table reaches past it;
- each source's runs, GOPs and frames follow those of the source before it, with
  nothing skipped and nothing left over at the end;
- the blob is read in order, each key, name and configuration right after the one
  before, with nothing left over, so no byte of it is taken twice: overlapping
  references would let a small file expand into gigabytes;
- each run has a known codec, configuration format 1, a frame duration in range, and a
  run key that matches its codec and configuration;
- each GOP entry has at least one packet, a run of its source, a size of at least 32,
  an offset that is a multiple of 4096, and a segment that agrees with the header
  flag; in a self-contained replay, its chunk lies between the tag slots and the
  index;
- the packet counts of a source's GOPs add up to its frame count; its frame times
  strictly increase; `in` is not after `out`, and both lie within the frame times.

A replay is listed as intact only when every segment it refers to exists as a regular
file.

Each GOP, when it is read:

- the chunk header at the offset the index gives must have type `GOP `, header size
  32, a valid CRC equal to the one the index names, and the size the index gives,
  checked against the file before its bytes are read;
- the payload CRC matches; the run key is the run's; the packet entries fit; the data
  size is exactly what is left; the first packet is a keyframe; packet times strictly
  increase; `dts` is not above `pts`, and `pts - dts` is at most 16; the packet sizes
  add up to the data size;
- the packet count equals the index's, and every packet time equals the index's frame
  time for it. A GOP whose times differ is not the one the index lists, whatever its
  CRCs say.

The bitstreams go to FFmpeg's decoder with the picture size limit above. Before an MP4
export, each sequence parameter set is checked to hold every field up to the last
count FFmpeg's muxer steps through as it writes the file.

### What another reader must check

A program of its own that reads these files needs at least the checks above. In
particular:

- Check every offset and size against the file size before reading or allocating, and
  compute `offset + size` so that it cannot overflow.
- Do not trust a count to size an allocation; bound it by the bytes that hold it.
- Treat each source key as untrusted text when building a segment path: make it a safe
  file name, so that it cannot reach outside `data`.
- Refuse references that overlap, in the blob and between GOP chunks of one file.
- Check that times strictly increase, and do arithmetic on times so that it cannot
  overflow.
- Refuse anything that is not a regular file, and do not block on a pipe.
- Treat the configuration and the packets as untrusted bitstreams, with the decoder's
  limits set.
