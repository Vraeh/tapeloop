// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"
#include "core/ReplayPlayerState.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tapeloop::obs {

// The output channel a replay goes on air through. Channels draw in order over the
// program on channel 0. The frontend keeps 1 to 6 for its audio devices, and the
// downstream keyer plugin starts at 7.
inline constexpr uint32_t kAirChannel = 32;

// What one of the user's transitions is made of, so that a copy of it can be made.
struct TransitionTemplate {
	std::string id;
	std::string settingsJson;

	bool operator==(const TransitionTemplate &) const = default;
};

// The transitions the user has in OBS, looked up by name on the UI thread.
class TransitionCatalog {
public:
	TransitionCatalog() = default;
	virtual ~TransitionCatalog() = default;

	TransitionCatalog(const TransitionCatalog &) = delete;
	TransitionCatalog &operator=(const TransitionCatalog &) = delete;
	TransitionCatalog(TransitionCatalog &&) = delete;
	TransitionCatalog &operator=(TransitionCatalog &&) = delete;

	virtual std::optional<TransitionTemplate> find(const std::string &name) const = 0;
	virtual std::vector<std::string> names() const = 0;
};

// Told on the UI thread what the composition saw happen on its own.
class AirListener {
public:
	AirListener() = default;
	virtual ~AirListener() = default;

	AirListener(const AirListener &) = delete;
	AirListener &operator=(const AirListener &) = delete;
	AirListener(AirListener &&) = delete;
	AirListener &operator=(AirListener &&) = delete;

	// The intro or outro on air played out.
	virtual void partEnded() = 0;
	// The composition is off air: the way back finished, or another source took the
	// channel, which show() can find as well, so this may come from inside that call.
	virtual void wentOffAir() = 0;
};

// Puts the parts of a replay on air over the program, on a channel of its own, and takes
// them off, without touching the user's scenes: the intro, the replay and the outro, each
// from the transition into air when nothing was on air and by a cut from the part before
// otherwise, and the way back to live through a transition of its own. The transitions
// are private copies of the user's picked ones, made ahead of the first use because they
// read their settings on a later tick and open their own video on a thread, and the intro
// and outro private media or image sources, all made and released here. Everything runs
// on the UI thread except the signals and the tick, which only note what they saw for the
// UI thread to act on.
class AirComposition {
public:
	// Shows this source as the replay. Both outlive the composition.
	AirComposition(obs_source_t *replay, AirListener &listener);
	~AirComposition();

	AirComposition(const AirComposition &) = delete;
	AirComposition &operator=(const AirComposition &) = delete;
	AirComposition(AirComposition &&) = delete;
	AirComposition &operator=(AirComposition &&) = delete;

	// Null, or one that outlives the composition. Without it, or for a name it does not
	// have, the transitions are OBS's fade.
	void setCatalog(const TransitionCatalog *catalog) noexcept { catalog_ = catalog; }
	// The names of the transitions that can be picked.
	std::vector<std::string> transitionNames() const;
	// A transition's length applies the next time it is used; which transition, and the
	// intro and outro, once nothing is on air.
	void setSettings(const AirSettings &settings);
	// Brings the transitions, intro and outro up to date before a replay starts, when
	// nothing is on air: a transition edited in OBS is made again, and an intro or outro
	// whose file was missing is looked for again. Blocks for a moment only when a
	// transition was just made and is not ready.
	void prepare();
	bool hasIntro() const noexcept { return intro_ != nullptr; }
	bool hasOutro() const noexcept { return outro_ != nullptr; }
	// The file of an intro or outro the settings ask for and that could not be made, empty
	// when there is none. As of the last look at it: when the settings change, when a
	// replay is about to go on air and when one has gone off.
	std::string missingIntro() const;
	std::string missingOutro() const;
	// Whether the channel is empty or holds the composition.
	bool channelFree() const;

	// Puts the part on air. Never Live, and an intro or outro only if hasIntro() or hasOutro()
	// said so before the replay started: those change only once nothing is on air. When
	// another source took the channel the composition goes off air instead, and the
	// listener hears wentOffAir().
	void show(AirPhase part);
	// Starts the way back to live, after which the listener hears wentOffAir(). False when
	// nothing is on air, and when there is no transition to go back with or another source
	// took the channel, in which case the composition is taken off at once.
	bool leave();
	// Takes the composition off air at once. The listener hears nothing.
	void cut();
	// Takes the composition off air and releases everything it made, for good. Before
	// libobs shuts down, as the sources it made belong to other modules.
	void destroy();

	bool onAir() const noexcept { return onChannel_ != nullptr; }
	bool leaving() const noexcept { return leaving_; }
	// Live once the way back started.
	AirPhase shown() const noexcept { return shown_; }
	// What the part shows: the replay, or the intro or outro made from the settings. Null
	// for Live and for a part there is nothing to show for.
	obs_source_t *partSource(AirPhase part) const noexcept;

private:
	struct Task {
		std::weak_ptr<int> alive;
		AirComposition *composition = nullptr;
		uint64_t watch = 0;
		bool lost = false;
		bool probe = false;
		bool ended = false;
	};

	// Makes the transition from the user's pick, again only when the pick changed. Left
	// null when no transition can be made. Never the one on the channel.
	void makeTransition(obs_source_t *&transition, TransitionTemplate &made, const AirTransition &wanted,
			    const char *name);
	TransitionTemplate pick(const std::string &name) const;
	void releaseTransition(obs_source_t *&transition);
	void applyTransitions();
	bool transitionReady(obs_source_t *transition) const;
	void awaitTransitions();
	void makeMedia(obs_source_t *&media, AirMedia &made, const AirMedia &wanted, const char *name);
	void releaseMedia(obs_source_t *&media);
	void applyMedia();
	bool holdsChannel() const;
	// Whether the channel holds anything but what the composition put there, in which case
	// the composition goes off air and leaves it. Checked before each write, so that the
	// tick that would tell is not waited for. libobs cannot write a channel only if it is
	// unchanged, so a source that takes it right after this is still overwritten.
	bool channelLost();
	void takeChannel(obs_source_t *source);
	// Leaves the transition showing that source, or nothing, and ends a run it was on as
	// running out would, so a watch still on it sees the stop.
	void endTransition(obs_source_t *transition, obs_source_t *showing);
	// Starts a new watch, which makes whatever the last one saw stale: the end of this
	// media, the stop of this transition, or the deadline, whichever comes first. Zero
	// for no deadline.
	void watch(obs_source_t *media, obs_source_t *transition, uint64_t deadline);
	// Empties the channel too when asked and it still holds what the composition put
	// there.
	void offAir(bool emptyChannel);

	void onTask(const Task &task);
	void probeDuration();

	static void handleTick(void *data, float seconds) noexcept;
	static void runTask(void *param) noexcept;
	static void handleChannelChange(void *data, calldata_t *call) noexcept;
	static void handleMediaStarted(void *data, calldata_t *call) noexcept;
	static void handleMediaEnded(void *data, calldata_t *call) noexcept;
	static void handleTransitionStop(void *data, calldata_t *call) noexcept;

	obs_source_t *replay_ = nullptr;
	AirListener &listener_;
	const TransitionCatalog *catalog_ = nullptr;
	AirSettings settings_;
	bool destroyed_ = false;

	// The UI thread only.
	obs_source_t *in_ = nullptr;
	obs_source_t *out_ = nullptr;
	TransitionTemplate inMade_;
	TransitionTemplate outMade_;
	obs_source_t *intro_ = nullptr;
	obs_source_t *outro_ = nullptr;
	AirMedia introMade_;
	AirMedia outroMade_;
	obs_source_t *onChannel_ = nullptr;
	AirPhase shown_ = AirPhase::Live;
	bool leaving_ = false;
	// libobs may not have seen what went off air go inactive before this tick.
	uint64_t goneSeenAtTick_ = 0;
	// A transition was made and going on air has not yet waited for it.
	bool settling_ = false;
	uint64_t madeAtTick_ = 0;

	// What the signals and the tick share with the UI thread, which alone changes watch_.
	// No libobs call is made while it is held: the channel signal comes with libobs's
	// channel lock held.
	std::mutex mutex_;
	uint64_t watch_ = 0;
	obs_source_t *watchedMedia_ = nullptr;
	bool started_ = false;
	uint64_t startedAt_ = 0;
	bool probe_ = false;
	bool ended_ = false;
	obs_source_t *watchedTransition_ = nullptr;
	bool stopped_ = false;
	// os_gettime_ns() time; zero for none.
	uint64_t deadline_ = 0;
	// The deadline asks the video its length again rather than ending it.
	bool reprobe_ = false;
	obs_source_t *expected_ = nullptr;
	bool onAir_ = false;
	bool lost_ = false;
	// Set while the composition is on the channel, so that the tick looks only then. Relaxed
	// is enough: the flag only gates a look at what mutex_ orders, and a stale value costs a
	// tick's delay, or a look that finds nothing to do.
	std::atomic<bool> armed_{false};
	std::atomic<uint64_t> ticks_{0};

	std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace tapeloop::obs
