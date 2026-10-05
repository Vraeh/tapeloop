// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "TestEncoders.hpp"

#include <obs-module.h>

#include <cstdint>

namespace tapeloop::test {
namespace {

int brokenState = 0;
int av1Initializations = 0;
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
}

int av1EncoderInitializations()
{
	return av1Initializations;
}

} // namespace tapeloop::test
