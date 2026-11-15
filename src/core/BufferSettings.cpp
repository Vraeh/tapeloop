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

std::string mediaKindName(AirMediaKind kind)
{
	switch (kind) {
	case AirMediaKind::Video:
		return "video";
	case AirMediaKind::Image:
		return "image";
	case AirMediaKind::None:
		break;
	}
	return "none";
}

std::chrono::milliseconds clampMilliseconds(int64_t ms, std::chrono::milliseconds low, std::chrono::milliseconds high)
{
	return std::chrono::milliseconds(std::clamp<int64_t>(ms, low.count(), high.count()));
}

SavedMedia saveMedia(const AirMedia &media)
{
	return {mediaKindName(media.kind), media.path, media.imageTime.count()};
}

AirMedia loadMedia(const SavedMedia &saved)
{
	AirMedia media;
	if (saved.kind == "video") {
		media.kind = AirMediaKind::Video;
	} else if (saved.kind == "image") {
		media.kind = AirMediaKind::Image;
	}
	media.path = saved.path;
	media.imageTime = clampMilliseconds(saved.imageMs, kMinImageTime, kMaxImageTime);
	return media;
}

AirTransition loadTransition(const SavedTransition &saved)
{
	return {saved.name, clampMilliseconds(saved.durationMs, kMinAirTransition, kMaxAirTransition)};
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

EncoderPreferences BufferSettings::encoderPreferences() const
{
	EncoderPreferences preferences;
	preferences.preferHevc = !forceH264;
	preferences.chosen = replayEncoder;
	preferences.otherAdapters = allowOtherAdapters;
	return preferences;
}

namespace {

const char *placeName(PlayerPlace place) noexcept
{
	return place == PlayerPlace::Window ? "window" : "dock";
}

PlayerPlace placeFrom(const std::string &name) noexcept
{
	return name == "window" ? PlayerPlace::Window : PlayerPlace::Dock;
}

const char *showsName(PlayerShows shows) noexcept
{
	switch (shows) {
	case PlayerShows::Controls:
		return "controls";
	case PlayerShows::Picture:
		return "picture";
	case PlayerShows::Both:
		break;
	}
	return "both";
}

PlayerShows showsFrom(const std::string &name) noexcept
{
	if (name == "controls") {
		return PlayerShows::Controls;
	}
	if (name == "picture") {
		return PlayerShows::Picture;
	}
	return PlayerShows::Both;
}

} // namespace

SavedSettings saveSettings(const BufferSettings &settings)
{
	SavedSettings saved;
	saved.lengthSeconds = toSeconds(settings.length);
	saved.resolution = resolutionName(settings.resolution.mode);
	saved.height = settings.resolution.height;
	saved.startWithOutputs = settings.startWithOutputs;
	saved.activateOffAir = settings.activateOffAir;
	saved.forceH264 = settings.forceH264;
	saved.replayEncoder = settings.replayEncoder;
	saved.allowOtherAdapters = settings.allowOtherAdapters;
	saved.showAdvanced = settings.showAdvanced;
	saved.playerPlace = placeName(settings.playerPlace);
	saved.playerShows = showsName(settings.playerShows);
	saved.playerGeometry = settings.playerGeometry;
	saved.bufferMemoryMiB = settings.bufferMemoryMiB;
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
	for (const SequenceEntry &entry : settings.sequence) {
		saved.sequence.push_back({entry.sourceKey, {}, entry.shown});
	}
	saved.intro = saveMedia(settings.air.intro);
	saved.into = {settings.air.into.name, settings.air.into.duration.count()};
	saved.outro = saveMedia(settings.air.outro);
	saved.back = {settings.air.back.name, settings.air.back.duration.count()};
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
	settings.forceH264 = saved.forceH264;
	settings.replayEncoder = saved.replayEncoder;
	settings.allowOtherAdapters = saved.allowOtherAdapters;
	settings.showAdvanced = saved.showAdvanced;
	settings.playerPlace = placeFrom(saved.playerPlace);
	settings.playerShows = showsFrom(saved.playerShows);
	settings.playerGeometry = saved.playerGeometry;
	settings.bufferMemoryMiB = std::max<int64_t>(saved.bufferMemoryMiB, 0);
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
	std::set<std::string> listed;
	for (const SavedSequenceEntry &entry : saved.sequence) {
		if (!entry.uuid.empty() && listed.insert(entry.uuid).second) {
			settings.sequence.push_back({entry.uuid, entry.shown});
		}
	}
	settings.air.intro = loadMedia(saved.intro);
	settings.air.into = loadTransition(saved.into);
	settings.air.outro = loadMedia(saved.outro);
	settings.air.back = loadTransition(saved.back);
	return settings;
}

namespace {

// The source each missing UUID moves to by its saved name: the one capturable source
// with that name, when nothing holds it yet and no other missing UUID claims it too.
std::map<std::string, std::string> claimByName(const std::vector<std::string> &missing,
					       const std::set<std::string> &held,
					       const std::map<std::string, std::string> &savedNames,
					       const std::vector<SourceIdentity> &sources)
{
	std::map<std::string, std::vector<std::string>> byName;
	for (const SourceIdentity &source : sources) {
		if (source.capturable) {
			byName[source.name].push_back(source.uuid);
		}
	}

	std::map<std::string, std::vector<std::string>> claims;
	for (const std::string &uuid : missing) {
		const auto name = savedNames.find(uuid);
		if (name == savedNames.end() || name->second.empty()) {
			continue;
		}
		const auto named = byName.find(name->second);
		if (named == byName.end() || named->second.size() != 1) {
			continue;
		}
		const std::string &target = named->second.front();
		if (!held.contains(target)) {
			claims[target].push_back(uuid);
		}
	}

	std::map<std::string, std::string> moves;
	for (const auto &[target, claimants] : claims) {
		if (claimants.size() == 1) {
			moves.emplace(claimants.front(), target);
		}
	}
	return moves;
}

std::set<std::string> presentIn(const std::vector<SourceIdentity> &sources)
{
	std::set<std::string> present;
	for (const SourceIdentity &source : sources) {
		present.insert(source.uuid);
	}
	return present;
}

} // namespace

std::map<std::string, std::string> matchSourcesByName(BufferSettings &settings,
						      const std::map<std::string, std::string> &savedNames,
						      const std::vector<SourceIdentity> &sources)
{
	const std::set<std::string> present = presentIn(sources);
	std::vector<std::string> missing;
	std::set<std::string> held;
	for (const auto &[uuid, source] : settings.sources) {
		held.insert(uuid);
		if (!present.contains(uuid)) {
			missing.push_back(uuid);
		}
	}

	const std::map<std::string, std::string> moves = claimByName(missing, held, savedNames, sources);
	for (const auto &[from, target] : moves) {
		auto node = settings.sources.extract(from);
		node.key() = target;
		settings.sources.insert(std::move(node));
	}
	return moves;
}

void matchSequenceByName(std::vector<SequenceEntry> &sequence, const std::map<std::string, std::string> &savedNames,
			 const std::vector<SourceIdentity> &sources)
{
	const std::set<std::string> present = presentIn(sources);
	std::vector<std::string> missing;
	std::set<std::string> held;
	for (const SequenceEntry &entry : sequence) {
		held.insert(entry.sourceKey);
		if (!present.contains(entry.sourceKey)) {
			missing.push_back(entry.sourceKey);
		}
	}

	const std::map<std::string, std::string> moves = claimByName(missing, held, savedNames, sources);
	std::vector<SequenceEntry> kept;
	kept.reserve(sequence.size());
	for (SequenceEntry &entry : sequence) {
		if (const auto moved = moves.find(entry.sourceKey); moved != moves.end()) {
			entry.sourceKey = moved->second;
		} else if (!present.contains(entry.sourceKey)) {
			continue;
		}
		kept.push_back(std::move(entry));
	}
	sequence = std::move(kept);
}

} // namespace tapeloop
