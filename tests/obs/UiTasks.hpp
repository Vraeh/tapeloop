// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <obs.h>

#include <cstddef>

namespace tapeloop::test {

// Stands in for the frontend's UI thread, which is the test thread that makes it. As in
// OBS, a task queued from that thread runs at once, and one queued from any other waits
// until the test thread runs it. One at a time, made after libobs starts; whatever is
// still queued when it goes runs then.
class UiTasks {
public:
	UiTasks();
	~UiTasks();

	UiTasks(const UiTasks &) = delete;
	UiTasks &operator=(const UiTasks &) = delete;
	UiTasks(UiTasks &&) = delete;
	UiTasks &operator=(UiTasks &&) = delete;

	// Runs the tasks queued so far, and those they queue, and returns how many ran.
	static size_t run();
	static size_t pending();

private:
	static void queue(obs_task_t task, void *param, bool wait) noexcept;
};

} // namespace tapeloop::test
