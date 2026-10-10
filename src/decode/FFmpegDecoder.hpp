// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FrameDecoder.hpp"
#include "decode/Picture.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct AVBufferPool;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace tapeloop::decode {

struct FFmpegDecoderConfig {
	// Windows: decode on a D3D11 device of the adapter with this LUID, and keep the
	// pictures on the GPU. Without it, elsewhere, or for a stream that adapter cannot
	// decode, FFmpeg decodes on the CPU. The LUID's HighPart goes in the upper 32 bits
	// and its LowPart in the lower ones.
	std::optional<uint64_t> adapterLuid;
	// Slice threads. Frame threads are never used: each one delays output by a frame.
	// One by default, since every replay has a decoder of its own.
	int threads = 1;
};

// The FrameDecoder of FFmpeg, for H.264 and HEVC. The configuration given to open() and
// parameter sets inside the packets are both accepted; the parameter sets a packet
// repeats from the configuration are not passed on.
//
// Pictures on the CPU stay in FFmpeg's buffers until released. Pictures decoded on the
// GPU are copied out of FFmpeg's fixed pool of surfaces into textures of the decoder's
// own, one per frame held, so the planner's budget decides how many there are.
class FFmpegDecoder final : public FrameDecoder {
public:
	explicit FFmpegDecoder(FFmpegDecoderConfig config = {});
	~FFmpegDecoder() override;

	FFmpegDecoder(const FFmpegDecoder &) = delete;
	FFmpegDecoder &operator=(const FFmpegDecoder &) = delete;
	FFmpegDecoder(FFmpegDecoder &&) = delete;
	FFmpegDecoder &operator=(FFmpegDecoder &&) = delete;

	// Unsupported when FFmpeg has no decoder for the codec or cannot open it, and
	// InvalidData for a configuration it rejects. Not every damaged configuration is
	// rejected: H.264's, and HEVC's in Annex B form as OBS writes it, open, and the
	// frames that need them never come out.
	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override;
	// InvalidData also when FFmpeg holds frames not yet received and takes no more
	// packets until they are: receive every frame before the next send.
	DecodeStatus send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept override;
	// Unsupported for a picture in a format other than 8-bit 4:2:0. A picture decoded on
	// the GPU is cropped at the right and the bottom only, which is all the encoders of
	// a capture crop.
	DecodeStatus receive(DecodedFrame &frame) noexcept override;
	DecodeStatus flush() noexcept override;
	void reset() noexcept override;
	void release(const DecodedFrame &frame) noexcept override;
	void close() noexcept override;

	// The picture of a frame this decoder returned and has not taken back; empty for
	// any other frame.
	std::optional<Picture> picture(const DecodedFrame &frame) const noexcept;
	// Whether a D3D11 device was created for the adapter of the configuration.
	bool hasDevice() const noexcept;
	size_t heldFrames() const noexcept;

private:
	struct Slot;
	struct Device;
	struct ContextDeleter {
		void operator()(AVCodecContext *context) const noexcept;
	};
	struct PacketDeleter {
		void operator()(AVPacket *packet) const noexcept;
	};
	struct FrameDeleter {
		void operator()(AVFrame *frame) const noexcept;
	};
	struct PoolDeleter {
		void operator()(AVBufferPool *pool) const noexcept;
	};

	// Windows only: a D3D11 device of that adapter, handed to FFmpeg; null on failure.
	static std::unique_ptr<Device> createDevice(uint64_t luid) noexcept;
	DecodeStatus statusOf(int result) const noexcept;
	Slot *slotOf(const DecodedFrame &frame) noexcept;
	const Slot *slotOf(const DecodedFrame &frame) const noexcept;
	// A free slot, or a new one; null when memory runs out.
	Slot *takeSlot(uint32_t &index) noexcept;
	DecodeStatus keepPicture(Slot &slot) noexcept;
	void dropSlot(Slot &slot, uint32_t index) noexcept;

	FFmpegDecoderConfig config_;
	std::unique_ptr<Device> device_;
	std::unique_ptr<AVCodecContext, ContextDeleter> context_;
	std::unique_ptr<AVPacket, PacketDeleter> packet_;
	std::unique_ptr<AVFrame, FrameDeleter> received_;
	std::unique_ptr<AVBufferPool, PoolDeleter> packetPool_;
	size_t packetBufferSize_ = 0;
	VideoCodec codec_ = VideoCodec::H264;
	std::vector<uint8_t> runConfig_;
	std::vector<Slot> slots_;
	std::vector<uint32_t> freeSlots_;
};

} // namespace tapeloop::decode
