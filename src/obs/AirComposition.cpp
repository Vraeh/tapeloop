// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/AirComposition.hpp"

#include <obs.hpp>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <new>
#include <utility>

namespace tapeloop::obs {
namespace {

constexpr const char *kFadeId = "fade_transition";
// What a video gets to start playing before it counts as unreadable, how long past its
// length it may run, and how long past its length the way back may run, should their
// signals never come.
constexpr uint64_t kStartWait = 5'000'000'000;
constexpr uint64_t kEndSlack = 2'000'000'000;
constexpr uint64_t kStopSlack = 30'000'000'000;
// How often a video that started without telling its length is asked again.
constexpr uint64_t kProbeEvery = 250'000'000;
// How long going on air waits for a transition just made to read its settings and for
// a video of its own to open.
constexpr uint64_t kSettleWait = 300'000'000;

uint32_t durationOf(const AirTransition &transition)
{
	return static_cast<uint32_t>(std::clamp(transition.duration, kMinAirTransition, kMaxAirTransition).count());
}

uint64_t nanoseconds(int64_t milliseconds)
{
	return static_cast<uint64_t>(milliseconds) * 1'000'000;
}

bool registered(const char *id)
{
	return obs_source_get_display_name(id) != nullptr;
}

// The parts fill the canvas whatever their size, keeping their shape.
void fit(obs_source_t *transition)
{
	obs_video_info video = {};
	if (!obs_get_video_info(&video)) {
		return;
	}
	obs_transition_set_size(transition, video.base_width, video.base_height);
	obs_transition_set_scale_type(transition, OBS_TRANSITION_SCALE_ASPECT);
	obs_transition_set_alignment(transition, OBS_ALIGN_CENTER);
}

obs_source_t *sourceOf(calldata_t *call)
{
	return static_cast<obs_source_t *>(calldata_ptr(call, "source"));
}

void noteUnopened(obs_source_t *, obs_source_t *child, void *param) noexcept
{
	if ((obs_source_get_output_flags(child) & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0 &&
	    obs_source_media_get_duration(child) <= 0) {
		*static_cast<bool *>(param) = true;
	}
}

} // namespace

AirComposition::AirComposition(obs_source_t *replay, AirListener &listener)
	: replay_(obs_source_get_ref(replay)),
	  listener_(listener)
{
	signal_handler_connect(obs_get_signal_handler(), "channel_change", handleChannelChange, this);
	obs_add_tick_callback(handleTick, this);
}

AirComposition::~AirComposition()
{
	destroy();
}

std::vector<std::string> AirComposition::transitionNames() const
{
	if (!catalog_) {
		return {};
	}
	return catalog_->names();
}

void AirComposition::setSettings(const AirSettings &settings)
{
	settings_ = settings;
	applyMedia();
	applyTransitions();
}

void AirComposition::prepare()
{
	applyMedia();
	applyTransitions();
	awaitTransitions();
}

bool AirComposition::channelFree() const
{
	obs_source_t *current = obs_get_output_source(kAirChannel);
	const bool free = !current || current == onChannel_;
	obs_source_release(current);
	return free;
}

void AirComposition::show(AirPhase part)
{
	if (onChannel_ && !leaving_ && part == shown_) {
		return;
	}
	if (channelLost()) {
		listener_.wentOffAir();
		return;
	}

	obs_source_t *target = partSource(part);
	// Watched before it goes on air, as a video says it started from the next tick on.
	const uint64_t now = os_gettime_ns();
	const AirMedia &media = part == AirPhase::Intro ? introMade_ : outroMade_;
	const bool video = part != AirPhase::Source && media.kind == AirMediaKind::Video;
	if (part == AirPhase::Source) {
		watch(nullptr, nullptr, 0);
	} else if (video) {
		watch(target, nullptr, now + kStartWait);
	} else {
		watch(nullptr, nullptr, now + nanoseconds(media.imageTime.count()));
	}

	if (!onChannel_ || leaving_) {
		if (video && (leaving_ || ticks_ < goneSeenAtTick_)) {
			// Held by the way back, or let go before a source pass could see it, it never
			// goes inactive as libobs sees it, so going on air would not start it over.
			// Should it have gone inactive, it starts a tick apart, of which the watch sees
			// the first.
			obs_source_media_restart(target);
		}
		if (in_) {
			fit(in_);
			if (!obs_transition_start(in_, OBS_TRANSITION_MODE_AUTO, durationOf(settings_.into), target)) {
				obs_transition_set(in_, target);
			}
			takeChannel(in_);
		} else {
			takeChannel(target);
		}
		if (out_) {
			endTransition(out_, nullptr);
		}
		leaving_ = false;
	} else if (onChannel_ == in_) {
		endTransition(in_, target);
	} else {
		takeChannel(target);
	}
	shown_ = part;
}

bool AirComposition::leave()
{
	if (!onChannel_ || channelLost()) {
		return false;
	}
	// Nothing is shown during a way back. A replay put on air in one comes here only if it
	// ran out of memory before it played, and the rest of the way back is cut.
	obs_source_t *from = partSource(shown_);
	if (!from || !out_) {
		cut();
		return false;
	}
	const uint32_t duration = durationOf(settings_.back);
	watch(nullptr, out_, os_gettime_ns() + nanoseconds(duration) + kStopSlack);
	fit(out_);
	obs_transition_set(out_, from);
	if (!obs_transition_start(out_, OBS_TRANSITION_MODE_AUTO, duration, nullptr)) {
		cut();
		return false;
	}
	// The part on air goes on through the way back: the way back holds it before the
	// transition that showed it lets go, so it never stops in between.
	takeChannel(out_);
	if (in_) {
		endTransition(in_, nullptr);
	}
	shown_ = AirPhase::Live;
	leaving_ = true;
	return true;
}

void AirComposition::cut()
{
	offAir(true);
}

void AirComposition::destroy()
{
	if (destroyed_) {
		return;
	}
	destroyed_ = true;
	offAir(true);
	// Waits for a tick running now, after which nothing is handed to the UI thread. The
	// signals wait in the same way for a handler running now.
	obs_remove_tick_callback(handleTick, this);
	signal_handler_disconnect(obs_get_signal_handler(), "channel_change", handleChannelChange, this);
	releaseTransition(in_);
	releaseTransition(out_);
	releaseMedia(intro_);
	releaseMedia(outro_);
	obs_source_release(replay_);
	replay_ = nullptr;
	alive_.reset();
}

obs_source_t *AirComposition::partSource(AirPhase part) const noexcept
{
	switch (part) {
	case AirPhase::Intro:
		return intro_;
	case AirPhase::Source:
		return replay_;
	case AirPhase::Outro:
		return outro_;
	case AirPhase::Live:
		break;
	}
	return nullptr;
}

TransitionTemplate AirComposition::pick(const std::string &name) const
{
	if (name.empty()) {
		return {kFadeId, {}};
	}
	std::optional<TransitionTemplate> found = catalog_ ? catalog_->find(name) : std::nullopt;
	if (found && registered(found->id.c_str())) {
		return std::move(*found);
	}
	blog(LOG_WARNING, "[tapeloop] OBS has no transition named %s, so the replay fades", name.c_str());
	return {kFadeId, {}};
}

void AirComposition::makeTransition(obs_source_t *&transition, TransitionTemplate &made, const AirTransition &wanted,
				    const char *name)
{
	TransitionTemplate picked = pick(wanted.name);
	if (transition && picked == made) {
		return;
	}
	releaseTransition(transition);
	made = std::move(picked);
	if (!registered(made.id.c_str())) {
		blog(LOG_WARNING, "[tapeloop] OBS has no %s, so the replay cuts", made.id.c_str());
		return;
	}
	OBSDataAutoRelease settings = made.settingsJson.empty() ? nullptr
								: obs_data_create_from_json(made.settingsJson.c_str());
	transition = obs_source_create_private(made.id.c_str(), name, settings);
	if (transition && obs_source_get_type(transition) != OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_release(transition);
		transition = nullptr;
	}
	if (transition) {
		signal_handler_connect(obs_source_get_signal_handler(transition), "transition_stop",
				       handleTransitionStop, this);
		settling_ = true;
		madeAtTick_ = ticks_;
	}
}

void AirComposition::releaseTransition(obs_source_t *&transition)
{
	if (!transition) {
		return;
	}
	signal_handler_disconnect(obs_source_get_signal_handler(transition), "transition_stop", handleTransitionStop,
				  this);
	obs_transition_clear(transition);
	obs_source_release(transition);
	transition = nullptr;
}

void AirComposition::applyTransitions()
{
	if (destroyed_ || onChannel_) {
		return;
	}
	makeTransition(in_, inMade_, settings_.into, "Tapeloop in");
	makeTransition(out_, outMade_, settings_.back, "Tapeloop out");
}

bool AirComposition::transitionReady(obs_source_t *transition) const
{
	if (!transition) {
		return true;
	}
	// A tick reaches this callback before the sources, so one source pass has run only
	// after the second.
	if (ticks_ < madeAtTick_ + 2) {
		return false;
	}
	if (!obs_transition_fixed(transition)) {
		return true;
	}
	bool unopened = false;
	obs_source_enum_full_tree(transition, noteUnopened, &unopened);
	return !unopened;
}

void AirComposition::awaitTransitions()
{
	if (!settling_) {
		return;
	}
	settling_ = false;
	const uint64_t giveUp = os_gettime_ns() + kSettleWait;
	while (!transitionReady(in_) || !transitionReady(out_)) {
		if (os_gettime_ns() >= giveUp) {
			blog(LOG_WARNING, "[tapeloop] A transition is not ready yet, so it may run short this once");
			return;
		}
		os_sleep_ms(1);
	}
}

std::string AirComposition::missingIntro() const
{
	return !destroyed_ && !intro_ && introMade_.present() ? introMade_.path : std::string();
}

std::string AirComposition::missingOutro() const
{
	return !destroyed_ && !outro_ && outroMade_.present() ? outroMade_.path : std::string();
}

void AirComposition::makeMedia(obs_source_t *&media, AirMedia &made, const AirMedia &wanted, const char *name)
{
	// A file that was missing is looked for again.
	if (made == wanted && (media || !wanted.present())) {
		return;
	}
	releaseMedia(media);
	made = wanted;
	if (!wanted.present()) {
		return;
	}
	const bool video = wanted.kind == AirMediaKind::Video;
	const char *id = video ? "ffmpeg_source" : "image_source";
	if (!registered(id)) {
		blog(LOG_WARNING, "[tapeloop] OBS has no %s, so the replay goes on air without %s", id, name);
		return;
	}
	if (!os_file_exists(wanted.path.c_str())) {
		blog(LOG_WARNING, "[tapeloop] %s is not there, so the replay goes on air without %s",
		     wanted.path.c_str(), name);
		return;
	}
	OBSDataAutoRelease settings = obs_data_create();
	if (video) {
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", wanted.path.c_str());
		// Played once from the start each time it goes on air, holding its last frame
		// until the composition moves on, and kept open in between so that it starts at
		// once.
		obs_data_set_bool(settings, "looping", false);
		obs_data_set_bool(settings, "restart_on_activate", true);
		obs_data_set_bool(settings, "clear_on_media_end", false);
		obs_data_set_bool(settings, "close_when_inactive", false);
	} else {
		obs_data_set_string(settings, "file", wanted.path.c_str());
	}
	media = obs_source_create_private(id, name, settings);
	if (media && video) {
		signal_handler_t *signals = obs_source_get_signal_handler(media);
		signal_handler_connect(signals, "media_started", handleMediaStarted, this);
		signal_handler_connect(signals, "media_ended", handleMediaEnded, this);
	}
}

void AirComposition::releaseMedia(obs_source_t *&media)
{
	if (!media) {
		return;
	}
	signal_handler_t *signals = obs_source_get_signal_handler(media);
	signal_handler_disconnect(signals, "media_started", handleMediaStarted, this);
	signal_handler_disconnect(signals, "media_ended", handleMediaEnded, this);
	obs_source_release(media);
	media = nullptr;
}

void AirComposition::applyMedia()
{
	// What is on air stays as it is until it goes off.
	if (destroyed_ || onChannel_) {
		return;
	}
	makeMedia(intro_, introMade_, settings_.intro, "Tapeloop intro");
	makeMedia(outro_, outroMade_, settings_.outro, "Tapeloop outro");
}

bool AirComposition::holdsChannel() const
{
	obs_source_t *current = obs_get_output_source(kAirChannel);
	const bool held = current && current == onChannel_;
	obs_source_release(current);
	return held;
}

bool AirComposition::channelLost()
{
	obs_source_t *current = obs_get_output_source(kAirChannel);
	const bool lost = current != onChannel_;
	obs_source_release(current);
	if (lost) {
		blog(LOG_INFO, "[tapeloop] Another source took the replay's channel, so the replay is off air");
		offAir(false);
	}
	return lost;
}

void AirComposition::takeChannel(obs_source_t *source)
{
	{
		const std::lock_guard lock(mutex_);
		expected_ = source;
		onAir_ = true;
	}
	armed_.store(true, std::memory_order_relaxed);
	onChannel_ = source;
	obs_set_output_source(kAirChannel, source);
}

void AirComposition::endTransition(obs_source_t *transition, obs_source_t *showing)
{
	// Until it stops, the way in holds the part as its second source and the way back,
	// which goes to nothing, as its first.
	obs_source_t *held = obs_transition_get_source(transition, transition == out_ ? OBS_TRANSITION_SOURCE_A
										      : OBS_TRANSITION_SOURCE_B);
	const bool running = held != nullptr;
	obs_source_release(held);
	// Setting it ends the run without telling the transition, so a stinger's video and
	// audio would go on under what comes next.
	obs_transition_set(transition, showing);
	if (running) {
		obs_transition_force_stop(transition);
	}
}

void AirComposition::watch(obs_source_t *media, obs_source_t *transition, uint64_t deadline)
{
	const std::lock_guard lock(mutex_);
	++watch_;
	watchedMedia_ = media;
	started_ = false;
	startedAt_ = 0;
	probe_ = false;
	ended_ = false;
	watchedTransition_ = transition;
	stopped_ = false;
	deadline_ = deadline;
	reprobe_ = false;
}

void AirComposition::offAir(bool emptyChannel)
{
	const bool wasOnAir = onChannel_ != nullptr;
	watch(nullptr, nullptr, 0);
	{
		const std::lock_guard lock(mutex_);
		onAir_ = false;
		expected_ = nullptr;
		lost_ = false;
	}
	armed_.store(false, std::memory_order_relaxed);
	if (onChannel_ && emptyChannel && holdsChannel()) {
		obs_set_output_source(kAirChannel, nullptr);
	}
	onChannel_ = nullptr;
	if (in_) {
		endTransition(in_, nullptr);
	}
	if (out_) {
		endTransition(out_, nullptr);
	}
	if (wasOnAir) {
		// Read once the parts are let go. A tick reaches this callback before the sources,
		// so a source pass that saw them go has run only after the second.
		goneSeenAtTick_ = ticks_ + 2;
	}
	shown_ = AirPhase::Live;
	leaving_ = false;
	applyMedia();
}

void AirComposition::onTask(const Task &task)
{
	// The channel's signal may come from anywhere, so it is checked against the channel.
	if (task.lost && onChannel_ && channelLost()) {
		listener_.wentOffAir();
		return;
	}
	if (task.watch != watch_) {
		return;
	}
	if (task.probe) {
		probeDuration();
	}
	if (!task.ended) {
		return;
	}
	if (leaving_) {
		offAir(true);
		listener_.wentOffAir();
	} else {
		listener_.partEnded();
	}
}

void AirComposition::probeDuration()
{
	obs_source_t *media = partSource(shown_);
	const int64_t length = media ? obs_source_media_get_duration(media) : 0;
	const int64_t time = media ? obs_source_media_get_time(media) : 0;
	const uint64_t now = os_gettime_ns();
	const std::lock_guard lock(mutex_);
	if (!started_) {
		return;
	}
	if (length > 0) {
		deadline_ = startedAt_ + nanoseconds(length) + kEndSlack;
	} else if (time > 0) {
		// It plays without a length to go by, so only its end ends it.
		deadline_ = 0;
	} else if (now < startedAt_ + kStartWait) {
		deadline_ = now + kProbeEvery;
		reprobe_ = true;
		return;
	} else {
		// Nothing came of it: a file it cannot read starts all the same.
		deadline_ = now;
	}
	reprobe_ = false;
}

void AirComposition::handleTick(void *data, float) noexcept
{
	auto &self = *static_cast<AirComposition *>(data);
	++self.ticks_;
	if (!self.armed_.load(std::memory_order_relaxed)) {
		return;
	}
	const uint64_t now = os_gettime_ns();
	std::unique_ptr<Task> task;
	{
		const std::lock_guard lock(self.mutex_);
		const bool due = self.deadline_ != 0 && now >= self.deadline_;
		const bool probe = self.probe_ || (due && self.reprobe_);
		const bool ended = self.ended_ || self.stopped_ || (due && !self.reprobe_);
		if (!self.lost_ && !probe && !ended) {
			return;
		}
		// Should it fail, the next tick tries again.
		std::unique_ptr<Task> made(new (std::nothrow) Task);
		if (!made) {
			return;
		}
		made->alive = self.alive_;
		made->composition = &self;
		made->watch = self.watch_;
		made->lost = std::exchange(self.lost_, false);
		made->probe = probe;
		made->ended = ended;
		task = std::move(made);
		self.probe_ = false;
		if (due) {
			self.deadline_ = 0;
			self.reprobe_ = false;
		}
		if (ended) {
			self.watchedMedia_ = nullptr;
			self.watchedTransition_ = nullptr;
			self.ended_ = false;
			self.stopped_ = false;
		}
	}
	obs_queue_task(OBS_TASK_UI, runTask, task.release(), false);
}

void AirComposition::runTask(void *param) noexcept
{
	const std::unique_ptr<Task> task(static_cast<Task *>(param));
	if (!task->alive.lock()) {
		return;
	}
	try {
		task->composition->onTask(*task);
	} catch (...) {
		blog(LOG_ERROR, "[tapeloop] Following the replay on air failed");
	}
}

void AirComposition::handleChannelChange(void *data, calldata_t *call) noexcept
{
	if (calldata_int(call, "channel") != kAirChannel) {
		return;
	}
	auto &self = *static_cast<AirComposition *>(data);
	obs_source_t *source = sourceOf(call);
	const std::lock_guard lock(self.mutex_);
	if (self.onAir_ && source != self.expected_) {
		self.lost_ = true;
	}
}

void AirComposition::handleMediaStarted(void *data, calldata_t *call) noexcept
{
	auto &self = *static_cast<AirComposition *>(data);
	obs_source_t *source = sourceOf(call);
	const uint64_t now = os_gettime_ns();
	const std::lock_guard lock(self.mutex_);
	if (source != self.watchedMedia_ || self.started_) {
		return;
	}
	self.started_ = true;
	self.startedAt_ = now;
	self.probe_ = true;
	self.deadline_ = 0;
}

void AirComposition::handleMediaEnded(void *data, calldata_t *call) noexcept
{
	auto &self = *static_cast<AirComposition *>(data);
	obs_source_t *source = sourceOf(call);
	const std::lock_guard lock(self.mutex_);
	// A video taken off air ends late, from its own thread; only an end after it started
	// again counts.
	if (source == self.watchedMedia_ && self.started_) {
		self.ended_ = true;
	}
}

void AirComposition::handleTransitionStop(void *data, calldata_t *call) noexcept
{
	auto &self = *static_cast<AirComposition *>(data);
	obs_source_t *source = sourceOf(call);
	const std::lock_guard lock(self.mutex_);
	if (source == self.watchedTransition_) {
		self.stopped_ = true;
	}
}

} // namespace tapeloop::obs
