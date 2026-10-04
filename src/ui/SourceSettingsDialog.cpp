// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/SourceSettingsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>

#include <chrono>

namespace tapeloop::ui {

SourceSettingsDialog::SourceSettingsDialog(const QString &sourceName, const SourceSettings &current,
					   const BufferSettings &global, const TextLookup &text, QWidget *parent)
	: QDialog(parent),
	  ownLength_(new QCheckBox(text("SourceSettings.OwnLength"), this)),
	  length_(new QSpinBox(this)),
	  ownResolution_(new QCheckBox(text("SourceSettings.OwnResolution"), this)),
	  resolution_(new QComboBox(this)),
	  ownActivation_(new QCheckBox(text("SourceSettings.OwnActivation"), this)),
	  activate_(new QCheckBox(text("SourceSettings.Activate"), this))
{
	setWindowTitle(text("SourceSettings.Title").arg(sourceName));

	length_->setObjectName("length");
	length_->setRange(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(kMinBufferLength).count()),
			  static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(kMaxBufferLength).count()));
	length_->setSuffix(text("Dock.SecondsSuffix"));
	length_->setKeyboardTracking(false);
	length_->setValue(static_cast<int>(
		std::chrono::duration_cast<std::chrono::seconds>(current.length.value_or(global.length)).count()));
	ownLength_->setObjectName("ownLength");
	ownLength_->setChecked(current.length.has_value());
	length_->setEnabled(current.length.has_value());

	addResolutions(*resolution_, text);
	resolution_->setObjectName("resolution");
	resolution_->setCurrentIndex(indexOfResolution(current.resolution.value_or(global.resolution)));
	ownResolution_->setObjectName("ownResolution");
	ownResolution_->setChecked(current.resolution.has_value());
	resolution_->setEnabled(current.resolution.has_value());

	activate_->setObjectName("activate");
	activate_->setToolTip(text("Dock.ActivateOffAir.Tooltip"));
	activate_->setChecked(current.activateOffAir.value_or(global.activateOffAir));
	ownActivation_->setObjectName("ownActivation");
	ownActivation_->setChecked(current.activateOffAir.has_value());
	activate_->setEnabled(current.activateOffAir.has_value());

	connect(ownLength_, &QCheckBox::toggled, length_, &QWidget::setEnabled);
	connect(ownResolution_, &QCheckBox::toggled, resolution_, &QWidget::setEnabled);
	connect(ownActivation_, &QCheckBox::toggled, activate_, &QWidget::setEnabled);

	auto *form = new QFormLayout;
	form->addRow(ownLength_, length_);
	form->addRow(ownResolution_, resolution_);
	form->addRow(ownActivation_, activate_);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);
}

SourceSettings SourceSettingsDialog::result() const
{
	SourceSettings settings;
	settings.length = ownLength_->isChecked() ? std::optional<Nanoseconds>(std::chrono::seconds(length_->value()))
						  : std::nullopt;
	settings.resolution = ownResolution_->isChecked()
				      ? std::optional<ReplayResolution>(resolutionAt(resolution_->currentIndex()))
				      : std::nullopt;
	settings.activateOffAir = ownActivation_->isChecked() ? std::optional<bool>(activate_->isChecked())
							      : std::nullopt;
	return settings;
}

} // namespace tapeloop::ui
