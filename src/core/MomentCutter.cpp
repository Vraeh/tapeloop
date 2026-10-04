// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/MomentCutter.hpp"

#include <algorithm>
#include <utility>

namespace tapeloop {

MomentCut cutMoment(std::span<const MomentSource> sources, Nanoseconds anchor, Nanoseconds preRoll)
{
	MomentCut cut;
	cut.moment.start = saturatingSub(anchor, std::max(preRoll, Nanoseconds{0}));
	cut.moment.end = anchor;

	for (const MomentSource &source : sources) {
		Clip clip = source.buffer.get().clip(cut.moment.start, cut.moment.end);
		if (clip.empty()) {
			cut.skipped.push_back(source.sourceKey);
		} else {
			cut.moment.clips.push_back({source.sourceKey, std::move(clip)});
		}
	}
	return cut;
}

} // namespace tapeloop
