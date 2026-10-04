// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SourceCapture.hpp"

#include "core/MediaTime.hpp"
#include "obs/ObsEncoders.hpp"

#include <util/base.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>

namespace tapeloop::obs {
namespace {

int32_t toInt32(uint32_t value)
{
	return static_cast<int32_t>(std::clamp<uint32_t>(value, 1, std::numeric_limits<int32_t>::max()));
}

// The largest view the capture creates. libobs creates the view's textures at the
// source's size inside the graphics context, and a failure there leaves the context
// entered and the graphics thread stuck; Direct3D 11 at feature level 10 allows 8192.
constexpr uint32_t kMaxViewSize = 8192;

// A buffer can take a new byte budget but not a new length or frame duration.
bool canReuse(const SourceBufferConfig &existing, const SourceBufferConfig &wanted)
{
	return existing.window == wanted.window && existing.frameDuration == wanted.frameDuration;
}

} // namespace

SourceCapture::~SourceCapture()
{
	stop();
}

StartResult SourceCapture::start(obs_source_t *source, const CaptureSettings &settings, bool keepBuffer)
{
	if (output_)
		return StartResult::AlreadyRunning;

	const char *name = obs_source_get_name(source);
	obs_video_info video = {};
	if (!obs_get_video_info(&video)) {
		blog(LOG_WARNING, "[tapeloop] OBS has no video, not capturing '%s'", name);
		return StartResult::ViewFailed;
	}
	const FrameSize sourceSize{obs_source_get_width(source), obs_source_get_height(source)};
	if (sourceSize.width == 0 || sourceSize.height == 0) {
		blog(LOG_INFO, "[tapeloop] '%s' has no size yet, not capturing it", name);
		return StartResult::NoSourceSize;
	}
	if (sourceSize.width > kMaxViewSize || sourceSize.height > kMaxViewSize) {
		blog(LOG_WARNING, "[tapeloop] '%s' is %ux%u, larger than the %u pixels a capture allows", name,
		     sourceSize.width, sourceSize.height, kMaxViewSize);
		return StartResult::SourceTooLarge;
	}

	const uint32_t height = targetHeight(settings.resolution, {video.base_width, video.base_height},
					     {video.output_width, video.output_height});
	const std::optional<FrameSize> outputSize = replayOutputSize(sourceSize, height);
	if (!outputSize) {
		blog(LOG_WARNING, "[tapeloop] '%s' is %ux%u, too small to encode at height %u", name, sourceSize.width,
		     sourceSize.height, height);
		return StartResult::NoOutputSize;
	}

	try {
		const std::vector<EncoderInfo> candidates =
			settings.candidates.empty()
				? replayEncoderCandidates(registeredVideoEncoders(), renderAdapterVendor(),
							  settings.encoderPreferences)
				: settings.candidates;
		if (candidates.empty()) {
			blog(LOG_WARNING, "[tapeloop] No encoder can capture '%s'", name);
			return StartResult::NoEncoder;
		}

		ReplayEncoderParams params;
		params.width = outputSize->width;
		params.height = outputSize->height;
		params.frameDuration = {toInt32(video.fps_den), toInt32(video.fps_num)};

		SourceBufferConfig bufferConfig;
		bufferConfig.window = settings.bufferLength;
		bufferConfig.maxBytes = replayByteBudget(replayBitrateKbps(params), settings.bufferLength);
		bufferConfig.frameDuration =
			Nanoseconds{std::max<int64_t>(rescale(1, params.frameDuration, kNanosecondTimebase), 1)};
		// A start that fails leaves the buffer as it was: a new one replaces it only on
		// success, and an existing one is emptied by the output once the encoder runs.
		const bool reuse = buffer_ && canReuse(bufferConfig_, bufferConfig);
		std::unique_ptr<SourceBuffer> replacement;
		if (!reuse)
			replacement = std::make_unique<SourceBuffer>(bufferConfig);
		target_.buffer = reuse ? buffer_.get() : replacement.get();
		target_.clearOnStart = reuse && !keepBuffer;

		view_ = obs_view_create();
		obs_view_set_source(view_, 0, source);
		video.base_width = sourceSize.width;
		video.base_height = sourceSize.height;
		video.output_width = outputSize->width;
		video.output_height = outputSize->height;
		video.output_format = VIDEO_FORMAT_NV12;
		video.gpu_conversion = true;
		video_t *mix = obs_view_add2(view_, &video);
		if (!mix) {
			blog(LOG_WARNING, "[tapeloop] Could not create a view of '%s' at %ux%u", name,
			     outputSize->width, outputSize->height);
			target_.buffer = buffer_.get();
			tearDown();
			return StartResult::ViewFailed;
		}

		const std::string label = std::string("tapeloop: ") + name;
		output_ = createCaptureOutput(label.c_str(), target_);
		for (const EncoderInfo &candidate : candidates) {
			obs_data_t *encoderSettings = createEncoderSettings(buildReplaySettings(candidate, params));
			obs_encoder_t *encoder =
				obs_video_encoder_create(candidate.id.c_str(), label.c_str(), encoderSettings, nullptr);
			obs_data_release(encoderSettings);
			if (!encoder)
				continue;

			obs_encoder_set_video(encoder, mix);
			obs_output_set_video_encoder(output_, encoder);
			if (obs_output_start(output_)) {
				encoder_ = encoder;
				if (reuse) {
					buffer_->setByteBudget(bufferConfig.maxBytes);
				} else {
					buffer_ = std::move(replacement);
				}
				bufferConfig_ = bufferConfig;
				source_ = obs_source_get_weak_source(source);
				sourceSize_ = sourceSize;
				outputSize_ = *outputSize;
				encoderId_ = candidate.id;
				blog(LOG_INFO, "[tapeloop] Capturing '%s' at %ux%u with %s", name, outputSize->width,
				     outputSize->height, candidate.id.c_str());
				return StartResult::Started;
			}

			const char *error = obs_output_get_last_error(output_);
			blog(LOG_INFO, "[tapeloop] %s could not encode '%s'%s%s", candidate.id.c_str(), name,
			     error ? ": " : "", error ? error : "");
			obs_output_set_video_encoder(output_, nullptr);
			obs_encoder_release(encoder);
		}
		blog(LOG_WARNING, "[tapeloop] No encoder could start for '%s'", name);
		target_.buffer = buffer_.get();
		tearDown();
		return StartResult::NoEncoder;
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Out of memory while starting the capture of '%s'", name);
		stop();
		target_.buffer = buffer_.get();
		return StartResult::Error;
	}
}

void SourceCapture::stop()
{
	if (output_) {
		obs_output_stop(output_);
		// Encoders stop on a libobs thread. Until they have, the view's mix must stay: a
		// timeout here would free it under a running encoder, so this only keeps saying
		// that it waits.
		const auto started = std::chrono::steady_clock::now();
		auto nextWarning = started + std::chrono::seconds(5);
		while (obs_output_active(output_)) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			if (std::chrono::steady_clock::now() >= nextWarning) {
				const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - started);
				blog(LOG_WARNING, "[tapeloop] Still waiting for the encoder to stop after %lld s",
				     static_cast<long long>(waited.count()));
				nextWarning += std::chrono::seconds(5);
			}
		}
	}
	tearDown();
}

void SourceCapture::tearDown()
{
	if (output_) {
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (encoder_) {
		obs_encoder_release(encoder_);
		encoder_ = nullptr;
	}
	if (view_) {
		obs_view_remove(view_);
		obs_view_destroy(view_);
		view_ = nullptr;
	}
	source_ = nullptr;
	encoderId_.clear();
	outputSize_ = {};
}

bool SourceCapture::sourceSizeMatches() const
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(source_);
	if (!source)
		return true;
	return obs_source_get_width(source) == sourceSize_.width && obs_source_get_height(source) == sourceSize_.height;
}

CaptureStats SourceCapture::stats() const
{
	CaptureStats stats;
	if (output_)
		stats.state = target_.failed || !obs_output_active(output_) ? CaptureState::Failed
									    : CaptureState::Running;
	stats.encoderId = encoderId_;
	stats.outputSize = outputSize_;
	if (buffer_) {
		stats.buffer = buffer_->stats();
		stats.bufferedDuration = stats.buffer.newestTime - stats.buffer.oldestTime;
	}
	return stats;
}

} // namespace tapeloop::obs
