// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockBackend.hpp"
#include "ui/DockText.hpp"

#include <QWidget>

#include <optional>
#include <string>
#include <utility>
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
	void updateEncoders(const std::string &chosen);
	void changeSettings(void (*change)(BufferSettings &, int), int value);
	void openSourceSettings(int row);
	void applySourceSettings(const std::string &uuid, const SourceSettings &chosen);

	DockBackend &backend_;
	TextLookup text_;
	std::vector<std::string> shownUuids_;
	// What the encoder list holds, as id and name pairs.
	std::vector<std::pair<std::string, std::string>> shownEncoders_;
	std::vector<std::string> shownNames_;
	QTableWidget *sources_;
	QPushButton *sourceSettings_;
	QSpinBox *length_;
	QComboBox *resolution_;
	QCheckBox *startWithOutputs_;
	QCheckBox *activateOffAir_;
	QCheckBox *forceH264_;
	QCheckBox *advanced_;
	QWidget *advancedSettings_;
	QComboBox *replayEncoder_;
	QCheckBox *otherAdapters_;
	QLabel *note_;
	QPushButton *startStop_;
	QLabel *followsOutputs_;
	QPushButton *captureReplay_;
	QComboBox *tagFilter_;
	QListWidget *replays_;
	QLineEdit *tagName_;
	QPushButton *addTag_;
	// The ids the replay list shows, newest first, zero for the rows naming a broadcast,
	// and the text and tooltip of each.
	std::vector<uint64_t> shownReplays_;
	std::vector<QString> shownReplayTexts_;
	std::vector<QString> shownReplayTips_;
	std::vector<std::string> shownTags_;
	// The last capture the list has seen, to tell a new capture from a pick.
	uint64_t lastCaptureSeen_ = 0;
	bool refreshQueued_ = false;
	bool leftButtonHeld_ = false;
	// The replay to show next when the left button was pressed on the list, until the
	// click of that press is handled.
	std::optional<uint64_t> currentAtPress_;
};

} // namespace tapeloop::ui
