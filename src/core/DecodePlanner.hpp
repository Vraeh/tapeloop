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

inline constexpr size_t kDefaultDecodedCacheBytes = size_t{512} << 20;

struct DecodePlannerConfig {
	// GOPs whose decoded frames are kept, for stepping back and playing in reverse; at
	// least one. Two let reverse play decode the GOP before the one on screen ahead of
	// time.
	size_t keptGops = 2;
	// The most memory the kept frames may hold. What does not fit is not kept, and
	// stepping back or playing in reverse decode it again when they need it; the frame
	// asked for is kept whatever its size.
	size_t maxBytes = kDefaultDecodedCacheBytes;
};

// Where a replay's decoded frames live. A discrete adapter decodes them into its own
// video memory. The CPU path keeps them in system memory, and so does an integrated
// adapter, whose own memory is at most a small part of system memory set aside for it.
enum class FrameMemory { Video, System };

struct MemorySizes {
	uint64_t dedicatedVideo = 0;
	uint64_t system = 0;
};

// The sizes of the decoded-frame cache a user may set, in whole MiB: 128 MiB to a quarter
// of the memory its frames live in, at most 4 GiB, and never below the 128 MiB.
struct DecodedCacheBounds {
	uint64_t leastMiB = 0;
	uint64_t mostMiB = 0;
};

DecodedCacheBounds decodedCacheBounds(FrameMemory where, MemorySizes sizes) noexcept;
// The cache's size in bytes for a setting in MiB, kept within its bounds.
size_t decodedCacheBytes(int64_t settingMiB, FrameMemory where, MemorySizes sizes) noexcept;

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
// decoded frames of the last GOPs are kept, within maxBytes, so that stepping back costs
// nothing while they fit, a new run opens the decoder again, and reverse play can have
// the previous GOP decoded ahead.
// Runs are told apart by their codec and configuration; two runs of one codec without a
// configuration carry their parameter sets in the stream, so a reset is enough between
// them. A frame the decoder never gives is reported as InvalidData, without decoding its
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

	// Decodes ahead what play in that direction needs next: in reverse, the GOP before the
	// one last shown, keeping of it the last frames that fit. Playing forward needs
	// nothing ahead, since its next frames follow from what was decoded. Does nothing when
	// only one GOP is kept, or when the GOP on screen leaves no room for a frame.
	DecodeStatus prefetch(PlayDirection direction);

	const DecodeWork &work() const noexcept { return work_; }
	size_t keptGopCount() const noexcept { return kept_.size(); }
	size_t heldFrames() const noexcept;
	size_t heldBytes() const noexcept { return heldBytes_; }

private:
	struct KeptGop {
		size_t gop = 0;
		// By packet index within the GOP, in decode order.
		std::vector<std::optional<DecodedFrame>> frames;
		// Frames given back to stay within maxBytes, which a pass from the keyframe
		// brings back; by packet index, as frames.
		std::vector<char> dropped;
		size_t received = 0;
		// The packet after the last frame any pass over this GOP brought out: a frame
		// before it that is neither held nor given back is one the decoder does not give,
		// since a decoder skips the same frames on every pass.
		size_t reached = 0;
		// Every frame the decoder gives for this GOP came out: all of them, or what was
		// left once it was flushed at the end of the GOP.
		bool complete = false;
	};

	KeptGop &keep(size_t gop);
	KeptGop *find(size_t gop) noexcept;
	void release(KeptGop &kept) noexcept;
	void releaseAll() noexcept;
	void drop(KeptGop &kept, size_t packet) noexcept;
	// Gives back kept frames until a frame of that size fits: first those of GOPs other
	// than its own and the one on screen, from their end farthest from it; then those of
	// its own GOP past the one asked for, latest first; then its earliest. Never the one
	// asked for.
	// False when the frame still does not fit.
	bool makeRoom(KeptGop &kept, size_t bytes, size_t wanted) noexcept;
	// Decodes GOP gop until the frame of packet `packet` has come out or the pass has gone
	// past it, or the whole GOP when packet is past its end.
	DecodeStatus decode(size_t gop, size_t packet);
	DecodeStatus startAt(size_t gop);
	DecodeStatus receiveAll(KeptGop &kept, size_t wanted);
	// Ends a pass that failed. A lost device also lets go of every kept frame and of
	// the decoder, which the next pass opens again.
	DecodeStatus fail(DecodeStatus status) noexcept;

	FrameDecoder &decoder_;
	DecodePlannerConfig config_;
	Clip clip_;
	std::vector<KeptGop> kept_;
	size_t heldBytes_ = 0;
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
	// The packet after the last one whose frame came out in this pass over the GOP.
	size_t passNext_ = 0;
	bool flushed_ = false;
	bool fresh_ = true;
	std::optional<size_t> lastGop_;
};

} // namespace tapeloop
