// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/PictureSurface.hpp"

#include <cstdint>
#include <functional>

namespace tapeloop::test {

// What a fake picture surface was asked, and whether it is still there.
struct SurfaceLog {
	// Makes attach() refuse.
	bool refuse = false;
	int attaches = 0;
	int resizes = 0;
	int detaches = 0;
	// Calls out of order: resizing or enabling with no window, attaching with one.
	int misuses = 0;
	bool attached = false;
	bool enabled = false;
	uintptr_t window = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool destroyed = false;
	// Called on each detach from a window.
	std::function<void()> onDetach;
};

// Records into a log the test keeps, so it can be read after the surface is gone. Like
// libobs's displays it starts enabled.
class FakeSurface final : public tapeloop::ui::PictureSurface {
public:
	explicit FakeSurface(SurfaceLog &log) : log_(log) {}
	~FakeSurface() override { log_.destroyed = true; }

	FakeSurface(const FakeSurface &) = delete;
	FakeSurface &operator=(const FakeSurface &) = delete;
	FakeSurface(FakeSurface &&) = delete;
	FakeSurface &operator=(FakeSurface &&) = delete;

	bool attach(uintptr_t window, uint32_t width, uint32_t height) override
	{
		++log_.attaches;
		if (log_.attached) {
			++log_.misuses;
		}
		if (log_.refuse) {
			return false;
		}
		log_.attached = true;
		log_.enabled = true;
		log_.window = window;
		log_.width = width;
		log_.height = height;
		return true;
	}

	void resize(uint32_t width, uint32_t height) override
	{
		++log_.resizes;
		if (!log_.attached) {
			++log_.misuses;
		}
		log_.width = width;
		log_.height = height;
	}

	void setEnabled(bool enabled) override
	{
		if (!log_.attached) {
			++log_.misuses;
		}
		log_.enabled = enabled;
	}

	void detach() noexcept override
	{
		if (log_.attached) {
			++log_.detaches;
			if (log_.onDetach) {
				log_.onDetach();
			}
		}
		log_.attached = false;
	}

private:
	SurfaceLog &log_;
};

} // namespace tapeloop::test
