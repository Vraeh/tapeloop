// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "LogCounter.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"
#include "UiTasks.hpp"

#include "core/BufferSettings.hpp"
#include "core/OverlayLayout.hpp"
#include "obs/CaptureManager.hpp"
#include "obs/Composition.hpp"
#include "obs/SettingsData.hpp"

#include <catch2/catch_test_macros.hpp>
#include <graphics/matrix4.h>
#include <obs.hpp>

#include <string>
#include <vector>

using tapeloop::BufferSettings;
using tapeloop::CanvasSize;
using tapeloop::OverlayAnchor;
using tapeloop::OverlayBlend;
using tapeloop::OverlayFit;
using tapeloop::OverlayItem;
using tapeloop::OverlaySettings;
using tapeloop::obs::CaptureManager;
using tapeloop::obs::Composition;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;
using tapeloop::test::UiTasks;

namespace {

class QuietHost final : public tapeloop::obs::CaptureHost {
public:
	bool streamingActive() const override { return false; }
	bool recordingActive() const override { return false; }
	void requestSave() override {}
	std::string recordingFolder() const override { return {}; }
	std::string sceneCollectionName() const override { return "Tests"; }
	void buffersStopped() override {}
};

OverlayItem overlayOf(obs_source_t *source)
{
	OverlayItem item;
	item.uuid = obs_source_get_uuid(source);
	item.name = obs_source_get_name(source);
	item.width = 64;
	item.height = 36;
	return item;
}

// Bottom first. The scene owns them.
std::vector<obs_sceneitem_t *> itemsOf(obs_source_t *composition)
{
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		obs_scene_from_source(composition),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	return items;
}

std::vector<obs_source_t *> sourcesOf(obs_source_t *composition)
{
	std::vector<obs_source_t *> sources;
	for (obs_sceneitem_t *item : itemsOf(composition)) {
		sources.push_back(obs_sceneitem_get_source(item));
	}
	return sources;
}

bool isMainScene(obs_source_t *source)
{
	struct Search {
		obs_source_t *source;
		bool found;
	} search{source, false};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			auto &looking = *static_cast<Search *>(param);
			looking.found = looking.found || scene == looking.source;
			return true;
		},
		&search);
	return search.found;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "an overlay is placed above the replay as its model says", "[obs][composition]")
{
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease logo = createTestPattern(100, 50, "Logo");
	Composition composition(replay);
	REQUIRE(composition.source());
	CHECK(obs_obj_is_private(composition.source()));
	CHECK_FALSE(isMainScene(composition.source()));

	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	OverlayItem item = overlayOf(logo);
	item.anchor = OverlayAnchor::BottomRight;
	item.x = 10;
	item.y = 20;
	item.width = 160;
	item.height = 90;
	item.fit = OverlayFit::Fill;
	item.crop = {1, 2, 3, 4};
	item.blend = OverlayBlend::Additive;
	overlays.items.push_back(item);
	composition.setOverlays(overlays);
	composition.apply();

	const std::vector<obs_sceneitem_t *> items = itemsOf(composition.source());
	REQUIRE(items.size() == 2);
	CHECK(obs_sceneitem_get_source(items[0]) == replay.Get());
	obs_transform_info fitted = {};
	obs_sceneitem_get_info2(items[0], &fitted);
	CHECK(fitted.bounds_type == OBS_BOUNDS_SCALE_INNER);
	CHECK(fitted.bounds.x == 640.0f);
	CHECK(fitted.bounds.y == 360.0f);
	CHECK(fitted.pos.x == 320.0f);
	CHECK(fitted.pos.y == 180.0f);

	CHECK(obs_sceneitem_get_source(items[1]) == logo.Get());
	obs_transform_info info = {};
	obs_sceneitem_get_info2(items[1], &info);
	CHECK(info.pos.x == 630.0f);
	CHECK(info.pos.y == 340.0f);
	CHECK(info.alignment == (OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT));
	CHECK(info.bounds_alignment == (OBS_ALIGN_BOTTOM | OBS_ALIGN_RIGHT));
	CHECK(info.bounds_type == OBS_BOUNDS_SCALE_OUTER);
	CHECK(info.bounds.x == 160.0f);
	CHECK(info.bounds.y == 90.0f);
	CHECK(info.crop_to_bounds);
	obs_sceneitem_crop crop = {};
	obs_sceneitem_get_crop(items[1], &crop);
	CHECK(crop.left == 1);
	CHECK(crop.top == 2);
	CHECK(crop.right == 3);
	CHECK(crop.bottom == 4);
	CHECK(obs_sceneitem_get_blending_mode(items[1]) == OBS_BLEND_ADDITIVE);

	// The box libobs draws the item in.
	matrix4 box = {};
	obs_sceneitem_get_box_transform(items[1], &box);
	CHECK(box.t.x == 470.0f);
	CHECK(box.t.y == 250.0f);
	CHECK(box.x.x == 160.0f);
	CHECK(box.y.y == 90.0f);
}

TEST_CASE_METHOD(ObsFixture, "overlays stack in their order and hidden ones are left out", "[obs][composition]")
{
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease first = createTestPattern(64, 64, "First");
	OBSSourceAutoRelease second = createTestPattern(64, 64, "Second");
	OBSSourceAutoRelease third = createTestPattern(64, 64, "Third");
	Composition composition(replay);

	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	overlays.items = {overlayOf(first), overlayOf(second), overlayOf(third)};
	overlays.items[1].visible = false;
	composition.setOverlays(overlays);
	composition.apply();
	CHECK(sourcesOf(composition.source()) == std::vector<obs_source_t *>{replay, first, third});

	overlays.items = {overlayOf(third), overlayOf(first), overlayOf(second)};
	composition.setOverlays(overlays);
	// Nothing changes before the next apply.
	CHECK(sourcesOf(composition.source()).size() == 3);
	composition.apply();
	CHECK(sourcesOf(composition.source()) == std::vector<obs_source_t *>{replay, third, first, second});
	CHECK(composition.overlays() == overlays);
}

TEST_CASE_METHOD(ObsFixture, "an overlay of a source the collection does not have is left out", "[obs][composition]")
{
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease logo = createTestPattern(64, 64, "Logo");
	OBSSourceAutoRelease silence = obs_source_create(tapeloop::test::kSilenceId, "Silence", nullptr, nullptr);
	REQUIRE(silence);
	Composition composition(replay);
	const tapeloop::test::LogCounter said("is not in this scene collection");

	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	OverlayItem gone;
	gone.uuid = "9f1c0a7e-0000-4000-8000-000000000002";
	gone.name = "Gone";
	// Neither the composition itself nor a source without video can be an overlay.
	OverlayItem itself;
	itself.uuid = obs_source_get_uuid(composition.source());
	overlays.items = {gone, overlayOf(silence), itself, overlayOf(logo)};
	composition.setOverlays(overlays);
	composition.apply();
	CHECK(sourcesOf(composition.source()) == std::vector<obs_source_t *>{replay, logo});
	CHECK(said.lines == 3);

	// Nothing changed, so nothing is built or said again.
	composition.apply();
	CHECK(said.lines == 3);
}

TEST_CASE_METHOD(ObsFixture, "an overlay whose source OBS removes is let go of", "[obs][composition]")
{
	const UiTasks ui;
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease kept = createTestPattern(64, 64, "Kept");
	OBSSourceAutoRelease removed = createTestPattern(64, 64, "Removed");
	OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(removed);
	Composition composition(replay);

	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	overlays.items = {overlayOf(removed), overlayOf(kept)};
	composition.setOverlays(overlays);
	composition.apply();
	REQUIRE(sourcesOf(composition.source()).size() == 3);

	obs_source_remove(removed);
	removed = nullptr;
	UiTasks::run();
	CHECK(sourcesOf(composition.source()) == std::vector<obs_source_t *>{replay, kept});
	CHECK(obs_weak_source_expired(weak));
	// The model keeps it, for the next save to drop.
	CHECK(composition.overlays() == overlays);
}

TEST_CASE_METHOD(ObsFixture, "a destroyed composition lets go of the replay and every overlay", "[obs][composition]")
{
	const UiTasks ui;
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease logo = createTestPattern(64, 64, "Logo");
	OBSWeakSourceAutoRelease weakReplay = obs_source_get_weak_source(replay);
	OBSWeakSourceAutoRelease weakLogo = obs_source_get_weak_source(logo);
	Composition composition(replay);
	OverlaySettings overlays;
	overlays.items = {overlayOf(logo)};
	composition.setOverlays(overlays);
	composition.apply();

	composition.destroy();
	CHECK(composition.source() == nullptr);
	replay = nullptr;
	logo = nullptr;
	CHECK(obs_weak_source_expired(weakReplay));
	CHECK(obs_weak_source_expired(weakLogo));

	// Nothing is left to do for a source removed later.
	OBSSourceAutoRelease other = createTestPattern(64, 64, "Other");
	obs_source_remove(other);
	CHECK(UiTasks::run() == 0);
	composition.apply();
	CHECK(composition.source() == nullptr);
}

TEST_CASE_METHOD(ObsFixture, "overlays are placed at the canvas OBS has when applied", "[obs][composition]")
{
	OBSSourceAutoRelease replay = createTestPattern(320, 180, "Replay");
	OBSSourceAutoRelease logo = createTestPattern(64, 64, "Logo");
	Composition composition(replay);
	OverlaySettings overlays;
	overlays.canvas = {640, 360};
	OverlayItem item = overlayOf(logo);
	item.x = 10;
	item.y = 20;
	item.width = 160;
	item.height = 90;
	overlays.items = {item};
	composition.setOverlays(overlays);
	composition.apply();

	REQUIRE(ObsFixture::resetCanvas({1280, 720, 30}) == OBS_VIDEO_SUCCESS);
	composition.apply();
	const std::vector<obs_sceneitem_t *> items = itemsOf(composition.source());
	REQUIRE(items.size() == 2);
	obs_transform_info fitted = {};
	obs_sceneitem_get_info2(items[0], &fitted);
	CHECK(fitted.bounds.x == 1280.0f);
	CHECK(fitted.bounds.y == 720.0f);
	obs_transform_info info = {};
	obs_sceneitem_get_info2(items[1], &info);
	CHECK(info.pos.x == 20.0f);
	CHECK(info.pos.y == 40.0f);
	CHECK(info.bounds.x == 320.0f);
	CHECK(info.bounds.y == 180.0f);
	// The model stays as it was made.
	CHECK(composition.overlays() == overlays);
}

TEST_CASE_METHOD(ObsFixture, "overlays are saved with the collection and loaded at another canvas",
		 "[obs][composition]")
{
	OBSSourceAutoRelease logo = createTestPattern(64, 64, "Logo");
	QuietHost host;
	BufferSettings settings;
	settings.overlays.canvas = {640, 360};
	OverlayItem item = overlayOf(logo);
	item.name = "Renamed since";
	item.anchor = OverlayAnchor::Right;
	item.x = 10;
	item.y = 20;
	item.width = 160;
	item.height = 90;
	item.opacity = 70;
	OverlayItem gone;
	gone.uuid = "9f1c0a7e-0000-4000-8000-000000000003";
	gone.name = "Gone";
	settings.overlays.items = {gone, item, item};

	OBSDataAutoRelease collection = obs_data_create();
	{
		CaptureManager manager(host);
		manager.setSettings(settings);
		manager.save(collection);
	}
	OBSDataAutoRelease written = obs_data_get_obj(collection, tapeloop::obs::kSettingsKey);
	OBSDataAutoRelease overlays = obs_data_get_obj(written, "overlays");
	REQUIRE(overlays);
	OBSDataAutoRelease canvas = obs_data_get_obj(overlays, "canvas");
	CHECK(obs_data_get_int(canvas, "width") == 640);
	OBSDataArrayAutoRelease items = obs_data_get_array(overlays, "items");
	// The overlay of a source that is gone is dropped; one shown twice stays twice.
	REQUIRE(obs_data_array_count(items) == 2);
	OBSDataAutoRelease first = obs_data_array_item(items, 0);
	CHECK(std::string(obs_data_get_string(first, "name")) == "Logo");
	CHECK(std::string(obs_data_get_string(first, "anchor")) == "right");
	OBSDataAutoRelease crop = obs_data_get_obj(first, "crop");
	CHECK(obs_data_has_user_value(crop, "bottom"));

	REQUIRE(ObsFixture::resetCanvas({1280, 720, 30}) == OBS_VIDEO_SUCCESS);
	CaptureManager loaded(host);
	loaded.load(collection);
	const OverlaySettings &got = loaded.settings().overlays;
	CHECK(got.canvas == CanvasSize{1280, 720});
	REQUIRE(got.items.size() == 2);
	CHECK(got.items[0].uuid == item.uuid);
	CHECK(got.items[0].name == "Logo");
	CHECK(got.items[0].anchor == OverlayAnchor::Right);
	CHECK(got.items[0].x == 20);
	CHECK(got.items[0].y == 40);
	CHECK(got.items[0].width == 320);
	CHECK(got.items[0].height == 180);
	CHECK(got.items[0].opacity == 70);
	CHECK(got.items[1] == got.items[0]);
}

TEST_CASE_METHOD(ObsFixture, "a damaged saved overlay keeps the defaults of what it got wrong", "[obs][composition]")
{
	OBSSourceAutoRelease logo = createTestPattern(64, 64, "Logo");
	const std::string json = std::string(R"({"tapeloop": {"version": 1, "overlays": {"mode": 3, "canvas": "wide",
		"scene": [], "items": [{"uuid": ")") +
				 obs_source_get_uuid(logo) + R"(", "name": "Logo", "visible": "no", "anchor": 7,
		"x": "far", "y": 12.6, "width": -5, "height": null, "fit": "sideways", "opacity": "half",
		"blend": true, "crop": [1, 2]}, {"x": 4}, "loose"]}}})";
	OBSDataAutoRelease collection = obs_data_create_from_json(json.c_str());
	REQUIRE(collection);

	QuietHost host;
	CaptureManager manager(host);
	manager.load(collection);
	const OverlaySettings &got = manager.settings().overlays;
	CHECK(got.mode == tapeloop::OverlayMode::Simple);
	CHECK(got.scene == tapeloop::OverlayScene{});
	CHECK(got.canvas == CanvasSize{640, 360});
	REQUIRE(got.items.size() == 1);
	const OverlayItem &item = got.items[0];
	CHECK(item.name == "Logo");
	CHECK(item.visible);
	CHECK(item.anchor == OverlayAnchor::TopLeft);
	CHECK(item.x == 0);
	CHECK(item.y == 13);
	CHECK(item.width == tapeloop::kMinOverlaySize);
	CHECK(item.height == tapeloop::kMinOverlaySize);
	CHECK(item.fit == OverlayFit::Inner);
	CHECK(item.opacity == 100);
	CHECK(item.blend == OverlayBlend::Normal);
	CHECK(item.crop == tapeloop::OverlayCrop{});

	// Overlays that are not an object at all are no overlays.
	OBSDataAutoRelease settings = obs_data_get_obj(collection, tapeloop::obs::kSettingsKey);
	obs_data_set_string(settings, "overlays", "none");
	manager.load(collection);
	CHECK(manager.settings().overlays.items.empty());
}
