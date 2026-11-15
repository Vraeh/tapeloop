// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SequencePlayer.hpp"

#include "core/GopReader.hpp"
#include "core/ReplayWriter.hpp"
#include "obs/ReplaySource.hpp"
#include "obs/ReplayView.hpp"

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

// Null when it has none and its manifest cannot be read.
std::shared_ptr<const ReplayIndex> indexOf(const Replay &replay)
{
	if (replay.index) {
		return replay.index;
	}
	if (replay.manifest.empty()) {
		return nullptr;
	}
	std::optional<ReplayIndex> read = readReplayIndex(replay.manifest);
	if (!read) {
		return nullptr;
	}
	return std::make_shared<const ReplayIndex>(std::move(*read));
}

std::shared_ptr<const ReplayFiles> filesOf(const Replay &replay, const std::shared_ptr<const ReplayIndex> &index)
{
	if (replay.files) {
		return replay.files;
	}
	// Found on disk, or let go of by the buffers: whatever files it has are known.
	auto own = std::make_shared<ReplayFiles>();
	if (!replay.manifest.empty()) {
		own->set(replay.manifest, index);
	}
	return own;
}

size_t framesOf(const StoredSource &source)
{
	return static_cast<size_t>(
		std::count_if(source.frameTimes.begin(), source.frameTimes.end(),
			      [&](Nanoseconds time) { return time >= source.in && time <= source.out; }));
}

} // namespace

SequencePlayer::SequencePlayer(const ReplayLibrary &library)
	: library_(library),
	  source_(createReplaySource("Tapeloop replay")),
	  air_(source_, *this)
{
	if (source_) {
		player_ = replayPlayer(source_);
		setViewedReplay(source_);
	}
	obs_add_tick_callback(handleTick, this);
}

SequencePlayer::~SequencePlayer()
{
	// Waits for a tick running now, after which no end is handed over.
	obs_remove_tick_callback(handleTick, this);
	alive_.reset();
	if (source_) {
		unsetViewedReplay(source_);
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
	const bool moved = state_.move(from, to);
	follow();
	return moved;
}

bool SequencePlayer::setShown(std::string_view sourceKey, bool shown)
{
	keepRate();
	const bool known = state_.setShown(sourceKey, shown);
	follow();
	return known;
}

bool SequencePlayer::putOnAir(uint64_t id)
{
	refusal_ = AirRefusal::None;
	const Replay *replay = library_.find(id);
	if (closed_ || !player_ || state_.phase() != AirPhase::Live || !replay) {
		return false;
	}
	if (replay->state == ReplayState::Damaged) {
		refusal_ = AirRefusal::Damaged;
		return false;
	}
	if (!air_.channelFree()) {
		refuseChannel(id);
		return false;
	}
	std::shared_ptr<const ReplayIndex> index = indexOf(*replay);
	if (!index) {
		refusal_ = AirRefusal::Unreadable;
		blog(LOG_WARNING, "[tapeloop] Replay %llu cannot be read, so it cannot go on air",
		     static_cast<unsigned long long>(id));
		return false;
	}
	std::shared_ptr<const ReplayFiles> files = filesOf(*replay, index);
	// Read before anything changes the list, whose first source the cue follows.
	const bool carry = cued_ == id && clip_ != 0;
	const int32_t cuedRate = carry ? std::abs(player_->rateAhead()) : 0;

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
	air_.prepare();
	state_.setConfig({air_.hasIntro(), air_.hasOutro()});
	if (!state_.start(std::move(sources))) {
		return false;
	}
	if (carry) {
		state_.setRate(cuedRate);
		carriedClip_ = clip_;
		carriedSource_ = std::move(cuedSource_);
		// It goes on from here once its source is on air, after the intro if there is one.
		player_->pause();
	}
	cued_ = 0;
	cuedSource_.clear();
	replay_ = id;
	files_ = std::move(files);
	index_ = std::move(index);
	live_ = replay->live;
	follow();
	// The look above can be a transition's wait old. Whoever took the channel since has the
	// replay off air by the next tick at the latest, and only this press can say why.
	if (!air_.channelFree()) {
		refuseChannel(id);
		return false;
	}
	return true;
}

void SequencePlayer::refuseChannel(uint64_t id)
{
	refusal_ = AirRefusal::ChannelBusy;
	blog(LOG_WARNING, "[tapeloop] Another source is on the replay's channel, so replay %llu cannot go on air",
	     static_cast<unsigned long long>(id));
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

void SequencePlayer::programWentBlack()
{
	air_.cut();
	wentOffAir();
}

void SequencePlayer::close()
{
	closed_ = true;
	refusal_ = AirRefusal::None;
	// Off air first, so that nothing below shows the outro or starts the way back.
	air_.cut();
	// cutShort() goes to the outro when there is one; cut short again, the replay is live.
	while (state_.phase() != AirPhase::Live) {
		state_.cutShort();
	}
	state_.setEntries({});
	follow();
	uncue();
	// The last frame a way back under way kept loaded goes too.
	if (player_) {
		player_->load(nullptr);
	}
}

void SequencePlayer::destroyAir()
{
	destroyed_ = true;
	try {
		close();
	} catch (...) {
		// What the composition made still goes before the modules it came from, and whoever
		// called this is told.
		air_.destroy();
		throw;
	}
	air_.destroy();
}

void SequencePlayer::follow()
{
	while (state_.token() != token_) {
		const uint64_t token = state_.token();
		token_ = token;
		const AirPhase phase = state_.phase();
		if (phase == AirPhase::Source) {
			if (carriedClip_ != 0 && state_.source() == carriedSource_) {
				clip_ = carriedClip_;
				player_->setRate(state_.rate());
				player_->play();
			} else {
				clip_ = play(state_.source());
			}
			carriedClip_ = 0;
			carriedSource_.clear();
			if (clip_ != 0) {
				watching_.store(true, std::memory_order_relaxed);
				air_.show(phase);
				return;
			}
			// A source that cannot play ends at once, and the next one gets its turn.
			state_.ended(token_);
			continue;
		}
		watching_.store(false, std::memory_order_relaxed);
		clip_ = 0;
		if (phase == AirPhase::Live) {
			carriedClip_ = 0;
			carriedSource_.clear();
			// The way back fades out the last frame of the replay, which stays loaded until
			// wentOffAir().
			if (!air_.leave() && player_) {
				player_->load(nullptr);
			}
			replay_ = 0;
			files_.reset();
			index_.reset();
			live_.clear();
			continue;
		}
		air_.show(phase);
		// A channel another source took has show() run wentOffAir(), which followed the
		// sequence back to live and cued the current replay, so nothing here is left to do.
		if (token_ != token) {
			continue;
		}
		if (phase == AirPhase::Intro && carriedClip_ != 0) {
			continue;
		}
		carriedClip_ = 0;
		carriedSource_.clear();
		if (player_) {
			player_->load(nullptr);
		}
	}
	if (state_.phase() == AirPhase::Live) {
		cueCurrent();
	}
}

void SequencePlayer::cueCurrent()
{
	// During the way back the replay source still shows the last frame on air.
	if (closed_ || !player_ || state_.phase() != AirPhase::Live || air_.leaving()) {
		return;
	}
	const uint64_t id = library_.current();
	const Replay *replay = library_.find(id);
	if (!replay || replay->state == ReplayState::Damaged) {
		uncue();
		return;
	}
	if (id != replay_) {
		uncue();
		std::shared_ptr<const ReplayIndex> index = indexOf(*replay);
		if (index) {
			std::vector<std::vector<std::weak_ptr<const Gop>>> live = replay->live;
			files_ = filesOf(*replay, index);
			live_ = std::move(live);
			index_ = std::move(index);
		}
		// Last, so that a replay is looked at again after a failure on the way.
		replay_ = id;
	}
	if (!index_) {
		return;
	}
	std::optional<std::string> first = firstShown();
	if (!first) {
		unloadCue();
		return;
	}
	if (cued_ == id && *first == cuedSource_) {
		return;
	}
	const uint64_t clip = load(*first);
	if (clip == 0) {
		unloadCue();
		return;
	}
	// A replay plays at real time until someone changes it, whatever the one before had.
	player_->setRate(1000);
	cued_ = id;
	cuedSource_ = std::move(*first);
	clip_ = clip;
	watching_.store(true, std::memory_order_relaxed);
}

void SequencePlayer::unloadCue() noexcept
{
	if (cued_ != 0 && player_) {
		player_->load(nullptr);
	}
	cued_ = 0;
	cuedSource_.clear();
	clip_ = 0;
	watching_.store(false, std::memory_order_relaxed);
}

void SequencePlayer::uncue() noexcept
{
	unloadCue();
	replay_ = 0;
	files_.reset();
	index_.reset();
	live_.clear();
}

std::optional<std::string> SequencePlayer::firstShown() const
{
	std::vector<std::string> available;
	const StoredSource *joining = nullptr;
	for (const StoredSource &source : index_->sources) {
		if (framesOf(source) < 2) {
			continue;
		}
		available.push_back(source.key);
		if (!state_.sequence().indexOf(source.key) && (!joining || nameBefore(source.name, joining->name))) {
			joining = &source;
		}
	}
	std::sort(available.begin(), available.end());
	if (const std::optional<size_t> first = state_.sequence().nextShown(0, available)) {
		return state_.sequence().entries()[*first].sourceKey;
	}
	// Sources the list does not have join it shown, by name, once the replay goes on air.
	if (joining) {
		return joining->key;
	}
	return std::nullopt;
}

uint64_t SequencePlayer::load(std::string_view sourceKey)
{
	if (!player_ || !index_ || !files_) {
		return 0;
	}
	for (size_t i = 0; i < index_->sources.size(); ++i) {
		const StoredSource &source = index_->sources[i];
		if (source.key != sourceKey) {
			continue;
		}
		std::unique_ptr<GopSource> gops;
		try {
			gops = std::make_unique<StoredGops>(
				files_, source, i < live_.size() ? live_[i] : std::vector<std::weak_ptr<const Gop>>());
		} catch (const std::bad_alloc &) {
			return 0;
		}
		return player_->load(std::move(gops));
	}
	return 0;
}

uint64_t SequencePlayer::play(std::string_view sourceKey)
{
	const uint64_t clip = load(sourceKey);
	if (clip != 0) {
		// The player keeps the speed and direction of the clip before; the sequence
		// always plays forward.
		player_->setRate(state_.rate());
		player_->play();
	}
	return clip;
}

void SequencePlayer::keepRate() noexcept
{
	if (state_.phase() != AirPhase::Source || !player_) {
		return;
	}
	// The speed given since the last tick counts: the next clip is loaded before that
	// speed would show in the player's state.
	state_.setRate(std::abs(player_->rateAhead()));
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

void SequencePlayer::partEnded()
{
	state_.ended(token_);
	follow();
}

void SequencePlayer::wentOffAir()
{
	if (state_.phase() == AirPhase::Live) {
		// The way back is over, or the program went black with nothing on air and the cue
		// stays.
		if (cued_ == 0 && player_) {
			player_->load(nullptr);
		}
		cueCurrent();
		return;
	}
	// Another source took the channel: the replay is over, with nothing more to show.
	keepRate();
	while (state_.phase() != AirPhase::Live) {
		state_.cutShort();
	}
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
