// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "TestEncoders.hpp"

#include "AllocationCounter.hpp"

#include <obs-module.h>

#include <atomic>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>

namespace tapeloop::test {
namespace {

int brokenState = 0;
int av1Initializations = 0;
std::atomic<bool> nvencEnabled{false};
std::atomic<bool> nvencFails{false};
std::atomic<bool> nvencStoreFails{false};
// Not a real HEVC frame; nothing decodes what the HEVC test encoder makes.
const uint8_t kHevcFrame[] = {0, 0, 0, 1, 0x26, 0x01, 0xaf};

const char *failingName(void *) noexcept
{
	return "Tapeloop failing test encoder";
}

const char *brokenName(void *) noexcept
{
	return "Tapeloop broken test encoder";
}

const char *av1Name(void *) noexcept
{
	return "Tapeloop AV1 test encoder";
}

const char *hevcName(void *) noexcept
{
	return "Tapeloop HEVC test encoder";
}

void *createNothing(obs_data_t *, obs_encoder_t *) noexcept
{
	return nullptr;
}

void *createSomething(obs_data_t *, obs_encoder_t *) noexcept
{
	return &brokenState;
}

void *createCounted(obs_data_t *, obs_encoder_t *) noexcept
{
	++av1Initializations;
	return &brokenState;
}

void destroy(void *) noexcept {}

const char *nvencName(void *) noexcept
{
	return "Tapeloop NVENC test encoder";
}

struct NvencState {
	int64_t frames = 0;
};

void *createNvenc(obs_data_t *, obs_encoder_t *) noexcept
{
	return nvencEnabled ? new (std::nothrow) NvencState : nullptr;
}

void destroyNvenc(void *data) noexcept
{
	delete static_cast<NvencState *>(data);
}

bool encodeNvenc(void *data, encoder_frame *frame, encoder_packet *packet, bool *received) noexcept
{
	if (nvencFails) {
		return false;
	}
	auto &state = *static_cast<NvencState *>(data);
	packet->data = const_cast<uint8_t *>(kHevcFrame);
	packet->size = sizeof(kHevcFrame);
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->keyframe = state.frames++ % 30 == 0;
	packet->type = OBS_ENCODER_VIDEO;
	*received = true;
	// Storing a keyframe after the first seals a GOP, which allocates.
	if (packet->keyframe && state.frames > 1 && nvencStoreFails.exchange(false)) {
		thread_local std::optional<AllocationFailure> failure;
		failure.emplace(0);
	}
	return true;
}

bool fail(void *, encoder_frame *, encoder_packet *, bool *) noexcept
{
	return false;
}

bool encodeKeyframe(void *, encoder_frame *frame, encoder_packet *packet, bool *received) noexcept
{
	packet->data = const_cast<uint8_t *>(kHevcFrame);
	packet->size = sizeof(kHevcFrame);
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->keyframe = true;
	packet->type = OBS_ENCODER_VIDEO;
	*received = true;
	return true;
}

bool encodeTextureKeyframe(void *data, encoder_texture *, int64_t pts, uint64_t lockKey, uint64_t *nextKey,
			   encoder_packet *packet, bool *received) noexcept
{
	*nextKey = lockKey;
	encoder_frame frame = {};
	frame.pts = pts;
	return encodeKeyframe(data, &frame, packet, received);
}

obs_encoder_info videoEncoder(const char *id, const char *codec)
{
	obs_encoder_info info = {};
	info.id = id;
	info.type = OBS_ENCODER_VIDEO;
	info.codec = codec;
	info.destroy = destroy;
	info.encode = fail;
	return info;
}

} // namespace

void registerTestEncoders()
{
	obs_encoder_info failing = videoEncoder(kFailingEncoderId, "h264");
	failing.get_name = failingName;
	failing.create = createNothing;
	obs_register_encoder(&failing);

	obs_encoder_info broken = videoEncoder(kBrokenEncoderId, "h264");
	broken.get_name = brokenName;
	broken.create = createSomething;
	obs_register_encoder(&broken);

	av1Initializations = 0;
	obs_encoder_info av1 = videoEncoder(kAv1EncoderId, "av1");
	av1.get_name = av1Name;
	av1.create = createCounted;
	obs_register_encoder(&av1);

	obs_encoder_info hevc = videoEncoder(kHevcEncoderId, "hevc");
	hevc.get_name = hevcName;
	hevc.create = createSomething;
	hevc.encode = encodeKeyframe;
	obs_register_encoder(&hevc);

	obs_encoder_info texture = hevc;
	texture.id = kHevcTextureEncoderId;
	texture.caps = OBS_ENCODER_CAP_PASS_TEXTURE;
	texture.encode_texture2 = encodeTextureKeyframe;
	obs_register_encoder(&texture);

	nvencEnabled = false;
	nvencFails = false;
	nvencStoreFails = false;
	for (const auto &[id, codec] : {std::pair{kNvencHevcId, "hevc"}, std::pair{kNvencH264Id, "h264"}}) {
		obs_encoder_info nvenc = videoEncoder(id, codec);
		nvenc.get_name = nvencName;
		nvenc.create = createNvenc;
		nvenc.destroy = destroyNvenc;
		nvenc.encode = encodeNvenc;
		obs_register_encoder(&nvenc);
	}
}

void enableTestNvenc()
{
	nvencEnabled = true;
}

void failTestNvenc(bool fail)
{
	nvencFails = fail;
}

void failTestNvencStore()
{
	nvencStoreFails = true;
}

int av1EncoderInitializations()
{
	return av1Initializations;
}

} // namespace tapeloop::test
