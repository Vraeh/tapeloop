// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/OverlayLayout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using tapeloop::boxOf;
using tapeloop::CanvasPoint;
using tapeloop::CanvasSize;
using tapeloop::DragOptions;
using tapeloop::kMaxOverlayExtent;
using tapeloop::kMaxOverlays;
using tapeloop::loadOverlays;
using tapeloop::matchingPreset;
using tapeloop::newOverlay;
using tapeloop::OverlayAnchor;
using tapeloop::OverlayBlend;
using tapeloop::OverlayBox;
using tapeloop::OverlayFit;
using tapeloop::OverlayItem;
using tapeloop::OverlayLayout;
using tapeloop::OverlayMode;
using tapeloop::OverlayPreset;
using tapeloop::OverlayScene;
using tapeloop::OverlaySettings;
using tapeloop::placeBox;
using tapeloop::ResizeHandle;
using tapeloop::SavedOverlay;
using tapeloop::SavedOverlays;
using tapeloop::saveOverlays;

namespace {

constexpr CanvasSize kHd{1920, 1080};
constexpr std::array<OverlayAnchor, 9> kAnchors = {
	OverlayAnchor::TopLeft,    OverlayAnchor::Top,    OverlayAnchor::TopRight,
	OverlayAnchor::Left,       OverlayAnchor::Center, OverlayAnchor::Right,
	OverlayAnchor::BottomLeft, OverlayAnchor::Bottom, OverlayAnchor::BottomRight,
};

OverlayItem item(std::string uuid, OverlayAnchor anchor, int32_t x, int32_t y, int32_t width, int32_t height)
{
	OverlayItem made;
	made.uuid = std::move(uuid);
	made.name = made.uuid;
	made.anchor = anchor;
	made.x = x;
	made.y = y;
	made.width = width;
	made.height = height;
	return made;
}

// One item at the top left of a 1920x1080 canvas, selected.
OverlayLayout layoutWith(const OverlayBox &box)
{
	OverlaySettings settings;
	settings.items.push_back(item("a", OverlayAnchor::TopLeft, box.left, box.top, box.width, box.height));
	OverlayLayout layout(settings);
	layout.select(0);
	return layout;
}

OverlayBox selectedBox(const OverlayLayout &layout)
{
	return boxOf(layout.items().at(layout.selected().value()), layout.canvas());
}

} // namespace

TEST_CASE("an anchor and its distances give the box")
{
	CHECK(boxOf(item("a", OverlayAnchor::BottomRight, 40, 40, 480, 270), kHd) == OverlayBox{1400, 770, 480, 270});
	CHECK(boxOf(item("a", OverlayAnchor::TopLeft, 40, 30, 480, 270), kHd) == OverlayBox{40, 30, 480, 270});
	CHECK(boxOf(item("a", OverlayAnchor::Center, 0, 0, 480, 270), kHd) == OverlayBox{720, 405, 480, 270});
	CHECK(boxOf(item("a", OverlayAnchor::Center, 10, -10, 481, 271), kHd) == OverlayBox{729, 394, 481, 271});
	CHECK(boxOf(item("a", OverlayAnchor::Top, 0, 20, 480, 270), kHd) == OverlayBox{720, 20, 480, 270});
	CHECK(boxOf(item("a", OverlayAnchor::Right, 20, 0, 480, 270), kHd) == OverlayBox{1420, 405, 480, 270});
	CHECK(boxOf(item("a", OverlayAnchor::Bottom, 0, 0, 2000, 1200), kHd) == OverlayBox{-40, -120, 2000, 1200});
	CHECK(boxOf(item("a", OverlayAnchor::Center, 0, 0, 3001, 1201), kHd) == OverlayBox{-541, -61, 3001, 1201});
}

TEST_CASE("a box converts back to the same distances from every anchor")
{
	for (const OverlayAnchor anchor : kAnchors) {
		for (const OverlayBox box :
		     {OverlayBox{0, 0, 1, 1}, OverlayBox{-300, 900, 641, 359}, OverlayBox{1700, -20, 4000, 3}}) {
			OverlayItem placed = item("a", anchor, 0, 0, 1, 1);
			placeBox(placed, box, kHd);
			CHECK(boxOf(placed, kHd) == box);
		}
	}
}

TEST_CASE("a box is brought within the ranges when placed")
{
	OverlayItem placed = item("a", OverlayAnchor::TopLeft, 0, 0, 1, 1);
	placeBox(placed, {-100000, 100000, 0, 99999}, kHd);
	CHECK(placed.x == -kMaxOverlayExtent);
	CHECK(placed.y == kMaxOverlayExtent);
	CHECK(placed.width == 1);
	CHECK(placed.height == kMaxOverlayExtent);
}

TEST_CASE("changing the anchor keeps the box in place")
{
	for (const OverlayAnchor anchor : kAnchors) {
		OverlayItem moved = item("a", OverlayAnchor::BottomRight, 40, 40, 480, 270);
		tapeloop::setAnchor(moved, anchor, kHd);
		CHECK(moved.anchor == anchor);
		CHECK(boxOf(moved, kHd) == OverlayBox{1400, 770, 480, 270});
	}
	OverlayItem moved = item("a", OverlayAnchor::BottomRight, 40, 40, 480, 270);
	tapeloop::setAnchor(moved, OverlayAnchor::TopLeft, kHd);
	CHECK(moved.x == 1400);
	CHECK(moved.y == 770);
}

TEST_CASE("the safe margins are 5% of each side")
{
	CHECK(tapeloop::safeMarginX(kHd) == 96);
	CHECK(tapeloop::safeMarginY(kHd) == 54);
	CHECK(tapeloop::safeMarginX({1280, 720}) == 64);
	CHECK(tapeloop::safeMarginY({1280, 720}) == 36);
	CHECK(tapeloop::safeMarginX({10, 9}) == 1);
	CHECK(tapeloop::safeMarginY({10, 9}) == 0);
	CHECK(tapeloop::safeMarginX({-5, 0}) == 0);
}

TEST_CASE("a canvas is brought within its range")
{
	CHECK(tapeloop::clampCanvas({0, -4}) == CanvasSize{1, 1});
	CHECK(tapeloop::clampCanvas({100000, 720}) == CanvasSize{16384, 720});
}

TEST_CASE("presets place the overlay")
{
	OverlayItem placed = item("a", OverlayAnchor::Center, 5, 5, 480, 270);

	tapeloop::applyPreset(placed, OverlayPreset::BottomRight, kHd);
	CHECK(placed.anchor == OverlayAnchor::BottomRight);
	CHECK(boxOf(placed, kHd) == OverlayBox{1344, 756, 480, 270});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::BottomRight);

	tapeloop::applyPreset(placed, OverlayPreset::TopLeft, kHd);
	CHECK(boxOf(placed, kHd) == OverlayBox{96, 54, 480, 270});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::TopLeft);

	tapeloop::applyPreset(placed, OverlayPreset::TopRight, kHd);
	CHECK(boxOf(placed, kHd) == OverlayBox{1344, 54, 480, 270});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::TopRight);

	tapeloop::applyPreset(placed, OverlayPreset::BottomLeft, kHd);
	CHECK(boxOf(placed, kHd) == OverlayBox{96, 756, 480, 270});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::BottomLeft);

	tapeloop::applyPreset(placed, OverlayPreset::LowerThird, kHd);
	CHECK(placed.anchor == OverlayAnchor::Bottom);
	CHECK(boxOf(placed, kHd) == OverlayBox{96, 666, 1728, 360});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::LowerThird);

	tapeloop::applyPreset(placed, OverlayPreset::FullScreen, kHd);
	CHECK(boxOf(placed, kHd) == OverlayBox{0, 0, 1920, 1080});
	CHECK(matchingPreset(placed, kHd) == OverlayPreset::FullScreen);

	placed.x = 3;
	CHECK_FALSE(matchingPreset(placed, kHd));
}

TEST_CASE("full screen wins on a canvas too small for a margin")
{
	OverlayItem placed;
	tapeloop::applyPreset(placed, OverlayPreset::FullScreen, {8, 8});
	CHECK(matchingPreset(placed, {8, 8}) == OverlayPreset::FullScreen);
}

TEST_CASE("another canvas scales distances and sizes but not the crop")
{
	OverlaySettings settings;
	OverlayItem scaled = item("a", OverlayAnchor::BottomRight, 96, 54, 480, 270);
	scaled.crop = {10, 20, 30, 40};
	settings.items.push_back(scaled);
	settings.items.push_back(item("b", OverlayAnchor::Center, -31, 1, 1, 1));

	tapeloop::rescaleOverlays(settings, {1280, 720});
	CHECK(settings.canvas == CanvasSize{1280, 720});
	CHECK(settings.items[0].x == 64);
	CHECK(settings.items[0].y == 36);
	CHECK(settings.items[0].width == 320);
	CHECK(settings.items[0].height == 180);
	CHECK(settings.items[0].crop == scaled.crop);
	CHECK(settings.items[1].x == -21);
	CHECK(settings.items[1].y == 1);
	CHECK(settings.items[1].width == 1);

	const OverlaySettings before = settings;
	tapeloop::rescaleOverlays(settings, {1280, 720});
	CHECK(settings == before);

	tapeloop::rescaleOverlays(settings, {16384, 16384});
	CHECK(settings.items[0].height == 4096);
}

TEST_CASE("a new overlay is centered at its source's size within the canvas")
{
	OverlayItem added = newOverlay("u", "Score", 400, 100, kHd);
	CHECK(added.uuid == "u");
	CHECK(added.name == "Score");
	CHECK(added.visible);
	CHECK(added.opacity == 100);
	CHECK(boxOf(added, kHd) == OverlayBox{760, 490, 400, 100});

	added = newOverlay("u", "Wide", 3840, 1000, kHd);
	CHECK(boxOf(added, kHd) == OverlayBox{0, 290, 1920, 500});

	added = newOverlay("u", "Tall", 1000, 2160, kHd);
	CHECK(boxOf(added, kHd) == OverlayBox{710, 0, 500, 1080});

	added = newOverlay("u", "Unknown", 0, 720, kHd);
	CHECK(boxOf(added, kHd) == OverlayBox{720, 405, 480, 270});

	added = newOverlay("u", "Huge", std::numeric_limits<int64_t>::max(), 1, kHd);
	CHECK(added.width == 1920);
	CHECK(added.height == 1);
}

TEST_CASE("the topmost visible overlay under a point is hit")
{
	OverlaySettings settings;
	settings.items.push_back(item("bottom", OverlayAnchor::TopLeft, 0, 0, 200, 200));
	settings.items.push_back(item("top", OverlayAnchor::TopLeft, 100, 100, 200, 200));
	settings.items.push_back(item("hidden", OverlayAnchor::TopLeft, 0, 0, 1920, 1080));
	settings.items[2].visible = false;
	const OverlayLayout layout(settings);

	CHECK(layout.itemAt({50, 50}) == 0U);
	CHECK(layout.itemAt({150, 150}) == 1U);
	CHECK(layout.itemAt({299, 299}) == 1U);
	CHECK_FALSE(layout.itemAt({300, 300}));
	CHECK(layout.itemAt({199, 99}) == 0U);
	CHECK_FALSE(layout.itemAt({200, 99}));
	CHECK_FALSE(layout.itemAt({-1, 0}));
}

TEST_CASE("the handle nearest the point within the tolerance is hit")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	CHECK(layout.handleAt({100, 100}, 6) == ResizeHandle::TopLeft);
	CHECK(layout.handleAt({205, 94}, 6) == ResizeHandle::Top);
	CHECK(layout.handleAt({300, 100}, 6) == ResizeHandle::TopRight);
	CHECK(layout.handleAt({96, 150}, 6) == ResizeHandle::Left);
	CHECK(layout.handleAt({304, 150}, 6) == ResizeHandle::Right);
	CHECK(layout.handleAt({100, 200}, 6) == ResizeHandle::BottomLeft);
	CHECK(layout.handleAt({200, 200}, 6) == ResizeHandle::Bottom);
	CHECK(layout.handleAt({306, 206}, 6) == ResizeHandle::BottomRight);
	CHECK_FALSE(layout.handleAt({307, 200}, 6));
	CHECK_FALSE(layout.handleAt({150, 150}, 6));
	CHECK_FALSE(layout.handleAt({100, 100}, -1));

	// Overlapping handles of a small box: the nearest wins.
	layout.setBox(0, {100, 100, 4, 4});
	CHECK(layout.handleAt({104, 104}, 6) == ResizeHandle::BottomRight);
	CHECK(layout.handleAt({102, 99}, 6) == ResizeHandle::Top);

	layout.setVisible(0, false);
	CHECK_FALSE(layout.handleAt({100, 100}, 6));
	layout.select(std::nullopt);
	CHECK_FALSE(layout.handleAt({100, 100}, 6));
}

TEST_CASE("a move follows the pointer and snaps within the distance")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	REQUIRE(layout.beginMove({150, 150}));
	CHECK(layout.dragging());

	layout.dragTo({160, 130}, {});
	CHECK(selectedBox(layout) == OverlayBox{110, 80, 200, 100});

	// Left edge to the canvas edge, top to the safe margin.
	layout.dragTo({57, 110}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{0, 54, 200, 100});

	// The center to the center lines.
	layout.dragTo({150 + 763, 150 + 388}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{860, 490, 200, 100});

	// Right edge to the right safe margin and bottom to the bottom edge.
	layout.dragTo({150 + 1521, 150 + 877}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{1624, 980, 200, 100});

	// Too far, or snapping off.
	layout.dragTo({150 + 12, 150 + 12}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{112, 112, 200, 100});
	layout.dragTo({52, 52}, {0, false});
	CHECK(selectedBox(layout) == OverlayBox{2, 2, 200, 100});

	// The nearest line wins.
	layout.dragTo({50 + 94, 50}, {8, false});
	CHECK(selectedBox(layout).left == 96);

	layout.endDrag();
	CHECK_FALSE(layout.dragging());
	CHECK(selectedBox(layout).left == 96);
}

TEST_CASE("a move keeps the size and stops at the range")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	REQUIRE(layout.beginMove({0, 0}));
	layout.dragTo({std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}, {8, false});
	const OverlayItem &moved = layout.items()[0];
	CHECK(moved.x == -kMaxOverlayExtent);
	CHECK(moved.y == kMaxOverlayExtent);
	CHECK(moved.width == 200);
	CHECK(moved.height == 100);
}

TEST_CASE("a resize moves the edges of its handle and keeps the others")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	REQUIRE(layout.beginResize(ResizeHandle::Right, {300, 150}));
	layout.dragTo({350, 400}, {});
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 250, 100});

	REQUIRE(layout.beginResize(ResizeHandle::TopLeft, {100, 100}));
	layout.dragTo({50, 120}, {});
	CHECK(selectedBox(layout) == OverlayBox{50, 120, 300, 80});

	// Past the opposite edge it stops at the smallest size.
	layout.dragTo({1000, 1000}, {});
	CHECK(selectedBox(layout) == OverlayBox{349, 199, 1, 1});

	REQUIRE(layout.beginResize(ResizeHandle::Bottom, {350, 200}));
	layout.dragTo({0, 300}, {});
	CHECK(selectedBox(layout) == OverlayBox{349, 199, 1, 101});

	REQUIRE(layout.beginResize(ResizeHandle::BottomLeft, {349, 300}));
	layout.dragTo({300, 310}, {});
	CHECK(selectedBox(layout) == OverlayBox{300, 199, 50, 111});

	REQUIRE(layout.beginResize(ResizeHandle::Top, {320, 199}));
	layout.dragTo({320, 189}, {});
	CHECK(selectedBox(layout) == OverlayBox{300, 189, 50, 121});

	REQUIRE(layout.beginResize(ResizeHandle::TopRight, {350, 189}));
	layout.dragTo({360, 179}, {});
	CHECK(selectedBox(layout) == OverlayBox{300, 179, 60, 131});

	REQUIRE(layout.beginResize(ResizeHandle::Left, {300, 200}));
	layout.dragTo({290, 0}, {});
	CHECK(selectedBox(layout) == OverlayBox{290, 179, 70, 131});
}

TEST_CASE("a resized edge snaps to the nearest line")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	REQUIRE(layout.beginResize(ResizeHandle::BottomRight, {300, 200}));
	layout.dragTo({956, 535}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 860, 440});

	layout.dragTo({1915, 1077}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 1820, 980});

	REQUIRE(layout.beginResize(ResizeHandle::TopLeft, {100, 100}));
	layout.dragTo({4, 50}, {8, false});
	CHECK(selectedBox(layout) == OverlayBox{0, 54, 1920, 1026});
}

TEST_CASE("a resize with the aspect kept follows the axis asking for the larger box")
{
	const DragOptions keep{0, true};
	OverlayLayout layout = layoutWith({100, 100, 160, 90});
	REQUIRE(layout.beginResize(ResizeHandle::BottomRight, {260, 190}));
	layout.dragTo({420, 200}, keep);
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 320, 180});

	layout.dragTo({270, 370}, keep);
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 480, 270});

	// The top left handle keeps the bottom right corner.
	REQUIRE(layout.beginResize(ResizeHandle::TopLeft, {100, 100}));
	layout.dragTo({340, 100}, keep);
	CHECK(selectedBox(layout) == OverlayBox{100, 100, 480, 270});
	layout.dragTo({340, 235}, keep);
	CHECK(selectedBox(layout) == OverlayBox{340, 235, 240, 135});

	// An edge handle sets its axis and the other follows from the top left.
	REQUIRE(layout.beginResize(ResizeHandle::Right, {580, 300}));
	layout.dragTo({820, 0}, keep);
	CHECK(selectedBox(layout) == OverlayBox{340, 235, 480, 270});

	REQUIRE(layout.beginResize(ResizeHandle::Top, {580, 235}));
	layout.dragTo({0, 370}, keep);
	CHECK(selectedBox(layout) == OverlayBox{340, 370, 240, 135});
}

TEST_CASE("a resize with the aspect kept snaps the nearer edge")
{
	OverlayLayout layout = layoutWith({0, 0, 300, 320});
	REQUIRE(layout.beginResize(ResizeHandle::BottomRight, {300, 320}));
	// 958 wide gives 1022 high: the right edge is 2 from the center line, the bottom 4
	// from the safe margin.
	layout.dragTo({958, 0}, {8, true});
	CHECK(selectedBox(layout) == OverlayBox{0, 0, 960, 1024});

	// 963 wide gives 1027 high, 1 from the margin.
	layout.dragTo({963, 0}, {8, true});
	CHECK(selectedBox(layout) == OverlayBox{0, 0, 962, 1026});

	// 500 wide gives 533 high, 7 from the center line.
	layout.dragTo({500, 0}, {8, true});
	CHECK(selectedBox(layout) == OverlayBox{0, 0, 506, 540});

	layout.dragTo({958, 0}, {0, true});
	CHECK(selectedBox(layout) == OverlayBox{0, 0, 958, 1022});
}

TEST_CASE("a resize with the aspect kept stops both sides at the range")
{
	OverlayLayout layout = layoutWith({0, 0, 400, 100});
	REQUIRE(layout.beginResize(ResizeHandle::Bottom, {200, 100}));
	layout.dragTo({200, 20000}, {0, true});
	CHECK(selectedBox(layout) == OverlayBox{0, 0, kMaxOverlayExtent, 8192});

	layout.cancelDrag();
	REQUIRE(layout.beginResize(ResizeHandle::Left, {0, 50}));
	layout.dragTo({1000, 50}, {0, true});
	CHECK(selectedBox(layout) == OverlayBox{399, 0, 1, 1});
}

TEST_CASE("a cancelled drag puts the item back")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	const OverlayItem before = layout.items()[0];
	REQUIRE(layout.beginResize(ResizeHandle::BottomRight, {300, 200}));
	layout.dragTo({600, 700}, {});
	CHECK(layout.items()[0] != before);
	layout.cancelDrag();
	CHECK_FALSE(layout.dragging());
	CHECK(layout.items()[0] == before);

	// Without a drag nothing happens.
	layout.cancelDrag();
	layout.dragTo({0, 0}, {});
	CHECK(layout.items()[0] == before);
}

TEST_CASE("any other change ends a drag and keeps where it got to")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	REQUIRE(layout.beginMove({0, 0}));
	layout.dragTo({10, 10}, {});
	layout.setVisible(0, true);
	CHECK_FALSE(layout.dragging());
	layout.cancelDrag();
	CHECK(selectedBox(layout) == OverlayBox{110, 110, 200, 100});
}

TEST_CASE("a drag needs a visible selected item")
{
	OverlaySettings settings;
	settings.items.push_back(item("a", OverlayAnchor::TopLeft, 0, 0, 10, 10));
	OverlayLayout layout(settings);
	CHECK_FALSE(layout.beginMove({0, 0}));
	layout.select(0);
	layout.setVisible(0, false);
	CHECK_FALSE(layout.beginResize(ResizeHandle::Left, {0, 0}));
	CHECK_FALSE(layout.dragging());
}

TEST_CASE("a nudge moves the selected item without snapping")
{
	OverlayLayout layout = layoutWith({100, 100, 200, 100});
	layout.nudge(1, 0);
	CHECK(selectedBox(layout) == OverlayBox{101, 100, 200, 100});
	layout.nudge(0, -10);
	CHECK(selectedBox(layout) == OverlayBox{101, 90, 200, 100});
	layout.nudge(-101, -90);
	CHECK(selectedBox(layout) == OverlayBox{0, 0, 200, 100});

	layout.nudge(std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min());
	CHECK(layout.items()[0].x == kMaxOverlayExtent);
	CHECK(layout.items()[0].y == -kMaxOverlayExtent);

	layout.select(std::nullopt);
	const OverlayItem before = layout.items()[0];
	layout.nudge(1, 1);
	CHECK(layout.items()[0] == before);
}

TEST_CASE("the list adds on top, removes and reorders with the selection")
{
	OverlayLayout layout(OverlaySettings{});
	CHECK(layout.add(item("a", OverlayAnchor::TopLeft, 0, 0, 10, 10)));
	CHECK(layout.add(item("b", OverlayAnchor::TopLeft, 0, 0, 10, 10)));
	CHECK(layout.add(item("c", OverlayAnchor::TopLeft, 0, 0, 10, 10)));
	CHECK(layout.add(item("d", OverlayAnchor::TopLeft, 0, 0, 10, 10)));
	CHECK(layout.selected() == 3U);

	const auto order = [&layout] {
		std::string uuids;
		for (const OverlayItem &each : layout.items()) {
			uuids += each.uuid;
		}
		return uuids;
	};

	layout.select(1);
	layout.moveItem(1, 3);
	CHECK(order() == "acdb");
	CHECK(layout.selected() == 3U);
	layout.moveItem(3, 0);
	CHECK(order() == "bacd");
	CHECK(layout.selected() == 0U);

	layout.select(2);
	layout.moveItem(0, 3);
	CHECK(order() == "acdb");
	CHECK(layout.selected() == 1U);
	layout.moveItem(3, 0);
	CHECK(order() == "bacd");
	CHECK(layout.selected() == 2U);
	layout.moveItem(0, 1);
	CHECK(layout.selected() == 2U);
	layout.moveItem(1, 1);
	layout.moveItem(1, 9);
	CHECK(order() == "abcd");

	layout.select(2);
	layout.remove(0);
	CHECK(order() == "bcd");
	CHECK(layout.selected() == 1U);
	layout.remove(2);
	CHECK(layout.selected() == 1U);
	layout.remove(1);
	CHECK(order() == "b");
	CHECK_FALSE(layout.selected());
	layout.remove(5);
	CHECK(order() == "b");

	layout.select(4);
	CHECK_FALSE(layout.selected());
	layout.moveItem(0, 0);
	layout.select(0);
	layout.moveItem(0, 0);
	CHECK(layout.selected() == 0U);
}

TEST_CASE("the list holds at most kMaxOverlays items")
{
	OverlaySettings settings;
	settings.items.resize(kMaxOverlays + 3, item("a", OverlayAnchor::TopLeft, 0, 0, 10, 10));
	OverlayLayout layout(settings);
	CHECK(layout.items().size() == kMaxOverlays);
	CHECK_FALSE(layout.add(item("b", OverlayAnchor::TopLeft, 0, 0, 10, 10)));
	CHECK(layout.items().size() == kMaxOverlays);
}

TEST_CASE("edits bring every value within its range")
{
	OverlaySettings settings;
	settings.canvas = {0, 99999};
	OverlayItem wild = item("a", OverlayAnchor::Center, -99999, 99999, -5, 99999);
	wild.opacity = 101;
	wild.crop = {-1, 99999, 0, 5};
	settings.items.push_back(wild);
	OverlayLayout layout(settings);
	CHECK(layout.canvas() == CanvasSize{1, 16384});
	const OverlayItem &kept = layout.items()[0];
	CHECK(kept.x == -kMaxOverlayExtent);
	CHECK(kept.y == kMaxOverlayExtent);
	CHECK(kept.width == 1);
	CHECK(kept.height == kMaxOverlayExtent);
	CHECK(kept.opacity == 100);
	CHECK(kept.crop == tapeloop::OverlayCrop{0, kMaxOverlayExtent, 0, 5});

	wild.opacity = -3;
	layout.setItem(0, wild);
	CHECK(layout.items()[0].opacity == 0);
	CHECK(layout.items()[0].width == 1);
	layout.setItem(1, wild);
	CHECK(layout.items().size() == 1);
}

TEST_CASE("the layout's own changes reach the settings")
{
	OverlaySettings settings;
	settings.items.push_back(item("a", OverlayAnchor::TopLeft, 96, 54, 480, 270));
	OverlayLayout layout(settings);

	layout.setMode(OverlayMode::Scene);
	layout.setScene({"s", "Replay scene"});
	CHECK(layout.settings().mode == OverlayMode::Scene);
	CHECK(layout.settings().scene == OverlayScene{"s", "Replay scene"});

	layout.setAnchor(0, OverlayAnchor::BottomRight);
	CHECK(boxOf(layout.items()[0], kHd) == OverlayBox{96, 54, 480, 270});
	layout.applyPreset(0, OverlayPreset::FullScreen);
	CHECK(boxOf(layout.items()[0], kHd) == OverlayBox{0, 0, 1920, 1080});
	layout.setBox(0, {10, 20, 30, 40});
	CHECK(boxOf(layout.items()[0], kHd) == OverlayBox{10, 20, 30, 40});

	layout.setCanvas({960, 540});
	CHECK(layout.canvas() == CanvasSize{960, 540});
	CHECK(boxOf(layout.items()[0], layout.canvas()) == OverlayBox{5, 10, 15, 20});

	layout.setAnchor(3, OverlayAnchor::Top);
	layout.applyPreset(3, OverlayPreset::TopLeft);
	layout.setBox(3, {});
	layout.setVisible(3, false);
	CHECK(layout.items().size() == 1);
}

TEST_CASE("every name reads back as what it names")
{
	for (const OverlayAnchor anchor : kAnchors) {
		CHECK(tapeloop::overlayAnchorFrom(tapeloop::overlayAnchorName(anchor)) == anchor);
	}
	for (const OverlayFit fit : {OverlayFit::Inner, OverlayFit::Stretch, OverlayFit::Fill}) {
		CHECK(tapeloop::overlayFitFrom(tapeloop::overlayFitName(fit)) == fit);
	}
	for (const OverlayBlend blend :
	     {OverlayBlend::Normal, OverlayBlend::Additive, OverlayBlend::Subtract, OverlayBlend::Screen,
	      OverlayBlend::Multiply, OverlayBlend::Lighten, OverlayBlend::Darken}) {
		CHECK(tapeloop::overlayBlendFrom(tapeloop::overlayBlendName(blend)) == blend);
	}
	CHECK(tapeloop::overlayAnchorName(OverlayAnchor::BottomRight) == "bottom_right");
	CHECK_FALSE(tapeloop::overlayAnchorFrom("middle"));
	CHECK_FALSE(tapeloop::overlayFitFrom(""));
	CHECK_FALSE(tapeloop::overlayBlendFrom("Normal"));
}

TEST_CASE("saved overlays load back as they were")
{
	OverlaySettings settings;
	settings.mode = OverlayMode::Scene;
	settings.scene = {"s", "Replay scene"};
	settings.canvas = {1280, 720};
	OverlayItem first = item("a", OverlayAnchor::BottomRight, 64, 36, 320, 180);
	first.fit = OverlayFit::Fill;
	first.opacity = 55;
	first.blend = OverlayBlend::Screen;
	first.crop = {1, 2, 3, 4};
	first.visible = false;
	settings.items.push_back(first);
	settings.items.push_back(item("", OverlayAnchor::Center, -5, 7, 100, 50));
	settings.items.back().name = "Only a name";

	const SavedOverlays saved = saveOverlays(settings);
	CHECK(saved.mode == "scene");
	CHECK(saved.canvasWidth == 1280);
	CHECK(saved.items[0].anchor == "bottom_right");
	CHECK(saved.items[0].fit == "fill");
	CHECK(saved.items[0].blend == "screen");
	CHECK(loadOverlays(saved, {1280, 720}) == settings);

	const OverlaySettings scaled = loadOverlays(saved, kHd);
	CHECK(scaled.canvas == kHd);
	CHECK(scaled.items[0].x == 96);
	CHECK(scaled.items[0].width == 480);
	CHECK(scaled.items[0].crop == first.crop);
}

TEST_CASE("loaded overlays are brought within range")
{
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	SavedOverlays saved;
	saved.mode = "fancy";
	saved.canvasWidth = 1920;
	saved.canvasHeight = 1080;

	SavedOverlay wild;
	wild.uuid = "a";
	wild.anchor = "middle";
	wild.fit = "zoom";
	wild.blend = "overlay";
	wild.x = nan;
	wild.y = -inf;
	wild.width = -400;
	wild.height = 1e300;
	wild.opacity = nan;
	wild.crop = {nan, -2, 2.6, inf};
	saved.items.push_back(wild);

	SavedOverlay faint;
	faint.name = "b";
	faint.width = nan;
	faint.height = 10.5;
	faint.opacity = 250;
	saved.items.push_back(faint);

	SavedOverlay nobody;
	nobody.width = 10;
	saved.items.push_back(nobody);

	SavedOverlay negative = faint;
	negative.opacity = -1;
	saved.items.push_back(negative);

	const OverlaySettings loaded = loadOverlays(saved, kHd);
	CHECK(loaded.mode == OverlayMode::Simple);
	REQUIRE(loaded.items.size() == 3);
	const OverlayItem &first = loaded.items[0];
	CHECK(first.anchor == OverlayAnchor::TopLeft);
	CHECK(first.fit == OverlayFit::Inner);
	CHECK(first.blend == OverlayBlend::Normal);
	CHECK(first.x == 0);
	CHECK(first.y == -kMaxOverlayExtent);
	CHECK(first.width == 1);
	CHECK(first.height == kMaxOverlayExtent);
	CHECK(first.opacity == 100);
	CHECK(first.crop == tapeloop::OverlayCrop{0, 0, 3, kMaxOverlayExtent});
	CHECK(loaded.items[1].name == "b");
	CHECK(loaded.items[1].width == 1);
	CHECK(loaded.items[1].height == 11);
	CHECK(loaded.items[1].opacity == 100);
	CHECK(loaded.items[2].opacity == 0);
}

TEST_CASE("loading keeps the first kMaxOverlays items")
{
	SavedOverlays saved;
	for (size_t index = 0; index < kMaxOverlays + 5; ++index) {
		SavedOverlay each;
		each.uuid = std::to_string(index);
		each.width = 10;
		each.height = 10;
		saved.items.push_back(each);
	}
	const OverlaySettings loaded = loadOverlays(saved, kHd);
	REQUIRE(loaded.items.size() == kMaxOverlays);
	CHECK(loaded.items.back().uuid == std::to_string(kMaxOverlays - 1));
}

TEST_CASE("values saved at no valid canvas are taken as made at this one")
{
	SavedOverlays saved;
	SavedOverlay one;
	one.uuid = "a";
	one.x = 100;
	one.width = 200;
	one.height = 100;
	saved.items.push_back(one);
	for (const auto &[width, height] :
	     std::vector<std::pair<double, double>>{{0, 0},
						    {1920, -1},
						    {std::numeric_limits<double>::quiet_NaN(), 1080},
						    {99999, 1080}}) {
		saved.canvasWidth = width;
		saved.canvasHeight = height;
		const OverlaySettings loaded = loadOverlays(saved, {1280, 720});
		CHECK(loaded.canvas == CanvasSize{1280, 720});
		CHECK(loaded.items[0].x == 100);
		CHECK(loaded.items[0].width == 200);
	}
	saved.canvasWidth = 640;
	saved.canvasHeight = 360;
	const OverlaySettings loaded = loadOverlays(saved, {0, 0});
	CHECK(loaded.canvas == CanvasSize{1, 1});
	CHECK(loaded.items[0].width == 1);
}
