// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/SourceBuffer.hpp"

#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::EncodedPacket;
using tapeloop::Gop;
using tapeloop::Nanoseconds;
using tapeloop::PacketRecord;
using tapeloop::SourceBuffer;
using tapeloop::VideoCodec;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;

namespace {

using GopList = std::vector<std::shared_ptr<const Gop>>;

// Which packets the buffer keeps, as its header describes it, tracked apart from the
// buffer: a run of packets starts at a keyframe and ends at a discontinuity or a new
// codec configuration.
struct SyncModel {
	bool synced = false;
	Nanoseconds lastTime{0};
	int64_t lastDts = 0;
	uint64_t dropped = 0;
	uint64_t discontinuities = 0;
	VideoCodec codec = VideoCodec::H264;
	CodecConfig codecConfig;
	// Every codec and configuration set so far, and which of them each kept frame was
	// encoded with, by frame time.
	std::vector<VideoCodec> codecs{VideoCodec::H264};
	std::vector<CodecConfig> configs{CodecConfig{}};
	std::map<Nanoseconds, size_t> configOfFrame;

	void setCodecConfig(VideoCodec newCodec, std::span<const uint8_t> bytes)
	{
		if (synced) {
			++discontinuities;
		}
		synced = false;
		codec = newCodec;
		codecConfig.assign(bytes.begin(), bytes.end());
		codecs.push_back(codec);
		configs.push_back(codecConfig);
	}

	bool breaksRun(const EncodedPacket &packet) const
	{
		return synced && (packet.time <= lastTime || packet.dts < lastDts);
	}

	// True when the buffer must keep the packet.
	bool push(const EncodedPacket &packet)
	{
		if (breaksRun(packet)) {
			++discontinuities;
			synced = false;
		}
		if (!synced && !packet.keyframe) {
			++dropped;
			return false;
		}
		synced = true;
		lastTime = packet.time;
		lastDts = packet.dts;
		configOfFrame[packet.time] = configs.size() - 1;
		return true;
	}
};

GopList heldGops(const SourceBuffer &buffer)
{
	const Clip all = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	return {all.gops().begin(), all.gops().end()};
}

std::vector<Nanoseconds> heldTimes(const SourceBuffer &buffer)
{
	return buffer.clip(Nanoseconds::min(), Nanoseconds::max()).frameTimes();
}

size_t bytesOf(const GopList &gops)
{
	size_t bytes = 0;
	for (const auto &gop : gops) {
		bytes += gop->byteSize();
	}
	return bytes;
}

void checkClip(const Clip &clip)
{
	if (clip.empty()) {
		require(clip.byteSize() == 0 && clip.frameTimes().empty());
		return;
	}

	const std::vector<Nanoseconds> times = clip.frameTimes();
	require(!times.empty() && times.front() == clip.in() && times.back() == clip.out());
	for (size_t i = 1; i < times.size(); ++i) {
		require(times[i - 1] < times[i]);
	}

	const auto in = clip.locate(clip.in());
	const auto out = clip.locate(clip.out());
	require(clip.gops()[in.gop]->packets()[in.packet].time == clip.in());
	require(clip.gops()[out.gop]->packets()[out.packet].time == clip.out());
	require(clip.gops().front()->packets().front().keyframe);
}

// A clip of [from, to] runs from the first frame held at or after from to the last at
// or before to.
void checkRange(const Clip &clip, const std::vector<Nanoseconds> &held, Nanoseconds from, Nanoseconds to)
{
	checkClip(clip);
	const auto first = std::lower_bound(held.begin(), held.end(), from);
	const auto last = std::upper_bound(held.begin(), held.end(), to);
	if (from > to || first >= last) {
		require(clip.empty());
		return;
	}
	require(!clip.empty() && clip.in() == *first && clip.out() == *(last - 1));
	require(clip.frameTimes().size() == static_cast<size_t>(last - first));
}

// Every frame held is in a GOP with the configuration it was encoded with, a GOP never
// mixes runs, each configuration is counted once, and once the GOPs move on from one
// they never come back to it.
void checkCodecConfigs(const Clip &all, const tapeloop::SourceBufferStats &stats, const SyncModel &model)
{
	for (const auto &gop : all.gops()) {
		const auto first = model.configOfFrame.find(gop->packets().front().time);
		require(first != model.configOfFrame.end());
		for (const PacketRecord &packet : gop->packets()) {
			const auto found = model.configOfFrame.find(packet.time);
			require(found != model.configOfFrame.end() && found->second == first->second);
		}
		require(gop->codec() == model.codecs[first->second]);
		const CodecConfig &expected = model.configs[first->second];
		if (expected.empty()) {
			require(gop->codecConfig() == nullptr);
		} else {
			require(gop->codecConfig() && *gop->codecConfig() == expected);
		}
	}

	std::set<const CodecConfig *> seen;
	size_t configBytes = 0;
	const CodecConfig *previous = nullptr;
	for (const auto &gop : all.gops()) {
		const CodecConfig *config = gop->codecConfig();
		if (config && config != previous) {
			require(seen.insert(config).second);
			configBytes += config->size();
		}
		previous = config;
	}
	require(stats.configBytes == configBytes);
}

// Every GOP held starts with a keyframe, its packet table describes its bytes exactly,
// times increase across the whole buffer, and the statistics agree with what is held
// and with the model.
void checkBuffer(const SourceBuffer &buffer, const SyncModel &model)
{
	const tapeloop::SourceBufferStats stats = buffer.stats();
	const Clip all = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	checkClip(all);

	require(stats.droppedBeforeKeyframe == model.dropped);
	require(stats.discontinuities == model.discontinuities);
	require(all.gops().size() == stats.gopCount);
	require(all.byteSize() == stats.bytes);
	checkCodecConfigs(all, stats, model);

	if (all.empty()) {
		require(stats.bytes == 0 && stats.oldestTime == Nanoseconds{0} && stats.newestTime == Nanoseconds{0});
		return;
	}
	require(all.in() == stats.oldestTime && all.out() == stats.newestTime);

	bool first = true;
	Nanoseconds previous{0};
	for (const auto &gop : all.gops()) {
		const std::span<const PacketRecord> packets = gop->packets();
		size_t offset = 0;
		for (size_t i = 0; i < packets.size(); ++i) {
			require(packets[i].keyframe == (i == 0));
			require(packets[i].offset == offset);
			offset += packets[i].size;
			require(first || previous < packets[i].time);
			previous = packets[i].time;
			first = false;
		}
		require(offset == gop->byteSize());
	}
}

// A kept packet is the newest frame held, with its fields and bytes, in a GOP with the
// codec and configuration of its run.
void checkKept(const SourceBuffer &buffer, const EncodedPacket &packet, VideoCodec codec,
	       const CodecConfig &codecConfig)
{
	require(buffer.stats().newestTime == packet.time);
	const Clip newest = buffer.clip(packet.time, packet.time);
	require(!newest.empty());
	const auto at = newest.locate(packet.time);
	const Gop &gop = *newest.gops()[at.gop];
	const PacketRecord &record = gop.packets()[at.packet];
	require(record.time == packet.time && record.pts == packet.pts && record.dts == packet.dts);
	require(record.keyframe == packet.keyframe);
	const std::span<const uint8_t> stored = gop.packetData(at.packet);
	require(std::equal(stored.begin(), stored.end(), packet.data.begin(), packet.data.end()));
	require(gop.codec() == codec);
	if (codecConfig.empty()) {
		require(gop.codecConfig() == nullptr);
	} else {
		require(gop.codecConfig() && *gop.codecConfig() == codecConfig);
	}
}

// After a kept keyframe at `time` eviction has run on the sealed GOPs: neither rule would
// drop the oldest of them any more, the GOP the keyframe sealed stays unless it ends
// before the window, and whatever was dropped from the front had to go. `before` holds
// the sealed GOPs from before the push, and `sealedEnd` the end of the GOP the keyframe
// sealed, when it sealed one.
void checkEviction(const SourceBuffer &buffer, const tapeloop::SourceBufferConfig &config, Nanoseconds time,
		   const GopList &before, std::optional<Nanoseconds> sealedEnd, bool restarted)
{
	GopList sealed = heldGops(buffer);
	sealed.pop_back();
	const size_t bytes = bytesOf(sealed);
	const Nanoseconds start = tapeloop::saturatingSub(time, config.window);

	if (sealed.size() > 1) {
		require(bytes <= config.maxBytes);
	}
	if (!sealed.empty()) {
		require(sealed.front()->endTime() > start);
	}
	// The byte budget never takes the newest sealed GOP, so only its age can.
	if (sealedEnd && *sealedEnd > start) {
		require(!sealed.empty() && sealed.back()->endTime() == *sealedEnd);
	}

	// After a restart GOPs also leave from the back, which this check does not model.
	if (restarted) {
		return;
	}
	const auto survivor = sealed.empty() ? before.end() : std::find(before.begin(), before.end(), sealed.front());
	const size_t droppedCount = static_cast<size_t>(survivor - before.begin());
	if (droppedCount == 0) {
		return;
	}
	const Gop &lastDropped = *before[droppedCount - 1];
	require(lastDropped.endTime() <= start || bytes + lastDropped.byteSize() > config.maxBytes);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);

	tapeloop::SourceBufferConfig config;
	config.window = Nanoseconds{input.i64()};
	config.maxBytes = input.below(uint64_t{1} << 20);
	config.frameDuration = Nanoseconds{1 + static_cast<int64_t>(input.below(1'000'000'000))};
	SourceBuffer buffer(config);
	SyncModel model;

	// Mostly a well-formed stream, with jumps to arbitrary values now and then: it
	// reaches deeper states than independent random fields would.
	Nanoseconds time{0};
	int64_t dts = 0;
	while (!input.empty()) {
		switch (input.byte() % 6) {
		case 0:
		case 1: {
			if (input.flag()) {
				time = Nanoseconds{input.i64()};
				dts = input.i64();
			} else {
				time = tapeloop::saturatingAdd(
					time, Nanoseconds{static_cast<int64_t>(input.below(50'000'000))});
				dts = static_cast<int64_t>(static_cast<uint64_t>(dts) + 1);
			}
			const int64_t pts = input.byte() % 16 == 0 ? input.i64() : dts;
			const bool keyframe = input.byte() % 8 == 0;
			const std::span<const uint8_t> payload = input.bytes(input.below(256));
			const EncodedPacket packet{payload, pts, dts, time, keyframe};

			// A synced run has a GOP open, which a continuing keyframe seals.
			const bool restarted = keyframe && (!model.synced || model.breaksRun(packet));
			const bool sealsOpenGop = keyframe && model.synced && !restarted;
			GopList before = heldGops(buffer);
			std::optional<Nanoseconds> sealedEnd;
			if (model.synced && !before.empty()) {
				if (sealsOpenGop) {
					sealedEnd = before.back()->endTime();
				}
				before.pop_back();
			}

			buffer.push(packet);
			if (model.push(packet)) {
				checkKept(buffer, packet, model.codec, model.codecConfig);
				if (keyframe) {
					checkEviction(buffer, config, packet.time, before, sealedEnd, restarted);
				}
			}
			break;
		}
		case 2: {
			const Nanoseconds duration{input.i64()};
			const std::vector<Nanoseconds> held = heldTimes(buffer);
			const Clip clip = buffer.clip(duration);
			if (held.empty()) {
				require(clip.empty());
			} else {
				checkRange(clip, held,
					   tapeloop::saturatingSub(held.back(), std::max(duration, Nanoseconds{0})),
					   held.back());
			}
			break;
		}
		case 3: {
			const Nanoseconds from{input.i64()};
			const Nanoseconds to{input.i64()};
			checkRange(buffer.clip(from, to), heldTimes(buffer), from, to);
			break;
		}
		case 4: {
			buffer.clear();
			// The configuration outlives a clear.
			model.synced = false;
			model.dropped = 0;
			model.discontinuities = 0;
			model.configOfFrame.clear();
			break;
		}
		case 5: {
			const std::span<const uint8_t> codecConfig = input.bytes(input.below(64));
			// Taken from what was already read rather than from a new input byte, so the
			// seeds still read the same, and not from the bytes alone, so either codec comes
			// with any configuration, none included.
			const VideoCodec codec = ((codecConfig.size() + model.configs.size()) & 1) != 0
							 ? VideoCodec::Hevc
							 : VideoCodec::H264;
			buffer.setCodecConfig(codec, codecConfig);
			model.setCodecConfig(codec, codecConfig);
			break;
		}
		}
		checkBuffer(buffer, model);
	}
	return 0;
}
