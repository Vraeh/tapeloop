// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ObsFixture.hpp"

#include "BufferOutput.hpp"
#include "TestPattern.hpp"

#include <catch2/catch_test_macros.hpp>
#include <obs.h>
#include <util/bmem.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tapeloop::test {
namespace {

const std::string kPrefix = TAPELOOP_LIBOBS_PREFIX;

// libobs only logs the objects and views it frees at shutdown because nobody else did.
// The format strings are matched, not the messages, so that names in a message cannot
// match.
std::atomic<int> leftovers{0};

void logHandler(int level, const char *format, va_list args, void *) noexcept
{
	if (std::strstr(format, "(s) were remaining") || std::strstr(format, "Number of remaining views"))
		++leftovers;
	char message[4096];
	std::vsnprintf(message, sizeof(message), format, args);
	std::fprintf(level <= LOG_WARNING ? stderr : stdout, "%s\n", message);
}

void loadModule(const std::string &name)
{
	const std::string binary = kPrefix + "/lib/obs-plugins/" + name + ".so";
	const std::string data = kPrefix + "/share/obs/obs-plugins/" + name;
	obs_module_t *module = nullptr;
	if (obs_open_module(&module, binary.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(module))
		throw std::runtime_error("Could not load " + binary);
}

} // namespace

ObsFixture::ObsFixture() : allocationsBefore_(bnum_allocs())
{
	leftovers = 0;
	base_set_log_handler(logHandler, nullptr);
	if (!obs_startup("en-US", nullptr, nullptr))
		throw std::runtime_error("obs_startup failed");

	// The destructor does not run when the constructor throws.
	try {
		resetCanvas({});
		obs_audio_info audio = {48000, SPEAKERS_STEREO};
		if (!obs_reset_audio(&audio))
			throw std::runtime_error("obs_reset_audio failed");
		loadModule("obs-x264");
		registerTestPattern();
		registerBufferOutput();
	} catch (...) {
		obs_shutdown();
		throw;
	}
}

ObsFixture::~ObsFixture()
{
	obs_shutdown();
	CHECK(bnum_allocs() == allocationsBefore_);
	CHECK(leftovers == 0);
}

void ObsFixture::resetCanvas(CanvasFormat format)
{
	// libobs keeps the pointer to the module name.
	static const std::string kGraphicsModule = kPrefix + "/lib/libobs-opengl";

	obs_video_info video = {};
	video.graphics_module = kGraphicsModule.c_str();
	video.fps_num = format.fps;
	video.fps_den = 1;
	video.base_width = format.width;
	video.base_height = format.height;
	video.output_width = format.width;
	video.output_height = format.height;
	video.output_format = VIDEO_FORMAT_NV12;
	video.adapter = 0;
	video.gpu_conversion = true;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	video.scale_type = OBS_SCALE_BICUBIC;

	const int result = obs_reset_video(&video);
	if (result != OBS_VIDEO_SUCCESS)
		throw std::runtime_error("obs_reset_video failed with " + std::to_string(result));
}

} // namespace tapeloop::test
