// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace tapeloop {

struct FrameLocation {
	size_t gop = 0;
	size_t packet = 0;
};

// A cut of one source between two frames, in and out, both inclusive. It holds whole
// GOPs, starting at the one that contains the in frame, because decoding has to start
// at a keyframe. Each GOP carries the codec and configuration of its run, so a decoder
// can start at any of them, as long as the encoder reported one or repeats it in the
// stream. The GOPs are shared, never copied, so a clip outlives the buffer it was cut
// from and costs little to copy.
class Clip {
public:
	Clip() = default;

	// Keeps the frames of gops with times in [from, to]. The GOPs must be in time
	// order. If no frame falls in the range, the clip is empty.
	Clip(std::vector<std::shared_ptr<const Gop>> gops, Nanoseconds from, Nanoseconds to);

	bool empty() const noexcept { return gops_.empty(); }
	// Times of the first and last frame. Meaningless on an empty clip.
	Nanoseconds in() const noexcept { return in_; }
	Nanoseconds out() const noexcept { return out_; }
	std::span<const std::shared_ptr<const Gop>> gops() const noexcept { return gops_; }

	// Sorted times of every frame between in and out.
	std::vector<Nanoseconds> frameTimes() const;

	// The frame on screen at time t: the last one starting at or before t, with t
	// clamped to [in, out]. An empty clip has no frames and answers {0, 0}.
	FrameLocation locate(Nanoseconds t) const noexcept;

	// Bytes of every GOP held, including frames outside [in, out] that decoding needs.
	size_t byteSize() const noexcept;

private:
	std::vector<std::shared_ptr<const Gop>> gops_;
	Nanoseconds in_{0};
	Nanoseconds out_{0};
};

} // namespace tapeloop
