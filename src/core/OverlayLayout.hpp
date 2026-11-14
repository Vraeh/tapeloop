// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tapeloop {

// Every value of the overlay model is in canvas pixels.

inline constexpr int32_t kMaxCanvasSize = 16384;
// The largest box side, distance from an anchor and crop. Twice the largest canvas, so
// a box can cover any canvas from anywhere on it.
inline constexpr int32_t kMaxOverlayExtent = 32768;
inline constexpr int32_t kMinOverlaySize = 1;
// One private wrapper and one scene item each at most; far more than a replay needs.
inline constexpr size_t kMaxOverlays = 32;
// The graphics-safe area of Rec. ITU-R BT.1848-1, as OBS draws it: 5% of each side.
inline constexpr int32_t kSafeMarginPercent = 5;

struct CanvasSize {
	int32_t width = 1920;
	int32_t height = 1080;

	bool operator==(const CanvasSize &) const = default;
};

struct CanvasPoint {
	int32_t x = 0;
	int32_t y = 0;

	bool operator==(const CanvasPoint &) const = default;
};

// A box by its top left corner.
struct OverlayBox {
	int32_t left = 0;
	int32_t top = 0;
	int32_t width = kMinOverlaySize;
	int32_t height = kMinOverlaySize;

	int64_t right() const noexcept { return int64_t{left} + width; }
	int64_t bottom() const noexcept { return int64_t{top} + height; }
	bool contains(CanvasPoint point) const noexcept;
	bool operator==(const OverlayBox &) const = default;
};

// The point of the canvas an overlay keeps its distance from, and the point of its box
// that keeps it, as OBS's alignment.
enum class OverlayAnchor { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };

// How the source fills its box: whole and in proportion, stretched to it, or covering
// it in proportion and cropped to it.
enum class OverlayFit { Inner, Stretch, Fill };

// OBS's blend modes.
enum class OverlayBlend { Normal, Additive, Subtract, Screen, Multiply, Lighten, Darken };

enum class OverlayPreset { TopLeft, TopRight, BottomLeft, BottomRight, LowerThird, FullScreen };

// What a replay shows on top: Tapeloop's own list, or one of the user's scenes.
enum class OverlayMode { Simple, Scene };

// In source pixels, before the source is fitted to its box.
struct OverlayCrop {
	int32_t left = 0;
	int32_t top = 0;
	int32_t right = 0;
	int32_t bottom = 0;

	bool operator==(const OverlayCrop &) const = default;
};

struct OverlayItem {
	std::string uuid;
	// For finding the source again when its UUID is gone, and for log lines.
	std::string name;
	bool visible = true;
	OverlayAnchor anchor = OverlayAnchor::TopLeft;
	// Inward from the anchor; from the center line, a positive distance is to the right
	// or down.
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = kMinOverlaySize;
	int32_t height = kMinOverlaySize;
	OverlayFit fit = OverlayFit::Inner;
	// Percent.
	int32_t opacity = 100;
	OverlayBlend blend = OverlayBlend::Normal;
	OverlayCrop crop;

	bool operator==(const OverlayItem &) const = default;
};

struct OverlayScene {
	std::string uuid;
	std::string name;

	bool operator==(const OverlayScene &) const = default;
};

struct OverlaySettings {
	OverlayMode mode = OverlayMode::Simple;
	OverlayScene scene;
	// The canvas the values were made at.
	CanvasSize canvas;
	// Bottom first.
	std::vector<OverlayItem> items;

	bool operator==(const OverlaySettings &) const = default;
};

// Brings a canvas size within 1 to kMaxCanvasSize on each side.
CanvasSize clampCanvas(CanvasSize canvas) noexcept;
// Brings every value within its range and keeps the first kMaxOverlays items.
void clampOverlays(OverlaySettings &settings);

// The safe margin of each side of the canvas: 5% of its width or height.
int32_t safeMarginX(CanvasSize canvas) noexcept;
int32_t safeMarginY(CanvasSize canvas) noexcept;

OverlayBox boxOf(const OverlayItem &item, CanvasSize canvas) noexcept;
// Gives the item the box's size and the distances that put it there from its anchor,
// within their ranges.
void placeBox(OverlayItem &item, const OverlayBox &box, CanvasSize canvas) noexcept;
// Changes the anchor and keeps the box where it is.
void setAnchor(OverlayItem &item, OverlayAnchor anchor, CanvasSize canvas) noexcept;

// Corners keep the size and sit at the safe margins. The lower third is the safe width,
// a third of the height, on the bottom margin. Full screen is the canvas.
void applyPreset(OverlayItem &item, OverlayPreset preset, CanvasSize canvas) noexcept;
// The preset that gives this anchor, distances and size, if one does.
std::optional<OverlayPreset> matchingPreset(const OverlayItem &item, CanvasSize canvas) noexcept;

// Scales distances and sizes from the canvas they were made at to `canvas`, which they
// are then made at. Crops are source pixels and stay.
void rescaleOverlays(OverlaySettings &settings, CanvasSize canvas);

// A new overlay of a source of the given size, which is zero when unknown: centered, at
// that size or brought within the canvas in proportion, or a quarter of the canvas.
OverlayItem newOverlay(std::string uuid, std::string name, int64_t sourceWidth, int64_t sourceHeight,
		       CanvasSize canvas);

enum class ResizeHandle { TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

struct DragOptions {
	// Edges and the center snap to the canvas edges, its center lines and the safe
	// margins within this many canvas pixels. Zero turns snapping off.
	int32_t snapDistance = 0;
	// A resize keeps the proportions the box had when the drag began.
	bool keepAspect = false;
};

// The editing copy of the overlays: the list, the selected item and a drag in progress.
// Every change brings values within their ranges, so an item is always one that can be
// shown. Indexes out of range are ignored.
class OverlayLayout {
public:
	explicit OverlayLayout(OverlaySettings settings);

	const OverlaySettings &settings() const noexcept { return settings_; }
	const std::vector<OverlayItem> &items() const noexcept { return settings_.items; }
	CanvasSize canvas() const noexcept { return settings_.canvas; }
	std::optional<size_t> selected() const noexcept { return selected_; }
	bool dragging() const noexcept { return drag_.has_value(); }

	void setMode(OverlayMode mode);
	void setScene(OverlayScene scene);
	// Rescales every item to the new canvas.
	void setCanvas(CanvasSize canvas);

	void select(std::optional<size_t> index);
	// On top of the others and selected. False when the list is full.
	bool add(OverlayItem item);
	// The selection stays on the item it was on, or goes when that item does.
	void remove(size_t index);
	void moveItem(size_t from, size_t to);
	void setItem(size_t index, OverlayItem item);
	void setVisible(size_t index, bool visible);
	void setBox(size_t index, const OverlayBox &box);
	void setAnchor(size_t index, OverlayAnchor anchor);
	void applyPreset(size_t index, OverlayPreset preset);
	// Moves the selected item by this many canvas pixels, without snapping.
	void nudge(int32_t dx, int32_t dy);

	// The topmost visible item whose box holds the point.
	std::optional<size_t> itemAt(CanvasPoint point) const noexcept;
	// The handle of the selected item, if it is visible, nearest the point and within
	// `tolerance` of it on both axes.
	std::optional<ResizeHandle> handleAt(CanvasPoint point, int32_t tolerance) const noexcept;

	// A drag of the selected visible item, from `point`. Any change but dragTo ends it.
	bool beginMove(CanvasPoint point);
	bool beginResize(ResizeHandle handle, CanvasPoint point);
	// Puts the item where the pointer at `point` takes it from where the drag began.
	void dragTo(CanvasPoint point, const DragOptions &options);
	void endDrag() noexcept;
	// Puts the item back as it was when the drag began.
	void cancelDrag();

private:
	struct Drag {
		size_t index;
		std::optional<ResizeHandle> handle;
		CanvasPoint from;
		OverlayItem start;
	};

	bool beginDrag(std::optional<ResizeHandle> handle, CanvasPoint point);
	OverlayItem *itemFor(size_t index);

	OverlaySettings settings_;
	std::optional<size_t> selected_;
	std::optional<Drag> drag_;
};

// The overlays in the shape they are saved in, one field per key. Numbers are as read,
// so a hand-edited or damaged file can give any value, NaN included.
struct SavedOverlayCrop {
	double left = 0;
	double top = 0;
	double right = 0;
	double bottom = 0;
};

struct SavedOverlay {
	std::string uuid;
	std::string name;
	bool visible = true;
	std::string anchor = "top_left";
	double x = 0;
	double y = 0;
	double width = 0;
	double height = 0;
	std::string fit = "inner";
	double opacity = 100;
	std::string blend = "normal";
	SavedOverlayCrop crop;
};

struct SavedOverlays {
	std::string mode = "simple";
	std::string sceneUuid;
	std::string sceneName;
	double canvasWidth = 0;
	double canvasHeight = 0;
	std::vector<SavedOverlay> items;
};

std::string_view overlayAnchorName(OverlayAnchor anchor) noexcept;
std::optional<OverlayAnchor> overlayAnchorFrom(std::string_view name) noexcept;
std::string_view overlayFitName(OverlayFit fit) noexcept;
std::optional<OverlayFit> overlayFitFrom(std::string_view name) noexcept;
std::string_view overlayBlendName(OverlayBlend blend) noexcept;
std::optional<OverlayBlend> overlayBlendFrom(std::string_view name) noexcept;

SavedOverlays saveOverlays(const OverlaySettings &settings);

// Saved settings are untrusted. Numbers are rounded and brought within their ranges, NaN
// counting as zero except for an opacity, which counts as full. An anchor, fit or blend
// this build does not know becomes the top left, inner and normal, and an unknown mode
// simple. Items with neither a UUID nor a name are dropped, as are those past
// kMaxOverlays. Values made at a valid canvas other than `canvas` are rescaled to it.
OverlaySettings loadOverlays(const SavedOverlays &saved, CanvasSize canvas);

} // namespace tapeloop
