// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "obs/CaptureManager.hpp"
#include "obs/SequencePlayer.hpp"
#include "ui/DockBackend.hpp"

namespace tapeloop::obs {

// The user's scenes as OBS's scene list has them, read on the UI thread.
class SceneCatalog {
public:
	SceneCatalog() = default;
	virtual ~SceneCatalog() = default;

	SceneCatalog(const SceneCatalog &) = delete;
	SceneCatalog &operator=(const SceneCatalog &) = delete;
	SceneCatalog(SceneCatalog &&) = delete;
	SceneCatalog &operator=(SceneCatalog &&) = delete;

	virtual std::vector<OverlayScene> scenes() const = 0;
};

// Makes the overlay editor's live picture, a widget the plugin draws with OBS.
class OverlayViewFactory {
public:
	OverlayViewFactory() = default;
	virtual ~OverlayViewFactory() = default;

	OverlayViewFactory(const OverlayViewFactory &) = delete;
	OverlayViewFactory &operator=(const OverlayViewFactory &) = delete;
	OverlayViewFactory(OverlayViewFactory &&) = delete;
	OverlayViewFactory &operator=(OverlayViewFactory &&) = delete;

	virtual ui::OverlayView *createOverlayView(QWidget *parent) = 0;
};

// Opens OBS's properties dialog of a source, as the frontend does; UI thread only.
class PropertiesOpener {
public:
	PropertiesOpener() = default;
	virtual ~PropertiesOpener() = default;

	PropertiesOpener(const PropertiesOpener &) = delete;
	PropertiesOpener &operator=(const PropertiesOpener &) = delete;
	PropertiesOpener(PropertiesOpener &&) = delete;
	PropertiesOpener &operator=(PropertiesOpener &&) = delete;

	virtual void openProperties(obs_source_t *source) = 0;
};

// The dock's view of the capture manager and the sequence player: the video inputs of the
// scene collection with their selection and capture state, in the order of the sequence
// and then, for those it does not list yet, by name regardless of case. Without a
// sequence player every source is shown, they stay sorted by name and neither changes.
// Without a scene catalog there are no scenes to pick, without an opener no properties
// open, and without a view factory the overlay editor has no live picture.
class ManagerDockBackend : public ui::DockBackend {
public:
	explicit ManagerDockBackend(CaptureManager &manager, SequencePlayer *sequence = nullptr,
				    const SceneCatalog *scenes = nullptr, PropertiesOpener *properties = nullptr,
				    OverlayViewFactory *views = nullptr)
		: manager_(manager),
		  sequence_(sequence),
		  scenes_(scenes),
		  properties_(properties),
		  views_(views)
	{
	}

	std::vector<ui::DockSource> sources() const override;
	bool setShown(const std::string &uuid, bool shown) override;
	bool moveSource(const std::string &uuid, const std::string &target) override;
	BufferSettings settings() const override { return manager_.settings(); }
	void setSettings(const BufferSettings &settings) override;
	std::vector<ui::EncoderChoice> encoderChoices() const override;
	std::vector<std::string> transitions() const override;

	bool running() const override { return manager_.running(); }
	bool manualControlEnabled() const override { return manager_.manualControlEnabled(); }
	bool toggleRunning() override { return manager_.running() ? manager_.manualStop() : manager_.manualStart(); }
	// The cue follows the replay captured or picked.
	uint64_t captureReplay() override;
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
	void pickReplay(uint64_t id) override;
	AirRefusal airRefusal() const override { return sequence_ ? sequence_->airRefusal() : AirRefusal::None; }
	std::string missingIntro() const override
	{
		return sequence_ ? sequence_->air().missingIntro() : std::string();
	}
	std::string missingOutro() const override
	{
		return sequence_ ? sequence_->air().missingOutro() : std::string();
	}
	std::vector<OverlayScene> scenes() const override;
	bool sceneHoldsReplay(const std::string &uuid) const override;
	std::string missingScene() const override
	{
		return sequence_ ? sequence_->air().missingScene() : std::string();
	}
	std::vector<ui::OverlaySource> overlaySources() const override;
	CanvasSize canvas() const override;
	bool openProperties(const std::string &uuid) override;
	ui::OverlayView *createOverlayView(QWidget *parent) override
	{
		return views_ ? views_->createOverlayView(parent) : nullptr;
	}
	bool canExport(ExportFormat format) const override { return manager_.canExport(format); }
	bool exportReplay(uint64_t id, ExportFormat format) override { return manager_.exportReplay(id, format); }
	std::vector<ui::DockExport> exports() const override;
	std::unique_ptr<ui::PictureSurface> pictureSurface() override;
	ui::DockPlayer player() const override;
	void controlPlayer(ui::PlayerControl control, bool pressed) override;
	void seekPlayer(Nanoseconds position) override;

private:
	// Adds the listed sources the sequence does not have yet after its own, in the order
	// listed, which is where sources() puts them.
	void enlist(const std::vector<ui::DockSource> &listed);

	CaptureManager &manager_;
	SequencePlayer *sequence_;
	const SceneCatalog *scenes_;
	PropertiesOpener *properties_;
	OverlayViewFactory *views_;
};

} // namespace tapeloop::obs
