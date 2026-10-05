// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ClipDecoder.hpp"

#include "TestPattern.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace tapeloop::test {
namespace {

struct ContextDeleter {
	void operator()(AVCodecContext *context) const noexcept { avcodec_free_context(&context); }
};

struct PacketDeleter {
	void operator()(AVPacket *packet) const noexcept { av_packet_free(&packet); }
};

struct FrameDeleter {
	void operator()(AVFrame *frame) const noexcept { av_frame_free(&frame); }
};

void check(int result, const char *what)
{
	if (result < 0) {
		char message[AV_ERROR_MAX_STRING_SIZE] = {};
		av_strerror(result, message, sizeof(message));
		throw std::runtime_error(std::string(what) + ": " + message);
	}
}

std::vector<DecodedFrame> decodeGop(const Gop &gop)
{
	const AVCodec *decoder =
		avcodec_find_decoder(gop.codec() == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!decoder) {
		throw std::runtime_error("libavcodec has no decoder for the codec of the GOP");
	}

	std::unique_ptr<AVCodecContext, ContextDeleter> context(avcodec_alloc_context3(decoder));
	std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
	std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
	if (!context || !packet || !frame) {
		throw std::runtime_error("libavcodec allocation failed");
	}
	context->thread_count = 1;
	context->err_recognition |= AV_EF_EXPLODE;

	if (const CodecConfig *config = gop.codecConfig(); config && !config->empty()) {
		// libavcodec frees extradata with the context and reads past its end.
		context->extradata = static_cast<uint8_t *>(av_mallocz(config->size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!context->extradata) {
			throw std::runtime_error("libavcodec allocation failed");
		}
		std::memcpy(context->extradata, config->data(), config->size());
		context->extradata_size = static_cast<int>(config->size());
	}
	check(avcodec_open2(context.get(), decoder, nullptr), "avcodec_open2");

	std::vector<DecodedFrame> frames;
	const auto receive = [&] {
		for (;;) {
			const int result = avcodec_receive_frame(context.get(), frame.get());
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
				return;
			}
			check(result, "avcodec_receive_frame");
			if (frame->decode_error_flags != 0 || (frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
				throw std::runtime_error("libavcodec concealed damage in a frame");
			}
			frames.push_back(
				{frame->pts, frame->width, frame->height,
				 readFrameNumber(frame->data[0], frame->linesize[0], frame->width, frame->height)});
			av_frame_unref(frame.get());
		}
	};

	for (size_t i = 0; i < gop.packets().size(); ++i) {
		const std::vector<uint8_t> data = withoutParameterSets(gop.packetData(i), gop.codec());
		check(av_new_packet(packet.get(), static_cast<int>(data.size())), "av_new_packet");
		std::memcpy(packet->data, data.data(), data.size());
		packet->pts = gop.packets()[i].pts;
		packet->dts = gop.packets()[i].dts;
		const int sent = avcodec_send_packet(context.get(), packet.get());
		av_packet_unref(packet.get());
		check(sent, "avcodec_send_packet");
		receive();
	}
	check(avcodec_send_packet(context.get(), nullptr), "avcodec_send_packet");
	receive();
	return frames;
}

} // namespace

// Encoders write Annex B: each NAL unit follows a 00 00 01 start code, sometimes with one
// more zero byte in front.
std::vector<uint8_t> withoutParameterSets(std::span<const uint8_t> data, VideoCodec codec)
{
	const auto isParameterSet = [codec](uint8_t header) {
		if (codec == VideoCodec::Hevc) {
			const int type = (header >> 1) & 0x3f;
			// VPS, SPS and PPS.
			return type >= 32 && type <= 34;
		}
		const int type = header & 0x1f;
		// SPS and PPS.
		return type == 7 || type == 8;
	};

	std::vector<size_t> units;
	for (size_t i = 0; i + 2 < data.size(); ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			units.push_back(i + 3);
			i += 2;
		}
	}

	std::vector<uint8_t> kept;
	kept.reserve(data.size());
	for (size_t n = 0; n < units.size(); ++n) {
		const size_t begin = units[n];
		size_t end = n + 1 < units.size() ? units[n + 1] - 3 : data.size();
		// A NAL unit never ends in a zero byte; these belong to the next start code.
		while (end > begin && data[end - 1] == 0) {
			--end;
		}
		if (end == begin) {
			continue;
		}
		if (isParameterSet(data[begin])) {
			continue;
		}
		kept.insert(kept.end(), {0, 0, 0, 1});
		kept.insert(kept.end(), data.begin() + static_cast<ptrdiff_t>(begin),
			    data.begin() + static_cast<ptrdiff_t>(end));
	}
	return kept;
}

std::vector<std::vector<DecodedFrame>> decodeGops(const Clip &clip)
{
	std::vector<std::vector<DecodedFrame>> gops;
	for (const auto &gop : clip.gops()) {
		gops.push_back(decodeGop(*gop));
	}
	return gops;
}

} // namespace tapeloop::test
