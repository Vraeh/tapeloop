// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FrameDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace tapeloop::test {

// Decodes the way a decoder without B-frames does, holding `delay` frames back until
// more packets come or it is flushed. It checks how it is used and counts every misuse
// in violations: packets before open(), anything but a keyframe first after open() or
// reset(), a packet that is not the frame after the previous one, a packet after flush()
// without reset(), a frame released twice or never handed out.
class FakeDecoder : public FrameDecoder {
public:
	struct Session {
		VideoCodec codec;
		CodecConfig config;
	};

	struct Made {
		int64_t pts;
		// Index into sessions.
		size_t session;
	};

	FakeDecoder(std::function<bool(int64_t pts)> isKeyframe, size_t delay)
		: isKeyframe_(std::move(isKeyframe)),
		  delay_(delay)
	{
	}

	// What open() answers instead of opening, when not Ok.
	DecodeStatus openStatus = DecodeStatus::Ok;
	// A packet send() refuses once, as a corrupt one would be.
	std::optional<int64_t> failOnce;
	std::vector<Session> sessions;
	std::map<uint64_t, Made> made;
	std::set<uint64_t> outstanding;
	int violations = 0;

	DecodeStatus open(VideoCodec codec, std::span<const uint8_t> config) noexcept override
	{
		if (openStatus != DecodeStatus::Ok) {
			open_ = false;
			return openStatus;
		}
		sessions.push_back({codec, CodecConfig(config.begin(), config.end())});
		open_ = true;
		restart();
		return DecodeStatus::Ok;
	}

	DecodeStatus send(std::span<const uint8_t>, int64_t pts, int64_t) noexcept override
	{
		if (failOnce == pts) {
			failOnce.reset();
			return DecodeStatus::InvalidData;
		}
		const bool inOrder = expectKeyframe_ ? isKeyframe_(pts) : pts == lastPts_ + 1;
		if (!open_ || flushed_ || !inOrder) {
			++violations;
			return DecodeStatus::InvalidData;
		}
		expectKeyframe_ = false;
		lastPts_ = pts;
		const uint64_t id = nextId_++;
		made[id] = {pts, sessions.size() - 1};
		inside_.push_back(id);
		return DecodeStatus::Ok;
	}

	DecodeStatus receive(DecodedFrame &frame) noexcept override
	{
		if (inside_.empty() || (!flushed_ && inside_.size() <= delay_)) {
			return flushed_ ? DecodeStatus::Drained : DecodeStatus::NeedMore;
		}
		const uint64_t id = inside_.front();
		inside_.pop_front();
		outstanding.insert(id);
		frame = {id, made[id].pts};
		return DecodeStatus::Ok;
	}

	DecodeStatus flush() noexcept override
	{
		if (!open_) {
			++violations;
			return DecodeStatus::InvalidData;
		}
		flushed_ = true;
		return DecodeStatus::Ok;
	}

	void reset() noexcept override { restart(); }

	void release(const DecodedFrame &frame) noexcept override
	{
		if (outstanding.erase(frame.id) == 0) {
			++violations;
		}
	}

	void close() noexcept override
	{
		outstanding.clear();
		inside_.clear();
		open_ = false;
	}

	const Session &sessionOf(uint64_t id) const { return sessions[made.at(id).session]; }

private:
	void restart() noexcept
	{
		inside_.clear();
		flushed_ = false;
		expectKeyframe_ = true;
	}

	std::function<bool(int64_t pts)> isKeyframe_;
	size_t delay_;
	bool open_ = false;
	bool flushed_ = false;
	bool expectKeyframe_ = true;
	int64_t lastPts_ = 0;
	uint64_t nextId_ = 1;
	std::deque<uint64_t> inside_;
};

} // namespace tapeloop::test
