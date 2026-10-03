// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Clip.hpp"

#include <algorithm>
#include <utility>

namespace tapeloop {
namespace {

// Index of the first packet at or after t. The GOP must end at or after t.
size_t firstPacketFrom(const Gop &gop, Nanoseconds t)
{
	const std::span<const PacketRecord> packets = gop.packets();
	const auto found = std::partition_point(packets.begin(), packets.end(),
						[t](const PacketRecord &packet) { return packet.time < t; });
	return static_cast<size_t>(found - packets.begin());
}

// Index of the last packet at or before t. The GOP must start at or before t.
size_t lastPacketUntil(const Gop &gop, Nanoseconds t)
{
	const std::span<const PacketRecord> packets = gop.packets();
	const auto after = std::partition_point(packets.begin(), packets.end(),
						[t](const PacketRecord &packet) { return packet.time <= t; });
	return static_cast<size_t>(after - packets.begin()) - 1;
}

} // namespace

Clip::Clip(std::vector<std::shared_ptr<const Gop>> gops, Nanoseconds from, Nanoseconds to)
{
	if (from > to)
		return;

	const auto first = std::partition_point(gops.begin(), gops.end(),
						[from](const auto &gop) { return gop->lastTime() < from; });
	const auto last =
		std::partition_point(first, gops.end(), [to](const auto &gop) { return gop->startTime() <= to; });
	if (first == last)
		return;

	const Gop &head = **first;
	const Gop &tail = **(last - 1);
	const Nanoseconds in = head.packets()[firstPacketFrom(head, from)].time;
	const Nanoseconds out = tail.packets()[lastPacketUntil(tail, to)].time;

	// Only possible when a single GOP has frames on both sides of the range but none
	// inside it.
	if (in > out)
		return;

	gops.erase(last, gops.end());
	gops.erase(gops.begin(), first);
	gops_ = std::move(gops);
	in_ = in;
	out_ = out;
}

std::vector<Nanoseconds> Clip::frameTimes() const
{
	std::vector<Nanoseconds> times;
	for (const auto &gop : gops_) {
		for (const PacketRecord &packet : gop->packets()) {
			if (packet.time >= in_ && packet.time <= out_)
				times.push_back(packet.time);
		}
	}
	return times;
}

FrameLocation Clip::locate(Nanoseconds t) const noexcept
{
	if (empty())
		return {};
	t = std::clamp(t, in_, out_);

	// The first GOP starts at or before in, so some GOP starts at or before t.
	const auto after = std::partition_point(gops_.begin(), gops_.end(),
						[t](const auto &gop) { return gop->startTime() <= t; });
	const size_t gop = static_cast<size_t>(after - gops_.begin()) - 1;
	return {gop, lastPacketUntil(*gops_[gop], t)};
}

size_t Clip::byteSize() const noexcept
{
	size_t bytes = 0;
	for (const auto &gop : gops_)
		bytes += gop->byteSize();
	return bytes;
}

} // namespace tapeloop
