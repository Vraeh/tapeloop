// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "UiTasks.hpp"

#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace tapeloop::test {
namespace {

std::mutex tasksMutex;
std::deque<std::pair<obs_task_t, void *>> tasks;
std::thread::id uiThread;

} // namespace

UiTasks::UiTasks()
{
	uiThread = std::this_thread::get_id();
	obs_set_ui_task_handler(queue);
}

UiTasks::~UiTasks()
{
	obs_set_ui_task_handler(nullptr);
	run();
}

size_t UiTasks::run()
{
	size_t ran = 0;
	while (true) {
		std::pair<obs_task_t, void *> task;
		{
			const std::lock_guard lock(tasksMutex);
			if (tasks.empty()) {
				return ran;
			}
			task = tasks.front();
			tasks.pop_front();
		}
		task.first(task.second);
		++ran;
	}
}

size_t UiTasks::pending()
{
	const std::lock_guard lock(tasksMutex);
	return tasks.size();
}

void UiTasks::queue(obs_task_t task, void *param, bool wait) noexcept
{
	// OBS's frontend posts the task with Qt's automatic connection, which calls it at once
	// on the UI thread. Nothing Tapeloop queues waits, and a task that did would wait for
	// the test thread.
	if (wait || std::this_thread::get_id() == uiThread) {
		task(param);
		return;
	}
	const std::lock_guard lock(tasksMutex);
	tasks.emplace_back(task, param);
}

} // namespace tapeloop::test
