// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#ifdef __linux__

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tapeloop::test {

// A child process that holds a lease on a file, as a file server does for a client that
// has the file open, and lets it go a while after the kernel asks it to. A read lease is
// broken by an open for writing, a write lease by any open. One holder at a time: the
// child of a second one would keep the first one's pipe open.
class LeaseHolder {
public:
	enum class Kind { Read, Write };

	LeaseHolder(const std::filesystem::path &path, Kind kind, std::chrono::milliseconds letGoAfter)
	{
		int ready[2];
		int go[2];
		if (::pipe2(ready, O_CLOEXEC) != 0) {
			throw std::system_error(errno, std::generic_category(), "cannot make a pipe");
		}
		if (::pipe2(go, O_CLOEXEC) != 0) {
			const int code = errno;
			::close(ready[0]);
			::close(ready[1]);
			throw std::system_error(code, std::generic_category(), "cannot make a pipe");
		}
		child_ = ::fork();
		if (child_ == 0) {
			::close(ready[0]);
			::close(go[1]);
			hold(path.c_str(), kind == Kind::Read ? F_RDLCK : F_WRLCK, static_cast<int>(letGoAfter.count()),
			     ready[1], go[0]);
		}
		const int code = errno;
		::close(ready[1]);
		::close(go[0]);
		if (child_ < 0) {
			::close(ready[0]);
			::close(go[1]);
			throw std::system_error(code, std::generic_category(), "cannot fork");
		}
		go_ = go[1];
		char held = 0;
		while (::read(ready[0], &held, 1) < 0 && errno == EINTR) {
		}
		::close(ready[0]);
		held_ = held == 1;
	}

	~LeaseHolder() { letGo(); }

	LeaseHolder(const LeaseHolder &) = delete;
	LeaseHolder &operator=(const LeaseHolder &) = delete;

	// False where the file system or the system's settings allow no lease.
	bool held() const { return held_; }

	// Has the child let the lease go now, if it still holds it, and tells whether the
	// kernel had asked it to.
	bool letGo()
	{
		if (go_ >= 0) {
			// The child sees its end of the pipe close.
			::close(go_);
			go_ = -1;
		}
		if (child_ > 0) {
			int status = 0;
			while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
			}
			asked_ = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			child_ = -1;
		}
		return asked_;
	}

private:
	// Runs in the child, so it keeps to system calls.
	[[noreturn]] static void hold(const char *path, int type, int letGoAfterMs, int ready, int go)
	{
		// The kernel asks with SIGIO, which would end the child.
		sigset_t breaks;
		sigemptyset(&breaks);
		sigaddset(&breaks, SIGIO);
		sigprocmask(SIG_BLOCK, &breaks, nullptr);
		const int asked = ::signalfd(-1, &breaks, SFD_CLOEXEC);
		const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
		const char held = asked >= 0 && fd >= 0 && ::fcntl(fd, F_SETLEASE, type) == 0 ? 1 : 0;
		[[maybe_unused]] const ssize_t written = ::write(ready, &held, 1);
		if (held == 0) {
			::_exit(2);
		}
		pollfd waits[2] = {{asked, POLLIN, 0}, {go, POLLIN, 0}};
		while (::poll(waits, 2, -1) < 0 && errno == EINTR) {
		}
		const bool wasAsked = (waits[0].revents & POLLIN) != 0;
		if (wasAsked) {
			pollfd until{go, POLLIN, 0};
			while (::poll(&until, 1, letGoAfterMs) < 0 && errno == EINTR) {
			}
		}
		::fcntl(fd, F_SETLEASE, F_UNLCK);
		::_exit(wasAsked ? 0 : 1);
	}

	pid_t child_ = -1;
	int go_ = -1;
	bool held_ = false;
	bool asked_ = false;
};

} // namespace tapeloop::test

#endif
