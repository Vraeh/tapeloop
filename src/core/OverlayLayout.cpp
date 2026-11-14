// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/OverlayLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace tapeloop {
namespace {

// Far outside any box the ranges allow, and far inside int32_t, so that a box moved by
// any int32_t still has coordinates.
constexpr int64_t kMaxBoxCoordinate = int64_t{4} * kMaxOverlayExtent;

constexpr std::array<std::string_view, 9> kAnchorNames = {
	"top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right",
};
constexpr std::array<std::string_view, 3> kFitNames = {"inner", "stretch", "fill"};
constexpr std::array<std::string_view, 7> kBlendNames = {
	"normal", "additive", "subtract", "screen", "multiply", "lighten", "darken",
};

int32_t clampTo(int64_t value, int64_t low, int64_t high) noexcept
{
	return static_cast<int32_t>(std::clamp(value, low, high));
}

int32_t clampSize(int64_t size) noexcept
{
	return clampTo(size, kMinOverlaySize, kMaxOverlayExtent);
}

int32_t clampDistance(int64_t distance) noexcept
{
	return clampTo(distance, -kMaxOverlayExtent, kMaxOverlayExtent);
}

int32_t clampCoordinate(int64_t coordinate) noexcept
{
	return clampTo(coordinate, -kMaxBoxCoordinate, kMaxBoxCoordinate);
}

int64_t floorHalf(int64_t value) noexcept
{
	return value >= 0 ? value / 2 : -((1 - value) / 2);
}

// Rounds half away from zero. The divisor is positive.
int64_t roundDiv(int64_t numerator, int64_t divisor) noexcept
{
	const int64_t half = divisor / 2;
	return numerator >= 0 ? (numerator + half) / divisor : -((half - numerator) / divisor);
}

// Where the anchor sits on one axis: the start, the center line or the end.
enum class Side { Start, Middle, End };

Side columnOf(OverlayAnchor anchor) noexcept
{
	switch (anchor) {
	case OverlayAnchor::Top:
	case OverlayAnchor::Center:
	case OverlayAnchor::Bottom:
		return Side::Middle;
	case OverlayAnchor::TopRight:
	case OverlayAnchor::Right:
	case OverlayAnchor::BottomRight:
		return Side::End;
	case OverlayAnchor::TopLeft:
	case OverlayAnchor::Left:
	case OverlayAnchor::BottomLeft:
		break;
	}
	return Side::Start;
}

Side rowOf(OverlayAnchor anchor) noexcept
{
	switch (anchor) {
	case OverlayAnchor::Left:
	case OverlayAnchor::Center:
	case OverlayAnchor::Right:
		return Side::Middle;
	case OverlayAnchor::BottomLeft:
	case OverlayAnchor::Bottom:
	case OverlayAnchor::BottomRight:
		return Side::End;
	case OverlayAnchor::TopLeft:
	case OverlayAnchor::Top:
	case OverlayAnchor::TopRight:
		break;
	}
	return Side::Start;
}

// The start of a box of `size` on an axis of `length`, `distance` in from the anchor,
// and back.
int64_t startFrom(Side side, int64_t distance, int64_t size, int64_t length) noexcept
{
	switch (side) {
	case Side::Middle:
		return floorHalf(length - size) + distance;
	case Side::End:
		return length - size - distance;
	case Side::Start:
		break;
	}
	return distance;
}

int64_t distanceFrom(Side side, int64_t start, int64_t size, int64_t length) noexcept
{
	switch (side) {
	case Side::Middle:
		return start - floorHalf(length - size);
	case Side::End:
		return length - size - start;
	case Side::Start:
		break;
	}
	return start;
}

void clampItem(OverlayItem &item) noexcept
{
	item.x = clampDistance(item.x);
	item.y = clampDistance(item.y);
	item.width = clampSize(item.width);
	item.height = clampSize(item.height);
	item.opacity = clampTo(item.opacity, 0, 100);
	item.crop.left = clampTo(item.crop.left, 0, kMaxOverlayExtent);
	item.crop.top = clampTo(item.crop.top, 0, kMaxOverlayExtent);
	item.crop.right = clampTo(item.crop.right, 0, kMaxOverlayExtent);
	item.crop.bottom = clampTo(item.crop.bottom, 0, kMaxOverlayExtent);
}

int32_t scaleTo(int32_t value, int32_t to, int32_t from) noexcept
{
	return clampTo(roundDiv(int64_t{value} * to, from), std::numeric_limits<int32_t>::min(),
		       std::numeric_limits<int32_t>::max());
}

// The lines an edge snaps to on an axis of `length` with `margin`.
std::array<int64_t, 5> snapLines(int64_t length, int64_t margin) noexcept
{
	return {0, margin, floorHalf(length), length - margin, length};
}

// The smallest move within `distance` that puts `position` on one of `lines`.
std::optional<int64_t> snapDelta(int64_t position, const std::array<int64_t, 5> &lines, int64_t distance) noexcept
{
	std::optional<int64_t> best;
	for (const int64_t line : lines) {
		const int64_t delta = line - position;
		if (std::abs(delta) <= distance && (!best || std::abs(delta) < std::abs(*best))) {
			best = delta;
		}
	}
	return best;
}

std::optional<int64_t> nearer(std::optional<int64_t> a, std::optional<int64_t> b) noexcept
{
	if (!a || (b && std::abs(*b) < std::abs(*a))) {
		return b;
	}
	return a;
}

// A moved box's start on one axis, with its start, end or center snapped.
int64_t snapMove(int64_t start, int64_t size, int64_t length, int64_t margin, int64_t distance) noexcept
{
	if (distance <= 0) {
		return start;
	}
	const std::array<int64_t, 5> lines = snapLines(length, margin);
	std::optional<int64_t> delta =
		nearer(snapDelta(start, lines, distance), snapDelta(start + size, lines, distance));
	const int64_t centered = floorHalf(length - size) - start;
	if (std::abs(centered) <= distance) {
		delta = nearer(delta, centered);
	}
	return start + delta.value_or(0);
}

struct Size {
	int32_t width;
	int32_t height;
};

// The size on the other axis that keeps the proportions of `from` to `fromOther`, with
// `size` brought back when that would leave its range.
std::pair<int32_t, int32_t> keepProportions(int64_t size, int64_t from, int64_t fromOther) noexcept
{
	const int32_t driver = clampSize(size);
	const int64_t other = roundDiv(int64_t{driver} * fromOther, from);
	if (other > kMaxOverlayExtent) {
		return {clampSize(roundDiv(int64_t{kMaxOverlayExtent} * from, fromOther)), kMaxOverlayExtent};
	}
	return {driver, clampSize(other)};
}

Size fromWidth(int64_t width, const OverlayBox &from) noexcept
{
	const auto [w, h] = keepProportions(width, from.width, from.height);
	return {w, h};
}

Size fromHeight(int64_t height, const OverlayBox &from) noexcept
{
	const auto [h, w] = keepProportions(height, from.height, from.width);
	return {w, h};
}

struct Edges {
	bool movesStart = false;
	bool movesEnd = false;
};

Edges horizontalEdges(ResizeHandle handle) noexcept
{
	switch (handle) {
	case ResizeHandle::TopLeft:
	case ResizeHandle::Left:
	case ResizeHandle::BottomLeft:
		return {true, false};
	case ResizeHandle::TopRight:
	case ResizeHandle::Right:
	case ResizeHandle::BottomRight:
		return {false, true};
	case ResizeHandle::Top:
	case ResizeHandle::Bottom:
		break;
	}
	return {};
}

Edges verticalEdges(ResizeHandle handle) noexcept
{
	switch (handle) {
	case ResizeHandle::TopLeft:
	case ResizeHandle::Top:
	case ResizeHandle::TopRight:
		return {true, false};
	case ResizeHandle::BottomLeft:
	case ResizeHandle::Bottom:
	case ResizeHandle::BottomRight:
		return {false, true};
	case ResizeHandle::Left:
	case ResizeHandle::Right:
		break;
	}
	return {};
}

// One axis of a resize: the size the pointer gives, before any snapping, and where the
// moving edge is then.
struct Axis {
	Edges edges;
	int64_t start = 0;
	int64_t end = 0;
	int64_t length = 0;
	int64_t margin = 0;

	bool moves() const noexcept { return edges.movesStart || edges.movesEnd; }

	int64_t sizeFor(int64_t delta) const noexcept
	{
		if (edges.movesStart) {
			return end - (start + delta);
		}
		if (edges.movesEnd) {
			return end + delta - start;
		}
		return end - start;
	}

	int64_t movingEdge(int64_t size) const noexcept { return edges.movesStart ? end - size : start + size; }

	// The size change that snaps the moving edge of a box of `size`.
	std::optional<int64_t> snapSize(int64_t size, int64_t distance) const noexcept
	{
		if (!moves() || distance <= 0) {
			return std::nullopt;
		}
		const std::optional<int64_t> delta = snapDelta(movingEdge(size), snapLines(length, margin), distance);
		if (!delta) {
			return std::nullopt;
		}
		return edges.movesStart ? -*delta : *delta;
	}

	int64_t startFor(int64_t size) const noexcept { return edges.movesStart ? end - size : start; }
};

OverlayBox resizeBox(const OverlayBox &from, ResizeHandle handle, int64_t dx, int64_t dy, CanvasSize canvas,
		     const DragOptions &options) noexcept
{
	const Axis across{horizontalEdges(handle), from.left, from.right(), canvas.width, safeMarginX(canvas)};
	const Axis down{verticalEdges(handle), from.top, from.bottom(), canvas.height, safeMarginY(canvas)};
	const int64_t distance = std::clamp<int64_t>(options.snapDistance, 0, kMaxOverlayExtent);

	int64_t width = clampSize(across.sizeFor(dx));
	int64_t height = clampSize(down.sizeFor(dy));
	if (!options.keepAspect) {
		width = clampSize(width + across.snapSize(width, distance).value_or(0));
		height = clampSize(height + down.snapSize(height, distance).value_or(0));
	} else {
		// As in OBS, the axis asking for the larger box in proportion leads, unless only
		// the other one moves.
		bool widthLeads = width * from.height >= height * from.width;
		if (!across.moves() || !down.moves()) {
			widthLeads = across.moves();
		}
		Size size = widthLeads ? fromWidth(width, from) : fromHeight(height, from);
		const std::optional<int64_t> snapWidth = across.snapSize(size.width, distance);
		const std::optional<int64_t> snapHeight = down.snapSize(size.height, distance);
		if (snapWidth && (!snapHeight || std::abs(*snapWidth) <= std::abs(*snapHeight))) {
			size = fromWidth(size.width + *snapWidth, from);
		} else if (snapHeight) {
			size = fromHeight(size.height + *snapHeight, from);
		}
		width = size.width;
		height = size.height;
	}
	return {clampCoordinate(across.startFor(width)), clampCoordinate(down.startFor(height)),
		static_cast<int32_t>(width), static_cast<int32_t>(height)};
}

int32_t savedNumber(double value, double fallback, int64_t low, int64_t high) noexcept
{
	if (std::isnan(value)) {
		value = fallback;
	}
	return static_cast<int32_t>(
		std::llround(std::clamp(value, static_cast<double>(low), static_cast<double>(high))));
}

template<typename Enum, size_t Count>
std::optional<Enum> enumFrom(const std::array<std::string_view, Count> &names, std::string_view name) noexcept
{
	const auto found = std::find(names.begin(), names.end(), name);
	if (found == names.end()) {
		return std::nullopt;
	}
	return static_cast<Enum>(found - names.begin());
}

template<size_t Count> std::string_view nameOf(const std::array<std::string_view, Count> &names, size_t index) noexcept
{
	return index < Count ? names[index] : names[0];
}

} // namespace

bool OverlayBox::contains(CanvasPoint point) const noexcept
{
	return point.x >= left && point.x < right() && point.y >= top && point.y < bottom();
}

CanvasSize clampCanvas(CanvasSize canvas) noexcept
{
	return {clampTo(canvas.width, 1, kMaxCanvasSize), clampTo(canvas.height, 1, kMaxCanvasSize)};
}

void clampOverlays(OverlaySettings &settings)
{
	settings.canvas = clampCanvas(settings.canvas);
	if (settings.items.size() > kMaxOverlays) {
		settings.items.resize(kMaxOverlays);
	}
	for (OverlayItem &item : settings.items) {
		clampItem(item);
	}
}

int32_t safeMarginX(CanvasSize canvas) noexcept
{
	return static_cast<int32_t>(roundDiv(int64_t{clampCanvas(canvas).width} * kSafeMarginPercent, 100));
}

int32_t safeMarginY(CanvasSize canvas) noexcept
{
	return static_cast<int32_t>(roundDiv(int64_t{clampCanvas(canvas).height} * kSafeMarginPercent, 100));
}

OverlayBox boxOf(const OverlayItem &item, CanvasSize canvas) noexcept
{
	canvas = clampCanvas(canvas);
	const int32_t width = clampSize(item.width);
	const int32_t height = clampSize(item.height);
	return {static_cast<int32_t>(startFrom(columnOf(item.anchor), clampDistance(item.x), width, canvas.width)),
		static_cast<int32_t>(startFrom(rowOf(item.anchor), clampDistance(item.y), height, canvas.height)),
		width, height};
}

void placeBox(OverlayItem &item, const OverlayBox &box, CanvasSize canvas) noexcept
{
	canvas = clampCanvas(canvas);
	item.width = clampSize(box.width);
	item.height = clampSize(box.height);
	item.x = clampDistance(distanceFrom(columnOf(item.anchor), box.left, item.width, canvas.width));
	item.y = clampDistance(distanceFrom(rowOf(item.anchor), box.top, item.height, canvas.height));
}

void setAnchor(OverlayItem &item, OverlayAnchor anchor, CanvasSize canvas) noexcept
{
	const OverlayBox box = boxOf(item, canvas);
	item.anchor = anchor;
	placeBox(item, box, canvas);
}

void applyPreset(OverlayItem &item, OverlayPreset preset, CanvasSize canvas) noexcept
{
	canvas = clampCanvas(canvas);
	item.width = clampSize(item.width);
	item.height = clampSize(item.height);
	item.x = safeMarginX(canvas);
	item.y = safeMarginY(canvas);
	switch (preset) {
	case OverlayPreset::TopLeft:
		item.anchor = OverlayAnchor::TopLeft;
		break;
	case OverlayPreset::TopRight:
		item.anchor = OverlayAnchor::TopRight;
		break;
	case OverlayPreset::BottomLeft:
		item.anchor = OverlayAnchor::BottomLeft;
		break;
	case OverlayPreset::BottomRight:
		item.anchor = OverlayAnchor::BottomRight;
		break;
	case OverlayPreset::LowerThird:
		item.anchor = OverlayAnchor::Bottom;
		item.x = 0;
		item.width = clampSize(canvas.width - int64_t{2} * safeMarginX(canvas));
		item.height = clampSize(roundDiv(canvas.height, 3));
		break;
	case OverlayPreset::FullScreen:
		item.anchor = OverlayAnchor::TopLeft;
		item.x = 0;
		item.y = 0;
		item.width = canvas.width;
		item.height = canvas.height;
		break;
	}
}

std::optional<OverlayPreset> matchingPreset(const OverlayItem &item, CanvasSize canvas) noexcept
{
	// Full screen first: on a canvas too small for a margin it is also the top left.
	static constexpr std::array<OverlayPreset, 6> kOrder = {
		OverlayPreset::FullScreen, OverlayPreset::LowerThird, OverlayPreset::TopLeft,
		OverlayPreset::TopRight,   OverlayPreset::BottomLeft, OverlayPreset::BottomRight,
	};
	for (const OverlayPreset preset : kOrder) {
		OverlayItem placed;
		placed.width = item.width;
		placed.height = item.height;
		applyPreset(placed, preset, canvas);
		if (placed.anchor == item.anchor && placed.x == item.x && placed.y == item.y &&
		    placed.width == item.width && placed.height == item.height) {
			return preset;
		}
	}
	return std::nullopt;
}

void rescaleOverlays(OverlaySettings &settings, CanvasSize canvas)
{
	const CanvasSize from = clampCanvas(settings.canvas);
	const CanvasSize to = clampCanvas(canvas);
	settings.canvas = to;
	if (from == to) {
		return;
	}
	for (OverlayItem &item : settings.items) {
		item.x = clampDistance(scaleTo(item.x, to.width, from.width));
		item.width = clampSize(scaleTo(item.width, to.width, from.width));
		item.y = clampDistance(scaleTo(item.y, to.height, from.height));
		item.height = clampSize(scaleTo(item.height, to.height, from.height));
	}
}

OverlayItem newOverlay(std::string uuid, std::string name, int64_t sourceWidth, int64_t sourceHeight, CanvasSize canvas)
{
	canvas = clampCanvas(canvas);
	OverlayItem item;
	item.uuid = std::move(uuid);
	item.name = std::move(name);
	item.anchor = OverlayAnchor::Center;
	const int64_t width = std::clamp<int64_t>(sourceWidth, 0, std::numeric_limits<int32_t>::max());
	const int64_t height = std::clamp<int64_t>(sourceHeight, 0, std::numeric_limits<int32_t>::max());
	if (width == 0 || height == 0) {
		item.width = clampSize(roundDiv(canvas.width, 4));
		item.height = clampSize(roundDiv(canvas.height, 4));
	} else if (width <= canvas.width && height <= canvas.height) {
		item.width = static_cast<int32_t>(width);
		item.height = static_cast<int32_t>(height);
	} else if (width * canvas.height >= height * canvas.width) {
		item.width = canvas.width;
		item.height = clampSize(roundDiv(height * canvas.width, width));
	} else {
		item.width = clampSize(roundDiv(width * canvas.height, height));
		item.height = canvas.height;
	}
	return item;
}

OverlayLayout::OverlayLayout(OverlaySettings settings) : settings_(std::move(settings))
{
	clampOverlays(settings_);
}

void OverlayLayout::setMode(OverlayMode mode)
{
	endDrag();
	settings_.mode = mode;
}

void OverlayLayout::setScene(OverlayScene scene)
{
	endDrag();
	settings_.scene = std::move(scene);
}

void OverlayLayout::setCanvas(CanvasSize canvas)
{
	endDrag();
	rescaleOverlays(settings_, canvas);
}

void OverlayLayout::select(std::optional<size_t> index)
{
	endDrag();
	if (!index || *index < settings_.items.size()) {
		selected_ = index;
	}
}

bool OverlayLayout::add(OverlayItem item)
{
	endDrag();
	if (settings_.items.size() >= kMaxOverlays) {
		return false;
	}
	clampItem(item);
	settings_.items.push_back(std::move(item));
	selected_ = settings_.items.size() - 1;
	return true;
}

void OverlayLayout::remove(size_t index)
{
	endDrag();
	std::vector<OverlayItem> &items = settings_.items;
	if (index >= items.size()) {
		return;
	}
	items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
	if (selected_ == index) {
		selected_.reset();
	} else if (selected_ && *selected_ > index) {
		--*selected_;
	}
}

void OverlayLayout::moveItem(size_t from, size_t to)
{
	endDrag();
	std::vector<OverlayItem> &items = settings_.items;
	if (from >= items.size() || to >= items.size() || from == to) {
		return;
	}
	const auto at = [&items](size_t index) {
		return items.begin() + static_cast<std::ptrdiff_t>(index);
	};
	if (from < to) {
		std::rotate(at(from), at(from + 1), at(to + 1));
	} else {
		std::rotate(at(to), at(from), at(from + 1));
	}
	if (!selected_) {
		return;
	}
	if (*selected_ == from) {
		selected_ = to;
	} else if (from < *selected_ && *selected_ <= to) {
		--*selected_;
	} else if (to <= *selected_ && *selected_ < from) {
		++*selected_;
	}
}

void OverlayLayout::setItem(size_t index, OverlayItem item)
{
	endDrag();
	if (OverlayItem *target = itemFor(index)) {
		clampItem(item);
		*target = std::move(item);
	}
}

void OverlayLayout::setVisible(size_t index, bool visible)
{
	endDrag();
	if (OverlayItem *target = itemFor(index)) {
		target->visible = visible;
	}
}

void OverlayLayout::setBox(size_t index, const OverlayBox &box)
{
	endDrag();
	if (OverlayItem *target = itemFor(index)) {
		placeBox(*target, box, settings_.canvas);
	}
}

void OverlayLayout::setAnchor(size_t index, OverlayAnchor anchor)
{
	endDrag();
	if (OverlayItem *target = itemFor(index)) {
		tapeloop::setAnchor(*target, anchor, settings_.canvas);
	}
}

void OverlayLayout::applyPreset(size_t index, OverlayPreset preset)
{
	endDrag();
	if (OverlayItem *target = itemFor(index)) {
		tapeloop::applyPreset(*target, preset, settings_.canvas);
	}
}

void OverlayLayout::nudge(int32_t dx, int32_t dy)
{
	endDrag();
	if (!selected_) {
		return;
	}
	OverlayItem &item = settings_.items[*selected_];
	OverlayBox box = boxOf(item, settings_.canvas);
	box.left = clampCoordinate(int64_t{box.left} + dx);
	box.top = clampCoordinate(int64_t{box.top} + dy);
	placeBox(item, box, settings_.canvas);
}

std::optional<size_t> OverlayLayout::itemAt(CanvasPoint point) const noexcept
{
	const std::vector<OverlayItem> &items = settings_.items;
	for (size_t index = items.size(); index-- > 0;) {
		if (items[index].visible && boxOf(items[index], settings_.canvas).contains(point)) {
			return index;
		}
	}
	return std::nullopt;
}

std::optional<ResizeHandle> OverlayLayout::handleAt(CanvasPoint point, int32_t tolerance) const noexcept
{
	if (!selected_ || !settings_.items[*selected_].visible || tolerance < 0) {
		return std::nullopt;
	}
	const OverlayBox box = boxOf(settings_.items[*selected_], settings_.canvas);
	const std::array<int64_t, 3> columns = {box.left, box.left + box.width / 2, box.right()};
	const std::array<int64_t, 3> rows = {box.top, box.top + box.height / 2, box.bottom()};
	struct Place {
		ResizeHandle handle;
		size_t column;
		size_t row;
	};
	static constexpr std::array<Place, 8> kPlaces = {{
		{ResizeHandle::TopLeft, 0, 0},
		{ResizeHandle::Top, 1, 0},
		{ResizeHandle::TopRight, 2, 0},
		{ResizeHandle::Left, 0, 1},
		{ResizeHandle::Right, 2, 1},
		{ResizeHandle::BottomLeft, 0, 2},
		{ResizeHandle::Bottom, 1, 2},
		{ResizeHandle::BottomRight, 2, 2},
	}};
	std::optional<ResizeHandle> nearest;
	int64_t nearestDistance = int64_t{tolerance} + 1;
	for (const Place &place : kPlaces) {
		const int64_t distance =
			std::max(std::abs(point.x - columns[place.column]), std::abs(point.y - rows[place.row]));
		if (distance < nearestDistance) {
			nearest = place.handle;
			nearestDistance = distance;
		}
	}
	return nearest;
}

bool OverlayLayout::beginMove(CanvasPoint point)
{
	return beginDrag(std::nullopt, point);
}

bool OverlayLayout::beginResize(ResizeHandle handle, CanvasPoint point)
{
	return beginDrag(handle, point);
}

bool OverlayLayout::beginDrag(std::optional<ResizeHandle> handle, CanvasPoint point)
{
	endDrag();
	if (!selected_ || !settings_.items[*selected_].visible) {
		return false;
	}
	drag_ = Drag{*selected_, handle, point, settings_.items[*selected_]};
	return true;
}

void OverlayLayout::dragTo(CanvasPoint point, const DragOptions &options)
{
	if (!drag_) {
		return;
	}
	const CanvasSize canvas = settings_.canvas;
	const OverlayBox from = boxOf(drag_->start, canvas);
	const int64_t dx = int64_t{point.x} - drag_->from.x;
	const int64_t dy = int64_t{point.y} - drag_->from.y;
	OverlayBox box = from;
	if (drag_->handle) {
		box = resizeBox(from, *drag_->handle, dx, dy, canvas, options);
	} else {
		const int64_t distance = std::clamp<int64_t>(options.snapDistance, 0, kMaxOverlayExtent);
		box.left = clampCoordinate(
			snapMove(from.left + dx, from.width, canvas.width, safeMarginX(canvas), distance));
		box.top = clampCoordinate(
			snapMove(from.top + dy, from.height, canvas.height, safeMarginY(canvas), distance));
	}
	OverlayItem item = drag_->start;
	placeBox(item, box, canvas);
	settings_.items[drag_->index] = std::move(item);
}

void OverlayLayout::endDrag() noexcept
{
	drag_.reset();
}

void OverlayLayout::cancelDrag()
{
	if (drag_) {
		settings_.items[drag_->index] = drag_->start;
		drag_.reset();
	}
}

OverlayItem *OverlayLayout::itemFor(size_t index)
{
	return index < settings_.items.size() ? &settings_.items[index] : nullptr;
}

std::string_view overlayAnchorName(OverlayAnchor anchor) noexcept
{
	return nameOf(kAnchorNames, static_cast<size_t>(anchor));
}

std::optional<OverlayAnchor> overlayAnchorFrom(std::string_view name) noexcept
{
	return enumFrom<OverlayAnchor>(kAnchorNames, name);
}

std::string_view overlayFitName(OverlayFit fit) noexcept
{
	return nameOf(kFitNames, static_cast<size_t>(fit));
}

std::optional<OverlayFit> overlayFitFrom(std::string_view name) noexcept
{
	return enumFrom<OverlayFit>(kFitNames, name);
}

std::string_view overlayBlendName(OverlayBlend blend) noexcept
{
	return nameOf(kBlendNames, static_cast<size_t>(blend));
}

std::optional<OverlayBlend> overlayBlendFrom(std::string_view name) noexcept
{
	return enumFrom<OverlayBlend>(kBlendNames, name);
}

SavedOverlays saveOverlays(const OverlaySettings &settings)
{
	SavedOverlays saved;
	saved.mode = settings.mode == OverlayMode::Scene ? "scene" : "simple";
	saved.sceneUuid = settings.scene.uuid;
	saved.sceneName = settings.scene.name;
	saved.canvasWidth = settings.canvas.width;
	saved.canvasHeight = settings.canvas.height;
	saved.items.reserve(settings.items.size());
	for (const OverlayItem &item : settings.items) {
		SavedOverlay &out = saved.items.emplace_back();
		out.uuid = item.uuid;
		out.name = item.name;
		out.visible = item.visible;
		out.anchor = overlayAnchorName(item.anchor);
		out.x = item.x;
		out.y = item.y;
		out.width = item.width;
		out.height = item.height;
		out.fit = overlayFitName(item.fit);
		out.opacity = item.opacity;
		out.blend = overlayBlendName(item.blend);
		out.crop = {static_cast<double>(item.crop.left), static_cast<double>(item.crop.top),
			    static_cast<double>(item.crop.right), static_cast<double>(item.crop.bottom)};
	}
	return saved;
}

OverlaySettings loadOverlays(const SavedOverlays &saved, CanvasSize canvas)
{
	OverlaySettings settings;
	settings.mode = saved.mode == "scene" ? OverlayMode::Scene : OverlayMode::Simple;
	settings.scene = {saved.sceneUuid, saved.sceneName};
	settings.canvas = clampCanvas(canvas);
	const auto validSide = [](double side) {
		return side >= 1 && side <= kMaxCanvasSize;
	};
	if (validSide(saved.canvasWidth) && validSide(saved.canvasHeight)) {
		settings.canvas = {savedNumber(saved.canvasWidth, 1, 1, kMaxCanvasSize),
				   savedNumber(saved.canvasHeight, 1, 1, kMaxCanvasSize)};
	}
	for (const SavedOverlay &in : saved.items) {
		if (settings.items.size() == kMaxOverlays) {
			break;
		}
		if (in.uuid.empty() && in.name.empty()) {
			continue;
		}
		OverlayItem &item = settings.items.emplace_back();
		item.uuid = in.uuid;
		item.name = in.name;
		item.visible = in.visible;
		item.anchor = overlayAnchorFrom(in.anchor).value_or(OverlayAnchor::TopLeft);
		item.x = savedNumber(in.x, 0, -kMaxOverlayExtent, kMaxOverlayExtent);
		item.y = savedNumber(in.y, 0, -kMaxOverlayExtent, kMaxOverlayExtent);
		item.width = savedNumber(in.width, 0, kMinOverlaySize, kMaxOverlayExtent);
		item.height = savedNumber(in.height, 0, kMinOverlaySize, kMaxOverlayExtent);
		item.fit = overlayFitFrom(in.fit).value_or(OverlayFit::Inner);
		item.opacity = savedNumber(in.opacity, 100, 0, 100);
		item.blend = overlayBlendFrom(in.blend).value_or(OverlayBlend::Normal);
		item.crop = {savedNumber(in.crop.left, 0, 0, kMaxOverlayExtent),
			     savedNumber(in.crop.top, 0, 0, kMaxOverlayExtent),
			     savedNumber(in.crop.right, 0, 0, kMaxOverlayExtent),
			     savedNumber(in.crop.bottom, 0, 0, kMaxOverlayExtent)};
	}
	rescaleOverlays(settings, canvas);
	return settings;
}

} // namespace tapeloop
