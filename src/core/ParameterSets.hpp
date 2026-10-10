// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/Gop.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace tapeloop {

// Copies an Annex B packet into out, which is at least as large as the packet, without
// the parameter sets before its first slice that config, the run's configuration in
// Annex B, holds byte for byte: OBS's encoders repeat them before every keyframe, and a
// decoder that has them from the configuration parses each again, with the allocations
// that takes. Returns the bytes written. A packet that is not Annex B, or that would be
// left without anything, is copied whole.
size_t copyWithoutKnownParameterSets(VideoCodec codec, std::span<const uint8_t> config, std::span<const uint8_t> packet,
				     std::span<uint8_t> out) noexcept;

} // namespace tapeloop
