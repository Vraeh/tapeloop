// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/DecodePlanner.hpp"
#include "core/GopSource.hpp"
#include "core/MediaTime.hpp"
#include "core/Playhead.hpp"
#include "decode/Picture.hpp"
#include "decode/PictureDecoder.hpp"
#include "obs/GpuPictures.hpp"
#include "obs/PictureRenderer.hpp"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace tapeloop::obs {

// The speeds faster() and slower() move between, in thousandths of real time.
inline constexpr std::array<int32_t, 5> kSpeedPresets{250, 500, 750, 1000, 2000};

// What a player shows, for the source, the dock and tests.
struct PlayerState {
	bool loaded = false;
	bool playing = false;
	bool scrubbing = false;
	// Thousandths of real time; negative plays backwards.
	int32_t rate = 1000;
	Nanoseconds position{0};
	// The frame the playhead is on, and the frame the last render() drew, as indexes into
	// the clip's frames in time order. Nothing is drawn of a clip before its first frame
	// is decoded.
	size_t frame = 0;
	std::optional<size_t> drawn;
	size_t frames = 0;
	// The size of the picture last drawn.
	uint32_t width = 0;
	uint32_t height = 0;
};

// Plays one clip at a time. tick() moves the playhead by the elapsed time it is given,
// the decoder decodes the frame the playhead is on, and render() draws the last frame
// decoded for the current clip. Decoding runs on a thread of the player's own once
// start() is called, and never on the thread of tick() and render(), which in OBS is
// the graphics thread: a frame takes as long to decode as it takes, and the picture on
// screen follows when it is ready. Before start() the caller runs decodeNext() itself.
//
// Controls and load() may come from any thread and take effect at the next tick, in the
// order they were given. tick() and render() run on one thread. Once warm, playing,
// stepping and scrubbing allocate nothing on any of these threads, except what the
// planner needs for a GOP longer than any before it.
//
// Pictures in memory are copied into buffers of the player's own, which render() uploads
// into the caller's renderer. Pictures on the GPU are published into the GpuPictures the
// player was given and drawn from there; without one they are not shown. Whenever its
// generation changes, as after a device rebuild, the frame on screen is handed over again,
// since what was drawn from it is gone and textures in OBS may have lost what they held.
class ReplayPlayer {
public:
	// Makes the decoder on the decoding thread, the first time a clip needs one, so
	// that whatever device it opens is not opened on the caller's thread. Null, or an
	// exception, leaves the clip without pictures, and the next load() tries again.
	using DecoderFactory = std::function<std::unique_ptr<decode::PictureDecoder>()>;

	// The GpuPictures, if any, outlives the player.
	explicit ReplayPlayer(DecoderFactory decoders, DecodePlannerConfig planner = {}, GpuPictures *gpu = nullptr);
	// Calls stop(). The player holds nothing of the graphics context.
	~ReplayPlayer();

	ReplayPlayer(const ReplayPlayer &) = delete;
	ReplayPlayer &operator=(const ReplayPlayer &) = delete;
	ReplayPlayer(ReplayPlayer &&) = delete;
	ReplayPlayer &operator=(ReplayPlayer &&) = delete;

	// Starts the decoding thread, after which the caller no longer runs decodeNext().
	// False when the thread cannot be created, or after stop(). start() and stop() come
	// from the thread that owns the player.
	bool start() noexcept;
	// Waits for the frame being decoded and ends the decoding thread, which closes the
	// decoder it opened. Nothing is decoded after it.
	void stop() noexcept;

	// Starts over with another clip, paused on its first frame at the speed the
	// previous one had. Null leaves nothing to show. False, with nothing changed, when
	// memory for the clip's frame times runs out.
	bool load(std::unique_ptr<GopSource> gops) noexcept;

	void play() noexcept;
	void pause() noexcept;
	void togglePause() noexcept;
	// Thousandths of real time; negative plays backwards. Zero pauses.
	void setRate(int32_t rate) noexcept;
	// To the next speed of kSpeedPresets up or down, or to real time, keeping the
	// direction and the play state. From between two presets the next one is the
	// nearest that way; past the last one that way the speed stays.
	void faster() noexcept;
	void slower() noexcept;
	void normalSpeed() noexcept;
	void reverse() noexcept;
	// Pauses and moves by whole frames.
	void step(int frames) noexcept;
	void seek(Nanoseconds time) noexcept;
	void beginScrub(ScrubDirection direction) noexcept;
	void endScrub() noexcept;

	void tick(Nanoseconds elapsed) noexcept;
	// Inside the graphics context. Draws at the origin of the current matrix, scaled to
	// width by height; false when there is nothing to draw. The renderer is the same on
	// every call.
	bool render(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept;
	// Decodes what changed since the last call. False when nothing had.
	bool decodeNext() noexcept;

	PlayerState state() const noexcept;

private:
	enum class Control {
		Play,
		Pause,
		TogglePause,
		Rate,
		Faster,
		Slower,
		NormalSpeed,
		Reverse,
		Step,
		Seek,
		BeginScrub,
		EndScrub
	};
	struct Command {
		Control control = Control::Play;
		int64_t value = 0;
	};
	// The frame the playhead is on, as the decoder is asked for it.
	struct Wanted {
		uint64_t clip = 0;
		size_t frame = 0;
		Nanoseconds time{0};
		PlayDirection direction = PlayDirection::Forward;
	};
	struct Shown {
		size_t frame = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		bool onGpu = false;
	};
	// What the decoding side takes in one go. republish asks for the frame on screen
	// again.
	struct Request {
		bool newClip = false;
		uint64_t clip = 0;
		std::unique_ptr<GopSource> gops;
		std::optional<Wanted> wanted;
		bool republish = false;
	};
	// A picture in memory of the player's own: the decoding side fills one while
	// render() reads another, and the third holds the newest one ready. A picture on the
	// GPU went to the GpuPictures, and its copy only says which frame it was.
	struct Copy {
		std::vector<uint8_t> bytes;
		decode::Picture picture;
		uint64_t clip = 0;
		size_t frame = 0;
		bool onGpu = false;
	};

	// The controls of a tick are far fewer than this; one past them is dropped.
	static constexpr size_t kMaxCommands = 64;

	void command(Control control, int64_t value) noexcept;
	void apply(const Command &command) noexcept;
	bool takeRequest(Request &request, bool wait) noexcept;
	void decode(Request &request) noexcept;
	// Copies a picture in memory for render(), or publishes one on the GPU; false when
	// render() has nothing new to show.
	bool handOff(const decode::Picture &picture, const Wanted &wanted);
	bool handOffGpu(const decode::Picture &picture, const Wanted &wanted) noexcept;
	// Called by render() when the GpuPictures changes generation.
	void republish() noexcept;
	void decodeLoop() noexcept;

	DecoderFactory decoders_;
	DecodePlannerConfig plannerConfig_;
	GpuPictures *gpu_;

	// Under mutex_: the controls given since the last tick, the clip loaded last, what
	// the decoding side is asked for, and the state.
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::array<Command, kMaxCommands> commands_{};
	size_t commandCount_ = 0;
	uint64_t loads_ = 0;
	bool timesPending_ = false;
	// How many of the commands came before the load.
	size_t loadAt_ = 0;
	std::vector<Nanoseconds> pendingTimes_;
	bool gopsPending_ = false;
	std::unique_ptr<GopSource> pendingGops_;
	std::optional<Wanted> wanted_;
	bool republish_ = false;
	uint64_t requests_ = 0;
	bool stopping_ = false;
	PlayerState state_;

	// The thread of tick() and render() only.
	Playhead playhead_;
	std::array<Command, kMaxCommands> applying_{};
	std::vector<Nanoseconds> loadingTimes_;
	uint64_t clip_ = 0;
	size_t frames_ = 0;
	std::optional<Wanted> posted_;
	// The frame the renderer or the GpuPictures holds, while it is of the current clip.
	std::optional<Shown> onScreen_;
	// The size the GpuPictures was last prepared for, which is tried once for each clip,
	// and the generation it had at the last render().
	uint32_t preparedWidth_ = 0;
	uint32_t preparedHeight_ = 0;
	uint64_t gpuGeneration_ = 0;

	// The decoding side only. The planner goes before the decoder it gives frames back
	// to.
	std::unique_ptr<decode::PictureDecoder> decoder_;
	std::unique_ptr<DecodePlanner> planner_;
	uint64_t handled_ = 0;
	uint64_t decodingClip_ = 0;
	std::optional<Wanted> decoded_;
	std::optional<Wanted> handedOff_;

	// copies_[back_] belongs to the decoding side and copies_[front_] to render(); under
	// picturesMutex_, ready_ and fresh_, set when ready_ holds a picture render() has
	// not taken, and the size of the last picture on the GPU, for render() to prepare
	// the GpuPictures for.
	std::array<Copy, 3> copies_;
	size_t back_ = 0;
	size_t front_ = 1;
	std::mutex picturesMutex_;
	size_t ready_ = 2;
	bool fresh_ = false;
	uint32_t gpuWidth_ = 0;
	uint32_t gpuHeight_ = 0;

	std::thread thread_;
};

} // namespace tapeloop::obs
