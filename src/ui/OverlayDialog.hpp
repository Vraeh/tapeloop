// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/OverlayLayout.hpp"
#include "ui/DockBackend.hpp"
#include "ui/DockText.hpp"

#include <QDialog>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QSpinBox;

namespace tapeloop::ui {

class OverlayPreview;

// Edits the overlays of the replays on a copy: OK and Apply set it, Cancel drops it, so
// nothing changes on air while editing. The anchor, fit, crop, blend and the user's own
// replay scene show only with the dock's advanced settings. In the list, topmost first,
// Space shows or hides an overlay, Delete removes it, Alt+Up and Alt+Down move it.
class OverlayDialog : public QDialog {
public:
	OverlayDialog(DockBackend &backend, TextLookup text, QWidget *parent = nullptr);

	// The dock's advanced settings switch shown or hidden.
	void showAdvanced(bool advanced);
	const OverlayLayout &editing() const noexcept { return layout_; }
	OverlayPreview *preview() const noexcept { return preview_; }

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void buildScenes();
	void showItems();
	void showSelected();
	// After the preview changed the copy or chose another overlay.
	void followPreview();
	void showScene();
	// False when the scene collection changed under the dialog and nothing was written.
	bool apply();
	void openPicker();
	void addOverlay(const OverlaySource &source);
	void removeSelected();
	void moveSelected(bool up);
	void openProperties();
	void editSelected(const std::function<void(size_t index)> &edit);
	void changeSelected(void (*change)(OverlayItem &, int), int value);
	void chooseScene(int index);
	std::optional<size_t> indexOfRow(int row) const noexcept;
	int rowOfIndex(size_t index) const noexcept;

	DockBackend &backend_;
	TextLookup text_;
	OverlaySettings opened_;
	OverlaySettings start_;
	OverlayLayout layout_;
	std::vector<OverlayScene> scenes_;
	std::vector<std::string> knownSources_;

	QListWidget *overlays_;
	QPushButton *add_;
	QPushButton *remove_;
	QPushButton *moveUp_;
	QPushButton *moveDown_;
	QPushButton *properties_;
	QLabel *sceneNote_;
	QWidget *fields_;
	QComboBox *preset_;
	QSpinBox *x_;
	QSpinBox *y_;
	QSpinBox *width_;
	QSpinBox *height_;
	QSlider *opacitySlider_;
	QSpinBox *opacity_;
	QWidget *advancedFields_;
	QComboBox *anchor_;
	QSpinBox *distanceX_;
	QSpinBox *distanceY_;
	QComboBox *fit_;
	QSpinBox *cropLeft_;
	QSpinBox *cropTop_;
	QSpinBox *cropRight_;
	QSpinBox *cropBottom_;
	QComboBox *blend_;
	QWidget *sceneBox_;
	QComboBox *scene_;
	QLabel *sceneWarning_;
	OverlayPreview *preview_;
	QDialogButtonBox *buttons_;
};

} // namespace tapeloop::ui
