// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FailurePacer.hpp"
#include "decode/Picture.hpp"
#include "obs/GpuPictures.hpp"
#include "obs/PictureRenderer.hpp"

#include <obs.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace tapeloop::obs {

// Windows only. Hands pictures a decoder keeps on the GPU to the graphics thread without
// a trip through memory: OBS makes two NV12 textures shared with a keyed mutex, the
// decoder's device copies each picture into the one the graphics thread is not drawing,
// and the graphics thread switches to it at its next latch(). OBS's device and the
// decoder's are different devices on the same adapter (renderAdapterLuid).
//
// The constructor, the destructor, prepare(), latch(), draw() and generation() run on the
// graphics thread inside the graphics context; publish() on the decoder's thread. After a
// device loss OBS makes the textures again with new handles, which publish() then opens;
// what they held is gone, and the generation says so. A call on a shared texture or its
// keyed mutex that fails is said in the log, at most once a minute for each kind of call,
// since it can fail on every frame.
class SharedPictures final : public GpuPictures {
public:
	SharedPictures() noexcept;
	~SharedPictures() override;

	SharedPictures(const SharedPictures &) = delete;
	SharedPictures &operator=(const SharedPictures &) = delete;
	SharedPictures(SharedPictures &&) = delete;
	SharedPictures &operator=(SharedPictures &&) = delete;

	// Makes the textures for pictures of this size, if they are not made yet. False when
	// OBS has no shared NV12 textures, on adapters without NV12 support or outside
	// Direct3D 11; the pictures then have to come another way.
	bool prepare(uint32_t width, uint32_t height) noexcept override;
	// Copies a picture on the GPU, of the prepared size, into the texture that is not on
	// screen. False when nothing was copied; the picture stays the decoder's either way.
	bool publish(const decode::Picture &picture, Tag tag) noexcept override;
	// Takes the texture published last from the decoder, if there is one, and gives the
	// other one back to it.
	std::optional<Tag> latch() noexcept override;
	bool draw(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept override;
	uint64_t generation() const noexcept override;
	// Whether the last prepare() could not give shared textures, so that pictures have
	// to be copied through memory: the adapter or renderer has no shared NV12 textures,
	// said once in the log for all, or making them failed, said when it starts. A
	// prepare() that succeeds clears it. Read from any thread.
	bool copiesThroughMemory() const noexcept { return copiesThroughMemory_.load(std::memory_order_relaxed); }

private:
	struct Opened;

	static void releaseDevice(void *data) noexcept;
	static void rebuildDevice(void *device, void *data) noexcept;
	void destroyTextures() noexcept;

	// Graphics thread only.
	std::array<gs_texture_t *, 2> luma_{};
	std::array<gs_texture_t *, 2> chroma_{};
	FailurePacer takeFailures_;
	FailurePacer giveFailures_;
	FailurePacer setUpFailures_;

	// Shared by both threads, under mutex_. keys_ is the key each texture was last
	// released with; generation_ changes whenever the textures are made again.
	mutable std::mutex mutex_;
	std::array<uint32_t, 2> handles_{};
	std::array<uint64_t, 2> keys_{};
	std::array<decode::Picture, 2> colors_{};
	std::array<Tag, 2> tags_{};
	uint64_t generation_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	int ready_ = -1;
	int drawing_ = -1;
	int writing_ = -1;

	// Decoder thread only: the textures as its device opened them.
	std::unique_ptr<Opened> opened_;
	std::atomic<bool> copiesThroughMemory_{false};
};

} // namespace tapeloop::obs
