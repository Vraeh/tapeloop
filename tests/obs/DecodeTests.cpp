// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "../core/TempDirectory.hpp"
#include "AllocationCounter.hpp"
#include "ClipDecoder.hpp"
#include "Mp4Reader.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "core/DecodePlanner.hpp"
#include "core/FileIo.hpp"
#include "core/SourceBuffer.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "decode/FFmpegHeaders.hpp"
#include "decode/FFmpegVersion.hpp"
#include "decode/Mp4Writer.hpp"
#include "obs/SourceCapture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using tapeloop::Clip;
using tapeloop::CodecConfig;
using tapeloop::DecodedFrame;
using tapeloop::DecodePlanner;
using tapeloop::DecodeStatus;
using tapeloop::EncodedPacket;
using tapeloop::Gop;
using tapeloop::GopBuilder;
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::SourceBuffer;
using tapeloop::VideoCodec;
using tapeloop::decode::FFmpegDecoder;
using tapeloop::decode::Mp4Writer;
using tapeloop::decode::ColorMatrix;
using tapeloop::decode::Picture;
using tapeloop::obs::SourceCapture;
using tapeloop::obs::StartResult;
using tapeloop::test::annexBUnits;
using tapeloop::test::createTestPattern;
using tapeloop::test::Mp4Edit;
using tapeloop::test::Mp4Sample;
using tapeloop::test::Mp4Track;
using tapeloop::test::ObsFixture;
using tapeloop::test::waitFor;

namespace {

constexpr Nanoseconds kFrameInterval{33'333'333};
const std::string kHevcPattern = std::string(TAPELOOP_TEST_DATA_DIR) + "/hevc-pattern.bin";
const std::string kHevcFullRange601 = std::string(TAPELOOP_TEST_DATA_DIR) + "/hevc-pattern-full-601.bin";
const std::string kHevcMain10 = std::string(TAPELOOP_TEST_DATA_DIR) + "/hevc-pattern-main10.bin";

// A frame of a clip: when it shows, and the number the test pattern drew into it.
struct Expected {
	Nanoseconds time{0};
	uint32_t number = 0;
};

struct RecordedPacket {
	std::vector<uint8_t> data;
	bool keyframe = false;
};

// Packets written by tests/obs/data/make-hevc-pattern.py; frame n shows the number n.
struct RecordedRun {
	CodecConfig config;
	std::vector<RecordedPacket> packets;
};

RecordedRun loadRecordedRun(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	REQUIRE(file);
	const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	RecordedRun run;
	size_t at = 0;
	while (at < bytes.size()) {
		REQUIRE(at + 5 <= bytes.size());
		const uint8_t kind = bytes[at];
		const size_t size = static_cast<uint32_t>(bytes[at + 1]) | static_cast<uint32_t>(bytes[at + 2]) << 8 |
				    static_cast<uint32_t>(bytes[at + 3]) << 16 |
				    static_cast<uint32_t>(bytes[at + 4]) << 24;
		at += 5;
		REQUIRE(at + size <= bytes.size());
		std::vector<uint8_t> data(bytes.begin() + static_cast<ptrdiff_t>(at),
					  bytes.begin() + static_cast<ptrdiff_t>(at + size));
		at += size;
		if (kind == 0) {
			run.config = std::move(data);
		} else {
			run.packets.push_back({std::move(data), kind == 1});
		}
	}
	return run;
}

SourceBuffer makeBuffer()
{
	return SourceBuffer({1h, SIZE_MAX, kFrameInterval});
}

// Captures the test pattern with x264 until the buffer holds that many GOPs.
Clip captureClip(size_t gops, uint32_t width = 640, uint32_t height = 360)
{
	OBSSourceAutoRelease pattern = createTestPattern(width, height);
	SourceCapture capture;
	REQUIRE(capture.start(pattern, {}) == StartResult::Started);
	REQUIRE(waitFor([&] { return capture.buffer() && capture.buffer()->stats().gopCount >= gops; }, 60s));
	capture.stop();
	return capture.buffer()->clip(Nanoseconds::min(), Nanoseconds::max());
}

// What an independent decode of every GOP reads in each frame, in clip order.
std::vector<Expected> expectedFrames(const Clip &clip)
{
	const auto decoded = tapeloop::test::decodeGops(clip);
	REQUIRE(decoded.size() == clip.gops().size());
	std::vector<Expected> frames;
	for (size_t gop = 0; gop < decoded.size(); ++gop) {
		const auto packets = clip.gops()[gop]->packets();
		REQUIRE(decoded[gop].size() == packets.size());
		for (size_t i = 0; i < packets.size(); ++i) {
			REQUIRE(decoded[gop][i].frameNumber);
			frames.push_back({packets[i].time, *decoded[gop][i].frameNumber});
		}
	}
	return frames;
}

// Pushes every packet of a captured clip, shifted to start at `start` and `firstPts`,
// with or without the parameter sets the encoder repeats in the stream.
void pushClip(SourceBuffer &buffer, const Clip &clip, Nanoseconds start, int64_t firstPts, bool keepParameterSets,
	      std::vector<Expected> &expected)
{
	const std::vector<Expected> numbers = expectedFrames(clip);
	const tapeloop::PacketRecord &first = clip.gops().front()->packets().front();
	size_t frame = 0;
	for (const auto &gop : clip.gops()) {
		for (size_t i = 0; i < gop->packets().size(); ++i, ++frame) {
			const tapeloop::PacketRecord &record = gop->packets()[i];
			const std::vector<uint8_t> stripped =
				tapeloop::test::withoutParameterSets(gop->packetData(i), gop->codec());
			EncodedPacket packet;
			packet.data = keepParameterSets ? gop->packetData(i) : std::span<const uint8_t>(stripped);
			packet.pts = firstPts + record.pts - first.pts;
			packet.dts = firstPts + record.dts - first.pts;
			packet.time = start + (record.time - first.time);
			packet.keyframe = i == 0;
			buffer.push(packet);
			expected.push_back({packet.time, numbers[frame].number});
		}
	}
}

void pushRecorded(SourceBuffer &buffer, const RecordedRun &run, Nanoseconds start, int64_t firstPts,
		  bool keepParameterSets, std::vector<Expected> &expected)
{
	for (size_t i = 0; i < run.packets.size(); ++i) {
		const std::vector<uint8_t> stripped =
			tapeloop::test::withoutParameterSets(run.packets[i].data, VideoCodec::Hevc);
		EncodedPacket packet;
		packet.data = keepParameterSets ? std::span<const uint8_t>(run.packets[i].data)
						: std::span<const uint8_t>(stripped);
		packet.pts = firstPts + static_cast<int64_t>(i);
		packet.dts = packet.pts;
		packet.time = start + kFrameInterval * static_cast<int64_t>(i);
		packet.keyframe = run.packets[i].keyframe;
		buffer.push(packet);
		expected.push_back({packet.time, static_cast<uint32_t>(i)});
	}
}

std::optional<uint32_t> numberOf(const FFmpegDecoder &decoder, const DecodedFrame &frame)
{
	const std::optional<Picture> picture = decoder.picture(frame);
	REQUIRE(picture);
	REQUIRE(picture->texture == nullptr);
	return tapeloop::test::readFrameNumber(picture->planes[0], static_cast<ptrdiff_t>(picture->strides[0]),
					       static_cast<int>(picture->width), static_cast<int>(picture->height));
}

void checkFrame(DecodePlanner &planner, const FFmpegDecoder &decoder, const Expected &expected)
{
	CAPTURE(expected.time.count(), expected.number);
	const tapeloop::DecodeResult result = planner.frameAt(expected.time);
	REQUIRE(result.status == DecodeStatus::Ok);
	CHECK(numberOf(decoder, result.frame) == expected.number);
}

// Every frame forward, every frame backward with the previous GOP decoded ahead, and
// every frame again in an order that jumps across GOPs.
void checkEveryWay(DecodePlanner &planner, const FFmpegDecoder &decoder, const std::vector<Expected> &expected)
{
	for (const Expected &frame : expected) {
		checkFrame(planner, decoder, frame);
	}
	for (auto frame = expected.rbegin(); frame != expected.rend(); ++frame) {
		checkFrame(planner, decoder, *frame);
		REQUIRE(planner.prefetch(PlayDirection::Backward) == DecodeStatus::Ok);
	}
	// A stride that shares no factor with the frame count visits every frame once.
	size_t stride = 37;
	while (expected.size() % stride == 0) {
		++stride;
	}
	for (size_t i = 0; i < expected.size(); ++i) {
		checkFrame(planner, decoder, expected[i * stride % expected.size()]);
	}
	CHECK(decoder.heldFrames() == planner.heldFrames());
}

} // namespace

TEST_CASE("decoding uses the FFmpeg built for the plugin", "[decode]")
{
	CHECK(std::string(tapeloop::decode::ffmpegVersion()) == "8.1.3");
	// Headers and libraries from the same build, not the system's libraries.
	CHECK(avcodec_version() == LIBAVCODEC_VERSION_INT);
	CHECK(avutil_version() == LIBAVUTIL_VERSION_INT);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr);
	CHECK(avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) == nullptr);
}

#ifdef _WIN32
TEST_CASE("the FFmpeg built for the plugin decodes H.264 and HEVC on a D3D11 device", "[decode]")
{
	// FFmpeg's configure can keep a hwaccel whose code its Makefile then leaves out, and
	// the decoders offer AV_PIX_FMT_D3D11 only when the older d3d11va hwaccels are in.
	for (const AVCodecID id : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC}) {
		const AVCodec *codec = avcodec_find_decoder(id);
		REQUIRE(codec);
		bool onDevice = false;
		for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i); ++i) {
			onDevice = onDevice || (config->pix_fmt == AV_PIX_FMT_D3D11 &&
						(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0);
		}
		CAPTURE(codec->name);
		CHECK(onDevice);
	}
}
#endif

TEST_CASE_METHOD(ObsFixture, "the FFmpeg decoder shows every frame of a capture, either way and after seeks",
		 "[decode]")
{
	const Clip clip = captureClip(4);
	REQUIRE(clip.gops().size() >= 4);
	const std::vector<Expected> expected = expectedFrames(clip);

	FFmpegDecoder decoder;
	CHECK_FALSE(decoder.hasDevice());
	DecodePlanner planner(decoder);
	planner.load(clip);

	// A baseline of what decoding costs on the CPU; the real numbers come from the PC.
	const auto started = std::chrono::steady_clock::now();
	for (const Expected &frame : expected) {
		checkFrame(planner, decoder, frame);
	}
	const double elapsed =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
	CHECK(planner.work().framesReceived == expected.size());
	std::printf("[decode] %zu frames of 640x360 H.264 forward in %.1f ms, %.3f ms each\n", expected.size(), elapsed,
		    elapsed / static_cast<double>(expected.size()));

	// The harness's canvas is BT.709 at limited range, as OBS's is by default.
	const tapeloop::DecodeResult first = planner.frameAt(expected.front().time);
	REQUIRE(first.status == DecodeStatus::Ok);
	const std::optional<Picture> picture = decoder.picture(first.frame);
	REQUIRE(picture);
	CHECK(picture->matrix == ColorMatrix::Bt709);
	CHECK_FALSE(picture->fullRange);

	checkEveryWay(planner, decoder, expected);
	planner.load(Clip{});
	CHECK(decoder.heldFrames() == 0);
}

TEST_CASE_METHOD(ObsFixture, "the FFmpeg decoder takes parameter sets from the configuration or the stream", "[decode]")
{
	const Clip captured = captureClip(3);
	const CodecConfig *h264Config = captured.gops().front()->codecConfig();
	REQUIRE(h264Config);
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);
	REQUIRE(hevc.packets.size() == 30);

	for (const bool inStream : {false, true}) {
		CAPTURE(inStream);
		SourceBuffer h264Buffer = makeBuffer();
		h264Buffer.setCodecConfig(VideoCodec::H264, inStream ? std::span<const uint8_t>()
								     : std::span<const uint8_t>(*h264Config));
		std::vector<Expected> h264Expected;
		pushClip(h264Buffer, captured, 1s, 0, inStream, h264Expected);

		SourceBuffer hevcBuffer = makeBuffer();
		hevcBuffer.setCodecConfig(VideoCodec::Hevc, inStream ? std::span<const uint8_t>()
								     : std::span<const uint8_t>(hevc.config));
		std::vector<Expected> hevcExpected;
		pushRecorded(hevcBuffer, hevc, 1s, 0, inStream, hevcExpected);

		FFmpegDecoder decoder;
		DecodePlanner planner(decoder);
		planner.load(h264Buffer.clip(Nanoseconds::min(), Nanoseconds::max()));
		checkEveryWay(planner, decoder, h264Expected);
		planner.load(hevcBuffer.clip(Nanoseconds::min(), Nanoseconds::max()));
		checkEveryWay(planner, decoder, hevcExpected);
	}
}

TEST_CASE_METHOD(ObsFixture, "the FFmpeg decoder takes packets that repeat the configuration's parameter sets",
		 "[decode]")
{
	// As OBS hands them over: the configuration from the encoder, and the same parameter
	// sets again before every keyframe, which the decoder leaves out.
	const Clip captured = captureClip(3);
	const CodecConfig *h264Config = captured.gops().front()->codecConfig();
	REQUIRE(h264Config);
	REQUIRE(tapeloop::test::withoutParameterSets(captured.gops()[1]->packetData(0), VideoCodec::H264).size() <
		captured.gops()[1]->packetData(0).size());
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);

	SourceBuffer h264Buffer = makeBuffer();
	h264Buffer.setCodecConfig(VideoCodec::H264, *h264Config);
	std::vector<Expected> h264Expected;
	pushClip(h264Buffer, captured, 1s, 0, true, h264Expected);
	SourceBuffer hevcBuffer = makeBuffer();
	hevcBuffer.setCodecConfig(VideoCodec::Hevc, hevc.config);
	std::vector<Expected> hevcExpected;
	pushRecorded(hevcBuffer, hevc, 1s, 0, true, hevcExpected);

	FFmpegDecoder decoder;
	DecodePlanner planner(decoder);
	planner.load(h264Buffer.clip(Nanoseconds::min(), Nanoseconds::max()));
	checkEveryWay(planner, decoder, h264Expected);
	const uint64_t h264Skipped = decoder.skippedBytes();
	CHECK(h264Skipped > 0);
	planner.load(hevcBuffer.clip(Nanoseconds::min(), Nanoseconds::max()));
	checkEveryWay(planner, decoder, hevcExpected);
	CHECK(decoder.skippedBytes() > h264Skipped);
}

TEST_CASE_METHOD(ObsFixture, "the FFmpeg decoder takes parameter sets that differ from the configuration", "[decode]")
{
	// A run whose stream carries parameter sets other than those of its configuration,
	// here those of a capture of another size: the packets go whole, with the sets they
	// share with the configuration, which refer to the new ones.
	const Clip other = captureClip(1, 320, 180);
	const CodecConfig *otherConfig = other.gops().front()->codecConfig();
	REQUIRE(otherConfig);
	const Clip captured = captureClip(3);

	SourceBuffer buffer = makeBuffer();
	buffer.setCodecConfig(VideoCodec::H264, *otherConfig);
	std::vector<Expected> expected;
	pushClip(buffer, captured, 1s, 0, true, expected);
	FFmpegDecoder decoder;
	DecodePlanner planner(decoder);
	planner.load(buffer.clip(Nanoseconds::min(), Nanoseconds::max()));
	checkEveryWay(planner, decoder, expected);
	CHECK(decoder.skippedBytes() == 0);
}

TEST_CASE_METHOD(ObsFixture, "the FFmpeg decoder follows a clip from an HEVC run into an H.264 run", "[decode]")
{
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);
	const Clip captured = captureClip(2);
	REQUIRE(captured.gops().front()->codecConfig());

	// As a capture whose HEVC encoder failed and restarted on H.264.
	SourceBuffer buffer = makeBuffer();
	std::vector<Expected> expected;
	buffer.setCodecConfig(VideoCodec::Hevc, hevc.config);
	pushRecorded(buffer, hevc, 1s, 0, true, expected);
	buffer.setCodecConfig(VideoCodec::H264, *captured.gops().front()->codecConfig());
	pushClip(buffer, captured, expected.back().time + kFrameInterval, static_cast<int64_t>(expected.size()), true,
		 expected);
	const Clip clip = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	REQUIRE(clip.gops().front()->codec() == VideoCodec::Hevc);
	REQUIRE(clip.gops().back()->codec() == VideoCodec::H264);

	FFmpegDecoder decoder;
	DecodePlanner planner(decoder);
	planner.load(clip);
	checkEveryWay(planner, decoder, expected);
	// Across the change and back, more than once.
	CHECK(planner.work().opens >= 4);
}

TEST_CASE("the FFmpeg decoder hands out each frame once and takes it back", "[decode]")
{
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);
	FFmpegDecoder decoder;
	DecodedFrame frame;
	CHECK(decoder.send(hevc.packets[0].data, 0, 0) == DecodeStatus::InvalidData);
	CHECK(decoder.receive(frame) == DecodeStatus::InvalidData);
	CHECK(decoder.flush() == DecodeStatus::InvalidData);

	REQUIRE(decoder.open(VideoCodec::Hevc, hevc.config) == DecodeStatus::Ok);
	std::vector<DecodedFrame> frames;
	const auto receiveAll = [&] {
		DecodeStatus status = DecodeStatus::Ok;
		while ((status = decoder.receive(frame)) == DecodeStatus::Ok) {
			frames.push_back(frame);
		}
		return status;
	};
	// One frame out for each packet in, with no delay: a frame thread would hold one back.
	for (int64_t i = 0; i < 10; ++i) {
		REQUIRE(decoder.send(hevc.packets[static_cast<size_t>(i)].data, i, i) == DecodeStatus::Ok);
		REQUIRE(receiveAll() == DecodeStatus::NeedMore);
		CHECK(frames.size() == static_cast<size_t>(i + 1));
	}
	REQUIRE(decoder.flush() == DecodeStatus::Ok);
	REQUIRE(receiveAll() == DecodeStatus::Drained);
	REQUIRE(frames.size() == 10);
	for (size_t i = 0; i < frames.size(); ++i) {
		CHECK(frames[i].pts == static_cast<int64_t>(i));
		CHECK(numberOf(decoder, frames[i]) == i);
		// What the planner's cap counts: at least the picture's own 4:2:0 planes.
		CHECK(frames[i].bytes >= size_t{320} * 180 * 3 / 2);
		CHECK(frames[i].bytes < size_t{320} * 180 * 4);
	}
	CHECK(decoder.heldFrames() == 10);
	CHECK_FALSE(decoder.picture(DecodedFrame{}));
	// An id past every slot names nothing either.
	DecodedFrame stranger;
	stranger.id = (uint64_t{1} << 32) | 1000;
	CHECK_FALSE(decoder.picture(stranger));
	decoder.release(stranger);
	CHECK(decoder.heldFrames() == 10);

	decoder.release(frames[3]);
	CHECK_FALSE(decoder.picture(frames[3]));
	decoder.release(frames[3]);
	CHECK(decoder.heldFrames() == 9);

	// The next frame takes the slot frames[3] gave back, under another id.
	decoder.reset();
	REQUIRE(decoder.send(hevc.packets[0].data, 100, 100) == DecodeStatus::Ok);
	REQUIRE(decoder.flush() == DecodeStatus::Ok);
	REQUIRE(receiveAll() == DecodeStatus::Drained);
	REQUIRE(frames.size() == 11);
	CHECK(frames.back().id != frames[3].id);
	CHECK(frames.back().pts == 100);
	CHECK_FALSE(decoder.picture(frames[3]));

	// Frames outlive the run they came from.
	REQUIRE(decoder.open(VideoCodec::Hevc, hevc.config) == DecodeStatus::Ok);
	CHECK(numberOf(decoder, frames[0]) == 0u);
	CHECK(numberOf(decoder, frames[9]) == 9u);

	decoder.close();
	CHECK(decoder.heldFrames() == 0);
	CHECK_FALSE(decoder.picture(frames[0]));
	CHECK(decoder.receive(frame) == DecodeStatus::InvalidData);
}

TEST_CASE("the FFmpeg decoder takes frames back without allocating", "[decode]")
{
	if (!tapeloop::test::kExactAllocationCounts) {
		SKIP("allocations cannot be counted exactly in this configuration");
	}
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);
	FFmpegDecoder decoder;
	REQUIRE(decoder.open(VideoCodec::Hevc, hevc.config) == DecodeStatus::Ok);
	std::vector<DecodedFrame> frames;
	for (int64_t i = 0; i < 10; ++i) {
		REQUIRE(decoder.send(hevc.packets[static_cast<size_t>(i)].data, i, i) == DecodeStatus::Ok);
		DecodedFrame frame;
		while (decoder.receive(frame) == DecodeStatus::Ok) {
			frames.push_back(frame);
		}
	}
	REQUIRE(frames.size() == 10);
	tapeloop::test::AllocationCounter counter;
	for (const DecodedFrame &frame : frames) {
		decoder.release(frame);
	}
	CHECK(counter.count() == 0);
	CHECK(decoder.heldFrames() == 0);
}

TEST_CASE("the FFmpeg decoder reports the range and matrix of the stream", "[decode]")
{
	struct Case {
		const std::string *path;
		bool fullRange;
		ColorMatrix matrix;
	};
	for (const Case &each : {Case{&kHevcPattern, false, ColorMatrix::Unspecified},
				 Case{&kHevcFullRange601, true, ColorMatrix::Bt601}}) {
		CAPTURE(*each.path);
		const RecordedRun run = loadRecordedRun(*each.path);
		FFmpegDecoder decoder;
		REQUIRE(decoder.open(VideoCodec::Hevc, run.config) == DecodeStatus::Ok);
		REQUIRE(decoder.send(run.packets[0].data, 0, 0) == DecodeStatus::Ok);
		DecodedFrame frame;
		REQUIRE(decoder.receive(frame) == DecodeStatus::Ok);
		const std::optional<Picture> picture = decoder.picture(frame);
		REQUIRE(picture);
		CHECK(picture->fullRange == each.fullRange);
		CHECK(picture->matrix == each.matrix);
		CHECK(numberOf(decoder, frame) == 0u);
	}
}

TEST_CASE("the FFmpeg decoder refuses pictures of another format and keeps nothing of them", "[decode]")
{
	const RecordedRun main10 = loadRecordedRun(kHevcMain10);
	FFmpegDecoder decoder;
	REQUIRE(decoder.open(VideoCodec::Hevc, main10.config) == DecodeStatus::Ok);
	for (int64_t i = 0; i < 3; ++i) {
		REQUIRE(decoder.send(main10.packets[static_cast<size_t>(i)].data, i, i) == DecodeStatus::Ok);
		DecodedFrame frame;
		CHECK(decoder.receive(frame) == DecodeStatus::Unsupported);
		CHECK(decoder.receive(frame) == DecodeStatus::NeedMore);
		CHECK(decoder.heldFrames() == 0);
	}
}

TEST_CASE("a damaged configuration fails the frames that need it, not the decoder", "[decode]")
{
	const RecordedRun hevc = loadRecordedRun(kHevcPattern);
	// An hvcC header that ends after its fourth byte. FFmpeg opens on it anyway.
	const uint8_t broken[] = {1, 1, 0x60, 0};
	SourceBuffer damaged = makeBuffer();
	damaged.setCodecConfig(VideoCodec::Hevc, broken);
	std::vector<Expected> expected;
	pushRecorded(damaged, hevc, 1s, 0, false, expected);

	FFmpegDecoder decoder;
	DecodePlanner planner(decoder);
	planner.load(damaged.clip(Nanoseconds::min(), Nanoseconds::max()));
	CHECK(planner.frameAt(expected.front().time).status == DecodeStatus::InvalidData);
	CHECK(planner.frameAt(expected.back().time).status == DecodeStatus::InvalidData);

	// An hvcC long enough to be read as one, whose first parameter set runs past its end:
	// FFmpeg refuses to open on it.
	std::vector<uint8_t> truncated(32, 0);
	truncated[0] = 1;
	truncated[21] = 3;
	truncated[22] = 1;
	truncated[23] = 32;
	truncated[25] = 1;
	truncated[26] = 0xff;
	truncated[27] = 0xff;
	FFmpegDecoder refusing;
	CHECK(refusing.open(VideoCodec::Hevc, truncated) == DecodeStatus::InvalidData);
	SourceBuffer rejected = makeBuffer();
	rejected.setCodecConfig(VideoCodec::Hevc, truncated);
	expected.clear();
	pushRecorded(rejected, hevc, 1s, 0, false, expected);
	planner.load(rejected.clip(Nanoseconds::min(), Nanoseconds::max()));
	CHECK(planner.frameAt(expected.front().time).status == DecodeStatus::InvalidData);

	SourceBuffer intact = makeBuffer();
	intact.setCodecConfig(VideoCodec::Hevc, hevc.config);
	expected.clear();
	pushRecorded(intact, hevc, 1s, 0, false, expected);
	planner.load(intact.clip(Nanoseconds::min(), Nanoseconds::max()));
	checkFrame(planner, decoder, expected.back());
}

namespace {

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary);
	REQUIRE(file);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int64_t mp4Ticks(Nanoseconds time)
{
	return av_rescale_q_rnd(time.count(), {1, 1'000'000'000}, {1, 120'000}, AV_ROUND_NEAR_INF);
}

// Writes every packet of the clip, at its time from origin.
void writeMp4(const std::filesystem::path &path, const Clip &clip, Nanoseconds origin)
{
	Mp4Writer writer;
	writer.open(path, *clip.gops().front());
	for (const auto &gop : clip.gops()) {
		for (size_t i = 0; i < gop->packets().size(); ++i) {
			writer.write(*gop, i, gop->packets()[i].time - origin);
		}
	}
	writer.finish();
}

// The samples of the MP4 as GOPs again, each run with the configuration of its sample
// entry, as a player reading the file has them.
std::vector<std::shared_ptr<const Gop>> gopsOf(const Mp4Track &track, VideoCodec codec)
{
	GopBuilder builder(kFrameInterval);
	std::vector<std::shared_ptr<const Gop>> gops;
	uint32_t entry = 0;
	for (const Mp4Sample &sample : track.samples) {
		if (sample.sync && !builder.empty()) {
			gops.push_back(builder.seal());
		}
		if (sample.entry != entry) {
			REQUIRE(builder.empty());
			builder.setCodecConfig(
				codec, std::make_shared<const CodecConfig>(track.entries.at(sample.entry - 1).config));
			entry = sample.entry;
		}
		EncodedPacket packet;
		packet.data = sample.data;
		packet.pts = sample.pts;
		packet.dts = sample.dts;
		packet.time = Nanoseconds{av_rescale(sample.pts, 1'000'000'000, track.timescale)};
		packet.keyframe = sample.sync;
		builder.append(packet);
	}
	gops.push_back(builder.seal());
	return gops;
}

std::vector<uint32_t> numbersOf(std::span<const std::shared_ptr<const Gop>> gops)
{
	std::vector<uint32_t> numbers;
	const Clip clip({gops.begin(), gops.end()}, gops.front()->startTime(), gops.back()->lastTime());
	for (const auto &frames : tapeloop::test::decodeGops(clip)) {
		for (const tapeloop::test::DecodedFrame &frame : frames) {
			REQUIRE(frame.frameNumber);
			numbers.push_back(*frame.frameNumber);
		}
	}
	return numbers;
}

std::vector<std::vector<uint8_t>> sorted(std::vector<std::vector<uint8_t>> units)
{
	std::sort(units.begin(), units.end());
	return units;
}

bool isHevcParameterSet(const std::vector<uint8_t> &unit)
{
	const int type = (unit.at(0) >> 1) & 0x3F;
	return type >= 32 && type <= 34;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "an MP4 holds the packets of an H.264 capture as encoded and plays from the in point",
		 "[decode][export]")
{
	const Clip clip = captureClip(3);
	const auto &gops = clip.gops();
	const Nanoseconds start = gops.front()->startTime();
	const Nanoseconds in = gops.front()->packets()[2].time;
	tapeloop::test::TempDirectory dir;
	// A name that is not ASCII reaches the file as UTF-8.
	const std::filesystem::path path = dir.path() / tapeloop::pathFromUtf8("C\xC3\xA1mara 1.mp4");
	writeMp4(path, clip, in);

	const Mp4Track track = tapeloop::test::readMp4(readFile(path));
	CHECK(track.timescale == 120'000);
	REQUIRE(track.entries.size() == 1);
	CHECK(track.entries[0].type == "avc1");
	CHECK(track.entries[0].width == 640);
	CHECK(track.entries[0].height == 360);
	CHECK(annexBUnits(track.entries[0].config) == annexBUnits(*gops.front()->codecConfig()));
	size_t sample = 0;
	for (const auto &gop : gops) {
		for (size_t i = 0; i < gop->packets().size(); ++i, ++sample) {
			CAPTURE(sample);
			REQUIRE(sample < track.samples.size());
			const Mp4Sample &read = track.samples[sample];
			CHECK(annexBUnits(read.data) == annexBUnits(gop->packetData(i)));
			CHECK(read.sync == (i == 0));
			CHECK(read.entry == 1);
			const Nanoseconds time = gop->packets()[i].time;
			CHECK(read.dts == mp4Ticks(time - in) - mp4Ticks(start - in));
			CHECK(read.pts == read.dts);
		}
	}
	CHECK(sample == track.samples.size());
	// The two frames before the in point are decoded but not shown.
	REQUIRE_FALSE(track.edits.empty());
	CHECK(track.edits.back().mediaTime == mp4Ticks(in - start));
	CHECK(numbersOf(gopsOf(track, VideoCodec::H264)) == numbersOf(gops));
}

TEST_CASE_METHOD(ObsFixture, "an MP4 of HEVC is hvc1, its parameter sets in the sample entry", "[decode][export]")
{
	const RecordedRun run = loadRecordedRun(kHevcPattern);
	SourceBuffer buffer = makeBuffer();
	buffer.setCodecConfig(VideoCodec::Hevc, run.config);
	std::vector<Expected> expected;
	pushRecorded(buffer, run, 1s, 0, true, expected);
	const Clip clip = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	tapeloop::test::TempDirectory dir;
	const std::filesystem::path path = dir.path() / "hevc.mp4";
	writeMp4(path, clip, clip.gops().front()->startTime());

	const Mp4Track track = tapeloop::test::readMp4(readFile(path));
	REQUIRE(track.entries.size() == 1);
	CHECK(track.entries[0].type == "hvc1");
	CHECK(sorted(annexBUnits(track.entries[0].config)) == sorted(annexBUnits(run.config)));
	REQUIRE(track.samples.size() == run.packets.size());
	for (size_t i = 0; i < run.packets.size(); ++i) {
		CAPTURE(i);
		// hvc1 keeps the parameter sets in the sample entry only.
		std::vector<std::vector<uint8_t>> units = annexBUnits(run.packets[i].data);
		units.erase(std::remove_if(units.begin(), units.end(), isHevcParameterSet), units.end());
		CHECK(annexBUnits(track.samples[i].data) == units);
		CHECK(track.samples[i].sync == run.packets[i].keyframe);
	}
	for (const Mp4Edit &edit : track.edits) {
		CHECK(edit.mediaTime == 0);
	}
	std::vector<uint32_t> numbers;
	for (const Expected &frame : expected) {
		numbers.push_back(frame.number);
	}
	CHECK(numbersOf(gopsOf(track, VideoCodec::Hevc)) == numbers);
}

TEST_CASE_METHOD(ObsFixture, "an MP4 gives each configuration of its codec a sample entry of its own",
		 "[decode][export]")
{
	const RecordedRun first = loadRecordedRun(kHevcPattern);
	const RecordedRun second = loadRecordedRun(kHevcFullRange601);
	REQUIRE(first.config != second.config);
	SourceBuffer buffer = makeBuffer();
	std::vector<Expected> expected;
	buffer.setCodecConfig(VideoCodec::Hevc, first.config);
	pushRecorded(buffer, first, 1s, 0, true, expected);
	// The encoder restarted: a gap, and its pts from zero again.
	const Nanoseconds restart = 1s + kFrameInterval * static_cast<int64_t>(first.packets.size()) + 100ms;
	buffer.setCodecConfig(VideoCodec::Hevc, second.config);
	pushRecorded(buffer, second, restart, 0, true, expected);
	const Clip clip = buffer.clip(Nanoseconds::min(), Nanoseconds::max());
	tapeloop::test::TempDirectory dir;
	const std::filesystem::path path = dir.path() / "two runs.mp4";
	writeMp4(path, clip, 1s);

	const Mp4Track track = tapeloop::test::readMp4(readFile(path));
	REQUIRE(track.entries.size() == 2);
	CHECK(sorted(annexBUnits(track.entries[1].config)) == sorted(annexBUnits(second.config)));
	REQUIRE(track.samples.size() == first.packets.size() + second.packets.size());
	const size_t split = first.packets.size();
	CHECK(track.samples[split - 1].entry == 1);
	CHECK(track.samples[split].entry == 2);
	CHECK(track.samples[split].dts - track.samples[split - 1].dts == mp4Ticks(kFrameInterval + 100ms));
	std::vector<uint32_t> numbers;
	for (const Expected &frame : expected) {
		numbers.push_back(frame.number);
	}
	CHECK(numbersOf(gopsOf(track, VideoCodec::Hevc)) == numbers);
}

TEST_CASE_METHOD(ObsFixture, "an MP4 that cannot be written says why", "[decode][export]")
{
	const RecordedRun run = loadRecordedRun(kHevcPattern);
	SourceBuffer buffer = makeBuffer();
	buffer.setCodecConfig(VideoCodec::Hevc, run.config);
	std::vector<Expected> expected;
	pushRecorded(buffer, run, 1s, 0, true, expected);
	const auto gop = buffer.clip(Nanoseconds::min(), Nanoseconds::max()).gops().front();
	tapeloop::test::TempDirectory dir;

	Mp4Writer writer;
	CHECK_THROWS_AS(writer.write(*gop, 0, Nanoseconds{0}), std::logic_error);
	const std::filesystem::path missing = dir.path() / "missing" / "x.mp4";
	try {
		writer.open(missing, *gop);
		FAIL("a file in a folder that does not exist was opened");
	} catch (const std::runtime_error &e) {
		CHECK(std::string(e.what()).find(tapeloop::utf8FromPath(missing)) != std::string::npos);
	}

	// Packets out of decode order are refused before they reach the file.
	writer.open(dir.path() / "order.mp4", *gop);
	writer.write(*gop, 1, kFrameInterval);
	CHECK_THROWS_AS(writer.write(*gop, 2, Nanoseconds{0}), std::runtime_error);

	// Pictures whose size cannot be read.
	GopBuilder builder(kFrameInterval);
	builder.setCodecConfig(VideoCodec::H264, nullptr);
	const std::vector<uint8_t> garbage(64, 0x55);
	builder.append({garbage, 0, 0, Nanoseconds{0}, true});
	CHECK_THROWS_AS(writer.open(dir.path() / "garbage.mp4", *builder.seal()), std::runtime_error);
}
