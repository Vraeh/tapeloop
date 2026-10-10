// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/FileIo.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

namespace tapeloop {
namespace {

// Large enough that a GOP is one call, small enough for the 32-bit sizes of the Windows
// calls.
constexpr size_t kMaxTransfer = size_t{1} << 30;

std::string describe(std::string_view what, const std::filesystem::path &path)
{
	std::string text(what);
	text += " ";
	text += utf8FromPath(path);
	return text;
}

#ifdef _WIN32

// The code is read before the message is built, which can change it.
[[noreturn]] void throwLastError(std::string_view what, const std::filesystem::path *path = nullptr)
{
	const auto code = static_cast<int>(GetLastError());
	throw std::system_error(code, std::system_category(), path ? describe(what, *path) : std::string(what));
}

#else

// The code is read before the message is built, which can change it.
[[noreturn]] void throwErrno(std::string_view what, const std::filesystem::path *path = nullptr)
{
	const int code = errno;
	throw std::system_error(code, std::generic_category(), path ? describe(what, *path) : std::string(what));
}

int openFile(const std::filesystem::path &path, int flags)
{
	int fd = -1;
	do {
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open takes the mode of a new file as a variadic argument.
		fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
	} while (fd < 0 && errno == EINTR);
	return fd;
}

#endif

} // namespace

#ifdef _WIN32

std::filesystem::path pathFromUtf8(std::string_view utf8)
{
	if (utf8.empty()) {
		return {};
	}
	if (utf8.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
		throw std::length_error("path too long");
	}
	const int length = static_cast<int>(utf8.size());
	const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
	std::wstring wide(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, wide.data(), size);
	return {wide};
}

std::string utf8FromPath(const std::filesystem::path &path)
{
	const std::wstring &wide = path.native();
	if (wide.empty()) {
		return {};
	}
	if (wide.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
		throw std::length_error("path too long");
	}
	const int length = static_cast<int>(wide.size());
	const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), length, utf8.data(), size, nullptr, nullptr);
	return utf8;
}

#else

// The file systems Tapeloop runs on name files in bytes, which OBS gives as UTF-8.
std::filesystem::path pathFromUtf8(std::string_view utf8)
{
	return {std::string(utf8)};
}

std::string utf8FromPath(const std::filesystem::path &path)
{
	return path.string();
}

#endif

File::File(File &&other) noexcept
#ifdef _WIN32
	: handle_(std::exchange(other.handle_, nullptr))
#else
	: fd_(std::exchange(other.fd_, -1))
#endif
{
}

File &File::operator=(File &&other) noexcept
{
	if (this != &other) {
		close();
#ifdef _WIN32
		handle_ = std::exchange(other.handle_, nullptr);
#else
		fd_ = std::exchange(other.fd_, -1);
#endif
	}
	return *this;
}

File::~File()
{
	close();
}

#ifdef _WIN32

File::File(const std::filesystem::path &path, Mode mode)
{
	const DWORD access = mode == Mode::ReadOnly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
	const DWORD disposition = mode == Mode::CreateNew ? CREATE_NEW : OPEN_EXISTING;
	// Readers share with the writer that appends to the same segment.
	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	HANDLE handle = CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE) {
		throwLastError("cannot open", &path);
	}
	handle_ = handle;
}

bool File::isOpen() const noexcept
{
	return handle_ != nullptr;
}

void File::writeAt(uint64_t offset, std::span<const uint8_t> bytes)
{
	while (!bytes.empty()) {
		const auto chunk = static_cast<DWORD>(std::min(bytes.size(), kMaxTransfer));
		OVERLAPPED at{};
		at.Offset = static_cast<DWORD>(offset);
		at.OffsetHigh = static_cast<DWORD>(offset >> 32);
		DWORD written = 0;
		if (!WriteFile(handle_, bytes.data(), chunk, &written, &at)) {
			throwLastError("cannot write");
		}
		offset += written;
		bytes = bytes.subspan(written);
	}
}

size_t File::readAt(uint64_t offset, std::span<uint8_t> bytes)
{
	size_t total = 0;
	while (total < bytes.size()) {
		const auto chunk = static_cast<DWORD>(std::min(bytes.size() - total, kMaxTransfer));
		OVERLAPPED at{};
		at.Offset = static_cast<DWORD>(offset);
		at.OffsetHigh = static_cast<DWORD>(offset >> 32);
		DWORD read = 0;
		if (!ReadFile(handle_, bytes.data() + total, chunk, &read, &at)) {
			if (GetLastError() == ERROR_HANDLE_EOF) {
				break;
			}
			throwLastError("cannot read");
		}
		if (read == 0) {
			break;
		}
		offset += read;
		total += read;
	}
	return total;
}

uint64_t File::size() const
{
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(handle_, &size)) {
		throwLastError("cannot read the size of a file");
	}
	return static_cast<uint64_t>(size.QuadPart);
}

void File::flush()
{
	if (!FlushFileBuffers(handle_)) {
		throwLastError("cannot flush a file");
	}
}

void File::close() noexcept
{
	if (handle_ != nullptr) {
		CloseHandle(handle_);
		handle_ = nullptr;
	}
}

void syncDirectory(const std::filesystem::path &) {}

void renameFile(const std::filesystem::path &from, const std::filesystem::path &to)
{
	if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		throwLastError("cannot rename", &from);
	}
}

bool lowerThreadIoPriority() noexcept
{
	// Lowers the thread's CPU priority as well, which suits a thread that only writes.
	return SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
}

#else

File::File(const std::filesystem::path &path, Mode mode)
{
	int flags = O_RDONLY;
	if (mode == Mode::CreateNew) {
		flags = O_RDWR | O_CREAT | O_EXCL;
	} else if (mode == Mode::ReadWrite) {
		flags = O_RDWR;
	}
	fd_ = openFile(path, flags);
	if (fd_ < 0) {
		throwErrno("cannot open", &path);
	}
}

bool File::isOpen() const noexcept
{
	return fd_ >= 0;
}

void File::writeAt(uint64_t offset, std::span<const uint8_t> bytes)
{
	while (!bytes.empty()) {
		const ssize_t written =
			::pwrite(fd_, bytes.data(), std::min(bytes.size(), kMaxTransfer), static_cast<off_t>(offset));
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			throwErrno("cannot write");
		}
		offset += static_cast<uint64_t>(written);
		bytes = bytes.subspan(static_cast<size_t>(written));
	}
}

size_t File::readAt(uint64_t offset, std::span<uint8_t> bytes)
{
	size_t total = 0;
	while (total < bytes.size()) {
		const ssize_t read = ::pread(fd_, bytes.data() + total, std::min(bytes.size() - total, kMaxTransfer),
					     static_cast<off_t>(offset));
		if (read < 0) {
			if (errno == EINTR) {
				continue;
			}
			throwErrno("cannot read");
		}
		if (read == 0) {
			break;
		}
		offset += static_cast<uint64_t>(read);
		total += static_cast<size_t>(read);
	}
	return total;
}

uint64_t File::size() const
{
	struct stat info{};
	if (::fstat(fd_, &info) != 0) {
		throwErrno("cannot read the size of a file");
	}
	return static_cast<uint64_t>(info.st_size);
}

void File::flush()
{
#ifdef __APPLE__
	// fsync on macOS leaves the data in the drive's own cache; F_FULLFSYNC flushes that
	// too, and is refused by some file systems, which fsync then covers.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl takes its argument as a variadic one.
	if (::fcntl(fd_, F_FULLFSYNC) == 0) {
		return;
	}
#endif
	if (::fsync(fd_) != 0) {
		throwErrno("cannot flush a file");
	}
}

void File::close() noexcept
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
}

void syncDirectory(const std::filesystem::path &directory)
{
	const int fd = openFile(directory, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		throwErrno("cannot open the directory", &directory);
	}
#ifdef __APPLE__
	// As for a file: plain fsync leaves the rename in the drive's cache.
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fcntl takes its argument as a variadic one.
	int result = ::fcntl(fd, F_FULLFSYNC);
	if (result != 0) {
		result = ::fsync(fd);
	}
#else
	const int result = ::fsync(fd);
#endif
	const int error = errno;
	::close(fd);
	if (result != 0) {
		throw std::system_error(error, std::generic_category(),
					describe("cannot flush the directory", directory));
	}
}

void renameFile(const std::filesystem::path &from, const std::filesystem::path &to)
{
	if (::rename(from.c_str(), to.c_str()) != 0) {
		throwErrno("cannot rename", &from);
	}
	syncDirectory(to.parent_path());
}

bool lowerThreadIoPriority() noexcept
{
#if defined(__linux__)
	// The lowest level of the best-effort class, from linux/ioprio.h, which not every
	// distribution ships; who 1 with id 0 is the calling thread.
	constexpr long kWhoProcess = 1;
	constexpr long kClassBestEffort = 2;
	constexpr long kClassShift = 13;
	constexpr long kLowestLevel = 7;
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall takes the call's arguments as variadic ones.
	return ::syscall(SYS_ioprio_set, kWhoProcess, 0L, kClassBestEffort << kClassShift | kLowestLevel) == 0;
#elif defined(__APPLE__)
	return ::setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE) == 0;
#else
	return false;
#endif
}

#endif

} // namespace tapeloop
