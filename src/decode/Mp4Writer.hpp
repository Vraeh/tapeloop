// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"
#include "core/ReplayExport.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>

struct AVFormatContext;
struct AVPacket;

namespace tapeloop::decode {

// An MP4 file of an export, written by FFmpeg's muxer from the packets as they were
// encoded: H.264 as avc1 and HEVC as hvc1, which QuickTime needs. Each configuration of
// the codec gets a sample entry of its own, and the frames before the in point, which
// only serve decoding, are left out of what plays by an edit list. The moov box goes at
// the end, so the file is written in one pass.
class Mp4Writer final : public VideoFileWriter {
public:
	Mp4Writer() = default;
	~Mp4Writer() override;

	Mp4Writer(const Mp4Writer &) = delete;
	Mp4Writer &operator=(const Mp4Writer &) = delete;
	Mp4Writer(Mp4Writer &&) = delete;
	Mp4Writer &operator=(Mp4Writer &&) = delete;

	void open(const std::filesystem::path &path, const Gop &first) override;
	void write(const Gop &gop, size_t packet, Nanoseconds time) override;
	void finish() override;

private:
	void close() noexcept;

	AVFormatContext *context_ = nullptr;
	AVPacket *packet_ = nullptr;
	// The configuration of the last packet written.
	CodecConfig config_;
	int64_t frameTicks_ = 1;
	int64_t lastDts_ = std::numeric_limits<int64_t>::min();
};

} // namespace tapeloop::decode
