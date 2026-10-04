// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "TestEncoders.hpp"

#include <obs-module.h>

namespace tapeloop::test {
namespace {

int brokenState = 0;

const char *failingName(void *) noexcept
{
	return "Tapeloop failing test encoder";
}

const char *brokenName(void *) noexcept
{
	return "Tapeloop broken test encoder";
}

void *createNothing(obs_data_t *, obs_encoder_t *) noexcept
{
	return nullptr;
}

void *createSomething(obs_data_t *, obs_encoder_t *) noexcept
{
	return &brokenState;
}

void destroy(void *) noexcept {}

bool fail(void *, encoder_frame *, encoder_packet *, bool *) noexcept
{
	return false;
}

obs_encoder_info videoEncoder(const char *id)
{
	obs_encoder_info info = {};
	info.id = id;
	info.type = OBS_ENCODER_VIDEO;
	info.codec = "h264";
	info.destroy = destroy;
	info.encode = fail;
	return info;
}

} // namespace

void registerTestEncoders()
{
	obs_encoder_info failing = videoEncoder(kFailingEncoderId);
	failing.get_name = failingName;
	failing.create = createNothing;
	obs_register_encoder(&failing);

	obs_encoder_info broken = videoEncoder(kBrokenEncoderId);
	broken.get_name = brokenName;
	broken.create = createSomething;
	obs_register_encoder(&broken);
}

} // namespace tapeloop::test
