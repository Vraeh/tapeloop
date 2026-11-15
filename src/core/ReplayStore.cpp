// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayStore.hpp"

#include "core/FileIo.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <utility>

namespace tapeloop {

// Within a string's own buffer in every standard library.
static_assert(ReplayStore::kDropped.size() <= 15);

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

uint64_t ReplayStore::write(ReplayCapture capture, std::shared_ptr<ReplayFiles> files)
{
	Job job;
	job.kind = StoreResult::Kind::Capture;
	job.capture = std::move(capture);
	job.files = std::move(files);
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
	idle_.wait(lock, [this] { return jobs_.size() == dropped_; });
}

size_t ReplayStore::pending() const
{
	const std::lock_guard lock(mutex_);
	return jobs_.size() - dropped_;
}

size_t ReplayStore::heldBytes(std::span<const KeyedBuffer> buffers)
{
	const std::lock_guard lock(mutex_);
	return heldBytesLocked(buffers);
}

size_t ReplayStore::dropWaiting(size_t bound, std::span<const KeyedBuffer> buffers)
{
	size_t dropped = 0;
	while (true) {
		// Before the lock, so that the GOPs of a capture dropped go once it is released.
		std::vector<CaptureSource> released;
		const std::lock_guard lock(mutex_);
		if (heldBytesLocked(buffers) <= bound) {
			return dropped;
		}
		size_t newest = jobs_.size();
		for (size_t i = jobs_.size(); i-- > 1;) {
			if (jobs_[i].kind == StoreResult::Kind::Capture && !jobs_[i].dropped) {
				newest = i;
				break;
			}
		}
		// The first job is the one running, or the next to.
		size_t oldest = 1;
		while (oldest < newest && (jobs_[oldest].kind != StoreResult::Kind::Capture || jobs_[oldest].dropped)) {
			++oldest;
		}
		if (oldest >= newest) {
			return dropped;
		}
		Job &job = jobs_[oldest];
		StoreResult &result = job.slot.front();
		result.ticket = job.ticket;
		result.kind = job.kind;
		result.error = kDropped;
		released.swap(job.capture.sources);
		job.dropped = true;
		++dropped_;
		results_.splice(results_.end(), job.slot);
		++dropped;
	}
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
		if (jobs_.front().dropped) {
			jobs_.pop_front();
			--dropped_;
			continue;
		}
		// The job stays at the front while it runs, since moving it out would allocate
		// with some standard libraries; a deque keeps an element in place while others
		// are added behind it.
		Job &job = jobs_.front();
		const bool stopping = stopping_;
		lock.unlock();
		job.slot.front() = execute(job, stopping);
		// The capture's GOPs go after execute() has set its files, and before its result
		// is seen. They leave the job under the lock, where heldBytes() reads them, and
		// are let go of outside it.
		{
			std::vector<CaptureSource> written;
			lock.lock();
			written.swap(job.capture.sources);
			lock.unlock();
		}
		lock.lock();
		results_.splice(results_.end(), job.slot);
		jobs_.pop_front();
		idle_.notify_all();
	}
}

size_t ReplayStore::heldBytesLocked(std::span<const KeyedBuffer> buffers)
{
	counted_.clear();
	for (const Job &job : jobs_) {
		for (const CaptureSource &source : job.capture.sources) {
			const auto found =
				std::find_if(buffers.begin(), buffers.end(),
					     [&source](const KeyedBuffer &held) { return held.key == source.key; });
			const SourceBuffer *buffer = found != buffers.end() ? found->buffer : nullptr;
			for (const std::shared_ptr<const Gop> &gop : source.clip.gops()) {
				counted_.emplace_back(gop.get(), buffer);
			}
		}
	}
	// Captures after one another share most of their GOPs.
	std::sort(counted_.begin(), counted_.end(),
		  [](const auto &a, const auto &b) { return std::less<const Gop *>()(a.first, b.first); });
	size_t bytes = 0;
	const Gop *previous = nullptr;
	for (const auto &[gop, buffer] : counted_) {
		if (gop != previous && (!buffer || !buffer->holds(*gop))) {
			bytes += gop->byteSize();
		}
		previous = gop;
	}
	return bytes;
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
			if (job.files) {
				try {
					job.files->set(result.written->manifest,
						       std::make_shared<const ReplayIndex>(result.written->index));
					// NOLINTNEXTLINE(bugprone-empty-catch): the replay is written all the same, and the library sets its files once it learns so.
				} catch (...) {
				}
			}
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
