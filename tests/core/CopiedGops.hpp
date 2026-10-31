// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/Gop.hpp"
#include "core/GopSource.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace tapeloop::test {

// The GOPs of a clip as a replay on disk gives them: every read makes a new copy, with a
// configuration object of its own, and the reads of the GOPs marked unreadable fail.
// Counts the reads of each GOP.
class CopiedGops final : public GopSource {
public:
	explicit CopiedGops(Clip clip) : clip_(std::move(clip)) {}

	size_t gopCount() const noexcept override { return clip_.gops().size(); }
	size_t packetCount(size_t gop) const noexcept override { return clip_.gops()[gop]->packets().size(); }
	FrameLocation locate(Nanoseconds t) const noexcept override { return clip_.locate(t); }
	std::vector<Nanoseconds> frameTimes() const override { return clip_.frameTimes(); }

	std::shared_ptr<const Gop> read(size_t gop) override
	{
		++reads[gop];
		if (unreadable.contains(gop)) {
			return nullptr;
		}
		const Gop &held = *clip_.gops()[gop];
		const CodecConfig *config = held.codecConfig();
		GopBuilder builder(held.frameDuration());
		builder.setCodecConfig(held.codec(), config ? std::make_shared<const CodecConfig>(*config) : nullptr);
		for (size_t i = 0; i < held.packets().size(); ++i) {
			const PacketRecord &record = held.packets()[i];
			builder.append({held.packetData(i), record.pts, record.dts, record.time, record.keyframe});
		}
		return builder.seal();
	}

	std::map<size_t, int> reads;
	std::set<size_t> unreadable;

private:
	Clip clip_;
};

} // namespace tapeloop::test
