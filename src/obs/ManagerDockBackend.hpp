// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"
#include "obs/SequencePlayer.hpp"
#include "ui/DockBackend.hpp"

namespace tapeloop::obs {

// The dock's view of the capture manager and the sequence player: the video inputs of the
// scene collection with their selection and capture state, in the order of the sequence
// and then, for those it does not list yet, by name regardless of case. Without a
// sequence player every source is shown, they stay sorted by name and neither changes.
class ManagerDockBackend : public ui::DockBackend {
public:
	explicit ManagerDockBackend(CaptureManager &manager, SequencePlayer *sequence = nullptr)
		: manager_(manager),
		  sequence_(sequence)
	{
	}

	std::vector<ui::DockSource> sources() const override;
	bool setShown(const std::string &uuid, bool shown) override;
	bool moveSource(const std::string &uuid, const std::string &target) override;
	BufferSettings settings() const override { return manager_.settings(); }
	void setSettings(const BufferSettings &settings) override;
	std::vector<ui::EncoderChoice> encoderChoices() const override;

	bool running() const override { return manager_.running(); }
	bool manualControlEnabled() const override { return manager_.manualControlEnabled(); }
	bool toggleRunning() override { return manager_.running() ? manager_.manualStop() : manager_.manualStart(); }
	uint64_t captureReplay() override { return manager_.captureReplay(); }
	std::vector<ui::DockReplay> replays(const std::string &tag = {}) const override;
	std::vector<std::string> replayTags() const override;
	uint64_t physicalMemory() const override { return manager_.physicalMemory(); }
	uint64_t memoryNeeded() const override { return manager_.memoryNeeded(); }
	uint64_t memoryBudget() const override { return manager_.memoryBudget(); }
	bool tagReplay(uint64_t id, const std::string &tag) override { return manager_.tagReplay(id, tag); }
	bool untagReplay(uint64_t id, const std::string &tag) override { return manager_.untagReplay(id, tag); }
	bool deleteTag(const std::string &tag) override { return manager_.deleteReplayTag(tag); }
	uint64_t currentReplay() const override { return manager_.library().current(); }
	uint64_t lastCapture() const override { return manager_.library().lastCaptured(); }
	void pickReplay(uint64_t id) override { manager_.pickReplay(id); }
	bool canExport(ExportFormat format) const override { return manager_.canExport(format); }
	bool exportReplay(uint64_t id, ExportFormat format) override { return manager_.exportReplay(id, format); }
	std::vector<ui::DockExport> exports() const override;

private:
	// Adds the listed sources the sequence does not have yet after its own, in the order
	// listed, which is where sources() puts them.
	void enlist(const std::vector<ui::DockSource> &listed);

	CaptureManager &manager_;
	SequencePlayer *sequence_;
};

} // namespace tapeloop::obs
