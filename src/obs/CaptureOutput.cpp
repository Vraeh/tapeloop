// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureOutput.hpp"

#include <obs-module.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <optional>
#include <span>

namespace tapeloop::obs {
namespace {

struct CaptureOutput {
	obs_output_t *output = nullptr;
	CaptureTarget *target = nullptr;
};

const char *name(void *) noexcept
{
	return "Tapeloop capture";
}

void *create(obs_data_t *, obs_output_t *output) noexcept
{
	auto *capture = new (std::nothrow) CaptureOutput;
	if (capture) {
		capture->output = output;
	}
	return capture;
}

void destroy(void *data) noexcept
{
	delete static_cast<CaptureOutput *>(data);
}

bool start(void *data) noexcept
{
	auto &capture = *static_cast<CaptureOutput *>(data);
	if (!capture.target || !capture.target->buffer || !obs_output_can_begin_data_capture(capture.output, 0)) {
		return false;
	}

	// Before the encoder initializes, so a hardware encoder does not open a session only
	// to be refused. The codec cannot change at initialization: libobs refuses to reroute
	// an encoder to another codec.
	obs_encoder_t *encoder = obs_output_get_video_encoder(capture.output);
	const char *codecName = obs_encoder_get_codec(encoder);
	const std::optional<VideoCodec> codec = videoCodecFromName(codecName ? codecName : "");
	if (!codec) {
		char error[96];
		std::snprintf(error, sizeof(error), "A replay cannot hold %s",
			      codecName ? codecName : "an unknown codec");
		obs_output_set_last_error(capture.output, error);
		return false;
	}
	if (!obs_output_initialize_encoders(capture.output, 0)) {
		return false;
	}
	// Some encoders only know their configuration after the first keyframe; those repeat
	// it in the stream.
	uint8_t *config = nullptr;
	size_t size = 0;
	if (!obs_encoder_get_extra_data(encoder, &config, &size)) {
		size = 0;
	}
	try {
		if (capture.target->clearOnStart) {
			capture.target->buffer->clear();
		}
		capture.target->buffer->setCodecConfig(*codec, std::span<const uint8_t>(config, size));
	} catch (...) {
		return false;
	}
	capture.target->failed = false;
	capture.target->encoderFailed = false;
	return obs_output_begin_data_capture(capture.output, 0);
}

void stop(void *data, uint64_t) noexcept
{
	obs_output_end_data_capture(static_cast<CaptureOutput *>(data)->output);
}

void encodedPacket(void *data, encoder_packet *packet) noexcept
{
	auto &capture = *static_cast<CaptureOutput *>(data);
	if (!packet) {
		capture.target->encoderFailed = true;
		capture.target->failed = true;
		obs_output_signal_stop(capture.output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	// The packet data is the encoder's own buffer, valid only during this call; push
	// copies it.
	EncodedPacket encoded;
	encoded.data = std::span<const uint8_t>(packet->data, packet->size);
	encoded.pts = packet->pts;
	encoded.dts = packet->dts;
	encoded.time = std::chrono::microseconds(packet->sys_dts_usec);
	encoded.keyframe = packet->keyframe;
	try {
		capture.target->buffer->push(encoded);
	} catch (...) {
		capture.target->failed = true;
		obs_output_signal_stop(capture.output, OBS_OUTPUT_ERROR);
	}
}

} // namespace

void registerCaptureOutput()
{
	obs_output_info info = {};
	info.id = kCaptureOutputId;
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.start = start;
	info.stop = stop;
	info.encoded_packet = encodedPacket;
	obs_register_output(&info);
}

obs_output_t *createCaptureOutput(const char *outputName, CaptureTarget &target)
{
	obs_output_t *output = obs_output_create(kCaptureOutputId, outputName, nullptr, nullptr);
	auto *capture = static_cast<CaptureOutput *>(obs_obj_get_data(output));
	if (!capture) {
		obs_output_release(output);
		return nullptr;
	}
	capture->target = &target;
	return output;
}

} // namespace tapeloop::obs
