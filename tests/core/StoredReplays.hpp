// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/FileIo.hpp"
#include "core/Gop.hpp"
#include "core/ReplayFormat.hpp"
#include "core/ReplayWriter.hpp"

#include "SyntheticEncoder.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Captures of synthetic GOPs, and reading them back from disk, for the tests of the
// replay writer, store and reader.
namespace tapeloop::test {

using Gops = std::vector<std::shared_ptr<const Gop>>;

inline constexpr int64_t kGopLength = 5;

inline std::shared_ptr<const CodecConfig> h264Config()
{
	static const auto config = std::make_shared<const CodecConfig>(CodecConfig{0, 0, 0, 1, 0x67, 0x42});
	return config;
}

// GOPs of five frames from firstFrame on, as one encoder run would give them.
inline Gops makeGops(size_t count, int64_t firstFrame = 0, VideoCodec codec = VideoCodec::H264,
		     std::shared_ptr<const CodecConfig> config = h264Config())
{
	SyntheticEncoder::Config settings;
	settings.gopLength = kGopLength;
	settings.keyframeSize = 3000;
	settings.frameSize = 700;
	settings.firstFrame = firstFrame;
	SyntheticEncoder encoder(settings);
	GopBuilder builder(encoder.frameDuration());
	builder.setCodecConfig(codec, std::move(config));
	Gops gops;
	for (size_t gop = 0; gop < count; ++gop) {
		for (int64_t frame = 0; frame < kGopLength; ++frame) {
			builder.append(encoder.next());
		}
		gops.push_back(builder.seal());
	}
	return gops;
}

inline Gops slice(const Gops &gops, size_t from, size_t to)
{
	return {gops.begin() + static_cast<std::ptrdiff_t>(from), gops.begin() + static_cast<std::ptrdiff_t>(to)};
}

inline CaptureSource sourceOf(const std::string &key, const Gops &gops)
{
	return {key, "Camera " + key, Clip(gops, gops.front()->startTime(), gops.back()->lastTime())};
}

inline ReplayCapture captureOf(const std::filesystem::path &folder, std::vector<CaptureSource> sources,
			       const std::string &stem = "2026-10-09 21-05-42")
{
	ReplayCapture capture;
	capture.id = tapeloop::newReplayId();
	capture.folder = folder;
	capture.stem = stem;
	capture.capturedAtUtc = 1791600000000000000;
	capture.start = sources.front().clip.in();
	capture.end = sources.front().clip.out();
	for (const CaptureSource &source : sources) {
		capture.start = std::min(capture.start, source.clip.in());
		capture.end = std::max(capture.end, source.clip.out());
	}
	capture.capturedAtClock = capture.end;
	capture.sources = std::move(sources);
	return capture;
}

inline std::vector<uint8_t> fileBytes(const std::filesystem::path &path)
{
	File file(path, File::Mode::ReadOnly);
	std::vector<uint8_t> bytes(static_cast<size_t>(file.size()));
	REQUIRE(file.readAt(0, bytes) == bytes.size());
	return bytes;
}

inline std::optional<ReplayIndex> readManifest(const std::filesystem::path &manifest)
{
	return tapeloop::decodeManifest(fileBytes(manifest));
}

inline std::shared_ptr<const Gop> readBack(const std::filesystem::path &manifest, const StoredSource &source,
					   size_t gop)
{
	const tapeloop::StoredGop &stored = source.gops.at(gop);
	File file(tapeloop::segmentPath(manifest, source, stored.segment), File::Mode::ReadOnly);
	std::vector<uint8_t> chunk(static_cast<size_t>(stored.size));
	if (file.readAt(stored.offset, chunk) != chunk.size()) {
		return nullptr;
	}
	return tapeloop::decodeGopChunk(chunk, stored.headerCrc, source.runs.at(stored.run));
}

inline bool sameGop(const Gop &a, const Gop &b)
{
	if (a.packets().size() != b.packets().size() || a.codec() != b.codec() ||
	    a.frameDuration() != b.frameDuration() || (a.codecConfig() == nullptr) != (b.codecConfig() == nullptr) ||
	    (a.codecConfig() && *a.codecConfig() != *b.codecConfig())) {
		return false;
	}
	for (size_t i = 0; i < a.packets().size(); ++i) {
		const tapeloop::PacketRecord &x = a.packets()[i];
		const tapeloop::PacketRecord &y = b.packets()[i];
		const std::span<const uint8_t> dataX = a.packetData(i);
		const std::span<const uint8_t> dataY = b.packetData(i);
		if (x.pts != y.pts || x.dts != y.dts || x.time != y.time || x.keyframe != y.keyframe ||
		    !std::equal(dataX.begin(), dataX.end(), dataY.begin(), dataY.end())) {
			return false;
		}
	}
	return true;
}

// Every GOP of the source in the manifest reads back as the GOP it was written from.
inline void requireReadsBack(const std::filesystem::path &manifest, const StoredSource &source, const Gops &gops)
{
	REQUIRE(source.gops.size() == gops.size());
	for (size_t i = 0; i < gops.size(); ++i) {
		const std::shared_ptr<const Gop> read = readBack(manifest, source, i);
		REQUIRE(read);
		CHECK(sameGop(*read, *gops[i]));
	}
}

inline std::vector<std::string> segmentsOf(const std::filesystem::path &folder)
{
	std::vector<std::string> names;
	for (const auto &entry : std::filesystem::directory_iterator(folder / "data")) {
		names.push_back(tapeloop::utf8FromPath(entry.path().filename()));
	}
	std::sort(names.begin(), names.end());
	return names;
}

} // namespace tapeloop::test
