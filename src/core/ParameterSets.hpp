// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace tapeloop {

// Copies an Annex B packet into out, which is at least as large as the packet, without
// the parameter sets before its first slice when config, the run's configuration in
// Annex B, holds each of them byte for byte: OBS's encoders repeat them before every
// keyframe, and a decoder that has them from the configuration parses each again, with
// the allocations that takes. Returns the bytes written. When one of them differs, the
// packet is copied whole: the others refer to it, and a decoder drops them, or ties them
// to the set it had, when they come without it. So is a packet that is not Annex B, or
// that would be left without anything.
size_t copyWithoutKnownParameterSets(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet,
				     std::span<uint8_t> out) noexcept;

} // namespace tapeloop
