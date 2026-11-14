// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tapeloop::test {

// A named pipe made at a path, and a thread that opens its writing end every couple of
// seconds while the object lives. A reader that waits in open() for a writer is let go
// that way, so a test that expects a pipe to be refused at once fails when the reader
// waits, instead of hanging; released() tells whether the watchdog had to let one go.
class FifoWatchdog {
public:
	explicit FifoWatchdog(std::filesystem::path path) : path_(std::move(path))
	{
		if (::mkfifo(path_.c_str(), 0600) != 0) {
			throw std::system_error(errno, std::generic_category(), "cannot make a named pipe");
		}
		thread_ = std::thread([this] { watch(); });
	}

	~FifoWatchdog()
	{
		{
			const std::lock_guard lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
		thread_.join();
	}

	FifoWatchdog(const FifoWatchdog &) = delete;
	FifoWatchdog &operator=(const FifoWatchdog &) = delete;

	bool released() const
	{
		const std::lock_guard lock(mutex_);
		return released_;
	}

private:
	// Far longer than a refusal takes, even under the sanitizers.
	static constexpr std::chrono::seconds kPatience{2};

	void watch()
	{
		std::unique_lock lock(mutex_);
		while (!wake_.wait_for(lock, kPatience, [this] { return stopping_; })) {
			// Without waiting, the writing end opens only while a reader has the pipe
			// open, which a reader waiting in open() does.
			const int fd = ::open(path_.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd >= 0) {
				released_ = true;
				::close(fd);
			}
		}
	}

	const std::filesystem::path path_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	bool stopping_ = false;
	bool released_ = false;
	std::thread thread_;
};

} // namespace tapeloop::test

#endif
