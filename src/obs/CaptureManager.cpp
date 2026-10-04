// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "obs/CaptureManager.hpp"

#include "obs/ObsEncoders.hpp"
#include "obs/SettingsData.hpp"

#include <util/base.h>

#include <utility>

namespace tapeloop::obs {
namespace {

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
	settings_ = BufferSettings{};
	savedNames_.clear();
	foreignSettings_ = nullptr;
	lifecycle_.reset(settings_.startWithOutputs, lifecycle_.streaming(), lifecycle_.recording());
}

void CaptureManager::onExit()
{
	exiting_ = true;
	releaseAll();
}

void CaptureManager::poll()
{
	if (exiting_) {
		return;
	}

	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second->removed) {
			stop(*it->second);
			it = entries_.erase(it);
		} else {
			++it;
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
	}
	return status;
}

const SourceBuffer *CaptureManager::buffer(const std::string &uuid) const
{
	const auto found = entries_.find(uuid);
	return found != entries_.end() ? found->second->capture.buffer() : nullptr;
}

void CaptureManager::reconcile()
{
	if (exiting_) {
		return;
	}

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
		if (!found->second->capture.active() &&
		    start(uuid, *found->second, false, false, candidates) == StartOutcome::SourceRemoved) {
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
	if (!source || (quiet && (obs_source_get_width(source) == 0 || obs_source_get_height(source) == 0))) {
		entry.retry = true;
		return StartOutcome::Failed;
	}

	if (!candidates) {
		candidates = replayEncoderCandidates(registeredVideoEncoders(), renderAdapterVendor(), {});
	}
	CaptureSettings settings;
	settings.resolution = settings_.resolutionFor(uuid);
	settings.bufferLength = settings_.lengthFor(uuid);
	settings.candidates = *candidates;
	const StartResult result = entry.capture.start(source, settings, keepBuffer);
	if (result != StartResult::Started) {
		entry.retry = result == StartResult::NoSourceSize;
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
