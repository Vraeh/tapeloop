// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace tapeloop {

// The GOPs of one source of a replay, in time order, as a decode planner plays them: their
// frames are known without reading them, and a GOP is read when decoding comes to it, so
// a long replay on disk is never all in memory.
class GopSource {
public:
	GopSource() = default;
	virtual ~GopSource() = default;

	GopSource(const GopSource &) = delete;
	GopSource &operator=(const GopSource &) = delete;
	GopSource(GopSource &&) = delete;
	GopSource &operator=(GopSource &&) = delete;

	virtual size_t gopCount() const noexcept = 0;
	virtual size_t packetCount(size_t gop) const noexcept = 0;
	// As Clip::locate: the frame on screen at t, clamped to the in and out points.
	virtual FrameLocation locate(Nanoseconds t) const noexcept = 0;
	// The times of the frames between the in and out points, sorted.
	virtual std::vector<Nanoseconds> frameTimes() const = 0;
	// Null when it cannot be read.
	virtual std::shared_ptr<const Gop> read(size_t gop) = 0;
};

// The GOPs of a clip, which holds them all.
class ClipGops final : public GopSource {
public:
	explicit ClipGops(Clip clip) : clip_(std::move(clip)) {}

	size_t gopCount() const noexcept override { return clip_.gops().size(); }
	size_t packetCount(size_t gop) const noexcept override { return clip_.gops()[gop]->packets().size(); }
	FrameLocation locate(Nanoseconds t) const noexcept override { return clip_.locate(t); }
	std::vector<Nanoseconds> frameTimes() const override { return clip_.frameTimes(); }
	std::shared_ptr<const Gop> read(size_t gop) override { return clip_.gops()[gop]; }

private:
	Clip clip_;
};

} // namespace tapeloop
