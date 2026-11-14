// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/OverlayLayout.hpp"

#include "FuzzInput.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

using tapeloop::boxOf;
using tapeloop::CanvasPoint;
using tapeloop::CanvasSize;
using tapeloop::DragOptions;
using tapeloop::kMaxCanvasSize;
using tapeloop::kMaxOverlayExtent;
using tapeloop::kMaxOverlays;
using tapeloop::kMinOverlaySize;
using tapeloop::OverlayAnchor;
using tapeloop::OverlayBox;
using tapeloop::OverlayItem;
using tapeloop::OverlayLayout;
using tapeloop::OverlayMode;
using tapeloop::OverlayPreset;
using tapeloop::OverlaySettings;
using tapeloop::ResizeHandle;
using tapeloop::SavedOverlay;
using tapeloop::SavedOverlays;
using tapeloop::fuzz::FuzzInput;
using tapeloop::fuzz::require;

namespace {

// Every name the model writes, a few it does not, and the empty one.
constexpr std::array<std::string_view, 16> kNames = {
	"top_left", "center", "bottom_right", "bottom", "inner", "stretch", "fill", "normal",
	"screen",   "darken", "scene",        "simple", "Top",   "middle",  "",     "zoom",
};

std::string readName(FuzzInput &input)
{
	if (input.byte() % 4 != 0) {
		return std::string(kNames[input.byte() % kNames.size()]);
	}
	const auto taken = input.bytes(input.byte() % 16);
	return {taken.begin(), taken.end()};
}

// A number as a file can hold it: mostly a plain one near the ranges, sometimes any
// double, NaN and the infinities included.
double readNumber(FuzzInput &input)
{
	if (input.byte() % 4 == 0) {
		return std::bit_cast<double>(input.u64());
	}
	return static_cast<double>(input.i32() % 70000) + static_cast<double>(input.byte()) / 256.0;
}

SavedOverlays readSaved(FuzzInput &input)
{
	SavedOverlays saved;
	saved.mode = readName(input);
	saved.sceneUuid = readName(input);
	saved.sceneName = readName(input);
	saved.canvasWidth = readNumber(input);
	saved.canvasHeight = readNumber(input);
	saved.items.resize(input.below(kMaxOverlays + 8));
	for (SavedOverlay &item : saved.items) {
		item.uuid = readName(input);
		item.name = readName(input);
		item.visible = input.flag();
		item.anchor = readName(input);
		item.x = readNumber(input);
		item.y = readNumber(input);
		item.width = readNumber(input);
		item.height = readNumber(input);
		item.fit = readName(input);
		item.opacity = readNumber(input);
		item.blend = readName(input);
		item.crop = {readNumber(input), readNumber(input), readNumber(input), readNumber(input)};
	}
	return saved;
}

CanvasSize readCanvas(FuzzInput &input)
{
	if (input.flag()) {
		return {input.i32(), input.i32()};
	}
	return {static_cast<int32_t>(input.below(kMaxCanvasSize + 2)),
		static_cast<int32_t>(input.below(kMaxCanvasSize + 2))};
}

// Mostly on or near the canvas, sometimes anywhere.
CanvasPoint readPoint(FuzzInput &input)
{
	if (input.byte() % 8 == 0) {
		return {input.i32(), input.i32()};
	}
	return {static_cast<int32_t>(input.below(5000)) - 1000, static_cast<int32_t>(input.below(3000)) - 1000};
}

bool inRange(int32_t value, int32_t low, int32_t high)
{
	return value >= low && value <= high;
}

void checkSettings(const OverlaySettings &settings)
{
	require(inRange(settings.canvas.width, 1, kMaxCanvasSize));
	require(inRange(settings.canvas.height, 1, kMaxCanvasSize));
	require(settings.items.size() <= kMaxOverlays);
	for (const OverlayItem &item : settings.items) {
		require(inRange(item.x, -kMaxOverlayExtent, kMaxOverlayExtent));
		require(inRange(item.y, -kMaxOverlayExtent, kMaxOverlayExtent));
		require(inRange(item.width, kMinOverlaySize, kMaxOverlayExtent));
		require(inRange(item.height, kMinOverlaySize, kMaxOverlayExtent));
		require(inRange(item.opacity, 0, 100));
		require(inRange(item.crop.left, 0, kMaxOverlayExtent));
		require(inRange(item.crop.top, 0, kMaxOverlayExtent));
		require(inRange(item.crop.right, 0, kMaxOverlayExtent));
		require(inRange(item.crop.bottom, 0, kMaxOverlayExtent));
	}
	// What is saved loads back unchanged.
	require(tapeloop::loadOverlays(tapeloop::saveOverlays(settings), settings.canvas) == settings);
}

void checkLayout(const OverlayLayout &layout)
{
	checkSettings(layout.settings());
	const auto &items = layout.items();
	require(!layout.selected() || *layout.selected() < items.size());
	require(!layout.dragging() || layout.selected());
}

void checkHit(const OverlayLayout &layout, CanvasPoint point)
{
	const auto &items = layout.items();
	const std::optional<size_t> hit = layout.itemAt(point);
	const size_t above = hit ? *hit + 1 : 0;
	if (hit) {
		require(items[*hit].visible && boxOf(items[*hit], layout.canvas()).contains(point));
	}
	for (size_t index = above; index < items.size(); ++index) {
		require(!items[index].visible || !boxOf(items[index], layout.canvas()).contains(point));
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	FuzzInput input(data, size);

	const CanvasSize canvas = readCanvas(input);
	const OverlaySettings loaded = tapeloop::loadOverlays(readSaved(input), canvas);
	require(loaded.canvas == tapeloop::clampCanvas(canvas));
	checkSettings(loaded);

	OverlayLayout layout(loaded);
	require(layout.settings() == loaded);
	// The item as the current drag began, and whether that drag is a move.
	std::optional<OverlayItem> start;
	bool moving = false;
	while (!input.empty()) {
		switch (input.byte() % 16) {
		case 0:
			layout.select(input.flag() ? std::nullopt
						   : std::optional<size_t>(input.below(kMaxOverlays + 2)));
			break;
		case 1: {
			const size_t before = layout.items().size();
			// The round trip of checkSettings holds for items with a UUID or a name, as
			// every item from a source has.
			OverlayItem added = tapeloop::newOverlay("u" + readName(input), readName(input), input.i64(),
								 input.i64(), layout.canvas());
			added.opacity = input.i32();
			const bool fits = before < kMaxOverlays;
			require(layout.add(added) == fits);
			require(layout.items().size() == before + (fits ? 1 : 0));
			if (fits) {
				require(layout.selected() == before);
			}
			break;
		}
		case 2:
			layout.remove(input.below(kMaxOverlays + 2));
			break;
		case 3:
			layout.moveItem(input.below(kMaxOverlays + 2), input.below(kMaxOverlays + 2));
			break;
		case 4: {
			OverlayItem item;
			item.uuid = "u" + readName(input);
			item.anchor = static_cast<OverlayAnchor>(input.below(9));
			item.x = input.i32();
			item.y = input.i32();
			item.width = input.i32();
			item.height = input.i32();
			item.opacity = input.i32();
			item.crop = {input.i32(), input.i32(), input.i32(), input.i32()};
			layout.setItem(input.below(kMaxOverlays + 2), item);
			break;
		}
		case 5:
			layout.setVisible(input.below(kMaxOverlays + 2), input.flag());
			break;
		case 6:
			layout.setBox(input.below(kMaxOverlays + 2),
				      {input.i32(), input.i32(), input.i32(), input.i32()});
			break;
		case 7:
			layout.setAnchor(input.below(kMaxOverlays + 2), static_cast<OverlayAnchor>(input.below(9)));
			break;
		case 8: {
			const size_t index = input.below(kMaxOverlays + 2);
			const auto preset = static_cast<OverlayPreset>(input.below(6));
			layout.applyPreset(index, preset);
			if (index < layout.items().size()) {
				require(tapeloop::matchingPreset(layout.items()[index], layout.canvas()).has_value());
			}
			break;
		}
		case 9:
			if (input.flag()) {
				layout.nudge(static_cast<int32_t>(input.below(21)) - 10,
					     static_cast<int32_t>(input.below(21)) - 10);
			} else {
				layout.nudge(input.i32(), input.i32());
			}
			break;
		case 10:
			layout.setCanvas(readCanvas(input));
			break;
		case 11:
			layout.setMode(input.flag() ? OverlayMode::Scene : OverlayMode::Simple);
			layout.setScene({readName(input), readName(input)});
			break;
		case 12: {
			const CanvasPoint point = readPoint(input);
			checkHit(layout, point);
			const std::optional<ResizeHandle> handle = layout.handleAt(point, input.i32() % 64);
			require(!handle || (layout.selected() && layout.items()[*layout.selected()].visible));
			break;
		}
		case 13: {
			const CanvasPoint point = readPoint(input);
			moving = input.flag();
			const bool began =
				moving ? layout.beginMove(point)
				       : layout.beginResize(static_cast<ResizeHandle>(input.below(8)), point);
			require(began == (layout.selected() && layout.items()[*layout.selected()].visible));
			require(layout.dragging() == began);
			start = began ? std::optional<OverlayItem>(layout.items()[*layout.selected()]) : std::nullopt;
			break;
		}
		case 14: {
			const DragOptions options{input.flag() ? input.i32() : static_cast<int32_t>(input.below(16)),
						  input.flag()};
			layout.dragTo(readPoint(input), options);
			if (layout.dragging() && moving) {
				const OverlayItem &item = layout.items()[*layout.selected()];
				require(item.width == start->width && item.height == start->height);
			}
			break;
		}
		case 15:
			if (input.flag()) {
				const bool wasDragging = layout.dragging();
				layout.cancelDrag();
				if (wasDragging) {
					require(layout.items()[*layout.selected()] == *start);
				}
			} else {
				layout.endDrag();
			}
			require(!layout.dragging());
			break;
		default:
			break;
		}
		if (!layout.dragging()) {
			start.reset();
		}
		checkLayout(layout);
	}
	return 0;
}
