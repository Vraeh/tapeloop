// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FileIo.hpp"
#include "core/Gop.hpp"
#include "core/ReplayExport.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

namespace tapeloop::decode {

// An MP4 file of an export, written by FFmpeg's muxer from the packets as they were
// encoded: H.264 as avc1 and HEVC as hvc1, which QuickTime needs. Each configuration of
// the codec gets a sample entry of its own, and the frames before the in point, which
// only serve decoding, are left out of what plays by an edit list. The moov box goes at
// the end, so the file is written in one pass. The file is created anew: open() fails
// when one is at its path.
class Mp4Writer final : public VideoFileWriter {
public:
	Mp4Writer() = default;
	~Mp4Writer() override;

	Mp4Writer(const Mp4Writer &) = delete;
	Mp4Writer &operator=(const Mp4Writer &) = delete;
	Mp4Writer(Mp4Writer &&) = delete;
	Mp4Writer &operator=(Mp4Writer &&) = delete;

	void open(const std::filesystem::path &path, const Gop &first) override;
	// A GOP of another picture size does not fit: the sample entry and the track header
	// carry the size of the first.
	bool fits(const Gop &gop) const override;
	void write(const Gop &gop, size_t packet, Nanoseconds time) override;
	void finish() override;

private:
	void close() noexcept;
	// Throws for an FFmpeg call that failed, with the error of the file when that was it.
	[[noreturn]] void fail(const std::string &what, int code) const;
	static int writeIo(void *opaque, const uint8_t *data, int size) noexcept;
	static int64_t seekIo(void *opaque, int64_t offset, int whence) noexcept;

	AVFormatContext *context_ = nullptr;
	AVIOContext *io_ = nullptr;
	File file_;
	// Where the muxer writes next, and what failed in the file, if anything did.
	uint64_t position_ = 0;
	std::error_code ioError_;
	AVPacket *packet_ = nullptr;
	// The configuration of the last packet written.
	CodecConfig config_;
	int width_ = 0;
	int height_ = 0;
	int64_t frameTicks_ = 1;
	int64_t lastDts_ = std::numeric_limits<int64_t>::min();
};

} // namespace tapeloop::decode
