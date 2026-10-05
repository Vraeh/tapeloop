// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"
#include "ClipDecoder.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "core/DecodePlanner.hpp"
#include "core/SourceBuffer.hpp"
#include "decode/FFmpegDecoder.hpp"
#include "decode/FFmpegHeaders.hpp"
#include "decode/FFmpegVersion.hpp"
#include "obs/SourceCapture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
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
using tapeloop::Nanoseconds;
using tapeloop::PlayDirection;
using tapeloop::SourceBuffer;
using tapeloop::VideoCodec;
using tapeloop::decode::FFmpegDecoder;
using tapeloop::decode::ColorMatrix;
using tapeloop::decode::Picture;
using tapeloop::obs::SourceCapture;
using tapeloop::obs::StartResult;
using tapeloop::test::createTestPattern;
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
Clip captureClip(size_t gops)
{
	OBSSourceAutoRelease pattern = createTestPattern(640, 360);
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
