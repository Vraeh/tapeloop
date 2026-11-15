// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/ManagerDockBackend.hpp"

#include "core/FileIo.hpp"

#include "obs/Composition.hpp"
#include "obs/ObsEncoders.hpp"
#include "obs/ReplayHotkeys.hpp"
#include "obs/ReplayPlayer.hpp"
#include "obs/ReplayView.hpp"
#include "obs/SourceDisplay.hpp"
#include "obs/SourceLookup.hpp"

#include <obs.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

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

bool keepSource(void *param, obs_source_t *source) noexcept
{
	try {
		static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
	} catch (...) {
		return false;
	}
	return true;
}

bool lessByName(const std::string &a, const std::string &b)
{
	return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
					    [](unsigned char x, unsigned char y) {
						    return std::tolower(x) < std::tolower(y);
					    });
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
		source.chosenEncoder = status.stats.chosenEncoder;
		source.choiceSkipped = status.stats.choiceSkipped;
		source.hevcFailed = status.hevcFailed;
		source.budgetLimited = status.budgetLimited;
		source.totalFrames = status.stats.totalFrames;
		source.skippedFrames = status.stats.skippedFrames;
	}
	std::sort(sources.begin(), sources.end(),
		  [](const ui::DockSource &a, const ui::DockSource &b) { return lessByName(a.name, b.name); });
	if (!sequence_) {
		return sources;
	}

	const ReplaySequence &order = sequence_->state().sequence();
	const std::span<const SequenceEntry> entries = order.entries();
	std::vector<std::pair<size_t, ui::DockSource>> placed;
	placed.reserve(sources.size());
	for (ui::DockSource &source : sources) {
		const size_t place = order.indexOf(source.uuid).value_or(entries.size());
		source.shown = place == entries.size() || entries[place].shown;
		placed.emplace_back(place, std::move(source));
	}
	std::stable_sort(placed.begin(), placed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	for (size_t i = 0; i < placed.size(); ++i) {
		sources[i] = std::move(placed[i].second);
	}
	return sources;
}

uint64_t ManagerDockBackend::captureReplay()
{
	const uint64_t id = manager_.captureReplay();
	if (sequence_) {
		sequence_->cueCurrent();
	}
	return id;
}

void ManagerDockBackend::pickReplay(uint64_t id)
{
	manager_.pickReplay(id);
	if (sequence_) {
		sequence_->cueCurrent();
	}
}

bool ManagerDockBackend::setShown(const std::string &uuid, bool shown)
{
	if (!sequence_) {
		return false;
	}
	enlist(sources());
	return sequence_->setShown(uuid, shown);
}

bool ManagerDockBackend::moveSource(const std::string &uuid, const std::string &target)
{
	if (!sequence_) {
		return false;
	}
	enlist(sources());
	// Sources the dock does not list keep their places in the sequence, and the one moved
	// lands just past the one whose place it takes, as it does in the list.
	const ReplaySequence &order = sequence_->state().sequence();
	const std::optional<size_t> moving = order.indexOf(uuid);
	const std::optional<size_t> place = order.indexOf(target);
	return moving && place && sequence_->move(*moving, *place);
}

void ManagerDockBackend::enlist(const std::vector<ui::DockSource> &listed)
{
	const ReplaySequence &order = sequence_->state().sequence();
	const std::span<const SequenceEntry> known = order.entries();
	std::vector<SequenceEntry> entries(known.begin(), known.end());
	for (const ui::DockSource &source : listed) {
		if (!order.indexOf(source.uuid)) {
			entries.push_back({source.uuid, true});
		}
	}
	if (entries.size() != known.size()) {
		sequence_->setEntries(std::move(entries));
	}
}

void ManagerDockBackend::setSettings(const BufferSettings &settings)
{
	manager_.setSettings(settings);
	if (sequence_) {
		sequence_->setAir(manager_.settings().air);
		sequence_->setOverlays(manager_.settings().overlays);
	}
}

std::unique_ptr<ui::PictureSurface> ManagerDockBackend::pictureSurface()
{
	if (!sequence_ || !sequence_->source()) {
		return nullptr;
	}
	return std::make_unique<SourceDisplay>(sequence_->source());
}

ui::DockPlayer ManagerDockBackend::player() const
{
	ui::DockPlayer shown;
	const ReplayPlayer *player = sequence_ ? controlledPlayer(*sequence_) : nullptr;
	if (!player) {
		return shown;
	}
	const PlayerState state = player->state();
	shown.loaded = state.loaded;
	shown.onAir = sequence_->state().phase() == AirPhase::Source;
	shown.playing = state.playing;
	shown.rate = state.rate;
	shown.position = std::clamp(state.position, state.start, state.end) - state.start;
	shown.length = state.end - state.start;
	return shown;
}

void ManagerDockBackend::controlPlayer(ui::PlayerControl control, bool pressed)
{
	if (!sequence_) {
		return;
	}
	HotkeyAction action = HotkeyAction::PlayPause;
	switch (control) {
	case ui::PlayerControl::PlayPause:
		action = HotkeyAction::PlayPause;
		break;
	case ui::PlayerControl::Reverse:
		action = HotkeyAction::Reverse;
		break;
	case ui::PlayerControl::PreviousFrame:
		action = HotkeyAction::PreviousFrame;
		break;
	case ui::PlayerControl::NextFrame:
		action = HotkeyAction::NextFrame;
		break;
	case ui::PlayerControl::Slower:
		action = HotkeyAction::Slower;
		break;
	case ui::PlayerControl::Faster:
		action = HotkeyAction::Faster;
		break;
	case ui::PlayerControl::NormalSpeed:
		action = HotkeyAction::NormalSpeed;
		break;
	case ui::PlayerControl::ScrubBackward:
		action = HotkeyAction::ScrubBackward;
		break;
	case ui::PlayerControl::ScrubForward:
		action = HotkeyAction::ScrubForward;
		break;
	}
	runHotkey(action, pressed, manager_, *sequence_);
}

void ManagerDockBackend::seekPlayer(Nanoseconds position)
{
	ReplayPlayer *player = sequence_ ? controlledPlayer(*sequence_) : nullptr;
	if (player) {
		player->seekFromStart(position);
	}
}

std::vector<ui::DockExport> ManagerDockBackend::exports() const
{
	std::vector<ui::DockExport> exports;
	for (const ExportStatus &status : manager_.exports()) {
		ui::DockExport shown;
		shown.replay = status.replay;
		shown.format = status.format;
		shown.name = status.name;
		shown.progress = status.progress;
		shown.error = status.error;
		switch (status.state) {
		case ExportStatus::State::Waiting:
			shown.state = ui::DockExport::State::Waiting;
			break;
		case ExportStatus::State::Running:
			shown.state = ui::DockExport::State::Running;
			break;
		case ExportStatus::State::Done:
			shown.state = ui::DockExport::State::Done;
			break;
		case ExportStatus::State::Failed:
			shown.state = ui::DockExport::State::Failed;
			break;
		}
		if (!status.files.empty()) {
			shown.folder = utf8FromPath(status.files.front().parent_path());
		}
		exports.push_back(std::move(shown));
	}
	return exports;
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

std::vector<std::string> ManagerDockBackend::transitions() const
{
	if (!sequence_) {
		return {};
	}
	return sequence_->air().transitionNames();
}

std::vector<OverlayScene> ManagerDockBackend::scenes() const
{
	if (!scenes_) {
		return {};
	}
	return scenes_->scenes();
}

bool ManagerDockBackend::sceneHoldsReplay(const std::string &uuid) const
{
	const OBSSourceAutoRelease scene = sourceByUuid(uuid.c_str());
	if (!scene || !obs_scene_from_source(scene) || obs_source_removed(scene)) {
		return false;
	}
	return holdsReplayView(scene);
}

std::vector<ui::OverlaySource> ManagerDockBackend::overlaySources() const
{
	// Sizes are read once the enumeration has let go of libobs's source list.
	std::vector<OBSSource> found;
	obs_enum_sources(keepSource, &found);
	obs_enum_scenes(keepSource, &found);
	std::vector<ui::OverlaySource> sources;
	for (obs_source_t *source : found) {
		const obs_source_type type = obs_source_get_type(source);
		const char *uuid = obs_source_get_uuid(source);
		const char *name = obs_source_get_name(source);
		if ((type != OBS_SOURCE_TYPE_INPUT && type != OBS_SOURCE_TYPE_SCENE) || obs_source_is_group(source) ||
		    obs_obj_is_private(source) || obs_source_removed(source) ||
		    (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0 ||
		    std::string_view(obs_source_get_id(source)) == kReplayViewId || !uuid || !name) {
			continue;
		}
		sources.push_back({uuid, name, type == OBS_SOURCE_TYPE_SCENE, obs_source_get_width(source),
				   obs_source_get_height(source)});
	}
	std::stable_sort(sources.begin(), sources.end(), [](const ui::OverlaySource &a, const ui::OverlaySource &b) {
		return lessByName(a.name, b.name);
	});
	return sources;
}

CanvasSize ManagerDockBackend::canvas() const
{
	return mainCanvas();
}

bool ManagerDockBackend::openProperties(const std::string &uuid)
{
	const OBSSourceAutoRelease source = sourceByUuid(uuid.c_str());
	if (!properties_ || !source || obs_source_removed(source)) {
		return false;
	}
	properties_->openProperties(source);
	return true;
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
