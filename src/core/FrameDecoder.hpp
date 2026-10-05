// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"

#include <cstdint>
#include <span>

namespace tapeloop {

// What a FrameDecoder call reports. A decoder sits in front of C libraries and device
// drivers, so failures are values, never exceptions.
enum class DecodeStatus {
	Ok,
	// receive(): no frame is ready yet; send the next packet, or flush.
	NeedMore,
	// receive() after flush(): every frame has come out.
	Drained,
	// This codec, configuration or picture format cannot be decoded here.
	Unsupported,
	// The stream could not be decoded.
	InvalidData,
	OutOfMemory,
	// The GPU the decoder runs on is gone; the decoder has to be opened again.
	DeviceLost,
	// DecodePlanner::frameAt() on an empty clip: there is nothing to show.
	NoFrame,
};

// A decoded picture, owned by the decoder that made it. The id means something only to
// that decoder. The frame stays valid until it is given back with release() or the
// decoder is closed, also across open() for another run.
struct DecodedFrame {
	uint64_t id = 0;
	int64_t pts = 0;
};

// Turns the packets of one run at a time into pictures. Packets go in decode order,
// starting at a keyframe; frames come out in presentation order. Each replay has its
// own decoder, used from one thread. Replacing FFmpeg later replaces only the
// implementations of this interface.
class FrameDecoder {
public:
	FrameDecoder() = default;
	virtual ~FrameDecoder() = default;

	FrameDecoder(const FrameDecoder &) = delete;
	FrameDecoder &operator=(const FrameDecoder &) = delete;
	FrameDecoder(FrameDecoder &&) = delete;
	FrameDecoder &operator=(FrameDecoder &&) = delete;

	// Ready for a run of codec with its configuration, empty when the stream carries it,
	// in place of the run that was open. Frames already received stay valid.
	virtual DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept = 0;
	// The data is only borrowed for the call. The first packet after open() or reset()
	// is a keyframe.
	virtual DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept = 0;
	// Ok with frame filled, NeedMore, or Drained after flush().
	virtual DecodeStatus receive(DecodedFrame &frame) noexcept = 0;
	// No more packets for now: the frames still inside come out of receive(), and the
	// next packet needs a reset() first.
	virtual DecodeStatus flush() noexcept = 0;
	// Drops what is inside, to start again at a keyframe of the same run. Frames already
	// received stay valid.
	virtual void reset() noexcept = 0;
	virtual void release(const DecodedFrame &frame) noexcept = 0;
	// Releases every frame it still holds.
	virtual void close() noexcept = 0;
};

} // namespace tapeloop
