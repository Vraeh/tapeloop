// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Gop.hpp"

#include <cassert>
#include <utility>

namespace tapeloop {

std::optional<VideoCodec> videoCodecFromName(std::string_view name) noexcept
{
	if (name == "h264") {
		return VideoCodec::H264;
	}
	if (name == "hevc") {
		return VideoCodec::Hevc;
	}
	return std::nullopt;
}

Gop::Gop(Key, std::vector<uint8_t> bytes, std::vector<PacketRecord> packets, Nanoseconds frameDuration,
	 VideoCodec codec, std::shared_ptr<const CodecConfig> codecConfig)
	: bytes_(std::move(bytes)),
	  packets_(std::move(packets)),
	  frameDuration_(frameDuration),
	  codec_(codec),
	  codecConfig_(std::move(codecConfig))
{
	assert(!packets_.empty() && packets_.front().keyframe);
}

std::span<const uint8_t> Gop::packetData(size_t index) const noexcept
{
	const PacketRecord &packet = packets_[index];
	return std::span<const uint8_t>(bytes_).subspan(packet.offset, packet.size);
}

GopBuilder::GopBuilder(Nanoseconds frameDuration) : frameDuration_(frameDuration) {}

void GopBuilder::append(const EncodedPacket &packet)
{
	assert(!packets_.empty() || packet.keyframe);
	assert(packets_.empty() || packet.time > packets_.back().time);

	if (packets_.empty()) {
		// Room for a GOP half again as large as the last one, so that a slightly
		// larger GOP does not reallocate halfway through. Capacity is kept from one
		// GOP to the next, so this only allocates while GOPs keep growing.
		bytes_.reserve(lastByteCount_ + lastByteCount_ / 2);
		packets_.reserve(lastPacketCount_ + lastPacketCount_ / 2);
	}

	const size_t offset = bytes_.size();
	bytes_.insert(bytes_.end(), packet.data.begin(), packet.data.end());
	try {
		packets_.push_back({packet.pts, packet.dts, packet.time, packet.keyframe, offset, packet.data.size()});
	} catch (...) {
		bytes_.resize(offset);
		throw;
	}
}

std::shared_ptr<const Gop> GopBuilder::seal()
{
	std::shared_ptr<const Gop> gop = makeGop();
	clear();
	return gop;
}

std::shared_ptr<const Gop> GopBuilder::snapshot() const
{
	return makeGop();
}

void GopBuilder::clear() noexcept
{
	if (packets_.empty()) {
		return;
	}
	lastByteCount_ = bytes_.size();
	lastPacketCount_ = packets_.size();
	bytes_.clear();
	packets_.clear();
}

void GopBuilder::setCodecConfig(VideoCodec codec, std::shared_ptr<const CodecConfig> codecConfig) noexcept
{
	assert(packets_.empty());
	codec_ = codec;
	codecConfig_ = std::move(codecConfig);
}

std::shared_ptr<const Gop> GopBuilder::makeGop() const
{
	assert(!packets_.empty());
	return std::make_shared<const Gop>(Gop::Key{}, bytes_, packets_, frameDuration_, codec_, codecConfig_);
}

} // namespace tapeloop
