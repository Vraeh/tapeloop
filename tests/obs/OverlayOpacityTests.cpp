// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "AllocationCounter.hpp"
#include "LogCounter.hpp"
#include "ObsFixture.hpp"
#include "TestPattern.hpp"

#include "obs/OverlayOpacity.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using tapeloop::obs::createOverlayOpacity;
using tapeloop::obs::kOverlayOpacityId;
using tapeloop::obs::overlayTarget;
using tapeloop::obs::registerOverlayOpacity;
using tapeloop::test::AllocationCounter;
using tapeloop::test::createTestPattern;
using tapeloop::test::ObsFixture;

namespace {

constexpr uint32_t kSide = 64;

using Rgba = std::array<uint8_t, 4>;

// Draws a source on a cleared square canvas, as a scene draws its items, and reads back
// the pixel in the middle.
class Readback {
public:
	Readback()
	{
		obs_enter_graphics();
		target_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		stage_ = gs_stagesurface_create(kSide, kSide, GS_RGBA);
		obs_leave_graphics();
		REQUIRE(target_);
		REQUIRE(stage_);
	}

	~Readback()
	{
		obs_enter_graphics();
		gs_stagesurface_destroy(stage_);
		gs_texrender_destroy(target_);
		obs_leave_graphics();
	}

	Readback(const Readback &) = delete;
	Readback &operator=(const Readback &) = delete;

	// Counts the allocations of the source's draw alone into allocations when given.
	Rgba draw(obs_source_t *source, size_t *allocations = nullptr)
	{
		Rgba middle{};
		bool mapped = false;
		obs_enter_graphics();
		gs_texrender_reset(target_);
		const bool began = gs_texrender_begin(target_, kSide, kSide);
		if (began) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, static_cast<float>(kSide), 0.0f, static_cast<float>(kSide), -100.0f, 100.0f);
			// The device culls back faces until OBS's video thread first renders.
			gs_set_cull_mode(GS_NEITHER);
			gs_enable_depth_test(false);
			gs_blend_state_push();
			gs_reset_blend_state();
			const bool linear = gs_set_linear_srgb(false);
			if (allocations) {
				const AllocationCounter counter;
				obs_source_video_render(source);
				*allocations += counter.count();
			} else {
				obs_source_video_render(source);
			}
			gs_set_linear_srgb(linear);
			gs_blend_state_pop();
			gs_texrender_end(target_);

			uint8_t *data = nullptr;
			uint32_t stride = 0;
			gs_stage_texture(stage_, gs_texrender_get_texture(target_));
			mapped = gs_stagesurface_map(stage_, &data, &stride);
			if (mapped) {
				std::memcpy(middle.data(), data + (kSide / 2) * stride + (kSide / 2) * 4,
					    middle.size());
				gs_stagesurface_unmap(stage_);
			}
		}
		obs_leave_graphics();
		REQUIRE(began);
		REQUIRE(mapped);
		return middle;
	}

private:
	gs_texrender_t *target_ = nullptr;
	gs_stagesurf_t *stage_ = nullptr;
};

bool closeTo(uint8_t value, uint8_t expected)
{
	return value + 2 >= expected && value <= expected + 2;
}

} // namespace

TEST_CASE_METHOD(ObsFixture, "an overlay below full opacity scales its colour and alpha alike", "[obs][composition]")
{
	registerOverlayOpacity(TAPELOOP_OPACITY_EFFECT);
	const OBSSourceAutoRelease tone = obs_source_create(tapeloop::test::kToneId, "White", nullptr, nullptr);
	REQUIRE(tone);
	const OBSSourceAutoRelease half = createOverlayOpacity(tone, 50);
	REQUIRE(half);
	CHECK(obs_obj_is_private(half));
	CHECK(obs_source_get_width(half) == obs_source_get_width(tone));
	CHECK(obs_source_get_height(half) == obs_source_get_height(tone));

	Readback screen;
	const Rgba opaque = screen.draw(tone);
	CHECK(opaque == Rgba{255, 255, 255, 255});

	// White at half opacity, its colour premultiplied by its alpha.
	const Rgba drawn = screen.draw(half);
	CHECK(closeTo(drawn[3], 128));
	CHECK(closeTo(drawn[0], 128));
	CHECK(drawn[0] == drawn[1]);
	CHECK(drawn[1] == drawn[2]);

	// Nothing is allocated for a frame after the first.
	size_t allocations = 0;
	CHECK(screen.draw(half, &allocations) == drawn);
	CHECK(allocations == 0);
}

TEST_CASE_METHOD(ObsFixture, "an overlay below full opacity shows its target and lets go of it", "[obs][composition]")
{
	registerOverlayOpacity(TAPELOOP_OPACITY_EFFECT);
	OBSSourceAutoRelease logo = createTestPattern(64, 36, "Logo");
	OBSWeakSourceAutoRelease weakLogo = obs_source_get_weak_source(logo);
	OBSSourceAutoRelease translucent = createOverlayOpacity(logo, 40);
	REQUIRE(translucent);
	CHECK(std::strcmp(obs_source_get_unversioned_id(translucent), kOverlayOpacityId) == 0);
	CHECK(overlayTarget(translucent) == logo.Get());
	CHECK(overlayTarget(logo) == logo.Get());
	CHECK(overlayTarget(nullptr) == nullptr);

	obs_source_t *found = nullptr;
	obs_source_enum_active_sources(
		translucent,
		[](obs_source_t *, obs_source_t *child, void *param) { *static_cast<obs_source_t **>(param) = child; },
		&found);
	CHECK(found == logo.Get());

	logo = nullptr;
	CHECK_FALSE(obs_weak_source_expired(weakLogo));
	translucent = nullptr;
	obs_wait_for_destroy_queue();
	CHECK(obs_weak_source_expired(weakLogo));
}

TEST_CASE_METHOD(ObsFixture, "an overlay below full opacity is not made for a key that is no UUID",
		 "[obs][composition]")
{
	// A collection edited by hand can hold such a source. libobs alone would read past an
	// empty key and take the longer one for the logo.
	registerOverlayOpacity(TAPELOOP_OPACITY_EFFECT);
	const OBSSourceAutoRelease logo = createTestPattern(64, 36, "Logo");
	const std::string longer = std::string(obs_source_get_uuid(logo)) + "0";
	for (const char *target : {"", longer.c_str(), static_cast<const char *>(nullptr)}) {
		const OBSDataAutoRelease settings = obs_data_create();
		if (target) {
			obs_data_set_string(settings, "target", target);
		}
		const OBSSourceAutoRelease made = obs_source_create_private(kOverlayOpacityId, "Hand-made", settings);
		REQUIRE(made);
		CHECK(obs_obj_get_data(made) == nullptr);
		CHECK(overlayTarget(made) == made.Get());
	}
}

TEST_CASE_METHOD(ObsFixture, "an overlay below full opacity is not made without its effect", "[obs][composition]")
{
	const OBSSourceAutoRelease logo = createTestPattern(64, 36, "Logo");

	SECTION("the effect file is missing")
	{
		registerOverlayOpacity(TAPELOOP_TEST_DATA_DIR "/no-such.effect");
		const tapeloop::test::LogCounter said("The overlay opacity effect");
		CHECK(createOverlayOpacity(logo, 50) == nullptr);
		CHECK(said.lines == 1);
	}
	SECTION("module load found no effect")
	{
		registerOverlayOpacity({});
		const tapeloop::test::LogCounter said("The overlay opacity effect");
		CHECK(createOverlayOpacity(logo, 50) == nullptr);
		CHECK(said.lines == 0);
	}
	SECTION("the type is not registered")
	{
		CHECK(createOverlayOpacity(logo, 50) == nullptr);
	}
	CHECK(createOverlayOpacity(nullptr, 50) == nullptr);
}
