// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/Mp4Writer.hpp"
#include "decode/FFmpegHeaders.hpp"

#include "core/FileIo.hpp"
#include "core/ParameterSets.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace tapeloop::decode {
namespace {

// Every common frame rate is a whole number of these: a frame at 59.94 fps is 2002 of
// them, at 50 fps 2400.
constexpr AVRational kTimeBase{1, 120'000};
constexpr AVRational kNanoseconds{1, 1'000'000'000};
constexpr int kIoBufferSize = 256 * 1024;

std::runtime_error ffmpegError(const std::string &what, int code)
{
	char text[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, text, sizeof(text));
	return std::runtime_error(what + ": " + text);
}

// FFmpeg counts the bytes of a buffer in an int and reads past its end by the padding.
int ffmpegSize(size_t size)
{
	if (size > static_cast<size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
		throw std::runtime_error("a packet or its parameter sets are too large to write");
	}
	return static_cast<int>(size);
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
// parameter sets FFmpeg's parser reads in the configuration and the keyframe, if it finds
// them.
std::optional<std::pair<int, int>> readPictureSize(const Gop &gop)
{
	const CodecConfig *config = gop.codecConfig();
	const std::span<const uint8_t> keyframe = gop.packetData(0);
	std::vector<uint8_t> frame;
	if (config) {
		frame = *config;
	}
	frame.insert(frame.end(), keyframe.begin(), keyframe.end());
	const int size = ffmpegSize(frame.size());
	frame.resize(frame.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);

	AVCodecParserContext *parser = av_parser_init(codecIdOf(gop.codec()));
	AVCodecContext *context = avcodec_alloc_context3(nullptr);
	std::pair<int, int> found{0, 0};
	if (parser && context) {
		parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
		uint8_t *out = nullptr;
		int outSize = 0;
		av_parser_parse2(parser, context, &out, &outSize, frame.data(), size, AV_NOPTS_VALUE, AV_NOPTS_VALUE,
				 0);
		found = {parser->width, parser->height};
	}
	avcodec_free_context(&context);
	av_parser_close(parser);
	if (found.first <= 0 || found.second <= 0) {
		return std::nullopt;
	}
	return found;
}

std::pair<int, int> pictureSize(const Gop &gop)
{
	const std::optional<std::pair<int, int>> size = readPictureSize(gop);
	if (!size) {
		throw std::runtime_error("the size of the pictures cannot be read from their parameter sets");
	}
	return *size;
}

// The muxer reads the sequence parameter sets of each configuration as it finishes the
// file. One cut short holds it up there for minutes, so it is refused before it gets in.
void checkParameterSets(VideoCodec codec, std::span<const uint8_t> data)
{
	if (!sequenceParameterSetsAreWhole(codec, data)) {
		throw std::runtime_error("a sequence parameter set is cut short");
	}
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
	checkParameterSets(first.codec(), config_);
	configFromPacket_ = config_.empty();
	frameTicks_ = std::max<int64_t>(ticksOf(first.frameDuration()), 1);
	lastDts_ = std::numeric_limits<int64_t>::min();
	const auto [width, height] = pictureSize(first);
	width_ = width;
	height_ = height;

	int error = avformat_alloc_output_context2(&context_, nullptr, "mp4", nullptr);
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
		const int size = ffmpegSize(config_.size());
		parameters->extradata =
			static_cast<uint8_t *>(av_mallocz(config_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!parameters->extradata) {
			throw std::bad_alloc();
		}
		std::memcpy(parameters->extradata, config_.data(), config_.size());
		parameters->extradata_size = size;
	}

	// Through a file of our own, which is created only where none is: FFmpeg's file
	// protocol would empty a file already there, or one a link there points to.
	file_ = File(path, File::Mode::CreateNew);
	try {
		position_ = 0;
		ioError_.clear();
		auto *buffer = static_cast<uint8_t *>(av_malloc(kIoBufferSize));
		if (!buffer) {
			throw std::bad_alloc();
		}
		io_ = avio_alloc_context(buffer, kIoBufferSize, 1, this, nullptr, &Mp4Writer::writeIo,
					 &Mp4Writer::seekIo);
		if (!io_) {
			av_free(buffer);
			throw std::bad_alloc();
		}
		context_->pb = io_;
		context_->flags |= AVFMT_FLAG_CUSTOM_IO;
		error = avformat_write_header(context_, nullptr);
		if (error < 0) {
			fail("cannot write the start of " + utf8FromPath(path), error);
		}
	} catch (...) {
		// The caller deletes the file only once open has made it, so what failed here
		// removes it, after closing it.
		close();
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		throw;
	}
}

bool Mp4Writer::fits(const Gop &gop) const
{
	// Every configuration of a run has the size of its pictures, so only a new one is
	// read. Without one, the parameter sets in the keyframe tell the size, as for the
	// first GOP; a keyframe without them decodes with those before it, so it fits.
	const CodecConfig *config = gop.codecConfig();
	if (config && *config == config_) {
		return true;
	}
	if (config) {
		return pictureSize(gop) == std::pair{width_, height_};
	}
	const std::optional<std::pair<int, int>> size = readPictureSize(gop);
	return !size || *size == std::pair{width_, height_};
}

void Mp4Writer::write(const Gop &gop, size_t packet, Nanoseconds time)
{
	if (!context_) {
		throw std::logic_error("no MP4 file is open");
	}
	const PacketRecord &record = gop.packets()[packet];
	const std::span<const uint8_t> data = gop.packetData(packet);
	if (const CodecConfig *config = gop.codecConfig(); config && *config != config_) {
		checkParameterSets(gop.codec(), *config);
	}
	// Without a configuration in the first GOP, the muxer takes the first packet with
	// bytes for one.
	if (configFromPacket_ && !data.empty()) {
		checkParameterSets(gop.codec(), data);
		configFromPacket_ = false;
	}
	av_packet_unref(packet_);
	if (av_new_packet(packet_, ffmpegSize(data.size())) < 0) {
		throw std::bad_alloc();
	}
	// A GOP of empty packets has no bytes at all, and memcpy takes no null pointer even
	// for nothing.
	if (!data.empty()) {
		std::memcpy(packet_->data, data.data(), data.size());
	}
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
		fail("cannot write to the MP4 file", error);
	}
}

void Mp4Writer::finish()
{
	if (!context_) {
		throw std::logic_error("no MP4 file is open");
	}
	if (const int error = av_write_trailer(context_); error < 0) {
		fail("cannot finish the MP4 file", error);
	}
	avio_flush(io_);
	if (io_->error < 0) {
		fail("cannot finish the MP4 file", io_->error);
	}
	close();
}

void Mp4Writer::close() noexcept
{
	avformat_free_context(context_);
	context_ = nullptr;
	if (io_) {
		// Not avio_close(), which would take the context for one of FFmpeg's protocols.
		av_freep(&io_->buffer);
		avio_context_free(&io_);
	}
	file_.close();
	av_packet_free(&packet_);
}

void Mp4Writer::fail(const std::string &what, int code) const
{
	if (ioError_) {
		throw std::system_error(ioError_, what);
	}
	throw ffmpegError(what, code);
}

int Mp4Writer::writeIo(void *opaque, const uint8_t *data, int size) noexcept
{
	auto *writer = static_cast<Mp4Writer *>(opaque);
	try {
		writer->file_.writeAt(writer->position_, {data, static_cast<size_t>(size)});
	} catch (const std::system_error &e) {
		writer->ioError_ = e.code();
		return AVERROR(EIO);
	} catch (...) {
		writer->ioError_ = std::make_error_code(std::errc::io_error);
		return AVERROR(EIO);
	}
	writer->position_ += static_cast<uint64_t>(size);
	return size;
}

int64_t Mp4Writer::seekIo(void *opaque, int64_t offset, int whence) noexcept
{
	auto *writer = static_cast<Mp4Writer *>(opaque);
	try {
		int64_t base = 0;
		switch (whence & ~AVSEEK_FORCE) {
		case AVSEEK_SIZE:
			return static_cast<int64_t>(writer->file_.size());
		case SEEK_SET:
			break;
		case SEEK_CUR:
			base = static_cast<int64_t>(writer->position_);
			break;
		case SEEK_END:
			base = static_cast<int64_t>(writer->file_.size());
			break;
		default:
			return AVERROR(EINVAL);
		}
		if (offset < -base || offset > std::numeric_limits<int64_t>::max() - base) {
			return AVERROR(EINVAL);
		}
		writer->position_ = static_cast<uint64_t>(base + offset);
		return static_cast<int64_t>(writer->position_);
	} catch (const std::system_error &e) {
		writer->ioError_ = e.code();
		return AVERROR(EIO);
	} catch (...) {
		writer->ioError_ = std::make_error_code(std::errc::io_error);
		return AVERROR(EIO);
	}
}

} // namespace tapeloop::decode
