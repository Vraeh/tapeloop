// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FrameDecoder.hpp"
#include "decode/Picture.hpp"

#include <optional>

namespace tapeloop::decode {

// A FrameDecoder whose frames can be looked at. The code that shows the frames knows
// only this interface, so it builds without the library that decodes them.
class PictureDecoder : public FrameDecoder {
public:
	// The picture of a frame this decoder returned and has not taken back; empty for any
	// other frame. Valid until that frame is released or the decoder closed.
	virtual std::optional<Picture> picture(const DecodedFrame &frame) const noexcept = 0;
};

} // namespace tapeloop::decode
