// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <cstdint>
#include <optional>

namespace tapeloop::obs {

// The LUID of the adapter OBS renders with, so that a decoder can sit on the same GPU and
// share textures with OBS. Graphics thread, inside the graphics context. Empty unless OBS
// renders with Direct3D 11.
std::optional<uint64_t> renderAdapterLuid() noexcept;

} // namespace tapeloop::obs
