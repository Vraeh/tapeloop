// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/EncoderPolicy.hpp"
#include "core/ReplaySize.hpp"
#include "core/SourceBuffer.hpp"
#include "obs/CaptureOutput.hpp"

#include <obs.hpp>

#include <memory>
#include <string>
#include <vector>

namespace tapeloop::obs {

struct CaptureSettings {
	ReplayResolution resolution;
	EncoderPreferences encoderPreferences;
	Nanoseconds bufferLength = std::chrono::seconds(60);
	// The encoder candidates to try, best first. Empty means the ones the encoder policy
	// picks from every registered encoder.
	std::vector<EncoderInfo> candidates;
};

// Waiting: the capture's view shows a source that has no size yet.
enum class CaptureState { Stopped, Waiting, Running, Failed };

// Error stands for running out of memory.
enum class StartResult {
	Started,
	AlreadyRunning,
	NoSourceSize,
	SourceTooLarge,
	NoOutputSize,
	ViewFailed,
	NoEncoder,
	Error,
};

struct CaptureStats {
	CaptureState state = CaptureState::Stopped;
	// Failed because the encoder reported an error, not because a packet could not be
	// stored.
	bool encoderFailed = false;
	std::string encoderId;
	// Of the encoder running; Texture while none is.
	EncoderPath encoderPath = EncoderPath::Texture;
	ReadbackReason readbackReason = ReadbackReason::None;
	// The encoder running is the one the user chose.
	bool chosenEncoder = false;
	// The user chose an encoder that could not start, or that OBS does not offer, so
	// another one runs.
	bool choiceSkipped = false;
	FrameSize outputSize;
	SourceBufferStats buffer;
	// From the oldest frame held to the newest.
	Nanoseconds bufferedDuration{0};
};

// One source rendered on its own view, encoded and kept in a SourceBuffer. Every call
// runs on the UI thread or a test thread, never inside a render callback or with the
// graphics context entered; nothing here throws into libobs.
class SourceCapture {
public:
	SourceCapture() = default;
	~SourceCapture();

	SourceCapture(const SourceCapture &) = delete;
	SourceCapture &operator=(const SourceCapture &) = delete;

	// Starts encoding source, which needs no reference beyond the call. The buffer is
	// emptied once the encoder has started unless keepBuffer is set; a kept buffer takes
	// the new byte budget and sees the restart as a discontinuity. A new length or frame
	// rate needs a new buffer, kept or not. Logs why when it does not start. A source
	// without a size stays shown on the capture's view (see hold), waiting, and gives
	// NoSourceSize; a later start uses that view. Any other failure leaves the capture
	// stopped, and one before an encoder has initialized leaves the buffer as it was.
	StartResult start(obs_source_t *source, const CaptureSettings &settings, bool keepBuffer = false);

	// Shows the source on the capture's view, without encoding. Display, window and game
	// captures have no size until something shows them, so this is what lets them get
	// one. Does nothing while running; stop() lets go of the source.
	void hold(obs_source_t *source);

	// Stops encoding and tears the view down, waiting as long as libobs takes. The buffer
	// keeps its content.
	void stop();

	// Running, or failed and still to be stopped.
	bool active() const noexcept { return output_ != nullptr; }
	bool waiting() const noexcept { return view_ && !output_; }

	// False when the source no longer has the size the capture started with; the view
	// does not follow it until a restart.
	bool sourceSizeMatches() const;

	// The bytes the buffer would hold over its whole length at the bitrate of the encoder
	// running; zero while none runs. The buffers share the memory budget by these.
	size_t byteNeed() const noexcept { return byteNeed_; }
	// Has the buffer hold no more than this, nor more than its need.
	void limitBytes(size_t bytes);
	// A buffer kept through a restart needs what the encoder before wrote until its whole
	// length has passed, and from then on what the encoder running writes.
	void settleByteNeed(Nanoseconds now) noexcept;
	// What a buffer started now with these settings would need, as start() works it out
	// for the first encoder it tries; zero when nothing could capture the source. A source
	// with no picture yet, as a camera not shown since OBS started, is taken at the size of
	// the canvas.
	static size_t estimateByteNeed(obs_source_t *source, const CaptureSettings &settings);
	CaptureStats stats() const;

	// Null before the first start. The object only changes at a start that needs a new
	// buffer.
	const SourceBuffer *buffer() const noexcept { return buffer_.get(); }
	// Lets the buffer go of what ends before its window back from now; for a capture that
	// is not running, whose buffer gets no packets to evict at.
	void expireBuffer(Nanoseconds now);

private:
	void tearDown();

	std::unique_ptr<SourceBuffer> buffer_;
	SourceBufferConfig bufferConfig_;
	CaptureTarget target_;
	// The view's reference goes when the source is removed.
	OBSWeakSourceAutoRelease source_;
	FrameSize sourceSize_;
	FrameSize outputSize_;
	obs_view_t *view_ = nullptr;
	obs_encoder_t *encoder_ = nullptr;
	obs_output_t *output_ = nullptr;
	std::string encoderId_;
	EncoderPath encoderPath_ = EncoderPath::Texture;
	ReadbackReason readbackReason_ = ReadbackReason::None;
	size_t byteNeed_ = 0;
	size_t encoderNeed_ = 0;
	Nanoseconds keptNeedUntil_{0};
	bool chosenEncoder_ = false;
	bool choiceSkipped_ = false;
};

} // namespace tapeloop::obs
