// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/SharedPictures.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <new>

namespace tapeloop::obs {

using Microsoft::WRL::ComPtr;

namespace {

// A texture released with kFree is the decoder's to write; with kFilled, the graphics
// thread's to show. OBS holds key 0 on a texture it has just made.
constexpr uint64_t kFree = 1;
constexpr uint64_t kFilled = 2;
// Nobody else holds a texture the decoder writes, so this only guards against a driver
// that never answers.
constexpr DWORD kCopyWaitMs = 100;

} // namespace

struct SharedPictures::Opened {
	ComPtr<ID3D11Device> device;
	std::array<ComPtr<ID3D11Texture2D>, 2> textures;
	std::array<ComPtr<IDXGIKeyedMutex>, 2> mutexes;
	uint64_t generation = 0;
};

SharedPictures::SharedPictures() noexcept : opened_(new(std::nothrow) Opened)
{
	gs_device_loss callbacks = {};
	callbacks.device_loss_release = releaseDevice;
	callbacks.device_loss_rebuild = rebuildDevice;
	callbacks.data = this;
	gs_register_loss_callbacks(&callbacks);
}

SharedPictures::~SharedPictures()
{
	gs_unregister_loss_callbacks(this);
	destroyTextures();
}

bool SharedPictures::prepare(uint32_t width, uint32_t height) noexcept
{
	if (!opened_ || !gs_nv12_available() || width == 0 || height == 0 || (width & 1) != 0 || (height & 1) != 0) {
		return false;
	}
	// Only this thread writes the size, so reading it here needs no lock.
	if (luma_[0] && width == width_ && height == height_) {
		return true;
	}
	destroyTextures();

	std::array<uint32_t, 2> handles{};
	for (size_t i = 0; i < luma_.size(); ++i) {
		// Without real NV12 support libobs would make two separate textures instead,
		// which share no handle, so the handle is checked too.
		if (!gs_texture_create_nv12(&luma_[i], &chroma_[i], width, height, GS_SHARED_KM_TEX) ||
		    (handles[i] = gs_texture_get_shared_handle(luma_[i])) == GS_INVALID_HANDLE) {
			destroyTextures();
			return false;
		}
	}

	std::lock_guard lock(mutex_);
	for (size_t i = 0; i < luma_.size(); ++i) {
		gs_texture_release_sync(luma_[i], kFree);
		keys_[i] = kFree;
	}
	handles_ = handles;
	width_ = width;
	height_ = height;
	ready_ = -1;
	drawing_ = -1;
	++generation_;
	return true;
}

bool SharedPictures::publish(const decode::Picture &picture) noexcept
{
	if (!opened_ || !picture.texture) {
		return false;
	}

	int target = 0;
	uint32_t handle = 0;
	uint64_t key = 0;
	uint64_t generation = 0;
	{
		std::lock_guard lock(mutex_);
		if (handles_[0] == 0 || picture.width != width_ || picture.height != height_) {
			return false;
		}
		target = drawing_ == 0 ? 1 : 0;
		// A picture published but not shown yet is replaced by this one.
		if (ready_ == target) {
			ready_ = -1;
		}
		writing_ = target;
		handle = handles_[static_cast<size_t>(target)];
		key = keys_[static_cast<size_t>(target)];
		generation = generation_;
	}

	auto *source = static_cast<ID3D11Texture2D *>(picture.texture);
	ComPtr<ID3D11Device> device;
	source->GetDevice(&device);
	Opened &opened = *opened_;
	if (opened.generation != generation || opened.device != device) {
		opened.textures = {};
		opened.mutexes = {};
		opened.device = device;
		opened.generation = generation;
	}
	const auto slot = static_cast<size_t>(target);
	if (!opened.textures[slot]) {
		auto *shared = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
		if (FAILED(device->OpenSharedResource(shared, IID_PPV_ARGS(&opened.textures[slot]))) ||
		    FAILED(opened.textures[slot].As(&opened.mutexes[slot]))) {
			opened.textures[slot].Reset();
			opened.mutexes[slot].Reset();
		}
	}

	bool copied = false;
	// AcquireSync also succeeds, in the HRESULT sense, with WAIT_TIMEOUT.
	if (opened.mutexes[slot] && opened.mutexes[slot]->AcquireSync(key, kCopyWaitMs) == S_OK) {
		ComPtr<ID3D11DeviceContext> context;
		device->GetImmediateContext(&context);
		const D3D11_BOX box = {0, 0, 0, picture.width, picture.height, 1};
		context->CopySubresourceRegion(opened.textures[slot].Get(), 0, 0, 0, 0, source, picture.subresource,
					       &box);
		opened.mutexes[slot]->ReleaseSync(kFilled);
		copied = true;
	}

	std::lock_guard lock(mutex_);
	writing_ = -1;
	if (copied && generation == generation_) {
		keys_[slot] = kFilled;
		colors_[slot] = picture;
		ready_ = target;
	}
	return copied && generation == generation_;
}

bool SharedPictures::draw(PictureRenderer &renderer, uint32_t width, uint32_t height) noexcept
{
	decode::Picture colors;
	int shown = -1;
	{
		std::lock_guard lock(mutex_);
		if (ready_ >= 0 && ready_ != writing_ && ready_ != drawing_ &&
		    gs_texture_acquire_sync(luma_[static_cast<size_t>(ready_)], kFilled, 0) == 0) {
			if (drawing_ >= 0) {
				gs_texture_release_sync(luma_[static_cast<size_t>(drawing_)], kFree);
				keys_[static_cast<size_t>(drawing_)] = kFree;
			}
			drawing_ = ready_;
			ready_ = -1;
		}
		shown = drawing_;
		if (shown >= 0) {
			colors = colors_[static_cast<size_t>(shown)];
		}
	}
	if (shown < 0) {
		return false;
	}
	const auto slot = static_cast<size_t>(shown);
	return renderer.drawNv12(luma_[slot], chroma_[slot], colors, width, height);
}

void SharedPictures::releaseDevice(void *data) noexcept
{
	auto &self = *static_cast<SharedPictures *>(data);
	std::lock_guard lock(self.mutex_);
	self.handles_ = {};
	self.ready_ = -1;
	self.drawing_ = -1;
	++self.generation_;
}

void SharedPictures::rebuildDevice(void *, void *data) noexcept
{
	// OBS made the textures again, with new handles, and holds them with key 0.
	auto &self = *static_cast<SharedPictures *>(data);
	std::lock_guard lock(self.mutex_);
	if (!self.luma_[0]) {
		return;
	}
	for (size_t i = 0; i < self.luma_.size(); ++i) {
		self.handles_[i] = gs_texture_get_shared_handle(self.luma_[i]);
		gs_texture_release_sync(self.luma_[i], kFree);
		self.keys_[i] = kFree;
	}
	++self.generation_;
}

void SharedPictures::destroyTextures() noexcept
{
	{
		std::lock_guard lock(mutex_);
		handles_ = {};
		width_ = 0;
		height_ = 0;
		ready_ = -1;
		drawing_ = -1;
		++generation_;
	}
	for (size_t i = 0; i < luma_.size(); ++i) {
		if (luma_[i]) {
			gs_texture_destroy(luma_[i]);
		}
		if (chroma_[i]) {
			gs_texture_destroy(chroma_[i]);
		}
		luma_[i] = nullptr;
		chroma_[i] = nullptr;
	}
}

} // namespace tapeloop::obs
