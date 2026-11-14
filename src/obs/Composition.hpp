// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/OverlayLayout.hpp"

#include <obs.h>

#include <memory>
#include <vector>

namespace tapeloop::obs {

// The base size of OBS's main canvas, or the default size when OBS has no video.
CanvasSize mainCanvas();

// What a replay goes on air as: a private scene, never in OBS's scene list, holding the
// replay fitted to the canvas and, above it, the visible overlays in their order. The
// overlays are values made at some canvas; they are placed at the main canvas when
// applied. The UI thread only, but for the signal of a removed source, which leaves the
// work to the UI thread.
class Composition {
public:
	// Shows this source, which outlives the composition, as the replay. Without one there
	// is no scene.
	explicit Composition(obs_source_t *replay);
	~Composition();

	Composition(const Composition &) = delete;
	Composition &operator=(const Composition &) = delete;
	Composition(Composition &&) = delete;
	Composition &operator=(Composition &&) = delete;

	// The scene, or null when there is none.
	obs_source_t *source() const noexcept;
	const OverlaySettings &overlays() const noexcept { return overlays_; }
	// Shown from the next apply().
	void setOverlays(const OverlaySettings &overlays);
	// Builds the scene again when the overlays, the canvas or which of their sources OBS
	// has changed since it was last built, and only places the overlays again when they
	// just moved.
	void apply();
	// Empties the scene and lets go of it, for good.
	void destroy();

private:
	struct Task {
		std::weak_ptr<int> alive;
		Composition *composition = nullptr;
	};

	// Drops the overlays whose source OBS removed, which a scene only does itself while it
	// renders: off air, it would hold them through a scene collection switch.
	void prune();

	static void runTask(void *param) noexcept;
	static void handleSourceRemove(void *data, calldata_t *call) noexcept;

	obs_scene_t *scene_ = nullptr;
	// Owned by the scene.
	obs_sceneitem_t *replayItem_ = nullptr;
	OverlaySettings overlays_;
	bool stale_ = true;
	// Only where the overlays are placed changed.
	bool moved_ = false;
	CanvasSize placedAt_{0, 0};
	// Whether each visible overlay's source was found when the scene was built.
	std::vector<bool> found_;
	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace tapeloop::obs
