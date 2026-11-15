// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/ReplayWriter.hpp"
#include "core/SourceBuffer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

// The buffer the clips of a capture's source are cut from, by the source's key.
struct KeyedBuffer {
	std::string_view key;
	const SourceBuffer *buffer = nullptr;
};

// Every replay write and scan, on one thread of its own at low I/O priority, one job at a
// time in the order they were asked for, so a tag edit asked for after a capture lands
// after it. A capture keeps its GOPs alive until it is written or dropped and lets go of
// them before its result is reported: once on disk, a replay holds no picture in memory.
// Safe to call from any thread.
class ReplayStore {
public:
	// Why a capture dropped while it waited is not saved. Short enough for a string's own
	// buffer, so setting it needs no memory.
	static constexpr std::string_view kDropped = "disk too slow";

	explicit ReplayStore(ReplayWriterConfig config = {});
	// Waits for the captures and tag edits still asked for, so that none is lost when OBS
	// exits; scans not started yet are dropped.
	~ReplayStore();

	ReplayStore(const ReplayStore &) = delete;
	ReplayStore &operator=(const ReplayStore &) = delete;
	ReplayStore(ReplayStore &&) = delete;
	ReplayStore &operator=(ReplayStore &&) = delete;

	// Each returns the ticket its result will carry, from 1 up. A capture's files, when
	// given, are set once it is written and before it lets go of its GOPs, so that a
	// replay played meanwhile finds each GOP in the buffer or on disk.
	uint64_t write(ReplayCapture capture, std::shared_ptr<ReplayFiles> files = nullptr);
	uint64_t writeTags(std::filesystem::path manifest, std::vector<std::string> tags);
	uint64_t scan(std::filesystem::path base);

	// The results of the jobs finished since the last call, in the order they finished.
	std::vector<StoreResult> poll();
	// Returns once every job asked for so far has finished.
	void waitUntilIdle();
	// Jobs asked for that have not finished.
	size_t pending() const;

	// What the captures not written yet keep in memory beyond the buffers: the bytes of
	// their GOPs that the buffer of their source does not hold, each GOP counted once. A
	// source without a buffer among those given counts every GOP it has. It locks the
	// buffers while it holds the store's own lock, so nothing holding a buffer's lock may
	// call the store.
	size_t heldBytes(std::span<const KeyedBuffer> buffers);
	// Drops the oldest captures waiting to be written, until what the captures not
	// written keep, as heldBytes() counts it, is no more than bound. The capture being
	// written and the newest one are never dropped, so what they keep alone can stay
	// above it. A dropped capture lets go of its GOPs at once and fails with kDropped,
	// its files never set. Returns how many it dropped.
	size_t dropWaiting(size_t bound, std::span<const KeyedBuffer> buffers);
	// Whether the thread could lower its I/O priority; nothing until it has tried.
	std::optional<bool> lowPriority() const noexcept;

private:
	struct Job {
		uint64_t ticket = 0;
		StoreResult::Kind kind = StoreResult::Kind::Capture;
		ReplayCapture capture;
		std::shared_ptr<ReplayFiles> files;
		std::filesystem::path path;
		std::vector<std::string> tags;
		// Its result, made by the thread that asks, so that finishing it never allocates.
		std::list<StoreResult> slot;
		// Dropped while it waited: its result is out and its GOPs are gone, and the
		// store's thread only takes it off the queue.
		bool dropped = false;
	};

	uint64_t enqueue(Job job);
	void run() noexcept;
	StoreResult execute(const Job &job, bool stopping) noexcept;
	size_t heldBytesLocked(std::span<const KeyedBuffer> buffers);

	// Touched only by the store's thread.
	ReplayWriter writer_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable idle_;
	// The job running is the first, until it has finished. Jobs dropped stay in place,
	// since taking one out of the middle would move the one running.
	std::deque<Job> jobs_;
	size_t dropped_ = 0;
	std::list<StoreResult> results_;
	// Each GOP of the captures not written with the buffer of its source, kept from one
	// count to the next.
	std::vector<std::pair<const Gop *, const SourceBuffer *>> counted_;
	uint64_t nextTicket_ = 1;
	bool stopping_ = false;
	// 0 until the thread has tried, then 1 when lowered and 2 when not.
	std::atomic<int> lowPriority_{0};
	// Last, so that it starts after everything it uses. Not a std::jthread: macOS's libc++
	// has it without an experimental flag only from LLVM 18 on.
	std::thread worker_;
};

} // namespace tapeloop
