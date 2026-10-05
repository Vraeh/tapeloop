// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "core/BufferSettings.hpp"
#include "ui/DockText.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;

namespace tapeloop::ui {

// Lets one source have its own buffer length, resolution or activation off air instead
// of the global ones.
class SourceSettingsDialog : public QDialog {
public:
	SourceSettingsDialog(const QString &sourceName, const SourceSettings &current, const BufferSettings &global,
			     const TextLookup &text, QWidget *parent = nullptr);

	// The source's own length, resolution and activation as the dialog leaves them; the
	// selection is not the dialog's and stays unset.
	SourceSettings result() const;

private:
	QCheckBox *ownLength_;
	QSpinBox *length_;
	QCheckBox *ownResolution_;
	QComboBox *resolution_;
	QCheckBox *ownActivation_;
	QCheckBox *activate_;
};

} // namespace tapeloop::ui
