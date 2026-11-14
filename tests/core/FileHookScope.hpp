// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/FileIo.hpp"

namespace tapeloop::test {

// Sets the file hook for as long as the object lives.
class FileHookScope {
public:
	explicit FileHookScope(FileHook hook) noexcept { setFileHook(hook); }
	~FileHookScope() { setFileHook(nullptr); }

	FileHookScope(const FileHookScope &) = delete;
	FileHookScope &operator=(const FileHookScope &) = delete;
	FileHookScope(FileHookScope &&) = delete;
	FileHookScope &operator=(FileHookScope &&) = delete;
};

} // namespace tapeloop::test
