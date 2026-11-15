// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FileIo.hpp"

namespace tapeloop::test {

// Sets the claim hook for as long as the object lives.
class ClaimHookScope {
public:
	explicit ClaimHookScope(ClaimHook hook) noexcept { setClaimHook(hook); }
	~ClaimHookScope() { setClaimHook(nullptr); }

	ClaimHookScope(const ClaimHookScope &) = delete;
	ClaimHookScope &operator=(const ClaimHookScope &) = delete;
	ClaimHookScope(ClaimHookScope &&) = delete;
	ClaimHookScope &operator=(ClaimHookScope &&) = delete;
};

} // namespace tapeloop::test
