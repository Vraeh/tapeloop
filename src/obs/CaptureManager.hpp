// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferLifecycle.hpp"
#include "core/BufferSettings.hpp"
#include "obs/SourceCapture.hpp"

#include <obs.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tapeloop::obs {

// What the manager asks the OBS frontend. The plugin implements it with
// obs-frontend-api; tests replace it.
class CaptureHost {
public:
	virtual ~CaptureHost() = default;
	virtual bool streamingActive() const = 0;
	virtual bool recordingActive() const = 0;
	// The settings were edited: have them saved at once, so that a crash right after an
	// edit loses nothing.
	virtual void requestSave() = 0;
};

struct SourceStatus {
	bool selected = false;
	CaptureStats stats;
	// The settings ask to keep it active off air, but it restarts when it becomes active,
	// so it is not held.
	bool activationLeftOut = false;
};

// Every selected source with its capture and buffer, started and stopped as the buffer
// lifecycle and the settings say. Every call runs on the UI thread.
class CaptureManager {
public:
	explicit CaptureManager(CaptureHost &host);
	~CaptureManager();

	CaptureManager(const CaptureManager &) = delete;
	CaptureManager &operator=(const CaptureManager &) = delete;

	const BufferSettings &settings() const noexcept { return settings_; }
	// An edit of the settings, which the host is asked to save unless the collection
	// holds settings of another version, kept as they came. Sources selected while the
	// buffers run start at once and unselected ones stop and free their buffer; any other
	// change reaches a capture at its next start.
	void setSettings(BufferSettings settings);

	bool running() const noexcept { return lifecycle_.running(); }
	bool manualControlEnabled() const noexcept { return lifecycle_.manualControlEnabled(); }
	// False when refused.
	bool manualStart();
	bool manualStop();

	void onStreaming(bool active);
	void onRecording(bool active);
	// The scene collection is being cleared: every capture stops, every buffer goes and
	// no source reference is kept, so that the frontend can free them all.
	void onSceneCollectionCleanup();
	void onExit();

	// Stops the captures of removed sources, restarts captures whose source changed size
	// keeping their buffers, retries selected sources that had no size or were not found,
	// and catches up with output changes the frontend did not report. Meant to run every
	// second or so.
	void poll();

	// The settings under kSettingsKey of the scene collection's data. Saving leaves out
	// sources the collection no longer has. Loading finds a source whose UUID changed,
	// as every UUID does in a duplicated collection, by its saved name (see
	// matchSourcesByName), and starts the lifecycle over with the collection's own
	// settings.
	void save(obs_data_t *collection) const;
	void load(obs_data_t *collection);

	SourceStatus status(const std::string &uuid) const;
	// Null when the source has no buffer.
	const SourceBuffer *buffer(const std::string &uuid) const;

private:
	// Keeps a source active, as if it were on air, until reset or destroyed. Activation
	// adds the source to no view of the program: libobs mixes into the program audio
	// only the sources on a canvas that mixes audio, and a capture's view does not.
	class Activation {
	public:
		Activation() = default;
		~Activation() { reset(); }

		Activation(const Activation &) = delete;
		Activation &operator=(const Activation &) = delete;

		void hold(obs_source_t *source);
		void reset() noexcept;

	private:
		obs_source_t *source_ = nullptr;
	};

	struct Entry {
		SourceCapture capture;
		// Held while capturing, with the remove signal connected to removed.
		OBSSourceAutoRelease source;
		// Set by the remove signal, from whatever thread removes the source.
		std::atomic<bool> removed{false};
		// Starting failed for lack of a size or of the source itself, which poll retries
		// with the same keepBuffer.
		bool retry = false;
		bool retryKeepsBuffer = false;
		// Held while the capture runs or waits, when the settings ask for it and the source
		// does not restart when it becomes active.
		Activation activation;
		bool activationLeftOut = false;
	};

	// What a start found out about the source.
	enum class StartOutcome { Started, Failed, SourceRemoved };

	void reconcile();
	// candidates is filled on first use, so a batch of starts reads the encoders and the
	// render adapter once. A quiet start does not try a source without a size: it only
	// shows it on the capture's view, without logging.
	StartOutcome start(const std::string &uuid, Entry &entry, bool keepBuffer, bool quiet,
			   std::optional<std::vector<EncoderInfo>> &candidates);
	void stop(Entry &entry);
	// Holds the source active or lets go of it, as the settings and the source's own
	// restart setting now say.
	void updateActivation(const std::string &uuid, Entry &entry, obs_source_t *source);
	void releaseAll();
	void followOutputs();
	static void handleRemove(void *data, calldata_t *) noexcept;

	CaptureHost &host_;
	BufferSettings settings_;
	BufferLifecycle lifecycle_;
	std::map<std::string, std::unique_ptr<Entry>> entries_;
	// Names saved with the settings, to find a source whose UUID changed and for log
	// lines about sources that are not found.
	std::map<std::string, std::string> savedNames_;
	// Settings of a version this build does not know, written back as they came.
	OBSDataAutoRelease foreignSettings_;
	bool exiting_ = false;
};

} // namespace tapeloop::obs
