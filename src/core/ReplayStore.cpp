// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayStore.hpp"

#include "core/FileIo.hpp"

#include <exception>
#include <iterator>
#include <utility>

namespace tapeloop {

ReplayStore::ReplayStore(ReplayWriterConfig config) : writer_(config), worker_([this] { run(); }) {}

ReplayStore::~ReplayStore()
{
	{
		const std::lock_guard lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	worker_.join();
}

uint64_t ReplayStore::write(ReplayCapture capture)
{
	Job job;
	job.kind = StoreResult::Kind::Capture;
	job.capture = std::move(capture);
	return enqueue(std::move(job));
}

uint64_t ReplayStore::writeTags(std::filesystem::path manifest, std::vector<std::string> tags)
{
	Job job;
	job.kind = StoreResult::Kind::Tags;
	job.path = std::move(manifest);
	job.tags = std::move(tags);
	return enqueue(std::move(job));
}

uint64_t ReplayStore::scan(std::filesystem::path base)
{
	Job job;
	job.kind = StoreResult::Kind::Scan;
	job.path = std::move(base);
	return enqueue(std::move(job));
}

std::vector<StoreResult> ReplayStore::poll()
{
	std::list<StoreResult> done;
	{
		const std::lock_guard lock(mutex_);
		done.swap(results_);
	}
	return {std::make_move_iterator(done.begin()), std::make_move_iterator(done.end())};
}

void ReplayStore::waitUntilIdle()
{
	std::unique_lock lock(mutex_);
	idle_.wait(lock, [this] { return jobs_.empty(); });
}

size_t ReplayStore::pending() const
{
	const std::lock_guard lock(mutex_);
	return jobs_.size();
}

std::optional<bool> ReplayStore::lowPriority() const noexcept
{
	const int state = lowPriority_.load();
	if (state == 0) {
		return std::nullopt;
	}
	return state == 1;
}

uint64_t ReplayStore::enqueue(Job job)
{
	uint64_t ticket = 0;
	job.slot.emplace_back();
	{
		const std::lock_guard lock(mutex_);
		ticket = nextTicket_++;
		job.ticket = ticket;
		jobs_.push_back(std::move(job));
	}
	wake_.notify_one();
	return ticket;
}

void ReplayStore::run() noexcept
{
	lowPriority_ = lowerThreadIoPriority() ? 1 : 2;
	std::unique_lock lock(mutex_);
	while (true) {
		wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
		if (jobs_.empty()) {
			return;
		}
		// The job stays at the front while it runs, since moving it out would allocate
		// with some standard libraries; a deque keeps an element in place while others
		// are added behind it.
		Job &job = jobs_.front();
		const bool stopping = stopping_;
		lock.unlock();
		job.slot.front() = execute(job, stopping);
		// The capture's GOPs go outside the lock, and before its result is seen.
		job.capture.sources.clear();
		lock.lock();
		results_.splice(results_.end(), job.slot);
		jobs_.pop_front();
		idle_.notify_all();
	}
}

StoreResult ReplayStore::execute(const Job &job, bool stopping) noexcept
{
	StoreResult result;
	result.ticket = job.ticket;
	result.kind = job.kind;
	try {
		switch (job.kind) {
		case StoreResult::Kind::Capture:
			result.written = writer_.write(job.capture);
			break;
		case StoreResult::Kind::Tags:
			result.manifest = job.path;
			writer_.writeTags(job.path, job.tags);
			break;
		case StoreResult::Kind::Scan:
			if (stopping) {
				result.error = "not scanned, the store is closing";
			} else {
				result.scan = scanReplays(job.path);
			}
			break;
		}
	} catch (const std::exception &e) {
		try {
			result.error = e.what();
		} catch (...) {
			// Short enough for the string's own buffer, so it needs no memory.
			result.error = "out of memory";
		}
		if (result.error.empty()) {
			result.error = "unknown error";
		}
	} catch (...) {
		result.error = "unknown error";
	}
	return result;
}

} // namespace tapeloop
