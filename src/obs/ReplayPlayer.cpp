// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ReplayPlayer.hpp"

#include <util/base.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace tapeloop::obs {
namespace {

// Plays a clip held elsewhere as well.
class SharedGops final : public GopSource {
public:
	explicit SharedGops(std::shared_ptr<GopSource> gops) noexcept : gops_(std::move(gops)) {}

	size_t gopCount() const noexcept override { return gops_->gopCount(); }
	size_t packetCount(size_t gop) const noexcept override { return gops_->packetCount(gop); }
	FrameLocation locate(Nanoseconds t) const noexcept override { return gops_->locate(t); }
	std::vector<Nanoseconds> frameTimes() const override { return gops_->frameTimes(); }
	std::shared_ptr<const Gop> read(size_t gop) override { return gops_->read(gop); }

private:
	std::shared_ptr<GopSource> gops_;
};

int32_t nextPreset(int32_t rate, bool up) noexcept
{
	const int32_t speed = rate < 0 ? -rate : rate;
	int32_t next = speed;
	if (up) {
		const auto above = std::upper_bound(kSpeedPresets.begin(), kSpeedPresets.end(), speed);
		if (above != kSpeedPresets.end()) {
			next = *above;
		}
	} else {
		const auto below = std::lower_bound(kSpeedPresets.begin(), kSpeedPresets.end(), speed);
		if (below != kSpeedPresets.begin()) {
			next = *(below - 1);
		}
	}
	return rate < 0 ? -next : next;
}

} // namespace

ReplayPlayer::ReplayPlayer(DecoderFactory decoders, DecodePlannerConfig planner, GpuPictures *gpu)
	: decoders_(std::move(decoders)),
	  plannerConfig_(planner),
	  gpu_(gpu)
{
}

ReplayPlayer::~ReplayPlayer()
{
	stop();
}

bool ReplayPlayer::start() noexcept
{
	{
		const std::lock_guard lock(mutex_);
		if (stopping_) {
			return false;
		}
	}
	if (thread_.joinable()) {
		return true;
	}
	try {
		thread_ = std::thread([this] { decodeLoop(); });
	} catch (...) {
		return false;
	}
	return true;
}

void ReplayPlayer::stop() noexcept
{
	{
		const std::lock_guard lock(mutex_);
		stopping_ = true;
	}
	wake_.notify_all();
	if (thread_.joinable()) {
		thread_.join();
	}
}

bool ReplayPlayer::load(std::unique_ptr<GopSource> gops) noexcept
{
	std::vector<Nanoseconds> times;
	try {
		if (gops) {
			times = gops->frameTimes();
		}
	} catch (...) {
		return false;
	}
	// In the playhead's order, so that the frame count is the playhead's.
	std::sort(times.begin(), times.end());
	times.erase(std::unique(times.begin(), times.end()), times.end());

	// A clip no tick or decoder took yet goes once the lock is released.
	std::unique_ptr<GopSource> superseded;
	{
		const std::lock_guard lock(mutex_);
		++loads_;
		timesPending_ = true;
		loadAt_ = commandCount_;
		pendingTimes_.swap(times);
		gopsPending_ = true;
		superseded = std::exchange(pendingGops_, std::move(gops));
		++requests_;
	}
	wake_.notify_one();
	return true;
}

void ReplayPlayer::play() noexcept
{
	command(Control::Play, 0);
}

void ReplayPlayer::pause() noexcept
{
	command(Control::Pause, 0);
}

void ReplayPlayer::togglePause() noexcept
{
	command(Control::TogglePause, 0);
}

void ReplayPlayer::setRate(int32_t rate) noexcept
{
	command(Control::Rate, rate);
}

void ReplayPlayer::faster() noexcept
{
	command(Control::Faster, 0);
}

void ReplayPlayer::slower() noexcept
{
	command(Control::Slower, 0);
}

void ReplayPlayer::normalSpeed() noexcept
{
	command(Control::NormalSpeed, 0);
}

void ReplayPlayer::reverse() noexcept
{
	command(Control::Reverse, 0);
}

void ReplayPlayer::step(int frames) noexcept
{
	command(Control::Step, frames);
}

void ReplayPlayer::seek(Nanoseconds time) noexcept
{
	command(Control::Seek, time.count());
}

void ReplayPlayer::beginScrub(ScrubDirection direction) noexcept
{
	command(Control::BeginScrub, direction == ScrubDirection::Forward ? 1 : -1);
}

void ReplayPlayer::endScrub() noexcept
{
	command(Control::EndScrub, 0);
}

void ReplayPlayer::command(Control control, int64_t value) noexcept
{
	const std::lock_guard lock(mutex_);
	if (commandCount_ < commands_.size()) {
		commands_[commandCount_++] = {control, value};
	}
}

void ReplayPlayer::apply(const Command &command) noexcept
{
	switch (command.control) {
	case Control::Play:
		playhead_.play();
		break;
	case Control::Pause:
		playhead_.pause();
		break;
	case Control::TogglePause:
		playhead_.togglePause();
		break;
	case Control::Rate:
		playhead_.setRate(static_cast<int32_t>(command.value));
		break;
	case Control::Faster:
		playhead_.setRate(nextPreset(playhead_.rate(), true));
		break;
	case Control::Slower:
		playhead_.setRate(nextPreset(playhead_.rate(), false));
		break;
	case Control::NormalSpeed:
		playhead_.setRate(playhead_.rate() < 0 ? -1000 : 1000);
		break;
	case Control::Reverse:
		playhead_.reverse();
		break;
	case Control::Step:
		playhead_.step(static_cast<int>(command.value));
		break;
	case Control::Seek:
		playhead_.seek(Nanoseconds{command.value});
		break;
	case Control::BeginScrub:
		playhead_.beginScrub(command.value > 0 ? ScrubDirection::Forward : ScrubDirection::Backward);
		break;
	case Control::EndScrub:
		playhead_.endScrub();
		break;
	}
}

void ReplayPlayer::tick(Nanoseconds elapsed) noexcept
{
	size_t count = 0;
	std::optional<size_t> loadAt;
	uint64_t loaded = 0;
	{
		const std::lock_guard lock(mutex_);
		count = commandCount_;
		std::copy_n(commands_.begin(), count, applying_.begin());
		commandCount_ = 0;
		if (timesPending_) {
			timesPending_ = false;
			loadAt = loadAt_;
			loadingTimes_.swap(pendingTimes_);
			loaded = loads_;
		}
	}

	// The playhead moves before the controls apply, so that a step or a seek lands
	// where it was asked and is on screen at once.
	playhead_.advance(elapsed);
	for (size_t i = 0; i <= count; ++i) {
		if (loadAt == i) {
			frames_ = loadingTimes_.size();
			// Moving the times into the playhead leaves loadingTimes_ empty for the next
			// swap.
			playhead_.load(std::move(loadingTimes_));
			clip_ = loaded;
			onScreen_.reset();
		}
		if (i < count) {
			apply(applying_[i]);
		}
	}

	std::optional<Wanted> wanted;
	if (!playhead_.empty()) {
		// Paused, the direction is the one play would take, for the decoder to prepare.
		const int32_t rate = playhead_.effectiveRate() != 0 ? playhead_.effectiveRate() : playhead_.rate();
		wanted = Wanted{clip_, playhead_.displayedFrame(), playhead_.position(),
				rate < 0 ? PlayDirection::Backward : PlayDirection::Forward};
	}
	const bool post = wanted && !(posted_ && posted_->clip == wanted->clip && posted_->frame == wanted->frame &&
				      posted_->direction == wanted->direction);
	{
		const std::lock_guard lock(mutex_);
		if (post) {
			wanted_ = wanted;
			++requests_;
		}
		state_.loaded = !playhead_.empty();
		state_.playing = playhead_.playing();
		state_.scrubbing = playhead_.scrubbing();
		state_.rate = playhead_.rate();
		state_.position = playhead_.position();
		state_.frame = playhead_.displayedFrame();
		state_.frames = frames_;
		if (!onScreen_) {
			state_.drawn.reset();
		}
	}
	if (post) {
		posted_ = wanted;
		wake_.notify_one();
	}
}

bool ReplayPlayer::render(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept
{
	bool took = false;
	uint32_t gpuWidth = 0;
	uint32_t gpuHeight = 0;
	{
		const std::lock_guard lock(picturesMutex_);
		if (fresh_) {
			std::swap(front_, ready_);
			fresh_ = false;
			took = true;
		}
		gpuWidth = gpuWidth_;
		gpuHeight = gpuHeight_;
	}
	if (gpu_) {
		// A picture of a size the GpuPictures is not prepared for could not be published.
		// Preparing it changes the generation, which brings that picture back; when it
		// cannot be prepared, the picture comes back in memory instead.
		if (!gpuRefused_ && gpuWidth != 0 && (gpuWidth != preparedWidth_ || gpuHeight != preparedHeight_)) {
			preparedWidth_ = gpuWidth;
			preparedHeight_ = gpuHeight;
			if (!gpu_->prepare(gpuWidth, gpuHeight)) {
				gpuRefused_ = true;
				decodeInMemory();
			}
		}
		const uint64_t generation = gpu_->generation();
		if (generation != gpuGeneration_) {
			gpuGeneration_ = generation;
			republish();
		}
	}

	const Copy &copy = copies_[front_];
	const bool current = took && copy.clip == clip_;
	if (current && !copy.onGpu) {
		if (renderer.upload(copy.picture)) {
			onScreen_ = Shown{copy.frame, copy.picture.width, copy.picture.height, false};
		} else {
			onScreen_.reset();
		}
	} else if (gpu_ && (current || (onScreen_ && onScreen_->onGpu))) {
		// The picture published last may be newer than the copy that said one was, but
		// it is of the same clip or of one after it, never of one before.
		const std::optional<GpuPictures::Tag> tag = gpu_->latch();
		if (tag && tag->clip == clip_) {
			onScreen_ = Shown{tag->frame, tag->width, tag->height, true};
		} else {
			onScreen_.reset();
		}
	}
	bool drawn = false;
	if (onScreen_) {
		drawn = onScreen_->onGpu ? gpu_->draw(renderer, width, height) : renderer.draw(width, height);
	}

	const std::lock_guard lock(mutex_);
	if (drawn) {
		state_.drawn = onScreen_->frame;
		state_.width = onScreen_->width;
		state_.height = onScreen_->height;
	} else {
		state_.drawn.reset();
	}
	return drawn;
}

bool ReplayPlayer::decodeNext() noexcept
{
	Request request;
	if (!takeRequest(request, false)) {
		return false;
	}
	decode(request);
	return true;
}

bool ReplayPlayer::takeRequest(Request &request, bool wait) noexcept
{
	std::unique_lock lock(mutex_);
	if (wait) {
		wake_.wait(lock, [this] { return stopping_ || requests_ != handled_; });
	}
	if (stopping_ || requests_ == handled_) {
		return false;
	}
	// Whatever happens to it, a request is handled once, so that one that fails is not
	// tried again on every pass.
	handled_ = requests_;
	request.republish = std::exchange(republish_, false);
	request.inMemory = inMemory_;
	request.newClip = std::exchange(gopsPending_, false);
	if (request.newClip) {
		request.clip = loads_;
		request.gops = std::move(pendingGops_);
	}
	request.wanted = wanted_;
	return true;
}

void ReplayPlayer::decode(Request &request) noexcept
{
	if (request.republish) {
		decoded_.reset();
		handedOff_.reset();
	}
	bool loading = request.newClip;
	try {
		bool opening = loading;
		if (request.inMemory && decoder_ && decoderOnGpu_) {
			// The decoder made in its place picks the clip up at the frame the playhead
			// is on.
			planner_.reset();
			decoder_.reset();
			decoded_.reset();
			handedOff_.reset();
			opening = true;
			blog(LOG_WARNING,
			     "[tapeloop] A replay player cannot show pictures decoded on the GPU, so it decodes "
			     "them in memory from now on. Replays work, but this costs CPU time: it is not the "
			     "optimal path");
		}
		if (loading) {
			decodingClip_ = request.clip;
			decoded_.reset();
			handedOff_.reset();
			gops_ = std::move(request.gops);
		}
		if (opening && !planner_ && gops_) {
			if (!decoder_ && decoders_) {
				decoderOnGpu_ = gpu_ && !request.inMemory;
				decoder_ = decoders_(decoderOnGpu_);
			}
			if (decoder_) {
				planner_ = std::make_unique<DecodePlanner>(*decoder_, plannerConfig_);
			}
		}
		if (opening && planner_) {
			planner_->load(gops_ ? std::make_unique<SharedGops>(gops_) : nullptr);
		}
		loading = false;

		if (!planner_ || !request.wanted || request.wanted->clip != decodingClip_) {
			return;
		}
		const Wanted wanted = *request.wanted;
		if (decoded_ && decoded_->frame == wanted.frame && decoded_->direction == wanted.direction) {
			return;
		}
		decoded_ = wanted;
		const DecodeResult result = planner_->frameAt(wanted.time);
		// A frame that cannot be decoded leaves the one before on screen.
		if (result.status == DecodeStatus::Ok && !(handedOff_ && handedOff_->frame == wanted.frame)) {
			if (const std::optional<decode::Picture> picture = decoder_->picture(result.frame);
			    picture && handOff(*picture, wanted)) {
				handedOff_ = wanted;
			}
		}
		planner_->prefetch(wanted.direction);
	} catch (...) {
		// Out of memory. A planner that failed to take the new clip may still hold the
		// one before, whose frames must not show for it, and so may gops_.
		if (loading) {
			planner_.reset();
			gops_.reset();
		}
	}
}

bool ReplayPlayer::handOff(const decode::Picture &picture, const Wanted &wanted)
{
	if (picture.width == 0 || picture.height == 0) {
		return false;
	}
	if (picture.texture) {
		return handOffGpu(picture, wanted);
	}
	const bool nv12 = picture.layout == decode::PixelLayout::Nv12;
	const uint32_t chromaWidth = (picture.width + 1) / 2;
	const uint32_t chromaHeight = (picture.height + 1) / 2;
	const std::array<uint32_t, 3> rowBytes{picture.width, nv12 ? chromaWidth * 2 : chromaWidth,
					       nv12 ? 0 : chromaWidth};
	const std::array<uint32_t, 3> rows{picture.height, chromaHeight, nv12 ? 0 : chromaHeight};
	size_t total = 0;
	for (size_t plane = 0; plane < 3; ++plane) {
		total += size_t{rowBytes[plane]} * rows[plane];
	}

	// Only render() reads the others, so this one is written without the lock, and it
	// grows only when a picture is larger than any before.
	Copy &copy = copies_[back_];
	copy.bytes.resize(total);
	copy.picture = picture;
	uint8_t *at = copy.bytes.data();
	for (size_t plane = 0; plane < 3; ++plane) {
		if (rows[plane] == 0) {
			copy.picture.planes[plane] = nullptr;
			copy.picture.strides[plane] = 0;
			continue;
		}
		for (uint32_t row = 0; row < rows[plane]; ++row) {
			std::memcpy(at + size_t{row} * rowBytes[plane],
				    picture.planes[plane] + size_t{row} * picture.strides[plane], rowBytes[plane]);
		}
		copy.picture.planes[plane] = at;
		copy.picture.strides[plane] = rowBytes[plane];
		at += size_t{rowBytes[plane]} * rows[plane];
	}
	copy.clip = wanted.clip;
	copy.frame = wanted.frame;
	copy.onGpu = false;

	const std::lock_guard lock(picturesMutex_);
	std::swap(back_, ready_);
	fresh_ = true;
	return true;
}

bool ReplayPlayer::handOffGpu(const decode::Picture &picture, const Wanted &wanted) noexcept
{
	if (!gpu_) {
		return false;
	}
	{
		const std::lock_guard lock(picturesMutex_);
		gpuWidth_ = picture.width;
		gpuHeight_ = picture.height;
	}
	if (!gpu_->publish(picture, {wanted.clip, wanted.frame, picture.width, picture.height})) {
		return false;
	}
	Copy &copy = copies_[back_];
	copy.clip = wanted.clip;
	copy.frame = wanted.frame;
	copy.onGpu = true;

	const std::lock_guard lock(picturesMutex_);
	std::swap(back_, ready_);
	fresh_ = true;
	return true;
}

void ReplayPlayer::republish() noexcept
{
	{
		const std::lock_guard lock(mutex_);
		republish_ = true;
		++requests_;
	}
	wake_.notify_one();
}

void ReplayPlayer::decodeInMemory() noexcept
{
	{
		const std::lock_guard lock(mutex_);
		inMemory_ = true;
		++requests_;
	}
	wake_.notify_one();
}

void ReplayPlayer::decodeLoop() noexcept
{
	while (true) {
		Request request;
		if (!takeRequest(request, true)) {
			break;
		}
		decode(request);
	}
	// The decoder closes on the thread that opened it.
	planner_.reset();
	decoder_.reset();
	gops_.reset();
}

PlayerState ReplayPlayer::state() const noexcept
{
	const std::lock_guard lock(mutex_);
	return state_;
}

} // namespace tapeloop::obs
