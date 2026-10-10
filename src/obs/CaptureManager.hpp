// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferLifecycle.hpp"
#include "core/BufferSettings.hpp"
#include "core/ReplayLibrary.hpp"
#include "core/ReplayStore.hpp"
#include "obs/SourceCapture.hpp"

#include <obs.hpp>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
	// The folder OBS records into, in UTF-8; empty when it has none.
	virtual std::string recordingFolder() const = 0;
	virtual std::string sceneCollectionName() const = 0;
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
	// buffers run start at once and unselected ones stop and free their buffer, and
	// activation off air reaches running captures at once; any other change reaches a
	// capture at its next start.
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
	// catches up with output changes the frontend did not report, takes in what the store
	// did, and lets the library forget the frames of replays the buffers no longer hold.
	// Meant to run every second or so.
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

	// Keeps what every captured buffer holds as one replay: the range reaches back from
	// now to the oldest frame any buffer holds, and each source's clip is all its buffer
	// holds, a stopped buffer's included. Buffers keep recording. The replay is written in
	// the background into the broadcast folder named when the buffers last started, and
	// the library keeps only its index. Zero when no buffer holds anything.
	uint64_t captureReplay();
	// Reads back, in the background, the replays of every broadcast folder. Called once
	// OBS has loaded, and again when buffers start with OBS recording somewhere else.
	void loadLibrary();
	// Waits for the replays being written and takes in what became of them.
	void finishWrites();
	const ReplayLibrary &library() const noexcept { return library_; }
	// Each changes the library, and writes the tags of a replay stored on disk into its
	// manifest.
	bool tagReplay(uint64_t id, std::string_view tag);
	bool untagReplay(uint64_t id, std::string_view tag);
	bool deleteReplayTag(std::string_view tag);
	bool pickReplay(uint64_t id) { return library_.pick(id); }
	// Where the replays of the buffers that run now go: a folder named after the scene
	// collection and the minute they started, under OBS's recording folder. Empty while
	// OBS has no recording folder; then the first capture after it has one names it.
	const std::filesystem::path &broadcastFolder() const noexcept { return broadcastFolder_; }
	// Where every broadcast folder is.
	std::filesystem::path replayFolder() const;

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
	void nameBroadcast();
	void takeStoreResults();
	// Has the store write the replay's tags when it is stored on disk.
	void saveTags(uint64_t id);
	static void handleRemove(void *data, calldata_t *) noexcept;

	CaptureHost &host_;
	BufferSettings settings_;
	BufferLifecycle lifecycle_;
	ReplayLibrary library_;
	std::map<std::string, std::unique_ptr<Entry>> entries_;
	// Names saved with the settings, to find a source whose UUID changed and for log
	// lines about sources that are not found.
	std::map<std::string, std::string> savedNames_;
	// Settings of a version this build does not know, written back as they came.
	OBSDataAutoRelease foreignSettings_;
	bool exiting_ = false;
	// Whether the buffers ran at the last reconcile, to name a broadcast when they start.
	bool wasRunning_ = false;
	std::string broadcastName_;
	std::filesystem::path broadcastFolder_;
	// The folder of every broadcast the library was last read from.
	std::filesystem::path scannedFolder_;
	// The replay each capture the store is writing belongs to, by ticket.
	std::map<uint64_t, uint64_t> writing_;
	bool priorityLogged_ = false;
	ReplayStore store_;
};

} // namespace tapeloop::obs
