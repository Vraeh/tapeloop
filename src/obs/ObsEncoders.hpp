// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/EncoderPolicy.hpp"

#include <obs.h>

#include <vector>

namespace tapeloop::obs {

// Every registered video encoder type, as the encoder policy sees it.
std::vector<EncoderInfo> registeredVideoEncoders();

// The vendor of the adapter OBS renders on. Enters the graphics context, so it must not
// be called from a render callback or with the context already entered.
Vendor renderAdapterVendor();

// The settings in the obs_data form encoders read. The caller owns the result.
obs_data_t *createEncoderSettings(const EncoderSettings &settings);

} // namespace tapeloop::obs
