// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace tapeloop {

// Paths come from OBS as UTF-8 on every platform. A std::filesystem::path built from a
// plain std::string takes the Windows code page instead, so paths are built with these.
std::filesystem::path pathFromUtf8(std::string_view utf8);
std::string utf8FromPath(const std::filesystem::path &path);

// A file for positioned reads and writes, which never move a shared file position, so
// one thread can write a file while another reads it. Failures throw std::system_error.
class File {
public:
	enum class Mode {
		// Creates the file for reading and writing; fails when it exists.
		CreateNew,
		ReadWrite,
		ReadOnly,
	};

	File() = default;
	File(const std::filesystem::path &path, Mode mode);
	~File();

	File(File &&other) noexcept;
	File &operator=(File &&other) noexcept;
	File(const File &) = delete;
	File &operator=(const File &) = delete;

	bool isOpen() const noexcept;
	void writeAt(uint64_t offset, std::span<const uint8_t> bytes);
	// How many bytes were read: fewer than asked only at the end of the file.
	size_t readAt(uint64_t offset, std::span<uint8_t> bytes);
	uint64_t size() const;
	// Returns once everything written so far is on the disk, not only in a cache.
	void flush();
	void close() noexcept;

private:
#ifdef _WIN32
	void *handle_ = nullptr;
#else
	int fd_ = -1;
#endif
};

// Makes a new or renamed entry of the directory durable. Windows has nothing to do here:
// its renames are written through and its file creation is durable with the file.
void syncDirectory(const std::filesystem::path &directory);

// Renames from to to, replacing to if it exists, and returns once the rename is durable.
void renameFile(const std::filesystem::path &from, const std::filesystem::path &to);

// Lowers the I/O priority of the calling thread, so that its writes yield to OBS's own
// recording on a shared disk. Best effort: false where the platform refuses or has no
// such setting.
bool lowerThreadIoPriority() noexcept;

} // namespace tapeloop
