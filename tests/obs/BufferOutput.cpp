// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "BufferOutput.hpp"

#include <obs-module.h>

#include <chrono>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <variant>

namespace tapeloop::test {
namespace {

constexpr const char *kBufferKey = "buffer";

struct BufferOutput {
	obs_output_t *output = nullptr;
	SourceBuffer *buffer = nullptr;
};

const char *name(void *) noexcept
{
	return "Tapeloop test buffer output";
}

void *create(obs_data_t *settings, obs_output_t *output) noexcept
{
	auto *state = new (std::nothrow) BufferOutput;
	if (!state)
		return nullptr;
	state->output = output;
	state->buffer = reinterpret_cast<SourceBuffer *>(static_cast<intptr_t>(obs_data_get_int(settings, kBufferKey)));
	return state;
}

void destroy(void *data) noexcept
{
	delete static_cast<BufferOutput *>(data);
}

bool start(void *data) noexcept
{
	auto &state = *static_cast<BufferOutput *>(data);
	if (!state.buffer || !obs_output_can_begin_data_capture(state.output, 0) ||
	    !obs_output_initialize_encoders(state.output, 0))
		return false;

	uint8_t *config = nullptr;
	size_t size = 0;
	if (!obs_encoder_get_extra_data(obs_output_get_video_encoder(state.output), &config, &size))
		size = 0;
	try {
		state.buffer->setCodecConfig(std::span<const uint8_t>(config, size));
	} catch (...) {
		return false;
	}
	return obs_output_begin_data_capture(state.output, 0);
}

void stop(void *data, uint64_t) noexcept
{
	obs_output_end_data_capture(static_cast<BufferOutput *>(data)->output);
}

void encodedPacket(void *data, encoder_packet *packet) noexcept
{
	auto &state = *static_cast<BufferOutput *>(data);
	if (!packet) {
		obs_output_signal_stop(state.output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}
	EncodedPacket encoded;
	encoded.data = std::span<const uint8_t>(packet->data, packet->size);
	encoded.pts = packet->pts;
	encoded.dts = packet->dts;
	encoded.time = std::chrono::microseconds(packet->sys_dts_usec);
	encoded.keyframe = packet->keyframe;
	try {
		state.buffer->push(encoded);
	} catch (...) {
		obs_output_signal_stop(state.output, OBS_OUTPUT_ERROR);
	}
}

} // namespace

void registerBufferOutput()
{
	obs_output_info info = {};
	info.id = kBufferOutputId;
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.start = start;
	info.stop = stop;
	info.encoded_packet = encodedPacket;
	obs_register_output(&info);
}

OBSDataAutoRelease bufferOutputSettings(SourceBuffer &buffer)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, kBufferKey, static_cast<long long>(reinterpret_cast<intptr_t>(&buffer)));
	return settings;
}

OBSDataAutoRelease toObsData(const EncoderSettings &settings)
{
	OBSDataAutoRelease data = obs_data_create();
	for (const auto &[key, value] : settings) {
		if (const int64_t *number = std::get_if<int64_t>(&value))
			obs_data_set_int(data, key.c_str(), *number);
		else if (const bool *flag = std::get_if<bool>(&value))
			obs_data_set_bool(data, key.c_str(), *flag);
		else if (const std::string *text = std::get_if<std::string>(&value))
			obs_data_set_string(data, key.c_str(), text->c_str());
	}
	return data;
}

} // namespace tapeloop::test
