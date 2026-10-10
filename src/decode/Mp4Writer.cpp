// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/Mp4Writer.hpp"
#include "decode/FFmpegHeaders.hpp"

#include "core/FileIo.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tapeloop::decode {
namespace {

// Every common frame rate is a whole number of these: a frame at 59.94 fps is 2002 of
// them, at 50 fps 2400.
constexpr AVRational kTimeBase{1, 120'000};
constexpr AVRational kNanoseconds{1, 1'000'000'000};

std::runtime_error ffmpegError(const std::string &what, int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, text, sizeof(text));
	return std::runtime_error(what + ": " + text);
}

int64_t ticksOf(Nanoseconds time)
{
	return av_rescale_q_rnd(time.count(), kNanoseconds, kTimeBase,
				static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

AVCodecID codecIdOf(VideoCodec codec)
{
	return codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

// The size of the pictures, which the sample entry and the track header carry, from the
// parameter sets FFmpeg's parser reads in the configuration and the keyframe.
std::pair<int, int> pictureSize(const Gop &first)
{
	const CodecConfig *config = first.codecConfig();
	const std::span<const uint8_t> keyframe = first.packetData(0);
	std::vector<uint8_t> frame;
	if (config) {
		frame = *config;
	}
	frame.insert(frame.end(), keyframe.begin(), keyframe.end());
	const size_t size = frame.size();
	frame.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);

	AVCodecParserContext *parser = av_parser_init(codecIdOf(first.codec()));
	AVCodecContext *context = avcodec_alloc_context3(nullptr);
	std::pair<int, int> found{0, 0};
	if (parser && context) {
		parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
		uint8_t *out = nullptr;
		int outSize = 0;
		av_parser_parse2(parser, context, &out, &outSize, frame.data(), static_cast<int>(size), AV_NOPTS_VALUE,
				 AV_NOPTS_VALUE, 0);
		found = {parser->width, parser->height};
	}
	avcodec_free_context(&context);
	av_parser_close(parser);
	if (found.first <= 0 || found.second <= 0) {
		throw std::runtime_error("the size of the pictures cannot be read from their parameter sets");
	}
	return found;
}

} // namespace

Mp4Writer::~Mp4Writer()
{
	close();
}

void Mp4Writer::open(const std::filesystem::path &path, const Gop &first)
{
	close();
	config_ = first.codecConfig() ? *first.codecConfig() : CodecConfig{};
	frameTicks_ = std::max<int64_t>(ticksOf(first.frameDuration()), 1);
	lastDts_ = std::numeric_limits<int64_t>::min();
	const auto [width, height] = pictureSize(first);

	// The file protocol named outright, so that no part of a path reads as another one.
	const std::string url = "file:" + utf8FromPath(path);
	int error = avformat_alloc_output_context2(&context_, nullptr, "mp4", url.c_str());
	if (error < 0 || !context_) {
		context_ = nullptr;
		throw ffmpegError("cannot start the MP4 file", error < 0 ? error : AVERROR(ENOMEM));
	}
	AVStream *stream = avformat_new_stream(context_, nullptr);
	packet_ = av_packet_alloc();
	if (!stream || !packet_) {
		throw std::bad_alloc();
	}
	stream->time_base = kTimeBase;
	AVCodecParameters *parameters = stream->codecpar;
	parameters->codec_type = AVMEDIA_TYPE_VIDEO;
	parameters->codec_id = codecIdOf(first.codec());
	if (first.codec() == VideoCodec::Hevc) {
		parameters->codec_tag = MKTAG('h', 'v', 'c', '1');
	}
	parameters->width = width;
	parameters->height = height;
	// Annex B, as the encoders give it; the muxer writes avcC or hvcC from it.
	if (!config_.empty()) {
		parameters->extradata =
			static_cast<uint8_t *>(av_mallocz(config_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!parameters->extradata) {
			throw std::bad_alloc();
		}
		std::memcpy(parameters->extradata, config_.data(), config_.size());
		parameters->extradata_size = static_cast<int>(config_.size());
	}

	error = avio_open(&context_->pb, url.c_str(), AVIO_FLAG_WRITE);
	if (error < 0) {
		throw ffmpegError("cannot create " + utf8FromPath(path), error);
	}
	error = avformat_write_header(context_, nullptr);
	if (error < 0) {
		throw ffmpegError("cannot write the start of " + utf8FromPath(path), error);
	}
}

void Mp4Writer::write(const Gop &gop, size_t packet, Nanoseconds time)
{
	if (!context_) {
		throw std::logic_error("no MP4 file is open");
	}
	const PacketRecord &record = gop.packets()[packet];
	const std::span<const uint8_t> data = gop.packetData(packet);
	av_packet_unref(packet_);
	if (av_new_packet(packet_, static_cast<int>(data.size())) < 0) {
		throw std::bad_alloc();
	}
	std::memcpy(packet_->data, data.data(), data.size());
	packet_->stream_index = 0;
	packet_->pts = ticksOf(time);
	// Whatever the encoder put between decoding and showing a frame, in frames, stays.
	packet_->dts = packet_->pts - (record.pts - record.dts) * frameTicks_;
	packet_->duration = frameTicks_;
	if (record.keyframe) {
		packet_->flags |= AV_PKT_FLAG_KEY;
	}
	if (packet_->dts <= lastDts_) {
		throw std::runtime_error("the packets are not in decode order");
	}
	lastDts_ = packet_->dts;
	// A run of another configuration gets a sample entry of its own.
	if (const CodecConfig *config = gop.codecConfig(); config && *config != config_) {
		uint8_t *side = av_packet_new_side_data(packet_, AV_PKT_DATA_NEW_EXTRADATA, config->size());
		if (!side) {
			throw std::bad_alloc();
		}
		std::memcpy(side, config->data(), config->size());
		config_ = *config;
	}
	if (const int error = av_write_frame(context_, packet_); error < 0) {
		throw ffmpegError("cannot write to the MP4 file", error);
	}
}

void Mp4Writer::finish()
{
	if (!context_) {
		throw std::logic_error("no MP4 file is open");
	}
	if (const int error = av_write_trailer(context_); error < 0) {
		throw ffmpegError("cannot finish the MP4 file", error);
	}
	if (const int error = avio_closep(&context_->pb); error < 0) {
		throw ffmpegError("cannot finish the MP4 file", error);
	}
	close();
}

void Mp4Writer::close() noexcept
{
	if (context_) {
		if (context_->pb) {
			avio_closep(&context_->pb);
		}
		avformat_free_context(context_);
		context_ = nullptr;
	}
	av_packet_free(&packet_);
}

} // namespace tapeloop::decode
