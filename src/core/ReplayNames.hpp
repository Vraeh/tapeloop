// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tapeloop {

// A moment as the local clock shows it, for the names of folders and files.
struct LocalTime {
	int year = 1970;
	int month = 1;
	int day = 1;
	int hour = 0;
	int minute = 0;
	int second = 0;

	bool operator==(const LocalTime &) const = default;
};

// The epoch when the platform cannot convert the time.
LocalTime localTimeOf(std::chrono::system_clock::time_point time) noexcept;

// The name made valid as a file or folder name on Windows, macOS and Linux: characters
// one of them refuses and bytes that are not UTF-8 become '_', as does a leading dot,
// which would hide it; trailing spaces and dots go, since Windows drops them; a Windows
// device name gets a '_' in front; and it is cut to at most 120 bytes, at a character
// boundary. Never empty.
std::string safeFileName(std::string_view name);

// "<scene collection> YYYY-MM-DD HH-MM", the folder of one broadcast (D-064, D-073).
std::string broadcastFolderName(std::string_view sceneCollection, LocalTime time);
// "YYYY-MM-DD HH-MM-SS", the name of a replay's manifest without its extension.
std::string replayFileStem(LocalTime time);
// "<source key>-NNNNNN.tpls", one segment of a source in the data folder, with the key
// made safe.
std::string segmentFileName(std::string_view sourceKey, uint32_t sequence);
// The sequence of a segment of this source from its file name; nothing for any other
// file.
std::optional<uint32_t> segmentSequenceOf(std::string_view fileName, std::string_view sourceKey);

} // namespace tapeloop
