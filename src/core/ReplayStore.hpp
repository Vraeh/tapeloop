// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/ReplayWriter.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tapeloop {

// What one job of the store came to.
struct StoreResult {
	enum class Kind { Capture, Tags, Scan };

	uint64_t ticket = 0;
	Kind kind = Kind::Capture;
	// Why the job failed; empty when it did not.
	std::string error;
	// A capture that was written.
	std::optional<WrittenReplay> written;
	// The manifest a tag edit was for.
	std::filesystem::path manifest;
	// A scan that ran.
	std::optional<ReplayScan> scan;
};

// Every replay write and scan, on one thread of its own at low I/O priority, one job at a
// time in the order they were asked for, so a tag edit asked for after a capture lands
// after it. A capture keeps its GOPs alive until it is written and lets go of them
// before its result is reported: once on disk, a replay holds no picture in memory. Safe
// to call from any thread.
class ReplayStore {
public:
	explicit ReplayStore(ReplayWriterConfig config = {});
	// Waits for the captures and tag edits still asked for, so that none is lost when OBS
	// exits; scans not started yet are dropped.
	~ReplayStore();

	ReplayStore(const ReplayStore &) = delete;
	ReplayStore &operator=(const ReplayStore &) = delete;
	ReplayStore(ReplayStore &&) = delete;
	ReplayStore &operator=(ReplayStore &&) = delete;

	// Each returns the ticket its result will carry, from 1 up.
	uint64_t write(ReplayCapture capture);
	uint64_t writeTags(std::filesystem::path manifest, std::vector<std::string> tags);
	uint64_t scan(std::filesystem::path base);

	// The results of the jobs finished since the last call, in the order they finished.
	std::vector<StoreResult> poll();
	// Returns once every job asked for so far has finished.
	void waitUntilIdle();
	// Jobs asked for that have not finished.
	size_t pending() const;
	// Whether the thread could lower its I/O priority; nothing until it has tried.
	std::optional<bool> lowPriority() const noexcept;

private:
	struct Job {
		uint64_t ticket = 0;
		StoreResult::Kind kind = StoreResult::Kind::Capture;
		ReplayCapture capture;
		std::filesystem::path path;
		std::vector<std::string> tags;
		// Its result, made by the thread that asks, so that finishing it never allocates.
		std::list<StoreResult> slot;
	};

	uint64_t enqueue(Job job);
	void run() noexcept;
	StoreResult execute(const Job &job, bool stopping) noexcept;

	// Touched only by the store's thread.
	ReplayWriter writer_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable idle_;
	// The job running is the first, until it has finished.
	std::deque<Job> jobs_;
	std::list<StoreResult> results_;
	uint64_t nextTicket_ = 1;
	bool stopping_ = false;
	// 0 until the thread has tried, then 1 when lowered and 2 when not.
	std::atomic<int> lowPriority_{0};
	// Last, so that it starts after everything it uses. Not a std::jthread: macOS's libc++
	// has it without an experimental flag only from LLVM 18 on.
	std::thread worker_;
};

} // namespace tapeloop
