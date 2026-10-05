// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "core/BufferSettings.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>

namespace tapeloop {
namespace {

constexpr std::array<uint32_t, 5> kFixedHeights = {360, 480, 720, 1080, 2160};

int64_t toSeconds(Nanoseconds length)
{
	return std::chrono::round<std::chrono::seconds>(length).count();
}

Nanoseconds fromSeconds(int64_t seconds)
{
	const int64_t limit = toSeconds(kMaxBufferLength);
	return clampBufferLength(std::chrono::seconds(std::clamp<int64_t>(seconds, -limit, limit)));
}

std::string resolutionName(ResolutionMode mode)
{
	switch (mode) {
	case ResolutionMode::Output:
		return "output";
	case ResolutionMode::Fixed:
		return "fixed";
	case ResolutionMode::Canvas:
		break;
	}
	return "canvas";
}

ReplayResolution resolutionFrom(const std::string &name, int64_t height)
{
	if (name == "output") {
		return {ResolutionMode::Output, ReplayResolution{}.height};
	}
	if (name == "fixed" && height > 0 && height <= std::numeric_limits<uint32_t>::max() &&
	    isFixedHeight(static_cast<uint32_t>(height))) {
		return {ResolutionMode::Fixed, static_cast<uint32_t>(height)};
	}
	return {};
}

} // namespace

Nanoseconds clampBufferLength(Nanoseconds length)
{
	return std::clamp(length, kMinBufferLength, kMaxBufferLength);
}

bool isFixedHeight(uint32_t height)
{
	return std::find(kFixedHeights.begin(), kFixedHeights.end(), height) != kFixedHeights.end();
}

Nanoseconds BufferSettings::lengthFor(const std::string &uuid) const
{
	const auto found = sources.find(uuid);
	return found != sources.end() ? found->second.length.value_or(length) : length;
}

ReplayResolution BufferSettings::resolutionFor(const std::string &uuid) const
{
	const auto found = sources.find(uuid);
	return found != sources.end() ? found->second.resolution.value_or(resolution) : resolution;
}

bool BufferSettings::activateFor(const std::string &uuid) const
{
	const auto found = sources.find(uuid);
	return found != sources.end() ? found->second.activateOffAir.value_or(activateOffAir) : activateOffAir;
}

std::vector<std::string> BufferSettings::selectedSources() const
{
	std::vector<std::string> selected;
	for (const auto &[uuid, source] : sources) {
		if (source.selected) {
			selected.push_back(uuid);
		}
	}
	return selected;
}

SavedSettings saveSettings(const BufferSettings &settings)
{
	SavedSettings saved;
	saved.lengthSeconds = toSeconds(settings.length);
	saved.resolution = resolutionName(settings.resolution.mode);
	saved.height = settings.resolution.height;
	saved.startWithOutputs = settings.startWithOutputs;
	saved.activateOffAir = settings.activateOffAir;
	for (const auto &[uuid, source] : settings.sources) {
		SavedSource entry;
		entry.uuid = uuid;
		entry.selected = source.selected;
		entry.activateOffAir = source.activateOffAir;
		if (source.length) {
			entry.lengthSeconds = toSeconds(*source.length);
		}
		if (source.resolution) {
			entry.resolution = resolutionName(source.resolution->mode);
			entry.height = source.resolution->height;
		}
		saved.sources.push_back(std::move(entry));
	}
	return saved;
}

std::optional<BufferSettings> loadSettings(const SavedSettings &saved)
{
	if (saved.version != kSettingsVersion) {
		return std::nullopt;
	}

	BufferSettings settings;
	settings.length = fromSeconds(saved.lengthSeconds);
	settings.resolution = resolutionFrom(saved.resolution, saved.height);
	settings.startWithOutputs = saved.startWithOutputs;
	settings.activateOffAir = saved.activateOffAir;
	for (const SavedSource &entry : saved.sources) {
		if (entry.uuid.empty()) {
			continue;
		}
		SourceSettings source;
		source.selected = entry.selected;
		source.activateOffAir = entry.activateOffAir;
		if (entry.lengthSeconds) {
			source.length = fromSeconds(*entry.lengthSeconds);
		}
		if (entry.resolution) {
			source.resolution = resolutionFrom(*entry.resolution, entry.height.value_or(0));
		}
		settings.sources[entry.uuid] = source;
	}
	return settings;
}

std::map<std::string, std::string> matchSourcesByName(BufferSettings &settings,
						      const std::map<std::string, std::string> &savedNames,
						      const std::vector<SourceIdentity> &sources)
{
	std::set<std::string> present;
	std::map<std::string, std::vector<std::string>> byName;
	for (const SourceIdentity &source : sources) {
		present.insert(source.uuid);
		if (source.capturable) {
			byName[source.name].push_back(source.uuid);
		}
	}

	std::map<std::string, std::vector<std::string>> claims;
	for (const auto &[uuid, source] : settings.sources) {
		if (present.contains(uuid)) {
			continue;
		}
		const auto name = savedNames.find(uuid);
		if (name == savedNames.end() || name->second.empty()) {
			continue;
		}
		const auto named = byName.find(name->second);
		if (named == byName.end() || named->second.size() != 1) {
			continue;
		}
		const std::string &target = named->second.front();
		if (!settings.sources.contains(target)) {
			claims[target].push_back(uuid);
		}
	}

	std::map<std::string, std::string> moves;
	for (const auto &[target, claimants] : claims) {
		if (claimants.size() != 1) {
			continue;
		}
		const std::string &from = claimants.front();
		auto node = settings.sources.extract(from);
		node.key() = target;
		settings.sources.insert(std::move(node));
		moves.emplace(from, target);
	}
	return moves;
}

} // namespace tapeloop
