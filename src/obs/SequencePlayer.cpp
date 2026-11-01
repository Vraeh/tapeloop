// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SequencePlayer.hpp"

#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"
#include "obs/ReplaySource.hpp"

#include <util/base.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <new>
#include <optional>
#include <utility>

namespace tapeloop::obs {
namespace {

// What the end of a clip hands to the UI thread.
struct EndedTask {
	std::weak_ptr<int> alive;
	SequencePlayer *player = nullptr;
	uint64_t clip = 0;
};

bool nameBefore(const std::string &a, const std::string &b)
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
					    [](unsigned char x, unsigned char y) {
						    return std::tolower(x) < std::tolower(y);
					    });
}

size_t framesOf(const StoredSource &source)
{
	return static_cast<size_t>(
		std::count_if(source.frameTimes.begin(), source.frameTimes.end(),
			      [&](Nanoseconds time) { return time >= source.in && time <= source.out; }));
}

} // namespace

SequencePlayer::SequencePlayer() : source_(createReplaySource("Tapeloop replay"))
{
	if (source_) {
		player_ = replayPlayer(source_);
	}
	obs_add_tick_callback(handleTick, this);
}

SequencePlayer::~SequencePlayer()
{
	// Waits for a tick running now, after which no end is handed over.
	obs_remove_tick_callback(handleTick, this);
	alive_.reset();
	if (source_) {
		obs_source_release(source_);
	}
}

void SequencePlayer::setEntries(std::vector<SequenceEntry> entries)
{
	keepRate();
	state_.setEntries(std::move(entries));
	follow();
}

bool SequencePlayer::move(size_t from, size_t to)
{
	return state_.move(from, to);
}

bool SequencePlayer::setShown(std::string_view sourceKey, bool shown)
{
	keepRate();
	const bool known = state_.setShown(sourceKey, shown);
	follow();
	return known;
}

bool SequencePlayer::putOnAir(const Replay &replay)
{
	if (closed_ || !player_ || state_.phase() != AirPhase::Live || replay.state == ReplayState::Damaged) {
		return false;
	}
	std::shared_ptr<const ReplayIndex> index = replay.index;
	if (!index) {
		std::optional<ReplayIndex> read;
		if (!replay.manifest.empty()) {
			read = readReplayIndex(replay.manifest);
		}
		if (!read) {
			blog(LOG_WARNING, "[tapeloop] Replay %llu cannot be read, so it cannot go on air",
			     static_cast<unsigned long long>(replay.id));
			return false;
		}
		index = std::make_shared<const ReplayIndex>(std::move(*read));
	}

	std::vector<std::string> sources;
	std::vector<const StoredSource *> joining;
	for (const StoredSource &source : index->sources) {
		if (framesOf(source) < 2) {
			continue;
		}
		sources.push_back(source.key);
		if (!state_.sequence().indexOf(source.key)) {
			joining.push_back(&source);
		}
	}
	if (!joining.empty()) {
		std::stable_sort(joining.begin(), joining.end(), [](const StoredSource *a, const StoredSource *b) {
			return nameBefore(a->name, b->name);
		});
		const std::span<const SequenceEntry> listed = state_.sequence().entries();
		std::vector<SequenceEntry> entries(listed.begin(), listed.end());
		for (const StoredSource *source : joining) {
			entries.push_back({source->key, true});
		}
		state_.setEntries(std::move(entries));
	}
	if (!state_.start(std::move(sources))) {
		return false;
	}
	manifest_ = replay.manifest;
	index_ = std::move(index);
	live_ = replay.live;
	follow();
	return true;
}

void SequencePlayer::takeOffAir()
{
	state_.cutShort();
	follow();
}

void SequencePlayer::skip()
{
	if (state_.phase() == AirPhase::Live) {
		return;
	}
	keepRate();
	state_.ended(state_.token());
	follow();
}

void SequencePlayer::close()
{
	closed_ = true;
	// cutShort() goes to the outro when there is one; cut short again, the replay is live.
	while (state_.phase() != AirPhase::Live) {
		state_.cutShort();
	}
	state_.setEntries({});
	follow();
}

void SequencePlayer::follow()
{
	while (state_.token() != token_) {
		token_ = state_.token();
		if (state_.phase() == AirPhase::Source) {
			clip_ = play(state_.source());
			if (clip_ != 0) {
				watching_.store(true, std::memory_order_relaxed);
				return;
			}
			// A source that cannot play ends at once, and the next one gets its turn.
			state_.ended(token_);
			continue;
		}
		watching_.store(false, std::memory_order_relaxed);
		clip_ = 0;
		if (player_) {
			player_->load(nullptr);
		}
		if (state_.phase() == AirPhase::Live) {
			manifest_.clear();
			index_.reset();
			live_.clear();
		}
	}
}

uint64_t SequencePlayer::play(std::string_view sourceKey)
{
	if (!player_ || !index_) {
		return 0;
	}
	for (size_t i = 0; i < index_->sources.size(); ++i) {
		const StoredSource &source = index_->sources[i];
		if (source.key != sourceKey) {
			continue;
		}
		std::unique_ptr<GopSource> gops;
		try {
			gops = std::make_unique<StoredGops>(manifest_, source,
							    i < live_.size() ? live_[i]
									     : std::vector<std::weak_ptr<const Gop>>());
		} catch (const std::bad_alloc &) {
			return 0;
		}
		const uint64_t clip = player_->load(std::move(gops));
		if (clip != 0) {
			// The player keeps the speed and direction of the clip before; the sequence
			// always plays forward.
			player_->setRate(state_.rate());
			player_->play();
		}
		return clip;
	}
	return 0;
}

void SequencePlayer::keepRate() noexcept
{
	if (state_.phase() != AirPhase::Source || !player_) {
		return;
	}
	const int32_t rate = player_->state().rate;
	if (rate != 0) {
		state_.setRate(std::abs(rate));
	}
}

void SequencePlayer::onEnded(uint64_t clip)
{
	if (state_.phase() != AirPhase::Source || clip != clip_) {
		return;
	}
	const PlayerState now = player_->state();
	if (now.clip != clip || !now.ended) {
		return;
	}
	keepRate();
	state_.ended(token_);
	follow();
}

void SequencePlayer::handleTick(void *data, float) noexcept
{
	// Ticks run before the sources', so this sees the state of the frame before.
	auto &self = *static_cast<SequencePlayer *>(data);
	if (!self.watching_.load(std::memory_order_relaxed)) {
		return;
	}
	const PlayerState state = self.player_->state();
	const bool newEnd = state.ended && !(self.seenEnded_ && self.seenClip_ == state.clip);
	self.seenClip_ = state.clip;
	self.seenEnded_ = state.ended;
	if (!newEnd) {
		return;
	}
	std::unique_ptr<EndedTask> task(new (std::nothrow) EndedTask);
	if (!task) {
		return;
	}
	task->alive = self.alive_;
	task->player = &self;
	task->clip = state.clip;
	obs_queue_task(OBS_TASK_UI, runEnded, task.release(), false);
}

void SequencePlayer::runEnded(void *param) noexcept
{
	const std::unique_ptr<EndedTask> task(static_cast<EndedTask *>(param));
	if (!task->alive.lock()) {
		return;
	}
	try {
		task->player->onEnded(task->clip);
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Going on to the next source failed");
	}
}

} // namespace tapeloop::obs
