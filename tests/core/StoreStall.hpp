// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

namespace tapeloop::test {

// Stands for a disk that falls behind: the thread that lets go of a GOP held through the
// stall stops there until release(), so the store's thread, once it has written a capture
// with that GOP, leaves the captures asked for after it waiting.
class StoreStall {
public:
	StoreStall() : state_(std::make_shared<State>()) {}
	~StoreStall() { release(); }

	StoreStall(const StoreStall &) = delete;
	StoreStall &operator=(const StoreStall &) = delete;
	StoreStall(StoreStall &&) = delete;
	StoreStall &operator=(StoreStall &&) = delete;

	// The same GOP, through an owner that stops whoever lets go of it last. The thread
	// that does must not be the one that calls release().
	std::shared_ptr<const Gop> hold(std::shared_ptr<const Gop> gop) const
	{
		const Gop *held = gop.get();
		return std::shared_ptr<const Gop>(held, [state = state_, gop = std::move(gop)](const Gop *) {
			std::unique_lock lock(state->mutex);
			state->reached = true;
			state->changed.notify_all();
			state->changed.wait(lock, [&state] { return state->released; });
		});
	}

	// Returns once a thread has stopped at the stall.
	void waitReached() const
	{
		std::unique_lock lock(state_->mutex);
		state_->changed.wait(lock, [this] { return state_->reached; });
	}

	void release() const
	{
		{
			const std::lock_guard lock(state_->mutex);
			state_->released = true;
		}
		state_->changed.notify_all();
	}

private:
	// Shared with the owners it hands out, which can outlive the stall.
	struct State {
		std::mutex mutex;
		std::condition_variable changed;
		bool reached = false;
		bool released = false;
	};

	std::shared_ptr<State> state_;
};

} // namespace tapeloop::test
