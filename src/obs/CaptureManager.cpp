// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureManager.hpp"

#include "core/FileIo.hpp"
#include "core/MomentCutter.hpp"
#include "core/ReplayNames.hpp"
#include "core/ReplayWriter.hpp"

#include "obs/ObsEncoders.hpp"
#include "obs/SettingsData.hpp"

#include <obs.hpp>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tapeloop::obs {
namespace {

// The folder of every broadcast, inside OBS's recording folder.
constexpr std::string_view kReplayFolder = "Tapeloop";
// How many jobs the store may have, the one it is on included, before the log says the
// disk falls behind: a capture keeps its GOPs in memory until it is written, whatever
// the buffers drop.
constexpr size_t kStoreBacklogWarning = 3;
// Why a replay the store could not take is not saved. That happens when memory runs out,
// so the reason fits a string's own buffer in every standard library.
constexpr std::string_view kNotHandedOver = "not handed over";
static_assert(kNotHandedOver.size() <= 15);

bool addInput(void *param, obs_source_t *source) noexcept
{
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT) {
		return true;
	}
	try {
		const char *name = obs_source_get_name(source);
		const bool video = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0;
		static_cast<std::vector<SourceIdentity> *>(param)->push_back(
			{obs_source_get_uuid(source), name ? name : "", video});
	} catch (...) {
		return false;
	}
	return true;
}

// Sources that restart, unpause or refresh when they become active. Holding one active
// off air would keep it from doing so when it is cut to air, so activation leaves them
// out. The ids and settings are those of OBS 32's media source, VLC source, image
// slideshow, image source and browser source.
bool reactsToActivation(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id) {
		return false;
	}
	// An animated image starts over. Holding gives an image nothing else: it runs while
	// shown, which its capture already does.
	if (std::strcmp(id, "image_source") == 0) {
		return true;
	}
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (std::strcmp(id, "ffmpeg_source") == 0) {
		return obs_data_get_bool(settings, "restart_on_activate");
	}
	if (std::strcmp(id, "vlc_source") == 0 || std::strcmp(id, "slideshow") == 0) {
		// Held, one set to pause and unpause would go on playing off air and come to air
		// elsewhere than where it stopped.
		return std::strcmp(obs_data_get_string(settings, "playback_behavior"), "always_play") != 0;
	}
	if (std::strcmp(id, "browser_source") == 0) {
		return obs_data_get_bool(settings, "restart_when_active");
	}
	return false;
}

} // namespace

CaptureManager::CaptureManager(CaptureHost &host) : host_(host)
{
	lifecycle_.reset(settings_.startWithOutputs, host_.streamingActive(), host_.recordingActive());
}

CaptureManager::~CaptureManager()
{
	releaseAll();
}

void CaptureManager::setSettings(BufferSettings settings)
{
	if (foreignSettings_) {
		blog(LOG_WARNING, "[tapeloop] This scene collection holds settings of another version; "
				  "changes made here are not saved");
	}
	settings_ = std::move(settings);
	lifecycle_.setStartWithOutputs(settings_.startWithOutputs);
	reconcile();
	if (!foreignSettings_) {
		host_.requestSave();
	}
}

bool CaptureManager::manualStart()
{
	if (!lifecycle_.manualStart()) {
		return false;
	}
	reconcile();
	return true;
}

bool CaptureManager::manualStop()
{
	if (!lifecycle_.manualStop()) {
		return false;
	}
	reconcile();
	return true;
}

void CaptureManager::onStreaming(bool active)
{
	lifecycle_.setStreaming(active);
	reconcile();
}

void CaptureManager::onRecording(bool active)
{
	lifecycle_.setRecording(active);
	reconcile();
}

void CaptureManager::onSceneCollectionCleanup()
{
	releaseAll();
	// The next start names a broadcast of the next collection.
	wasRunning_ = false;
	settings_ = BufferSettings{};
	savedNames_.clear();
	foreignSettings_ = nullptr;
	lifecycle_.reset(settings_.startWithOutputs, lifecycle_.streaming(), lifecycle_.recording());
}

void CaptureManager::onExit()
{
	exiting_ = true;
	releaseAll();
	// The store would finish them anyway when it goes; waiting here logs what became of
	// them.
	finishWrites();
}

void CaptureManager::poll()
{
	if (exiting_) {
		return;
	}
	takeStoreResults();
	library_.releaseExpired();

	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second->removed) {
			stop(*it->second);
			it = entries_.erase(it);
		} else {
			++it;
		}
	}

	const Nanoseconds now{static_cast<int64_t>(os_gettime_ns())};
	for (const auto &[uuid, entry] : entries_) {
		if (entry->capture.stats().state != CaptureState::Running) {
			entry->capture.expireBuffer(now);
		}
	}

	followOutputs();
	if (!lifecycle_.running()) {
		return;
	}

	std::optional<std::vector<EncoderInfo>> candidates;
	for (const std::string &uuid : settings_.selectedSources()) {
		auto found = entries_.find(uuid);
		const bool fresh = found == entries_.end();
		if (fresh) {
			found = entries_.emplace(uuid, std::make_unique<Entry>()).first;
		}
		Entry &entry = *found->second;
		StartOutcome outcome = StartOutcome::Started;
		if (fresh) {
			outcome = start(uuid, entry, false, true, candidates);
		} else if (!entry.capture.active()) {
			if (entry.retry) {
				outcome = start(uuid, entry, entry.retryKeepsBuffer, true, candidates);
			}
		} else if (!entry.capture.sourceSizeMatches()) {
			// The view keeps the size it started with, so the capture restarts on the
			// source's new size; the buffer sees the restart as a discontinuity.
			blog(LOG_INFO, "[tapeloop] A captured source changed size, restarting its capture");
			stop(entry);
			outcome = start(uuid, entry, true, false, candidates);
		} else {
			// The restart setting of a media source can change while it is captured.
			updateActivation(uuid, entry, entry.source);
		}
		// A failed entry stays, so that only what poll is meant to retry is tried again.
		if (outcome == StartOutcome::SourceRemoved) {
			entries_.erase(found);
		}
	}
}

void CaptureManager::save(obs_data_t *collection) const
{
	if (foreignSettings_) {
		obs_data_set_obj(collection, kSettingsKey, foreignSettings_);
		return;
	}
	SavedSettings saved = saveSettings(settings_);
	// Settings of a source the collection no longer has would only pile up.
	std::vector<SavedSource> kept;
	for (SavedSource &source : saved.sources) {
		OBSSourceAutoRelease found = obs_get_source_by_uuid(source.uuid.c_str());
		if (found) {
			source.name = obs_source_get_name(found);
			kept.push_back(std::move(source));
		}
	}
	saved.sources = std::move(kept);
	OBSDataAutoRelease data = createSettingsData(saved);
	obs_data_set_obj(collection, kSettingsKey, data);
}

void CaptureManager::load(obs_data_t *collection)
{
	foreignSettings_ = nullptr;
	savedNames_.clear();
	BufferSettings settings;
	OBSDataAutoRelease data = obs_data_get_obj(collection, kSettingsKey);
	if (data) {
		const SavedSettings saved = readSettingsData(data);
		if (std::optional<BufferSettings> loaded = loadSettings(saved)) {
			settings = std::move(*loaded);
			for (const SavedSource &source : saved.sources) {
				savedNames_[source.uuid] = source.name;
			}
		} else {
			blog(LOG_WARNING,
			     "[tapeloop] This scene collection has settings of version %lld, which this version "
			     "does not know; they are kept as they are and the defaults apply",
			     static_cast<long long>(saved.version));
			foreignSettings_ = std::move(data);
		}
	}

	std::vector<SourceIdentity> inputs;
	obs_enum_sources(addInput, &inputs);
	for (const auto &move : matchSourcesByName(settings, savedNames_, inputs)) {
		blog(LOG_INFO, "[tapeloop] Found the saved source '%s' again by its name",
		     savedNames_.at(move.first).c_str());
	}

	settings_ = std::move(settings);
	lifecycle_.reset(settings_.startWithOutputs, host_.streamingActive(), host_.recordingActive());
	for (const std::string &uuid : settings_.selectedSources()) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (!source) {
			const auto name = savedNames_.find(uuid);
			blog(LOG_INFO, "[tapeloop] The selected source '%s' is not in this scene collection",
			     name != savedNames_.end() && !name->second.empty() ? name->second.c_str() : uuid.c_str());
		}
	}
	reconcile();
}

SourceStatus CaptureManager::status(const std::string &uuid) const
{
	SourceStatus status;
	const auto source = settings_.sources.find(uuid);
	status.selected = source != settings_.sources.end() && source->second.selected;
	const auto found = entries_.find(uuid);
	if (found != entries_.end()) {
		status.stats = found->second->capture.stats();
		status.activationLeftOut = found->second->activationLeftOut;
	}
	return status;
}

const SourceBuffer *CaptureManager::buffer(const std::string &uuid) const
{
	const auto found = entries_.find(uuid);
	return found != entries_.end() ? found->second->capture.buffer() : nullptr;
}

uint64_t CaptureManager::captureReplay()
{
	std::vector<MomentSource> sources;
	for (const auto &[uuid, entry] : entries_) {
		if (const SourceBuffer *held = entry->capture.buffer()) {
			sources.push_back({uuid, *held});
		}
	}
	// Packets carry the time of the frames they encode, on the clock OBS stamps video with.
	const Nanoseconds now{static_cast<int64_t>(os_gettime_ns())};
	// Each source gives the window of its own buffer, the length the buffer started with,
	// so the moment reaches back as far as the longest of those that hold anything.
	Nanoseconds reach{0};
	for (const MomentSource &source : sources) {
		if (source.buffer.get().stats().gopCount != 0) {
			reach = std::max(reach, source.buffer.get().window());
		}
	}
	MomentCut cut = cutMoment(sources, now, reach);
	if (cut.moment.clips.empty()) {
		return 0;
	}
	// OBS may have had no recording folder when the buffers started.
	if (broadcastFolder_.empty()) {
		placeBroadcast();
	}
	const auto capturedAt = std::chrono::system_clock::now();
	ReplayCapture capture;
	capture.id = newReplayId();
	capture.folder = broadcastFolder_;
	capture.stem = replayFileStem(localTimeOf(capturedAt));
	capture.capturedAtUtc =
		std::chrono::duration_cast<std::chrono::nanoseconds>(capturedAt.time_since_epoch()).count();
	capture.capturedAtClock = now;
	capture.start = cut.moment.start;
	capture.end = cut.moment.end;
	for (MomentClip &clip : cut.moment.clips) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(clip.sourceKey.c_str());
		const char *name = source ? obs_source_get_name(source) : nullptr;
		capture.sources.push_back({std::move(clip.sourceKey), name ? name : "", std::move(clip.clip)});
	}
	const uint64_t id = library_.addCaptured(capture, broadcastName_);
	if (id == 0) {
		return 0;
	}
	blog(LOG_INFO, "[tapeloop] Captured replay %llu from %zu sources, %zu of them with nothing in range",
	     static_cast<unsigned long long>(id), sources.size(), cut.skipped.size());
	if (capture.folder.empty()) {
		blog(LOG_WARNING, "[tapeloop] Replay %llu is not saved: OBS has no recording folder",
		     static_cast<unsigned long long>(id));
		library_.notSaved(id, "OBS has no recording folder");
		return id;
	}
	try {
		// The entry for the ticket is made first, since nothing may fail once the store has
		// the capture: the replay would be marked not saved while it is written. Tickets
		// start at 1.
		auto entry = writing_.extract(writing_.try_emplace(0, id).first);
		entry.key() = store_.write(std::move(capture));
		writing_.insert(std::move(entry));
	} catch (...) {
		blog(LOG_WARNING, "[tapeloop] Replay %llu could not be saved: it could not be handed to the writer",
		     static_cast<unsigned long long>(id));
		library_.notSaved(id, std::string(kNotHandedOver));
		throw;
	}
	const size_t backlog = store_.pending();
	if (backlog < kStoreBacklogWarning) {
		backlogLogged_ = false;
	} else if (!backlogLogged_) {
		blog(LOG_WARNING,
		     "[tapeloop] The disk falls behind: the writer has %zu replays, tag edits or scans to do, "
		     "and every replay among them keeps its footage in memory",
		     backlog);
		backlogLogged_ = true;
	}
	return id;
}

void CaptureManager::loadLibrary()
{
	const std::filesystem::path base = replayFolder();
	if (!base.empty()) {
		store_.scan(base);
	}
	scannedFolder_ = base;
}

void CaptureManager::finishWrites()
{
	// Taking in a capture can ask for its tags to be written.
	do {
		store_.waitUntilIdle();
		takeStoreResults();
	} while (store_.pending() != 0);
}

bool CaptureManager::tagReplay(uint64_t id, std::string_view tag)
{
	if (!library_.addTag(id, tag)) {
		return false;
	}
	saveTags(id);
	return true;
}

bool CaptureManager::untagReplay(uint64_t id, std::string_view tag)
{
	if (!library_.removeTag(id, tag)) {
		return false;
	}
	saveTags(id);
	return true;
}

bool CaptureManager::deleteReplayTag(std::string_view tag)
{
	const std::vector<uint64_t> carrying = library_.list(tag);
	if (!library_.deleteTag(tag)) {
		return false;
	}
	for (const uint64_t id : carrying) {
		saveTags(id);
	}
	return true;
}

std::filesystem::path CaptureManager::replayFolder() const
{
	const std::string recording = host_.recordingFolder();
	if (recording.empty()) {
		return {};
	}
	return pathFromUtf8(recording) / pathFromUtf8(kReplayFolder);
}

void CaptureManager::nameBroadcast()
{
	broadcastName_ =
		broadcastFolderName(host_.sceneCollectionName(), localTimeOf(std::chrono::system_clock::now()));
	placeBroadcast();
}

void CaptureManager::placeBroadcast()
{
	const std::filesystem::path base = replayFolder();
	broadcastFolder_ = base.empty() ? std::filesystem::path() : base / pathFromUtf8(broadcastName_);
	if (base.empty()) {
		return;
	}
	blog(LOG_INFO, "[tapeloop] Replays of these buffers go to '%s'", utf8FromPath(broadcastFolder_).c_str());
	// A recording folder set or changed since OBS loaded has replays of its own.
	if (base != scannedFolder_) {
		loadLibrary();
	}
}

void CaptureManager::takeStoreResults()
{
	for (StoreResult &result : store_.poll()) {
		switch (result.kind) {
		case StoreResult::Kind::Capture: {
			const auto found = writing_.find(result.ticket);
			if (found == writing_.end()) {
				break;
			}
			const uint64_t id = found->second;
			writing_.erase(found);
			if (!result.written) {
				blog(LOG_WARNING, "[tapeloop] Replay %llu could not be saved: %s",
				     static_cast<unsigned long long>(id), result.error.c_str());
				library_.notSaved(id, result.error);
				break;
			}
			blog(LOG_INFO,
			     "[tapeloop] Replay %llu saved as '%s': %llu bytes written, %zu GOPs new, %zu on disk already",
			     static_cast<unsigned long long>(id), utf8FromPath(result.written->manifest).c_str(),
			     static_cast<unsigned long long>(result.written->bytesAppended),
			     result.written->gopsAppended, result.written->gopsShared);
			// Tags given while it was being written were not in the capture.
			const bool tagged = !library_.tagsOf(id).empty();
			library_.stored(id, result.written->manifest, std::move(result.written->index));
			if (tagged) {
				saveTags(id);
			}
			break;
		}
		case StoreResult::Kind::Tags:
			if (!result.error.empty()) {
				blog(LOG_WARNING, "[tapeloop] The tags of '%s' could not be saved: %s",
				     utf8FromPath(result.manifest).c_str(), result.error.c_str());
			}
			break;
		case StoreResult::Kind::Scan:
			if (!result.scan) {
				blog(LOG_WARNING, "[tapeloop] The replays on disk could not be read: %s",
				     result.error.c_str());
				break;
			}
			for (const std::filesystem::path &removed : result.scan->removed) {
				blog(LOG_INFO, "[tapeloop] Deleted '%s', a replay a crash left half written",
				     utf8FromPath(removed).c_str());
			}
			for (const std::string &error : result.scan->errors) {
				blog(LOG_WARNING, "[tapeloop] Reading the replays on disk: %s", error.c_str());
			}
			for (const FoundReplay &found : result.scan->replays) {
				if (!found.intact) {
					blog(LOG_WARNING, "[tapeloop] The replay '%s' is damaged",
					     utf8FromPath(found.manifest).c_str());
				}
				library_.addFound(found);
			}
			blog(LOG_INFO, "[tapeloop] Found %zu replays on disk", result.scan->replays.size());
			break;
		}
	}
	if (!priorityLogged_) {
		if (const std::optional<bool> low = store_.lowPriority()) {
			priorityLogged_ = true;
			if (!*low) {
				blog(LOG_WARNING,
				     "[tapeloop] Replays are written at normal I/O priority: the system refused a "
				     "lower one");
			}
		}
	}
}

void CaptureManager::saveTags(uint64_t id)
{
	const Replay *replay = library_.find(id);
	if (replay && replay->state == ReplayState::Stored && !replay->manifest.empty()) {
		store_.writeTags(replay->manifest, replay->tags);
	}
}

void CaptureManager::reconcile()
{
	if (exiting_) {
		return;
	}
	if (lifecycle_.running() && !wasRunning_) {
		nameBroadcast();
	}
	wasRunning_ = lifecycle_.running();

	for (auto it = entries_.begin(); it != entries_.end();) {
		const auto source = settings_.sources.find(it->first);
		if (source == settings_.sources.end() || !source->second.selected) {
			stop(*it->second);
			it = entries_.erase(it);
		} else {
			++it;
		}
	}

	if (!lifecycle_.running()) {
		for (auto &[uuid, entry] : entries_) {
			stop(*entry);
		}
		return;
	}
	std::optional<std::vector<EncoderInfo>> candidates;
	for (const std::string &uuid : settings_.selectedSources()) {
		auto found = entries_.find(uuid);
		if (found == entries_.end()) {
			found = entries_.emplace(uuid, std::make_unique<Entry>()).first;
		}
		Entry &entry = *found->second;
		if (entry.capture.active()) {
			updateActivation(uuid, entry, entry.source);
		} else if (start(uuid, entry, false, false, candidates) == StartOutcome::SourceRemoved) {
			entries_.erase(found);
		}
	}
}

CaptureManager::StartOutcome CaptureManager::start(const std::string &uuid, Entry &entry, bool keepBuffer, bool quiet,
						   std::optional<std::vector<EncoderInfo>> &candidates)
{
	entry.retry = false;
	entry.retryKeepsBuffer = keepBuffer;
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
	// A removed source can still be found while something holds it.
	if (source && obs_source_removed(source)) {
		return StartOutcome::SourceRemoved;
	}
	if (!source) {
		entry.retry = true;
		return StartOutcome::Failed;
	}
	// Before the size: a media source that plays only while active has no size until it
	// is.
	updateActivation(uuid, entry, source);
	if (quiet && (obs_source_get_width(source) == 0 || obs_source_get_height(source) == 0)) {
		entry.capture.hold(source);
		entry.retry = true;
		return StartOutcome::Failed;
	}

	if (!candidates) {
		candidates = replayEncoderCandidates(registeredVideoEncoders(), renderAdapterVendor(),
						     settings_.encoderPreferences());
	}
	CaptureSettings settings;
	settings.resolution = settings_.resolutionFor(uuid);
	settings.bufferLength = settings_.lengthFor(uuid);
	settings.candidates = *candidates;
	const StartResult result = entry.capture.start(source, settings, keepBuffer);
	if (result != StartResult::Started) {
		entry.retry = result == StartResult::NoSourceSize;
		if (!entry.retry) {
			entry.activation.reset();
		}
		return StartOutcome::Failed;
	}

	entry.removed = false;
	signal_handler_connect(obs_source_get_signal_handler(source), "remove", handleRemove, &entry.removed);
	entry.source = std::move(source);
	return StartOutcome::Started;
}

void CaptureManager::stop(Entry &entry)
{
	if (entry.source) {
		signal_handler_disconnect(obs_source_get_signal_handler(entry.source), "remove", handleRemove,
					  &entry.removed);
		entry.source = nullptr;
	}
	entry.capture.stop();
	entry.activation.reset();
	entry.activationLeftOut = false;
}

void CaptureManager::updateActivation(const std::string &uuid, Entry &entry, obs_source_t *source)
{
	if (!source) {
		return;
	}
	const bool wanted = settings_.activateFor(uuid);
	entry.activationLeftOut = wanted && reactsToActivation(source);
	if (wanted && !entry.activationLeftOut) {
		entry.activation.hold(source);
	} else {
		entry.activation.reset();
	}
}

void CaptureManager::Activation::hold(obs_source_t *source)
{
	if (source_) {
		return;
	}
	source_ = obs_source_get_ref(source);
	if (source_) {
		obs_source_inc_active(source_);
	}
}

void CaptureManager::Activation::reset() noexcept
{
	if (source_) {
		obs_source_dec_active(source_);
		obs_source_release(source_);
		source_ = nullptr;
	}
}

void CaptureManager::releaseAll()
{
	for (auto &[uuid, entry] : entries_) {
		stop(*entry);
	}
	entries_.clear();
}

void CaptureManager::followOutputs()
{
	const bool streaming = host_.streamingActive();
	if (streaming != lifecycle_.streaming()) {
		onStreaming(streaming);
	}
	const bool recording = host_.recordingActive();
	if (recording != lifecycle_.recording()) {
		onRecording(recording);
	}
}

void CaptureManager::handleRemove(void *data, calldata_t *) noexcept
{
	// The signal can come inside libobs's own source enumeration on the UI thread, where
	// stopping a capture would hold its locks, so poll does the stopping.
	*static_cast<std::atomic<bool> *>(data) = true;
}

} // namespace tapeloop::obs
