// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ManagerDockBackend.hpp"

#include <algorithm>
#include <cctype>
#include <span>

namespace tapeloop::obs {
namespace {

bool addVideoInput(void *param, obs_source_t *source) noexcept
{
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
	    (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
		return true;
	}
	try {
		ui::DockSource entry;
		entry.uuid = obs_source_get_uuid(source);
		entry.name = obs_source_get_name(source);
		static_cast<std::vector<ui::DockSource> *>(param)->push_back(std::move(entry));
	} catch (...) {
		return false;
	}
	return true;
}

ui::SourceState stateOf(CaptureState state)
{
	switch (state) {
	case CaptureState::Running:
		return ui::SourceState::Running;
	case CaptureState::Failed:
		return ui::SourceState::Failed;
	case CaptureState::Waiting:
		return ui::SourceState::Waiting;
	case CaptureState::Stopped:
		break;
	}
	return ui::SourceState::Stopped;
}

} // namespace

std::vector<ui::DockSource> ManagerDockBackend::sources() const
{
	std::vector<ui::DockSource> sources;
	obs_enum_sources(addVideoInput, &sources);
	for (ui::DockSource &source : sources) {
		const SourceStatus status = manager_.status(source.uuid);
		source.selected = status.selected;
		source.state = stateOf(status.stats.state);
		source.buffered = status.stats.bufferedDuration;
		source.bytes = status.stats.buffer.bytes;
		source.activationLeftOut = status.activationLeftOut;
		source.encoderPath = status.stats.encoderPath;
	}
	std::sort(sources.begin(), sources.end(), [](const ui::DockSource &a, const ui::DockSource &b) {
		return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
						    [](unsigned char x, unsigned char y) {
							    return std::tolower(x) < std::tolower(y);
						    });
	});
	return sources;
}

void ManagerDockBackend::setSettings(const BufferSettings &settings)
{
	manager_.setSettings(settings);
}

std::vector<ui::DockReplay> ManagerDockBackend::replays(const std::string &tag) const
{
	const ReplayLibrary &library = manager_.library();
	std::vector<ui::DockReplay> replays;
	for (const uint64_t id : library.list(tag)) {
		ui::DockReplay replay;
		replay.id = id;
		replay.capturedAt = library.capturedAt(id);
		if (const Moment *moment = library.find(id)) {
			replay.sources = moment->clips.size();
		}
		const std::span<const std::string> tags = library.tagsOf(id);
		replay.tags.assign(tags.begin(), tags.end());
		replays.push_back(std::move(replay));
	}
	return replays;
}

std::vector<std::string> ManagerDockBackend::replayTags() const
{
	const std::span<const std::string> tags = manager_.library().tags();
	return {tags.begin(), tags.end()};
}

} // namespace tapeloop::obs
