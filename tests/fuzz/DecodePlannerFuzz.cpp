// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/DecodePlanner.hpp"

#include "../core/CopiedGops.hpp"
#include "../core/FakeDecoder.hpp"
#include "../core/SyntheticEncoder.hpp"
#include "FuzzInput.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <utility>
#include <vector>

using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::DecodePlanner;
using tapeloop::DecodePlannerConfig;
using tapeloop::DecodeResult;
using tapeloop::DecodeStatus;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::VideoCodec;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;
using tapeloop::test::CopiedGops;
using tapeloop::test::FakeDecoder;
using tapeloop::test::SyntheticEncoder;

namespace {

// A clip of whole GOPs in runs, for each frame number the codec and configuration of its
// run, and the GOPs that cannot be read.
struct Model {
	int64_t gopLength = 1;
	int64_t frames = 0;
	std::vector<VideoCodec> codecOfFrame;
	std::vector<CodecConfig> configOfFrame;
	std::set<size_t> unreadable;
	Clip clip;

	size_t gopOf(int64_t frame) const { return static_cast<size_t>(frame / gopLength); }
};

SyntheticEncoder::Config encoderConfig(int64_t gopLength)
{
	SyntheticEncoder::Config config;
	config.gopLength = gopLength;
	return config;
}

Model readClip(FuzzInput &input)
{
	Model model;
	model.gopLength = 1 + input.byte() % 8;
	SyntheticEncoder encoder(encoderConfig(model.gopLength));
	GopBuilder builder(encoder.frameDuration());
	std::vector<std::shared_ptr<const tapeloop::Gop>> gops;
	const int runs = 1 + input.byte() % 4;
	for (int run = 0; run < runs; ++run) {
		const VideoCodec codec = input.flag() ? VideoCodec::Hevc : VideoCodec::H264;
		const std::span<const uint8_t> bytes = input.bytes(input.byte() % 4);
		const CodecConfig config(bytes.begin(), bytes.end());
		builder.setCodecConfig(codec, config.empty() ? nullptr : std::make_shared<const CodecConfig>(config));
		const int gopCount = 1 + input.byte() % 4;
		for (int gop = 0; gop < gopCount; ++gop) {
			for (int64_t i = 0; i < model.gopLength; ++i) {
				builder.append(encoder.next());
				model.codecOfFrame.push_back(codec);
				model.configOfFrame.push_back(config);
			}
			gops.push_back(builder.seal());
		}
	}
	// Sometimes GOPs that cannot be read, whose frames the planner reports as InvalidData.
	for (int fails = input.byte() % 8 - 5; fails > 0; --fails) {
		model.unreadable.insert(input.byte() % gops.size());
	}
	model.frames = static_cast<int64_t>(model.codecOfFrame.size());
	model.clip = Clip(gops, gops.front()->startTime(), gops.back()->lastTime());
	return model;
}

// Through copies, each with a configuration object of its own, as GOPs read from disk are.
void load(DecodePlanner &planner, const Model &model)
{
	auto gops = std::make_unique<CopiedGops>(model.clip);
	gops->unreadable = model.unreadable;
	planner.load(std::move(gops));
}

void checkAlways(const DecodePlanner &planner, const FakeDecoder &decoder, const DecodePlannerConfig &config,
		 const Model &model)
{
	require(decoder.violations == 0);
	require(decoder.outstanding.size() == planner.heldFrames());
	require(planner.keptGopCount() <= config.keptGops);
	require(planner.heldFrames() <= config.keptGops * static_cast<size_t>(model.gopLength));
	// Within the cap, but for the one frame asked for when it alone is larger.
	require(planner.heldBytes() == planner.heldFrames() * decoder.frameBytes);
	require(planner.heldBytes() <= std::max(config.maxBytes, decoder.frameBytes));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);
	DecodePlannerConfig config;
	config.keptGops = 1 + input.byte() % 3;
	// Mostly a cap that holds a few frames or none at all.
	if (input.byte() % 4 != 0) {
		config.maxBytes = input.byte() % 16 * 10;
	}
	auto gopLength = std::make_shared<int64_t>(1);
	FakeDecoder decoder([gopLength](int64_t pts) { return pts % *gopLength == 0; }, input.byte() % 4);
	decoder.frameBytes = 1 + input.byte() % 40;
	// Sometimes frames the decoder never gives, which the planner reports as InvalidData.
	for (int skips = input.byte() % 8 - 4; skips > 0; --skips) {
		decoder.dropped.insert(input.byte() % 64);
	}
	{
		DecodePlanner planner(decoder, config);
		Model model = readClip(input);
		*gopLength = model.gopLength;
		load(planner, model);
		int64_t last = 0;
		// The GOP of the frame asked for last, which the planner decodes ahead from.
		size_t lastGop = 0;

		while (!input.empty()) {
			switch (input.byte() % 6) {
			case 0:
			case 1:
			case 2: {
				// Mostly steps around the last frame, sometimes anywhere, and past the ends.
				int64_t frame = last + static_cast<int8_t>(input.byte()) % 6;
				if (input.byte() % 4 == 0) {
					frame = static_cast<int64_t>(input.byte()) - 4;
				}
				const SyntheticEncoder encoder(encoderConfig(model.gopLength));
				const DecodeResult result = planner.frameAt(encoder.timeOf(frame));
				const int64_t shown = std::clamp<int64_t>(frame, 0, model.frames - 1);
				last = shown;
				lastGop = model.gopOf(shown);
				if (model.unreadable.contains(lastGop) || decoder.dropped.contains(shown)) {
					require(result.status == DecodeStatus::InvalidData);
					break;
				}
				require(result.status == DecodeStatus::Ok);
				require(result.frame.pts == shown);
				require(decoder.made.at(result.frame.id).pts == shown);
				const FakeDecoder::Session &session = decoder.sessionOf(result.frame.id);
				require(session.codec == model.codecOfFrame[static_cast<size_t>(shown)]);
				require(session.config == model.configOfFrame[static_cast<size_t>(shown)]);
				break;
			}
			case 3: {
				const bool backward = input.flag();
				const DecodeStatus status =
					planner.prefetch(backward ? PlayDirection::Backward : PlayDirection::Forward);
				// Only reverse play decodes ahead, and only the GOP before the last one asked for.
				const bool unreadable = backward && lastGop > 0 &&
							model.unreadable.contains(lastGop - 1);
				require(status == DecodeStatus::Ok ||
					(unreadable && status == DecodeStatus::InvalidData));
				break;
			}
			case 4: {
				// A decoder that cannot open leaves the planner able to go on afterwards.
				decoder.openStatus = DecodeStatus::Unsupported;
				const SyntheticEncoder encoder(encoderConfig(model.gopLength));
				const int64_t frame = input.byte() % 64;
				const DecodeResult result = planner.frameAt(encoder.timeOf(frame));
				const int64_t shown = std::min(frame, model.frames - 1);
				lastGop = model.gopOf(shown);
				const bool skipped = model.unreadable.contains(lastGop) ||
						     decoder.dropped.contains(shown);
				require(result.status == (skipped ? DecodeStatus::InvalidData : DecodeStatus::Ok) ||
					result.status == DecodeStatus::Unsupported);
				decoder.openStatus = DecodeStatus::Ok;
				break;
			}
			default:
				model = readClip(input);
				*gopLength = model.gopLength;
				load(planner, model);
				require(decoder.outstanding.empty());
				last = 0;
				lastGop = 0;
				break;
			}
			checkAlways(planner, decoder, config, model);
		}
	}
	require(decoder.outstanding.empty());
	require(decoder.violations == 0);
	return 0;
}
