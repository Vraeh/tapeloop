// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ReplaySource.hpp"

#include "obs/PictureRenderer.hpp"

#include <obs-module.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace tapeloop::obs {
namespace {

struct ReplaySource {
	explicit ReplaySource(ReplayPlayer::DecoderFactory decoders) : player(std::move(decoders)) {}

	ReplayPlayer player;
	// Lives and dies inside the graphics context.
	PictureRenderer renderer;
};

const char *name(void *) noexcept
{
	return "Tapeloop replay player";
}

void *create(obs_data_t *, obs_source_t *source) noexcept
{
	const auto *decoders = static_cast<const ReplayPlayer::DecoderFactory *>(obs_source_get_type_data(source));
	if (!decoders) {
		return nullptr;
	}
	std::unique_ptr<ReplaySource> replay;
	try {
		replay = std::make_unique<ReplaySource>(*decoders);
	} catch (...) {
		return nullptr;
	}
	// The renderer holds nothing yet, so it may go outside the graphics context here.
	if (!replay->player.start()) {
		return nullptr;
	}
	return replay.release();
}

void destroy(void *data) noexcept
{
	auto *replay = static_cast<ReplaySource *>(data);
	// Outside the graphics context: the frame being decoded may take a while.
	replay->player.stop();
	obs_enter_graphics();
	delete replay;
	obs_leave_graphics();
}

uint32_t width(void *data) noexcept
{
	return static_cast<ReplaySource *>(data)->player.state().width;
}

uint32_t height(void *data) noexcept
{
	return static_cast<ReplaySource *>(data)->player.state().height;
}

void tick(void *data, float seconds) noexcept
{
	// The controls given since the last tick apply even when no time passed.
	const Nanoseconds elapsed{
		std::isfinite(seconds) && seconds > 0.0f ? std::llround(static_cast<double>(seconds) * 1e9) : 0};
	static_cast<ReplaySource *>(data)->player.tick(elapsed);
}

void render(void *data, gs_effect_t *) noexcept
{
	auto &replay = *static_cast<ReplaySource *>(data);
	// At the size of the frame drawn last, which is the size the source reports; before
	// the first one that is zero, which draws a picture at its own size.
	const PlayerState state = replay.player.state();
	replay.player.render(replay.renderer, state.width, state.height);
}

void freeDecoders(void *typeData) noexcept
{
	delete static_cast<ReplayPlayer::DecoderFactory *>(typeData);
}

} // namespace

void registerReplaySource(ReplayPlayer::DecoderFactory decoders)
{
	auto typeData = std::make_unique<ReplayPlayer::DecoderFactory>(std::move(decoders));
	obs_source_info info = {};
	info.id = kReplaySourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_DISABLED;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.get_width = width;
	info.get_height = height;
	info.video_tick = tick;
	info.video_render = render;
	// libobs frees it with the type at shutdown, or at once when it refuses the type.
	info.type_data = typeData.release();
	info.free_type_data = freeDecoders;
	obs_register_source(&info);
}

obs_source_t *createReplaySource(const char *sourceName)
{
	obs_source_t *source = obs_source_create_private(kReplaySourceId, sourceName, nullptr);
	if (source && !obs_obj_get_data(source)) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

ReplayPlayer *replayPlayer(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	if (!id || std::strcmp(id, kReplaySourceId) != 0) {
		return nullptr;
	}
	auto *replay = static_cast<ReplaySource *>(obs_obj_get_data(source));
	return replay ? &replay->player : nullptr;
}

} // namespace tapeloop::obs
