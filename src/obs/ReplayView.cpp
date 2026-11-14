// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ReplayView.hpp"

#include "obs/Composition.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplaySource.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace tapeloop::obs {
namespace {

// The views render on the graphics thread while the UI thread sets what they show.
struct Viewed {
	std::mutex mutex;
	obs_source_t *replay = nullptr;
	ReplayPlayer *player = nullptr;
};

Viewed &viewed()
{
	static Viewed instance;
	return instance;
}

const char *name(void *typeData) noexcept
{
	return static_cast<const std::string *>(typeData)->c_str();
}

void *create(obs_data_t *, obs_source_t *source) noexcept
{
	// A view keeps nothing of its own; libobs only needs data that is not null.
	return obs_source_get_type_data(source);
}

void destroy(void *) noexcept {}

uint32_t width(void *) noexcept
{
	return static_cast<uint32_t>(mainCanvas().width);
}

uint32_t height(void *) noexcept
{
	return static_cast<uint32_t>(mainCanvas().height);
}

void render(void *, gs_effect_t *) noexcept
{
	Viewed &shared = viewed();
	const std::lock_guard lock(shared.mutex);
	if (!shared.player) {
		return;
	}
	const PlayerState state = shared.player->state();
	if (!state.loaded || state.width == 0 || state.height == 0) {
		return;
	}
	const CanvasSize canvas = mainCanvas();
	const auto boxWidth = static_cast<float>(canvas.width);
	const auto boxHeight = static_cast<float>(canvas.height);
	const auto pictureWidth = static_cast<float>(state.width);
	const auto pictureHeight = static_cast<float>(state.height);
	const float scale = std::min(boxWidth / pictureWidth, boxHeight / pictureHeight);
	gs_matrix_push();
	gs_matrix_translate3f((boxWidth - pictureWidth * scale) / 2.0f, (boxHeight - pictureHeight * scale) / 2.0f,
			      0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	obs_source_video_render(shared.replay);
	gs_matrix_pop();
}

void freeName(void *typeData) noexcept
{
	const std::unique_ptr<std::string> displayName(static_cast<std::string *>(typeData));
}

void noteView(obs_source_t *, obs_source_t *child, void *param) noexcept
{
	const char *id = obs_source_get_unversioned_id(child);
	if (id && std::strcmp(id, kReplayViewId) == 0) {
		*static_cast<bool *>(param) = true;
	}
}

} // namespace

void registerReplayView(std::string displayName)
{
	auto typeData = std::make_unique<std::string>(std::move(displayName));
	obs_source_info info = {};
	info.id = kReplayViewId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.get_width = width;
	info.get_height = height;
	info.video_render = render;
	// libobs frees it with the type at shutdown, or at once when it refuses the type.
	info.type_data = typeData.release();
	info.free_type_data = freeName;
	obs_register_source(&info);
}

void setViewedReplay(obs_source_t *replay)
{
	ReplayPlayer *player = replayPlayer(replay);
	Viewed &shared = viewed();
	const std::lock_guard lock(shared.mutex);
	shared.replay = player ? replay : nullptr;
	shared.player = player;
}

void unsetViewedReplay(obs_source_t *replay)
{
	Viewed &shared = viewed();
	const std::lock_guard lock(shared.mutex);
	if (replay && shared.replay == replay) {
		shared.replay = nullptr;
		shared.player = nullptr;
	}
}

bool holdsReplayView(obs_source_t *scene)
{
	bool held = false;
	if (scene) {
		obs_source_enum_full_tree(scene, noteView, &held);
	}
	return held;
}

} // namespace tapeloop::obs
