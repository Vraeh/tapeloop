// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/ReplayNames.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <limits>

namespace tapeloop {
namespace {

constexpr std::string_view kSegmentExtension = ".tpls";
constexpr size_t kSequenceDigits = 6;

// Characters Windows refuses in a name; '/' is the only one POSIX systems refuse too.
constexpr std::string_view kRefused = "<>:\"/\\|?*";

constexpr std::array<std::string_view, 4> kDeviceNames = {"CON", "PRN", "AUX", "NUL"};
constexpr std::array<std::string_view, 2> kNumberedDeviceNames = {"COM", "LPT"};

bool isContinuation(unsigned char byte)
{
	return (byte & 0xC0) == 0x80;
}

// The length of the valid UTF-8 sequence that starts at text[at], or zero.
size_t sequenceLength(std::string_view text, size_t at)
{
	const auto lead = static_cast<unsigned char>(text[at]);
	size_t length = 0;
	if (lead < 0x80) {
		return 1;
	}
	if (lead >= 0xC2 && lead <= 0xDF) {
		length = 2;
	} else if (lead >= 0xE0 && lead <= 0xEF) {
		length = 3;
	} else if (lead >= 0xF0 && lead <= 0xF4) {
		length = 4;
	} else {
		return 0;
	}
	if (text.size() - at < length) {
		return 0;
	}
	for (size_t i = 1; i < length; ++i) {
		if (!isContinuation(static_cast<unsigned char>(text[at + i]))) {
			return 0;
		}
	}
	const auto second = static_cast<unsigned char>(text[at + 1]);
	// Overlong forms, UTF-16 surrogates, and code points past U+10FFFF.
	if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F) || (lead == 0xF0 && second < 0x90) ||
	    (lead == 0xF4 && second > 0x8F)) {
		return 0;
	}
	return length;
}

// Windows drops spaces and dots at the end of a name.
void dropTrailing(std::string &name)
{
	while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
		name.pop_back();
	}
}

char upper(char c)
{
	return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool equalsIgnoringCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (upper(a[i]) != upper(b[i])) {
			return false;
		}
	}
	return true;
}

// Windows treats the name as a device even with an extension, as in "nul.txt".
bool isDeviceName(std::string_view name)
{
	const std::string_view base = name.substr(0, name.find('.'));
	for (const std::string_view device : kDeviceNames) {
		if (equalsIgnoringCase(base, device)) {
			return true;
		}
	}
	for (const std::string_view device : kNumberedDeviceNames) {
		if (base.size() == 4 && equalsIgnoringCase(base.substr(0, 3), device) && base[3] >= '1' &&
		    base[3] <= '9') {
			return true;
		}
	}
	return false;
}

void appendNumber(std::string &out, int value, size_t digits)
{
	std::string number = std::to_string(value < 0 ? 0 : value);
	if (number.size() < digits) {
		out.append(digits - number.size(), '0');
	}
	out += number;
}

std::string dateText(LocalTime time)
{
	std::string text;
	appendNumber(text, time.year, 4);
	text += '-';
	appendNumber(text, time.month, 2);
	text += '-';
	appendNumber(text, time.day, 2);
	text += ' ';
	appendNumber(text, time.hour, 2);
	text += '-';
	appendNumber(text, time.minute, 2);
	return text;
}

} // namespace

LocalTime localTimeOf(std::chrono::system_clock::time_point time) noexcept
{
	const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
	std::tm parts{};
#ifdef _WIN32
	if (localtime_s(&parts, &seconds) != 0) {
		return {};
	}
#else
	if (localtime_r(&seconds, &parts) == nullptr) {
		return {};
	}
#endif
	return {parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec};
}

std::string safeFileName(std::string_view name, size_t maxBytes)
{
	std::string safe;
	safe.reserve(std::min(name.size(), maxBytes));
	size_t at = 0;
	while (at < name.size()) {
		const size_t length = sequenceLength(name, at);
		const char c = name[at];
		const bool refused = length == 0 ||
				     (length == 1 && (static_cast<unsigned char>(c) < 0x20 || c == 0x7F ||
						      kRefused.find(c) != std::string_view::npos));
		const size_t taken = length == 0 ? 1 : length;
		if (safe.size() + (refused ? 1 : taken) > maxBytes) {
			break;
		}
		if (refused) {
			safe += '_';
		} else {
			safe.append(name.substr(at, taken));
		}
		at += taken;
	}
	dropTrailing(safe);
	if (!safe.empty() && safe.front() == '.') {
		safe.front() = '_';
	}
	if (safe.empty() || isDeviceName(safe)) {
		safe.insert(safe.begin(), '_');
		// The '_' in front can take the name past the limit.
		const size_t limit = std::max(maxBytes, size_t{1});
		if (safe.size() > limit) {
			size_t cut = limit;
			while (cut > 1 && isContinuation(static_cast<unsigned char>(safe[cut]))) {
				--cut;
			}
			safe.resize(cut);
			dropTrailing(safe);
		}
	}
	return safe;
}

std::string broadcastFolderName(std::string_view sceneCollection, LocalTime time)
{
	// The date goes after the name is made safe, so a long collection name never cuts it.
	std::string date = " " + dateText(time);
	return safeFileName(sceneCollection, kMaxFileNameBytes - date.size()) + date;
}

std::string replayFileStem(LocalTime time)
{
	std::string stem = dateText(time);
	stem += '-';
	appendNumber(stem, time.second, 2);
	return stem;
}

std::string segmentFileName(std::string_view sourceKey, uint32_t sequence)
{
	std::string name = safeFileName(sourceKey);
	name += '-';
	const std::string number = std::to_string(sequence);
	if (number.size() < kSequenceDigits) {
		name.append(kSequenceDigits - number.size(), '0');
	}
	name += number;
	name += kSegmentExtension;
	return name;
}

std::optional<uint32_t> segmentSequenceOf(std::string_view fileName, std::string_view sourceKey)
{
	const std::string prefix = safeFileName(sourceKey) + '-';
	if (fileName.size() <= prefix.size() + kSegmentExtension.size() || !fileName.starts_with(prefix) ||
	    !fileName.ends_with(kSegmentExtension)) {
		return std::nullopt;
	}
	const std::string_view digits =
		fileName.substr(prefix.size(), fileName.size() - prefix.size() - kSegmentExtension.size());
	uint64_t value = 0;
	for (const char c : digits) {
		if (c < '0' || c > '9') {
			return std::nullopt;
		}
		value = value * 10 + static_cast<uint64_t>(c - '0');
		if (value > std::numeric_limits<uint32_t>::max()) {
			return std::nullopt;
		}
	}
	return static_cast<uint32_t>(value);
}

} // namespace tapeloop
