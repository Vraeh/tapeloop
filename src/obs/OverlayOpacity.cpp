// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/OverlayOpacity.hpp"

#include "obs/SourceLookup.hpp"

#include <obs.hpp>
#include <util/base.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace tapeloop::obs {
namespace {

constexpr const char *kTargetKey = "target";
constexpr const char *kOpacityKey = "opacity";

struct OverlayOpacity {
	obs_source_t *self = nullptr;
	// Held, and set before the source can be drawn.
	obs_source_t *target = nullptr;
	float opacity = 1.0f;
	// libobs keeps an effect loaded from a file until its graphics go.
	gs_effect_t *effect = nullptr;
	gs_eparam_t *image = nullptr;
	gs_eparam_t *scale = nullptr;
	// Made again only when a draw is in another colour format.
	gs_texrender_t *texture = nullptr;
};

const char *name(void *) noexcept
{
	return "Tapeloop overlay opacity";
}

void destroy(void *data) noexcept
{
	std::unique_ptr<OverlayOpacity> wrapper(static_cast<OverlayOpacity *>(data));
	obs_source_release(wrapper->target);
	obs_enter_graphics();
	gs_texrender_destroy(wrapper->texture);
	obs_leave_graphics();
}

void *create(obs_data_t *settings, obs_source_t *source) noexcept
{
	const auto *effectPath = static_cast<const std::string *>(obs_source_get_type_data(source));
	// Module load has said so when the effect is missing.
	if (!effectPath || effectPath->empty()) {
		return nullptr;
	}
	std::unique_ptr<OverlayOpacity> wrapper(new (std::nothrow) OverlayOpacity);
	if (!wrapper) {
		return nullptr;
	}
	wrapper->self = source;
	wrapper->opacity =
		static_cast<float>(std::clamp<long long>(obs_data_get_int(settings, kOpacityKey), 0, 100)) / 100.0f;

	obs_enter_graphics();
	char *error = nullptr;
	wrapper->effect = gs_effect_create_from_file(effectPath->c_str(), &error);
	if (wrapper->effect) {
		wrapper->image = gs_effect_get_param_by_name(wrapper->effect, "image");
		wrapper->scale = gs_effect_get_param_by_name(wrapper->effect, "opacity");
	}
	wrapper->texture = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	const bool loaded = wrapper->effect && wrapper->image && wrapper->scale;
	if (!loaded) {
		blog(LOG_ERROR, "[tapeloop] The overlay opacity effect '%s' does not load: %s", effectPath->c_str(),
		     error ? error : "it is missing or lacks its parameters");
	}
	bfree(error);

	wrapper->target = obs_source_get_ref(sourceByUuid(obs_data_get_string(settings, kTargetKey)));
	if (!loaded || !wrapper->texture || !wrapper->target) {
		destroy(wrapper.release());
		return nullptr;
	}
	return wrapper.release();
}

uint32_t width(void *data) noexcept
{
	return obs_source_get_width(static_cast<OverlayOpacity *>(data)->target);
}

uint32_t height(void *data) noexcept
{
	return obs_source_get_height(static_cast<OverlayOpacity *>(data)->target);
}

void enumSources(void *data, obs_source_enum_proc_t callback, void *param) noexcept
{
	const auto &wrapper = *static_cast<OverlayOpacity *>(data);
	callback(wrapper.self, wrapper.target, param);
}

gs_color_space colorSpace(void *data, size_t count, const gs_color_space *preferred) noexcept
{
	return obs_source_get_color_space(static_cast<OverlayOpacity *>(data)->target, count, preferred);
}

void tick(void *data, float) noexcept
{
	gs_texrender_reset(static_cast<OverlayOpacity *>(data)->texture);
}

// Draws the target into the texture, premultiplied by its alpha as OBS's scene items
// are, in the colour space this draw is in.
bool drawTarget(OverlayOpacity &wrapper, uint32_t cx, uint32_t cy)
{
	const gs_color_space space = gs_get_color_space();
	const gs_color_format format = gs_get_format_from_space(space);
	if (wrapper.texture && gs_texrender_get_format(wrapper.texture) != format) {
		gs_texrender_destroy(wrapper.texture);
		wrapper.texture = nullptr;
	}
	if (!wrapper.texture) {
		wrapper.texture = gs_texrender_create(format, GS_ZS_NONE);
		if (!wrapper.texture) {
			return false;
		}
	}
	// Drawn once a frame, however many views show it.
	if (gs_texrender_begin_with_color_space(wrapper.texture, cx, cy, space)) {
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		const vec4 clear = {};
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy), -100.0f, 100.0f);
		obs_source_video_render(wrapper.target);
		gs_blend_state_pop();
		gs_texrender_end(wrapper.texture);
	}
	return true;
}

void render(void *data, gs_effect_t *) noexcept
{
	auto &wrapper = *static_cast<OverlayOpacity *>(data);
	// A scene only lets go of a removed source it holds itself.
	if (obs_source_removed(wrapper.target)) {
		return;
	}
	const uint32_t cx = obs_source_get_width(wrapper.target);
	const uint32_t cy = obs_source_get_height(wrapper.target);
	if (cx == 0 || cy == 0 || !drawTarget(wrapper, cx, cy)) {
		return;
	}
	gs_texture_t *texture = gs_texrender_get_texture(wrapper.texture);
	if (!texture) {
		return;
	}

	const bool linear = gs_get_linear_srgb();
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear) {
		gs_effect_set_texture_srgb(wrapper.image, texture);
	} else {
		gs_effect_set_texture(wrapper.image, texture);
	}
	gs_effect_set_float(wrapper.scale, wrapper.opacity);
	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(wrapper.effect, "Draw")) {
		gs_draw_sprite(texture, 0, cx, cy);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(srgb);
}

void freeEffectPath(void *typeData) noexcept
{
	const std::unique_ptr<std::string> effectPath(static_cast<std::string *>(typeData));
}

} // namespace

void registerOverlayOpacity(std::string effectPath)
{
	auto typeData = std::make_unique<std::string>(std::move(effectPath));
	obs_source_info info = {};
	info.id = kOverlayOpacityId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB | OBS_SOURCE_CAP_DISABLED;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.get_width = width;
	info.get_height = height;
	info.enum_active_sources = enumSources;
	info.enum_all_sources = enumSources;
	info.video_get_color_space = colorSpace;
	info.video_tick = tick;
	info.video_render = render;
	// libobs frees it with the type at shutdown, or at once when it refuses the type.
	info.type_data = typeData.release();
	info.free_type_data = freeEffectPath;
	obs_register_source(&info);
}

obs_source_t *createOverlayOpacity(obs_source_t *target, int32_t opacity)
{
	if (!target) {
		return nullptr;
	}
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, kTargetKey, obs_source_get_uuid(target));
	obs_data_set_int(settings, kOpacityKey, opacity);
	obs_source_t *source = obs_source_create_private(kOverlayOpacityId, obs_source_get_name(target), settings);
	if (source && !obs_obj_get_data(source)) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

obs_source_t *overlayTarget(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	if (!id || std::strcmp(id, kOverlayOpacityId) != 0) {
		return source;
	}
	auto *wrapper = static_cast<OverlayOpacity *>(obs_obj_get_data(source));
	return wrapper ? wrapper->target : source;
}

} // namespace tapeloop::obs
