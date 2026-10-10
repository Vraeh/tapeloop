// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/MediaTime.hpp"
#include "core/SourceBuffer.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tapeloop {

struct MomentClip {
	// Chosen by the caller to tell sources apart; the core does not interpret it.
	std::string sourceKey;
	Clip clip;
};

// One marked instant, cut from one or more sources, each over its own part of the range.
struct Moment {
	Nanoseconds start{0};
	Nanoseconds end{0};
	std::vector<MomentClip> clips;
};

// A view for one call: the buffer must outlive the call to cutMoment.
struct MomentSource {
	std::string sourceKey;
	std::reference_wrapper<const SourceBuffer> buffer;
};

struct MomentCut {
	Moment moment;
	// Sources that held nothing in the requested range.
	std::vector<std::string> skipped;
};

// Cuts the range [anchor - preRoll, anchor] from every source, but never reaching back
// further than the source's own buffer window, a negative pre-roll or window taken as
// zero: what is older than that is footage the buffer only kept for decoding, or that
// came before the buffer stopped or before an outage. Each clip covers the part of its
// range the buffer holds and records its own in and out frames; the moment's start and
// end are the range asked for, not the part the clips cover.
MomentCut cutMoment(std::span<const MomentSource> sources, Nanoseconds anchor, Nanoseconds preRoll);

} // namespace tapeloop
