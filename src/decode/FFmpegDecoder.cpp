// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "decode/FFmpegDecoder.hpp"
#include "decode/FFmpegHeaders.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}
#endif

#include <climits>
#include <cstdint>
#include <cstring>
#include <new>

namespace tapeloop::decode {

struct FFmpegDecoder::Slot {
	std::unique_ptr<AVFrame, FrameDeleter> frame;
#ifdef _WIN32
	Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
#endif
	Picture picture;
	// Starts at 1, so the id of a default DecodedFrame names no frame.
	uint32_t generation = 1;
	bool held = false;
};

namespace {

ColorMatrix matrixOf(AVColorSpace space) noexcept
{
	switch (space) {
	case AVCOL_SPC_BT709:
		return ColorMatrix::Bt709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return ColorMatrix::Bt601;
	default:
		return ColorMatrix::Unspecified;
	}
}

} // namespace

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

struct FFmpegDecoder::Device {
	struct BufferDeleter {
		void operator()(AVBufferRef *buffer) const noexcept { av_buffer_unref(&buffer); }
	};

	ComPtr<ID3D11Device> device;
	std::unique_ptr<AVBufferRef, BufferDeleter> hwDevice;

	AVD3D11VADeviceContext &hwContext() const noexcept
	{
		auto *context = reinterpret_cast<AVHWDeviceContext *>(hwDevice->data);
		return *static_cast<AVD3D11VADeviceContext *>(context->hwctx);
	}

	bool lost() const noexcept { return device->GetDeviceRemovedReason() != S_OK; }
};

std::unique_ptr<FFmpegDecoder::Device> FFmpegDecoder::createDevice(uint64_t luid) noexcept
{
	ComPtr<IDXGIFactory4> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		return nullptr;
	}
	LUID id = {};
	id.LowPart = static_cast<DWORD>(luid & 0xffffffffu);
	id.HighPart = static_cast<LONG>(luid >> 32);
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(factory->EnumAdapterByLuid(id, IID_PPV_ARGS(&adapter)))) {
		return nullptr;
	}

	std::unique_ptr<FFmpegDecoder::Device> device(new (std::nothrow) FFmpegDecoder::Device);
	if (!device) {
		return nullptr;
	}
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
				     levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device->device, nullptr,
				     nullptr))) {
		return nullptr;
	}
	// Microsoft asks for it on a device that decodes video.
	ComPtr<ID3D10Multithread> multithread;
	if (SUCCEEDED(device->device.As(&multithread))) {
		multithread->SetMultithreadProtected(TRUE);
	}

	device->hwDevice.reset(av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA));
	if (!device->hwDevice) {
		return nullptr;
	}
	// FFmpeg releases the device with its context, and fills in the immediate and video
	// contexts and a lock of its own.
	device->hwContext().device = device->device.Get();
	device->device->AddRef();
	if (av_hwdevice_ctx_init(device->hwDevice.get()) < 0) {
		return nullptr;
	}
	return device;
}

namespace {

AVPixelFormat chooseFormat(AVCodecContext *, const AVPixelFormat *formats) noexcept
{
	for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
		if (*format == AV_PIX_FMT_D3D11) {
			return *format;
		}
	}
	// The adapter cannot decode this stream, so FFmpeg does, on the CPU.
	for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
		const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(*format);
		if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
			return *format;
		}
	}
	return AV_PIX_FMT_NONE;
}

} // namespace

#else

struct FFmpegDecoder::Device {};

#endif

void FFmpegDecoder::ContextDeleter::operator()(AVCodecContext *context) const noexcept
{
	avcodec_free_context(&context);
}

void FFmpegDecoder::PacketDeleter::operator()(AVPacket *packet) const noexcept
{
	av_packet_free(&packet);
}

void FFmpegDecoder::FrameDeleter::operator()(AVFrame *frame) const noexcept
{
	av_frame_free(&frame);
}

void FFmpegDecoder::PoolDeleter::operator()(AVBufferPool *pool) const noexcept
{
	// The buffers FFmpeg still holds keep the pool until they come back.
	av_buffer_pool_uninit(&pool);
}

FFmpegDecoder::FFmpegDecoder(FFmpegDecoderConfig config) : config_(config)
{
#ifdef _WIN32
	if (config_.adapterLuid) {
		device_ = createDevice(*config_.adapterLuid);
	}
#endif
}

FFmpegDecoder::~FFmpegDecoder()
{
	close();
}

DecodeStatus FFmpegDecoder::open(VideoCodec codec, std::span<const uint8_t> config) noexcept
{
	context_.reset();
	const AVCodec *decoder = avcodec_find_decoder(codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!decoder) {
		return DecodeStatus::Unsupported;
	}
	if (config.size() > static_cast<size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
		return DecodeStatus::InvalidData;
	}
	if (!packet_) {
		packet_.reset(av_packet_alloc());
	}
	if (!received_) {
		received_.reset(av_frame_alloc());
	}
	std::unique_ptr<AVCodecContext, ContextDeleter> context(avcodec_alloc_context3(decoder));
	if (!packet_ || !received_ || !context) {
		return DecodeStatus::OutOfMemory;
	}

	context->thread_type = FF_THREAD_SLICE;
	context->thread_count = config_.threads;
	context->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (!config.empty()) {
		// libavcodec frees the extradata with the context and may read past its end.
		context->extradata = static_cast<uint8_t *>(av_mallocz(config.size() + AV_INPUT_BUFFER_PADDING_SIZE));
		if (!context->extradata) {
			return DecodeStatus::OutOfMemory;
		}
		std::memcpy(context->extradata, config.data(), config.size());
		context->extradata_size = static_cast<int>(config.size());
	}

#ifdef _WIN32
	if (config_.adapterLuid && (!device_ || device_->lost())) {
		device_ = createDevice(*config_.adapterLuid);
	}
	if (device_) {
		context->hw_device_ctx = av_buffer_ref(device_->hwDevice.get());
		if (!context->hw_device_ctx) {
			return DecodeStatus::OutOfMemory;
		}
		context->get_format = chooseFormat;
	}
#endif

	const int result = avcodec_open2(context.get(), decoder, nullptr);
	if (result == AVERROR(ENOMEM)) {
		return DecodeStatus::OutOfMemory;
	}
	if (result == AVERROR_INVALIDDATA) {
		return DecodeStatus::InvalidData;
	}
	if (result < 0) {
		return DecodeStatus::Unsupported;
	}
	context_ = std::move(context);
	return DecodeStatus::Ok;
}

DecodeStatus FFmpegDecoder::send(std::span<const uint8_t> data, int64_t pts, int64_t dts) noexcept
{
	if (!context_ || data.size() > static_cast<size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
		return DecodeStatus::InvalidData;
	}

	// A packet without a buffer of its own would be copied into a new one by FFmpeg, on
	// every frame. Buffers come from a pool instead, as large as the largest packet so far
	// and some more, so keyframes that grow a little do not start a new pool each time.
	const size_t needed = data.size() + AV_INPUT_BUFFER_PADDING_SIZE;
	if (needed > packetBufferSize_) {
		const size_t size = needed + needed / 2;
		packetPool_.reset(av_buffer_pool_init(size, nullptr));
		packetBufferSize_ = packetPool_ ? size : 0;
	}
	AVBufferRef *buffer = packetPool_ ? av_buffer_pool_get(packetPool_.get()) : nullptr;
	if (!buffer) {
		return DecodeStatus::OutOfMemory;
	}
	if (!data.empty()) {
		std::memcpy(buffer->data, data.data(), data.size());
	}
	std::memset(buffer->data + data.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);

	packet_->buf = buffer;
	packet_->data = buffer->data;
	packet_->size = static_cast<int>(data.size());
	packet_->pts = pts;
	packet_->dts = dts;
	const int result = avcodec_send_packet(context_.get(), packet_.get());
	av_packet_unref(packet_.get());
	// EAGAIN would mean frames of the previous packet were left in the decoder.
	return statusOf(result);
}

DecodeStatus FFmpegDecoder::receive(DecodedFrame &frame) noexcept
{
	if (!context_) {
		return DecodeStatus::InvalidData;
	}
	const int result = avcodec_receive_frame(context_.get(), received_.get());
	if (result == AVERROR(EAGAIN)) {
		return DecodeStatus::NeedMore;
	}
	if (result == AVERROR_EOF) {
		return DecodeStatus::Drained;
	}
	if (result < 0) {
		return statusOf(result);
	}

	const int64_t pts = received_->pts != AV_NOPTS_VALUE ? received_->pts : received_->best_effort_timestamp;
	uint32_t index = 0;
	Slot *slot = takeSlot(index);
	if (!slot) {
		av_frame_unref(received_.get());
		return DecodeStatus::OutOfMemory;
	}
	const DecodeStatus kept = keepPicture(*slot);
	if (kept != DecodeStatus::Ok) {
		freeSlots_.push_back(index);
		return kept;
	}
	slot->held = true;
	frame.id = (static_cast<uint64_t>(slot->generation) << 32) | index;
	frame.pts = pts;
	return DecodeStatus::Ok;
}

DecodeStatus FFmpegDecoder::flush() noexcept
{
	if (!context_) {
		return DecodeStatus::InvalidData;
	}
	return statusOf(avcodec_send_packet(context_.get(), nullptr));
}

void FFmpegDecoder::reset() noexcept
{
	if (context_) {
		avcodec_flush_buffers(context_.get());
	}
}

void FFmpegDecoder::release(const DecodedFrame &frame) noexcept
{
	if (Slot *slot = slotOf(frame)) {
		dropSlot(*slot, static_cast<uint32_t>(frame.id & 0xffffffffu));
	}
}

void FFmpegDecoder::close() noexcept
{
	context_.reset();
	for (size_t index = 0; index < slots_.size(); ++index) {
		Slot &slot = slots_[index];
		if (slot.held) {
			dropSlot(slot, static_cast<uint32_t>(index));
		}
#ifdef _WIN32
		slot.texture.Reset();
#endif
	}
	packetPool_.reset();
	packetBufferSize_ = 0;
}

std::optional<Picture> FFmpegDecoder::picture(const DecodedFrame &frame) const noexcept
{
	if (const Slot *slot = slotOf(frame)) {
		return slot->picture;
	}
	return std::nullopt;
}

bool FFmpegDecoder::hasDevice() const noexcept
{
	return device_ != nullptr;
}

size_t FFmpegDecoder::heldFrames() const noexcept
{
	return slots_.size() - freeSlots_.size();
}

DecodeStatus FFmpegDecoder::statusOf(int result) const noexcept
{
	if (result >= 0) {
		return DecodeStatus::Ok;
	}
	if (result == AVERROR(ENOMEM)) {
		return DecodeStatus::OutOfMemory;
	}
#ifdef _WIN32
	if (device_ && device_->lost()) {
		return DecodeStatus::DeviceLost;
	}
#endif
	return DecodeStatus::InvalidData;
}

FFmpegDecoder::Slot *FFmpegDecoder::slotOf(const DecodedFrame &frame) noexcept
{
	const auto index = static_cast<size_t>(frame.id & 0xffffffffu);
	const auto generation = static_cast<uint32_t>(frame.id >> 32);
	if (index >= slots_.size()) {
		return nullptr;
	}
	Slot &slot = slots_[index];
	return slot.held && slot.generation == generation ? &slot : nullptr;
}

const FFmpegDecoder::Slot *FFmpegDecoder::slotOf(const DecodedFrame &frame) const noexcept
{
	return const_cast<FFmpegDecoder *>(this)->slotOf(frame);
}

FFmpegDecoder::Slot *FFmpegDecoder::takeSlot(uint32_t &index) noexcept
{
	if (!freeSlots_.empty()) {
		index = freeSlots_.back();
		freeSlots_.pop_back();
		return &slots_[index];
	}
	if (slots_.size() >= UINT32_MAX) {
		return nullptr;
	}
	try {
		// Reserved now, so that release() never allocates.
		freeSlots_.reserve(slots_.size() + 1);
		slots_.emplace_back();
	} catch (...) {
		return nullptr;
	}
	Slot &slot = slots_.back();
	slot.frame.reset(av_frame_alloc());
	if (!slot.frame) {
		slots_.pop_back();
		return nullptr;
	}
	index = static_cast<uint32_t>(slots_.size() - 1);
	return &slot;
}

DecodeStatus FFmpegDecoder::keepPicture(Slot &slot) noexcept
{
	const AVFrame &frame = *received_;
	Picture picture;
	picture.width = static_cast<uint32_t>(frame.width);
	picture.height = static_cast<uint32_t>(frame.height);
	picture.fullRange = frame.color_range == AVCOL_RANGE_JPEG || frame.format == AV_PIX_FMT_YUVJ420P;
	picture.matrix = matrixOf(frame.colorspace);

#ifdef _WIN32
	if (frame.format == AV_PIX_FMT_D3D11) {
		// A 10-bit stream decodes into P010 surfaces, which an NV12 copy cannot take.
		const auto *surfaces = frame.hw_frames_ctx
					       ? reinterpret_cast<const AVHWFramesContext *>(frame.hw_frames_ctx->data)
					       : nullptr;
		if (!surfaces || surfaces->sw_format != AV_PIX_FMT_NV12) {
			av_frame_unref(received_.get());
			return DecodeStatus::Unsupported;
		}
		auto *surface = reinterpret_cast<ID3D11Texture2D *>(frame.data[0]);
		const auto surfaceIndex = static_cast<UINT>(reinterpret_cast<intptr_t>(frame.data[1]));
		// NV12 sizes are even; the surfaces FFmpeg decodes into are larger still.
		const UINT width = (picture.width + 1) & ~1u;
		const UINT height = (picture.height + 1) & ~1u;
		if (slot.texture) {
			D3D11_TEXTURE2D_DESC desc = {};
			slot.texture->GetDesc(&desc);
			ComPtr<ID3D11Device> owner;
			slot.texture->GetDevice(&owner);
			if (desc.Width != width || desc.Height != height || owner.Get() != device_->device.Get()) {
				slot.texture.Reset();
			}
		}
		if (!slot.texture) {
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_NV12;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			if (FAILED(device_->device->CreateTexture2D(&desc, nullptr, &slot.texture))) {
				av_frame_unref(received_.get());
				return device_->lost() ? DecodeStatus::DeviceLost : DecodeStatus::OutOfMemory;
			}
		}
		// The copy goes into the device's queue ahead of the next decode, which may then
		// reuse the surface. FFmpeg's lock guards the contexts it shares with us.
		AVD3D11VADeviceContext &hw = device_->hwContext();
		const D3D11_BOX box = {0, 0, 0, width, height, 1};
		hw.lock(hw.lock_ctx);
		hw.device_context->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, surface, surfaceIndex, &box);
		hw.unlock(hw.lock_ctx);
		av_frame_unref(received_.get());

		picture.layout = PixelLayout::Nv12;
		picture.texture = slot.texture.Get();
		slot.picture = picture;
		return DecodeStatus::Ok;
	}
#endif

	int planes = 0;
	switch (frame.format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		picture.layout = PixelLayout::I420;
		planes = 3;
		break;
	case AV_PIX_FMT_NV12:
		picture.layout = PixelLayout::Nv12;
		planes = 2;
		break;
	default:
		av_frame_unref(received_.get());
		return DecodeStatus::Unsupported;
	}
	av_frame_move_ref(slot.frame.get(), received_.get());
	for (int plane = 0; plane < planes; ++plane) {
		const auto at = static_cast<size_t>(plane);
		picture.planes[at] = slot.frame->data[plane];
		picture.strides[at] = static_cast<uint32_t>(slot.frame->linesize[plane]);
	}
	slot.picture = picture;
	return DecodeStatus::Ok;
}

void FFmpegDecoder::dropSlot(Slot &slot, uint32_t index) noexcept
{
	av_frame_unref(slot.frame.get());
	slot.picture = {};
	slot.held = false;
	++slot.generation;
	if (slot.generation == 0) {
		slot.generation = 1;
	}
	freeSlots_.push_back(index);
}

} // namespace tapeloop::decode
