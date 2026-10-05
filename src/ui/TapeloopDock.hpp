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
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace tapeloop::ui {

// The Tapeloop dock: the video sources of the scene collection with a checkbox each and
// the state of the selected ones, the global buffer settings, and the manual start and
// stop. While shown it reads the backend again every second. Everything follows the OBS
// theme. In the source list, Space toggles the current source and Enter opens its
// settings.
class TapeloopDock : public QWidget {
public:
	TapeloopDock(DockBackend &backend, TextLookup text, QWidget *parent = nullptr);

	// Reads the backend again.
	void refresh();

protected:
	void showEvent(QShowEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void rebuildSources(const std::vector<DockSource> &sources);
	void updateSources(const std::vector<DockSource> &sources);
	void updateReplays();
	QString statusText(const DockSource &source) const;
	void changeSettings(void (*change)(BufferSettings &, int), int value);
	void openSourceSettings(int row);
	void applySourceSettings(const std::string &uuid, const SourceSettings &chosen);

	DockBackend &backend_;
	TextLookup text_;
	std::vector<std::string> shownUuids_;
	std::vector<std::string> shownNames_;
	QTableWidget *sources_;
	QPushButton *sourceSettings_;
	QSpinBox *length_;
	QComboBox *resolution_;
	QCheckBox *startWithOutputs_;
	QCheckBox *activateOffAir_;
	QCheckBox *forceH264_;
	QLabel *note_;
	QPushButton *startStop_;
	QLabel *followsOutputs_;
	QPushButton *captureReplay_;
	QComboBox *tagFilter_;
	QListWidget *replays_;
	QLineEdit *tagName_;
	QPushButton *addTag_;
	// The ids the replay list shows, newest first, and the text of each.
	std::vector<uint64_t> shownReplays_;
	std::vector<QString> shownReplayTexts_;
	std::vector<std::string> shownTags_;
};

} // namespace tapeloop::ui
