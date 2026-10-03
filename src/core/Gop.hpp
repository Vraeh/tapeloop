// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/MediaTime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tapeloop {

// One packet as the encoder hands it over. The data is only borrowed for the duration
// of the call that receives it.
struct EncodedPacket {
	std::span<const uint8_t> data;
	int64_t pts = 0;
	int64_t dts = 0;
	Nanoseconds time{0};
	bool keyframe = false;
};

// The codec configuration an encoder reports for one run, as obs_encoder_get_extra_data
// gives it: the parameter sets a decoder needs before the first keyframe.
using CodecConfig = std::vector<uint8_t>;

struct PacketRecord {
	int64_t pts = 0;
	int64_t dts = 0;
	Nanoseconds time{0};
	bool keyframe = false;
	size_t offset = 0;
	size_t size = 0;
};

// A group of pictures: one contiguous block with the data of every packet, plus a
// table describing each packet, plus the codec configuration of its run. The first
// packet is always a keyframe and times are strictly increasing. Immutable once built,
// so it can be shared between threads.
class Gop {
public:
	// Only GopBuilder can make a Key, so only it can build a Gop, while make_shared
	// can still reach the constructor.
	class Key {
		friend class GopBuilder;
		Key() = default;
	};

	Gop(Key, std::vector<uint8_t> bytes, std::vector<PacketRecord> packets, Nanoseconds frameDuration,
	    std::shared_ptr<const CodecConfig> codecConfig);

	std::span<const PacketRecord> packets() const noexcept { return packets_; }
	std::span<const uint8_t> packetData(size_t index) const noexcept;

	// The configuration of the run the GOP belongs to: every GOP of a run points at the
	// same object, valid as long as any of them lives. Null when the encoder reported
	// none.
	const CodecConfig *codecConfig() const noexcept { return codecConfig_.get(); }

	Nanoseconds startTime() const noexcept { return packets_.front().time; }
	Nanoseconds lastTime() const noexcept { return packets_.back().time; }
	// The end of the last frame, one frame duration after it starts.
	Nanoseconds endTime() const noexcept { return saturatingAdd(lastTime(), frameDuration_); }

	size_t byteSize() const noexcept { return bytes_.size(); }

private:
	std::vector<uint8_t> bytes_;
	std::vector<PacketRecord> packets_;
	Nanoseconds frameDuration_;
	std::shared_ptr<const CodecConfig> codecConfig_;
};

// Collects the packets of the GOP being encoded. Its buffers keep their capacity from
// one GOP to the next and reserve headroom at the start of each GOP, so in steady
// state appending does not allocate; sealing copies the packets into a Gop of exactly
// the right size.
class GopBuilder {
public:
	explicit GopBuilder(Nanoseconds frameDuration);

	bool empty() const noexcept { return packets_.empty(); }
	size_t byteSize() const noexcept { return bytes_.size(); }
	Nanoseconds startTime() const noexcept { return packets_.front().time; }
	Nanoseconds lastTime() const noexcept { return packets_.back().time; }

	// The first packet of a GOP must be a keyframe, and times must increase. If it
	// throws, nothing was appended.
	void append(const EncodedPacket &packet);

	// Returns the collected packets as a Gop and starts a new one. Must not be empty.
	// If it throws, the builder still holds every packet.
	std::shared_ptr<const Gop> seal();

	// An immutable copy of the packets collected so far. Must not be empty.
	std::shared_ptr<const Gop> snapshot() const;

	// Drops the packets and starts a new GOP, keeping the buffers.
	void clear() noexcept;

	// The configuration the GOPs built from now on point at. The builder must be empty.
	void setCodecConfig(std::shared_ptr<const CodecConfig> codecConfig) noexcept;
	const CodecConfig *codecConfig() const noexcept { return codecConfig_.get(); }

private:
	std::shared_ptr<const Gop> makeGop() const;

	Nanoseconds frameDuration_;
	std::shared_ptr<const CodecConfig> codecConfig_;
	std::vector<uint8_t> bytes_;
	std::vector<PacketRecord> packets_;
	// Size of the last GOP, to reserve room for the next one.
	size_t lastByteCount_ = 0;
	size_t lastPacketCount_ = 0;
};

} // namespace tapeloop
