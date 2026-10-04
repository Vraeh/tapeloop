// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/Playhead.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace tapeloop {
namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
// Units of remainder per nanosecond.
constexpr int64_t kScale = 2000;

PlayheadConfig normalized(PlayheadConfig config) noexcept
{
	config.minRate = std::max(config.minRate, 1);
	config.maxRate = std::max(config.maxRate, config.minRate);
	config.scrubStartRate = std::max(config.scrubStartRate, 1);
	config.scrubEndRate = std::max(config.scrubEndRate, 1);
	config.scrubRampStart = std::max(config.scrubRampStart, Nanoseconds{0});
	config.scrubRampEnd = std::max(config.scrubRampEnd, config.scrubRampStart);
	return config;
}

} // namespace

Playhead::Playhead(PlayheadConfig config)
	: config_(normalized(config)),
	  rate_(std::clamp(1000, config_.minRate, config_.maxRate))
{
}

void Playhead::load(std::vector<Nanoseconds> frameTimes)
{
	if (std::adjacent_find(frameTimes.begin(), frameTimes.end(), std::greater_equal<>()) != frameTimes.end()) {
		std::sort(frameTimes.begin(), frameTimes.end());
		frameTimes.erase(std::unique(frameTimes.begin(), frameTimes.end()), frameTimes.end());
	}

	frames_ = std::move(frameTimes);
	position_ = frames_.empty() ? Nanoseconds{0} : frames_.front();
	remainder_ = 0;
	playing_ = false;
	scrubbing_ = false;
	rate_ = rate_ < 0 ? -rate_ : rate_;
}

void Playhead::advance(Nanoseconds elapsed)
{
	if (frames_.empty() || elapsed <= Nanoseconds{0}) {
		return;
	}

	if (scrubbing_) {
		advanceScrub(elapsed.count());
		return;
	}
	if (!playing_) {
		return;
	}

	move(elapsed.count(), 2 * int64_t{rate_});
	if (rate_ > 0 && position_ >= frames_.back()) {
		moveTo(frames_.back());
		playing_ = false;
	} else if (rate_ < 0 && position_ <= frames_.front()) {
		moveTo(frames_.front());
		playing_ = false;
	}
}

void Playhead::play() noexcept
{
	if (!frames_.empty()) {
		playing_ = rate_ > 0 ? position_ < frames_.back() : position_ > frames_.front();
	}
}

void Playhead::togglePause() noexcept
{
	if (playing_) {
		pause();
	} else {
		play();
	}
}

void Playhead::setRate(int32_t rate) noexcept
{
	if (rate == 0) {
		playing_ = false;
		return;
	}
	// Clamping before negating keeps INT32_MIN from overflowing.
	const int32_t magnitude =
		std::clamp(rate < 0 ? -std::max(rate, -config_.maxRate) : rate, config_.minRate, config_.maxRate);
	rate_ = rate < 0 ? -magnitude : magnitude;
}

void Playhead::step(int frames) noexcept
{
	playing_ = false;
	if (frames_.empty()) {
		return;
	}

	const int64_t last = static_cast<int64_t>(frames_.size()) - 1;
	const int64_t target = std::clamp(static_cast<int64_t>(displayedFrame()) + frames, int64_t{0}, last);
	moveTo(frames_[static_cast<size_t>(target)]);
}

void Playhead::seek(Nanoseconds time) noexcept
{
	if (!frames_.empty()) {
		moveTo(std::clamp(time, frames_.front(), frames_.back()));
	}
}

void Playhead::beginScrub(ScrubDirection direction) noexcept
{
	scrubbing_ = true;
	scrubDirection_ = direction;
	scrubHeld_ = Nanoseconds{0};
}

int32_t Playhead::effectiveRate() const noexcept
{
	if (scrubbing_) {
		const int32_t rate = scrubRate(scrubHeld_);
		return scrubDirection_ == ScrubDirection::Forward ? rate : -rate;
	}
	return playing_ ? rate_ : 0;
}

bool Playhead::atStart() const noexcept
{
	return frames_.empty() || position_ <= frames_.front();
}

bool Playhead::atEnd() const noexcept
{
	return frames_.empty() || position_ >= frames_.back();
}

size_t Playhead::displayedFrame() const noexcept
{
	const auto after = std::upper_bound(frames_.begin(), frames_.end(), position_);
	return after == frames_.begin() ? 0 : static_cast<size_t>(after - frames_.begin()) - 1;
}

int32_t Playhead::scrubRate(Nanoseconds held) const noexcept
{
	if (held <= config_.scrubRampStart) {
		return config_.scrubStartRate;
	}
	if (held >= config_.scrubRampEnd) {
		return config_.scrubEndRate;
	}

	int64_t offset = (held - config_.scrubRampStart).count();
	int64_t span = (config_.scrubRampEnd - config_.scrubRampStart).count();
	// Dropping low bits from both keeps offset * change within 64 bits for any ramp;
	// ramps up to about two seconds lose nothing.
	while (span > std::numeric_limits<int32_t>::max()) {
		offset >>= 1;
		span >>= 1;
	}
	const int64_t change = int64_t{config_.scrubEndRate} - config_.scrubStartRate;
	const int64_t product = offset * change;
	int64_t step = product / span;
	const int64_t rest = product % span;
	if (2 * (rest < 0 ? -rest : rest) >= span) {
		step += product < 0 ? -1 : 1;
	}
	return static_cast<int32_t>(config_.scrubStartRate + step);
}

void Playhead::advanceScrub(int64_t elapsed) noexcept
{
	const int64_t sign = scrubDirection_ == ScrubDirection::Forward ? 1 : -1;

	// The curve is constant, then linear, then constant. Each piece between its
	// corners moves at its exact average rate, so a single call covers exactly the
	// area under the curve and slicing time only adds the rounding of the rates at
	// the cuts.
	while (elapsed > 0) {
		int64_t piece = elapsed;
		int64_t doubledRate = 2 * int64_t{config_.scrubEndRate};
		if (scrubHeld_ < config_.scrubRampStart) {
			piece = std::min(piece, (config_.scrubRampStart - scrubHeld_).count());
			doubledRate = 2 * int64_t{config_.scrubStartRate};
		} else if (scrubHeld_ < config_.scrubRampEnd) {
			piece = std::min(piece, (config_.scrubRampEnd - scrubHeld_).count());
			doubledRate = int64_t{scrubRate(scrubHeld_)} +
				      scrubRate(saturatingAdd(scrubHeld_, Nanoseconds{piece}));
		}

		move(piece, sign * doubledRate);
		scrubHeld_ = saturatingAdd(scrubHeld_, Nanoseconds{piece});
		elapsed -= piece;
	}

	if (position_ > frames_.back()) {
		moveTo(frames_.back());
	} else if (position_ < frames_.front()) {
		moveTo(frames_.front());
	}
}

void Playhead::move(int64_t elapsed, int64_t doubledRate) noexcept
{
	if (doubledRate == 0) {
		return;
	}

	// position_ * kScale + remainder_ grows by exactly elapsed * doubledRate. A long
	// step goes in pieces small enough that whole * doubledRate + carry fits in 64 bits;
	// each piece then covers about half the range of the clock, so once the position
	// saturates at the end it is heading for, the loop stops after a round or two.
	const int64_t magnitude = doubledRate < 0 ? -doubledRate : doubledRate;
	const int64_t maxWhole = kMax / magnitude - 1;
	const Nanoseconds saturated = doubledRate > 0 ? Nanoseconds::max() : Nanoseconds::min();
	while (elapsed > 0 && position_ != saturated) {
		const int64_t piece = elapsed / kScale > maxWhole ? maxWhole * kScale : elapsed;
		elapsed -= piece;

		const int64_t scaledPart = (piece % kScale) * doubledRate + remainder_;
		int64_t carry = scaledPart / kScale;
		remainder_ = scaledPart % kScale;
		if (remainder_ < 0) {
			remainder_ += kScale;
			--carry;
		}
		position_ = saturatingAdd(position_, Nanoseconds{(piece / kScale) * doubledRate + carry});
	}
}

void Playhead::moveTo(Nanoseconds time) noexcept
{
	position_ = time;
	remainder_ = 0;
}

} // namespace tapeloop
