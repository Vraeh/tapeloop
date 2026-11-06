// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"
#include "ui/DockText.hpp"

#include <QDialog>
#include <QStringList>

#include <chrono>
#include <string>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace tapeloop::ui {

// Lets the user set how a replay goes on air and back to live. The transitions on offer
// are the ones the user has in OBS, in any order; one that was picked and is not among
// them any more stays on offer, marked, for it may come back.
class AirSettingsDialog : public QDialog {
public:
	AirSettingsDialog(const AirSettings &current, const std::vector<std::string> &transitions,
			  const TextLookup &text, QWidget *parent = nullptr);

	// What was not touched comes back as it came in, whatever the controls can show.
	AirSettings result() const;

private:
	struct Part {
		QComboBox *kind = nullptr;
		QLineEdit *path = nullptr;
		QPushButton *browse = nullptr;
		QDoubleSpinBox *imageTime = nullptr;
		QComboBox *transition = nullptr;
		QSpinBox *duration = nullptr;
		std::chrono::milliseconds savedImageTime{};
		std::chrono::milliseconds shownImageTime{};
	};

	QGroupBox *addPart(Part &part, const QString &name, const QString &title, const QString &kindLabel,
			   const AirMedia &media, const AirTransition &transition,
			   const std::vector<std::string> &transitions, const TextLookup &text);
	void browse(const Part &part);
	static AirMedia mediaOf(const Part &part);
	static AirTransition transitionOf(const Part &part);

	QStringList videoFilters_;
	QStringList imageFilters_;
	Part intro_;
	Part outro_;
};

} // namespace tapeloop::ui
