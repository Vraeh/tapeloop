// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace tapeloop::test {

// A new empty directory under the system's temporary one, removed with everything in it
// when the object goes.
class TempDirectory {
public:
	TempDirectory()
	{
		// ctest runs every test case in a process of its own, possibly at the same time.
		static std::atomic<unsigned> counter{0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path_ = std::filesystem::temp_directory_path() /
			("tapeloop-test-" + std::to_string(stamp) + "-" + std::to_string(std::random_device{}()) + "-" +
			 std::to_string(counter++));
		std::filesystem::create_directories(path_);
	}

	~TempDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path_, ignored);
	}

	TempDirectory(const TempDirectory &) = delete;
	TempDirectory &operator=(const TempDirectory &) = delete;

	const std::filesystem::path &path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
};

} // namespace tapeloop::test
