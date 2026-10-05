// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Clip.hpp"
#include "core/FrameDecoder.hpp"
#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tapeloop {

enum class PlayDirection { Forward, Backward };

struct DecodePlannerConfig {
	// GOPs whose decoded frames are kept, for stepping back and playing in reverse; at
	// least one. Two let reverse play decode the GOP before the one on screen ahead of
	// time.
	size_t keptGops = 2;
};

// What the planner asked of its decoder, for tests and measurements.
struct DecodeWork {
	uint64_t opens = 0;
	uint64_t resets = 0;
	uint64_t flushes = 0;
	uint64_t packetsSent = 0;
	uint64_t framesReceived = 0;
};

struct DecodeResult {
	DecodeStatus status = DecodeStatus::Ok;
	// Only meaningful when status is Ok. Owned by the planner, valid until its next call.
	DecodedFrame frame;
};

// Decides what to feed a FrameDecoder so that the frame the playhead wants is ready:
// decoding starts at the keyframe of its GOP and goes no further than needed, the
// decoded frames of the last GOPs are kept so that stepping back costs nothing, a new
// run opens the decoder again, and reverse play can have the previous GOP decoded ahead.
// A frame the decoder never gives is reported as InvalidData, without decoding its
// GOP again. Once warm, deciding allocates nothing unless a GOP is longer than any kept
// before. Everything runs on the caller's thread.
class DecodePlanner {
public:
	explicit DecodePlanner(FrameDecoder &decoder, DecodePlannerConfig config = {});
	// Gives every kept frame back and closes the decoder.
	~DecodePlanner();

	DecodePlanner(const DecodePlanner &) = delete;
	DecodePlanner &operator=(const DecodePlanner &) = delete;
	DecodePlanner(DecodePlanner &&) = delete;
	DecodePlanner &operator=(DecodePlanner &&) = delete;

	// Starts over with another clip; the frames of the previous one go back to the
	// decoder.
	void load(Clip clip);

	// The frame on screen at time t, as Clip::locate picks it, decoding what is missing.
	DecodeResult frameAt(Nanoseconds t);

	// Decodes ahead what play in that direction needs next: in reverse, the whole GOP
	// before the one last shown. Playing forward needs nothing ahead, since its next
	// frames follow from what was decoded. Does nothing when only one GOP is kept.
	DecodeStatus prefetch(PlayDirection direction);

	const DecodeWork &work() const noexcept { return work_; }
	size_t keptGopCount() const noexcept { return kept_.size(); }
	size_t heldFrames() const noexcept;

private:
	struct KeptGop {
		size_t gop = 0;
		// By packet index within the GOP, in decode order.
		std::vector<std::optional<DecodedFrame>> frames;
		size_t received = 0;
		// Every frame the decoder gives for this GOP is in: all of them, or what was left
		// once it was flushed at the end of the GOP.
		bool complete = false;
	};

	KeptGop &keep(size_t gop);
	KeptGop *find(size_t gop) noexcept;
	void release(KeptGop &kept) noexcept;
	void releaseAll() noexcept;
	// Decodes GOP gop until the frame of packet `packet` has come out, or the whole GOP
	// when packet is past its end.
	DecodeStatus decode(size_t gop, size_t packet);
	DecodeStatus startAt(size_t gop);
	DecodeStatus receiveAll(KeptGop &kept);
	// Ends a pass that failed. A lost device also lets go of every kept frame and of
	// the decoder, which the next pass opens again.
	DecodeStatus fail(DecodeStatus status) noexcept;

	FrameDecoder &decoder_;
	DecodePlannerConfig config_;
	Clip clip_;
	std::vector<KeptGop> kept_;
	DecodeWork work_;
	// The run the decoder is open for: the configuration object its GOPs share, which
	// may be null, and the codec.
	bool open_ = false;
	const CodecConfig *openConfig_ = nullptr;
	VideoCodec openCodec_ = VideoCodec::H264;
	// Where the decoder is in the stream: the GOP whose packets it was last given, the
	// next packet of it to send, whether it was flushed since, and whether it was given
	// anything at all since it was opened or reset.
	std::optional<size_t> streamGop_;
	size_t nextPacket_ = 0;
	bool flushed_ = false;
	bool fresh_ = true;
	std::optional<size_t> lastGop_;
};

} // namespace tapeloop
