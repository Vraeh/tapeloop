// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tapeloop::test {

// Produces packets the way an encoder at a fixed frame rate without B-frames would:
// pts and dts are the frame index and every gopLength-th frame is a keyframe. Each
// byte is derived from the frame index and its offset, so a reader can verify it.
class SyntheticEncoder {
public:
	struct Config {
		Rational frameTimebase{1, 60};
		int64_t gopLength = 30;
		size_t keyframeSize = 4096;
		size_t frameSize = 1024;
		int64_t firstFrame = 0;
		Nanoseconds origin{0};
	};

	explicit SyntheticEncoder(Config config) : config_(config), frame_(config.firstFrame) {}

	static uint8_t expectedByte(int64_t frame, size_t offset)
	{
		return static_cast<uint8_t>((static_cast<uint64_t>(frame) * 131 + offset * 7) & 0xff);
	}

	Nanoseconds frameDuration() const
	{
		return Nanoseconds{rescale(1, config_.frameTimebase, kNanosecondTimebase)};
	}

	Nanoseconds timeOf(int64_t frame) const
	{
		return config_.origin + Nanoseconds{rescale(frame, config_.frameTimebase, kNanosecondTimebase)};
	}

	bool isKeyframe(int64_t frame) const { return frame % config_.gopLength == 0; }

	size_t sizeOf(int64_t frame) const { return isKeyframe(frame) ? config_.keyframeSize : config_.frameSize; }

	int64_t nextFrame() const { return frame_; }

	// The returned data stays valid until the next call.
	EncodedPacket next()
	{
		const int64_t frame = frame_++;
		data_.resize(sizeOf(frame));
		for (size_t i = 0; i < data_.size(); ++i)
			data_[i] = expectedByte(frame, i);
		return {data_, frame, frame, timeOf(frame), isKeyframe(frame)};
	}

private:
	Config config_;
	int64_t frame_;
	std::vector<uint8_t> data_;
};

// True when every packet of the GOP holds the bytes SyntheticEncoder wrote for it.
inline bool hasExpectedBytes(const Gop &gop)
{
	for (size_t i = 0; i < gop.packets().size(); ++i) {
		const std::span<const uint8_t> data = gop.packetData(i);
		for (size_t offset = 0; offset < data.size(); ++offset) {
			if (data[offset] != SyntheticEncoder::expectedByte(gop.packets()[i].pts, offset))
				return false;
		}
	}
	return true;
}

} // namespace tapeloop::test
