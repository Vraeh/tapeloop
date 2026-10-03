// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockBackend.hpp"
#include "ui/DockText.hpp"

#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace tapeloop::ui {

// The Tapeloop dock: the video sources of the scene collection with a checkbox each and
// the state of the selected ones, the global buffer settings, and the manual start and
// stop. It reads the backend again every second. Everything follows the OBS theme.
class TapeloopDock : public QWidget {
public:
	TapeloopDock(DockBackend &backend, TextLookup text, QWidget *parent = nullptr);

	// Reads the backend again; a timer calls it every second.
	void refresh();

private:
	void rebuildSources(const std::vector<DockSource> &sources);
	void updateSources(const std::vector<DockSource> &sources);
	QString statusText(const DockSource &source) const;
	void changeSettings(void (*change)(BufferSettings &, int), int value);
	void editSourceSettings(int row);

	DockBackend &backend_;
	TextLookup text_;
	std::vector<std::string> shownUuids_;
	std::vector<std::string> shownNames_;
	QTableWidget *sources_;
	QPushButton *sourceSettings_;
	QSpinBox *length_;
	QComboBox *resolution_;
	QCheckBox *startWithOutputs_;
	QLabel *note_;
	QPushButton *startStop_;
};

} // namespace tapeloop::ui
