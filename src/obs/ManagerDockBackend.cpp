// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ManagerDockBackend.hpp"

#include "core/FileIo.hpp"

#include "obs/ObsEncoders.hpp"

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
		source.readbackReason = status.stats.readbackReason;
		source.hevcFailed = status.hevcFailed;
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
		const Replay *found = library.find(id);
		if (!found) {
			continue;
		}
		ui::DockReplay replay;
		replay.id = id;
		replay.capturedAt = found->capturedAt;
		replay.sources = found->sources.size();
		replay.tags = found->tags;
		replay.broadcast = found->broadcast;
		replay.state = found->state;
		replay.fileName = utf8FromPath(found->manifest.stem());
		replays.push_back(std::move(replay));
	}
	// A broadcast's replays together, the broadcast with the newest replay first; the
	// order within each stays newest first.
	std::vector<std::string> order;
	for (const ui::DockReplay &replay : replays) {
		if (std::find(order.begin(), order.end(), replay.broadcast) == order.end()) {
			order.push_back(replay.broadcast);
		}
	}
	std::stable_sort(replays.begin(), replays.end(), [&](const ui::DockReplay &a, const ui::DockReplay &b) {
		return std::find(order.begin(), order.end(), a.broadcast) <
		       std::find(order.begin(), order.end(), b.broadcast);
	});
	return replays;
}

std::vector<std::string> ManagerDockBackend::replayTags() const
{
	const std::span<const std::string> tags = manager_.library().tags();
	return {tags.begin(), tags.end()};
}

std::vector<ui::EncoderChoice> ManagerDockBackend::encoderChoices() const
{
	std::vector<ui::EncoderChoice> choices;
	for (const EncoderInfo &encoder : replayEncoderChoices(registeredVideoEncoders())) {
		const char *name = obs_encoder_get_display_name(encoder.id.c_str());
		choices.push_back({encoder.id, name ? name : encoder.id});
	}
	return choices;
}

} // namespace tapeloop::obs
