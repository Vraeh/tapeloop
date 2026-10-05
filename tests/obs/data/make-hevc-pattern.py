#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>
#
# Writes hevc-pattern.bin: 30 frames of the harness's test pattern (TestPattern.cpp)
# at 320x180, encoded by x265 into three GOPs of ten frames without B-frames, for
# tests that need an HEVC run and a harness that can only encode H.264.
# Usage: make-hevc-pattern.py <x265> <output> [x265 option ...]
#
# The files in this directory were made with x265 4.1 (Ubuntu's 4.1-4 package):
#   hevc-pattern.bin           no options
#   hevc-pattern-full-601.bin  --range full --colormatrix bt470bg
#   hevc-pattern-main10.bin    --output-depth 10 --profile main10
#
# The file is a list of records, each a kind byte (0 the configuration, 1 a keyframe,
# 2 any other frame), a 32-bit little-endian size and that many bytes. Keyframes keep
# the parameter sets x265 repeats in the stream.

import struct
import subprocess
import sys

WIDTH, HEIGHT, FRAMES, GOP = 320, 180, 30, 10
BITS, BAR_WIDTH, BAR_STEP = 16, 16, 8
# Limited-range luma of the pattern's grey, white and black.
GREY, WHITE, BLACK = 126, 235, 16


def frame(number):
    luma = bytearray([GREY]) * (WIDTH * HEIGHT)
    top = HEIGHT - HEIGHT // 4
    bar = number * BAR_STEP % (WIDTH - BAR_WIDTH)
    cell = WIDTH // BITS
    for y in range(HEIGHT):
        row = y * WIDTH
        if y < top:
            luma[row + bar:row + bar + BAR_WIDTH] = bytes([WHITE]) * BAR_WIDTH
        else:
            for bit in range(BITS):
                value = WHITE if (number >> (BITS - 1 - bit)) & 1 else BLACK
                luma[row + bit * cell:row + (bit + 1) * cell] = bytes([value]) * cell
    chroma = bytes([128]) * (WIDTH // 2 * HEIGHT // 2)
    return bytes(luma) + chroma + chroma


def nal_units(stream):
    starts = []
    i = 0
    while True:
        i = stream.find(b"\x00\x00\x01", i)
        if i < 0:
            break
        starts.append(i + 3)
        i += 3
    for n, begin in enumerate(starts):
        end = starts[n + 1] - 3 if n + 1 < len(starts) else len(stream)
        while end > begin and stream[end - 1] == 0:
            end -= 1
        yield stream[begin:end]


def main():
    x265, output, options = sys.argv[1], sys.argv[2], sys.argv[3:]
    raw = b"".join(frame(n) for n in range(FRAMES))
    stream = subprocess.run(
        [x265, "--input", "-", "--input-res", f"{WIDTH}x{HEIGHT}", "--input-csp", "i420",
         "--fps", "30", "--frames", str(FRAMES), "--keyint", str(GOP), "--min-keyint", str(GOP),
         "--no-scenecut", "--no-open-gop", "--bframes", "0", "--repeat-headers", "--no-info",
         "--preset", "ultrafast", "--qp", "20", "--log-level", "error", "--no-progress", "--output", "-",
         *options],
        input=raw, stdout=subprocess.PIPE, check=True).stdout

    # One slice per picture, so every VCL unit ends an access unit, with the non-VCL
    # units before it.
    units, pending, config = [], [], []
    for nal in nal_units(stream):
        kind = (nal[0] >> 1) & 0x3f
        pending.append(nal)
        if kind in (32, 33, 34) and len(config) < 3:
            config.append(nal)
        if kind < 32:
            units.append((16 <= kind <= 21, pending))
            pending = []
    if len(units) != FRAMES or len(config) != 3:
        sys.exit(f"expected {FRAMES} pictures and 3 parameter sets, got {len(units)} and {len(config)}")

    def annex_b(nals):
        return b"".join(b"\x00\x00\x00\x01" + nal for nal in nals)

    with open(output, "wb") as out:
        records = [(0, annex_b(config))] + [(1 if key else 2, annex_b(nals)) for key, nals in units]
        for kind, data in records:
            out.write(struct.pack("<BI", kind, len(data)) + data)


main()
