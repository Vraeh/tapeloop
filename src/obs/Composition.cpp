// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/Composition.hpp"

#include "obs/OverlayOpacity.hpp"
#include "obs/SourceLookup.hpp"

#include <obs.hpp>
#include <util/base.h>

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace tapeloop::obs {
namespace {

constexpr const char *kSceneName = "Tapeloop composition";

struct Layer {
	const OverlayItem *item = nullptr;
	obs_source_t *source = nullptr;
};

struct Build {
	obs_sceneitem_t *replayItem = nullptr;
	CanvasSize canvas;
	const std::vector<Layer> *layers = nullptr;
};

uint32_t alignmentOf(OverlayAnchor anchor)
{
	switch (anchor) {
	case OverlayAnchor::TopLeft:
		return OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	case OverlayAnchor::Top:
		return OBS_ALIGN_TOP;
	case OverlayAnchor::TopRight:
		return OBS_ALIGN_TOP | OBS_ALIGN_RIGHT;
	case OverlayAnchor::Left:
		return OBS_ALIGN_LEFT;
	case OverlayAnchor::Center:
		return OBS_ALIGN_CENTER;
	case OverlayAnchor::Right:
		return OBS_ALIGN_RIGHT;
	case OverlayAnchor::BottomLeft:
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_LEFT;
	case OverlayAnchor::Bottom:
		return OBS_ALIGN_BOTTOM;
	case OverlayAnchor::BottomRight:
		return OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT;
	}
	return OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
}

// Where along one side of its box an item's alignment puts its position.
float shareOf(uint32_t alignment, uint32_t start, uint32_t end)
{
	if ((alignment & start) != 0) {
		return 0.0f;
	}
	return (alignment & end) != 0 ? 1.0f : 0.5f;
}

obs_bounds_type boundsOf(OverlayFit fit)
{
	switch (fit) {
	case OverlayFit::Inner:
		return OBS_BOUNDS_SCALE_INNER;
	case OverlayFit::Stretch:
		return OBS_BOUNDS_STRETCH;
	case OverlayFit::Fill:
		return OBS_BOUNDS_SCALE_OUTER;
	}
	return OBS_BOUNDS_SCALE_INNER;
}

obs_blending_type blendingOf(OverlayBlend blend)
{
	switch (blend) {
	case OverlayBlend::Normal:
		return OBS_BLEND_NORMAL;
	case OverlayBlend::Additive:
		return OBS_BLEND_ADDITIVE;
	case OverlayBlend::Subtract:
		return OBS_BLEND_SUBTRACT;
	case OverlayBlend::Screen:
		return OBS_BLEND_SCREEN;
	case OverlayBlend::Multiply:
		return OBS_BLEND_MULTIPLY;
	case OverlayBlend::Lighten:
		return OBS_BLEND_LIGHTEN;
	case OverlayBlend::Darken:
		return OBS_BLEND_DARKEN;
	}
	return OBS_BLEND_NORMAL;
}

// One of the user's inputs or scenes with video. Private sources are left out, the
// replay and the composition among them.
bool showable(obs_source_t *source)
{
	if (!source || obs_source_removed(source) || obs_obj_is_private(source) || obs_source_is_group(source)) {
		return false;
	}
	const obs_source_type type = obs_source_get_type(source);
	return (type == OBS_SOURCE_TYPE_INPUT || type == OBS_SOURCE_TYPE_SCENE) &&
	       (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0;
}

void place(obs_sceneitem_t *sceneItem, const OverlayItem &item, CanvasSize canvas)
{
	// The anchor is OBS's alignment, for the item and for its content within the box, so
	// content that changes size stays at its anchor.
	const OverlayBox box = boxOf(item, canvas);
	const uint32_t alignment = alignmentOf(item.anchor);
	obs_transform_info info = {};
	vec2_set(&info.pos,
		 static_cast<float>(box.left) +
			 static_cast<float>(box.width) * shareOf(alignment, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT),
		 static_cast<float>(box.top) +
			 static_cast<float>(box.height) * shareOf(alignment, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM));
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.alignment = alignment;
	info.bounds_type = boundsOf(item.fit);
	info.bounds_alignment = alignment;
	vec2_set(&info.bounds, static_cast<float>(box.width), static_cast<float>(box.height));
	info.crop_to_bounds = item.fit == OverlayFit::Fill;
	obs_sceneitem_set_info2(sceneItem, &info);

	obs_sceneitem_crop crop = {item.crop.left, item.crop.top, item.crop.right, item.crop.bottom};
	obs_sceneitem_set_crop(sceneItem, &crop);
	obs_sceneitem_set_blending_mode(sceneItem, blendingOf(item.blend));
}

// Every item but the one given, which may be null.
bool removeOthers(obs_scene_t *, obs_sceneitem_t *item, void *param) noexcept
{
	if (item != param) {
		obs_sceneitem_remove(item);
	}
	return true;
}

struct Pruning {
	obs_sceneitem_t *replayItem = nullptr;
	bool dropped = false;
};

bool removeIfRemoved(obs_scene_t *, obs_sceneitem_t *item, void *param) noexcept
{
	auto &pruning = *static_cast<Pruning *>(param);
	if (item != pruning.replayItem && obs_source_removed(overlayTarget(obs_sceneitem_get_source(item)))) {
		obs_sceneitem_remove(item);
		pruning.dropped = true;
	}
	return true;
}

void fitReplay(obs_sceneitem_t *replayItem, CanvasSize canvas)
{
	obs_transform_info info = {};
	vec2_set(&info.pos, static_cast<float>(canvas.width) / 2.0f, static_cast<float>(canvas.height) / 2.0f);
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.alignment = OBS_ALIGN_CENTER;
	info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	vec2_set(&info.bounds, static_cast<float>(canvas.width), static_cast<float>(canvas.height));
	obs_sceneitem_set_info2(replayItem, &info);
}

void rebuild(void *data, obs_scene_t *scene) noexcept
{
	const auto &build = *static_cast<const Build *>(data);
	obs_scene_enum_items(scene, removeOthers, build.replayItem);
	fitReplay(build.replayItem, build.canvas);
	// Each one goes on top of the last.
	for (const Layer &layer : *build.layers) {
		obs_sceneitem_t *added = obs_scene_add(scene, layer.source);
		if (added) {
			place(added, *layer.item, build.canvas);
		}
	}
}

} // namespace

CanvasSize mainCanvas()
{
	obs_video_info video = {};
	if (!obs_get_video_info(&video)) {
		return {};
	}
	const auto side = [](uint32_t value) {
		return static_cast<int32_t>(std::min(value, static_cast<uint32_t>(kMaxCanvasSize)));
	};
	return clampCanvas({side(video.base_width), side(video.base_height)});
}

Composition::Composition(obs_source_t *replay)
{
	if (!replay) {
		return;
	}
	scene_ = obs_scene_create_private(kSceneName);
	if (!scene_) {
		return;
	}
	replayItem_ = obs_scene_add(scene_, replay);
	if (!replayItem_) {
		obs_scene_release(scene_);
		scene_ = nullptr;
		return;
	}
	signal_handler_connect(obs_get_signal_handler(), "source_remove", handleSourceRemove, this);
	apply();
}

Composition::~Composition()
{
	destroy();
}

obs_source_t *Composition::source() const noexcept
{
	return scene_ ? obs_scene_get_source(scene_) : nullptr;
}

void Composition::setOverlays(const OverlaySettings &overlays)
{
	if (overlays == overlays_) {
		return;
	}
	overlays_ = overlays;
	stale_ = true;
}

void Composition::apply()
{
	if (!scene_) {
		return;
	}
	const CanvasSize canvas = mainCanvas();
	OverlaySettings placed = overlays_;
	rescaleOverlays(placed, canvas);

	std::vector<OBSSourceAutoRelease> held;
	std::vector<Layer> layers;
	std::vector<bool> found;
	held.reserve(placed.items.size());
	layers.reserve(placed.items.size());
	for (const OverlayItem &item : placed.items) {
		if (!item.visible) {
			continue;
		}
		OBSSourceAutoRelease source = sourceByUuid(item.uuid.c_str());
		const bool shown = showable(source);
		found.push_back(shown);
		if (shown) {
			layers.push_back({&item, source.Get()});
			held.push_back(std::move(source));
		}
	}
	if (!stale_ && canvas == placedAt_ && found == found_) {
		return;
	}

	for (size_t i = 0, layer = 0; i < placed.items.size(); ++i) {
		const OverlayItem &item = placed.items[i];
		if (!item.visible) {
			continue;
		}
		if (!found[layer++]) {
			blog(LOG_INFO, "[tapeloop] The overlay '%s' is not in this scene collection, so it is left out",
			     item.name.empty() ? item.uuid.c_str() : item.name.c_str());
		}
	}
	// Below full opacity an overlay is drawn through a source of its own, made here for
	// this scene only and held by it alone. Should one fail, the overlay shows opaque.
	for (Layer &layer : layers) {
		if (layer.item->opacity >= 100) {
			continue;
		}
		OBSSourceAutoRelease translucent = createOverlayOpacity(layer.source, layer.item->opacity);
		if (translucent) {
			layer.source = translucent.Get();
			held.push_back(std::move(translucent));
		}
	}
	Build build{replayItem_, canvas, &layers};
	obs_scene_atomic_update(scene_, rebuild, &build);
	stale_ = false;
	placedAt_ = canvas;
	found_ = std::move(found);
}

void Composition::prune()
{
	if (!scene_) {
		return;
	}
	Pruning pruning{replayItem_, false};
	obs_scene_enum_items(scene_, removeIfRemoved, &pruning);
	if (pruning.dropped) {
		stale_ = true;
	}
}

void Composition::destroy()
{
	if (scene_) {
		// Waits for a handler running now, after which nothing is handed to the UI thread.
		signal_handler_disconnect(obs_get_signal_handler(), "source_remove", handleSourceRemove, this);
		obs_scene_enum_items(scene_, removeOthers, nullptr);
		obs_scene_release(scene_);
		scene_ = nullptr;
		replayItem_ = nullptr;
	}
	found_.clear();
	stale_ = true;
	alive_.reset();
}

void Composition::runTask(void *param) noexcept
{
	const std::unique_ptr<Task> task(static_cast<Task *>(param));
	if (!task->alive.lock()) {
		return;
	}
	try {
		task->composition->prune();
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Letting go of a removed overlay failed");
	}
}

void Composition::handleSourceRemove(void *data, calldata_t *) noexcept
{
	auto &self = *static_cast<Composition *>(data);
	// Should it fail, the scene lets go of the source when it is next built.
	std::unique_ptr<Task> task(new (std::nothrow) Task);
	if (!task) {
		return;
	}
	task->alive = self.alive_;
	task->composition = &self;
	obs_queue_task(OBS_TASK_UI, runTask, task.release(), false);
}

} // namespace tapeloop::obs
