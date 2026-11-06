// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/AirSettingsDialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace tapeloop::ui {
namespace {

// What OBS's own media and image sources offer to open.
constexpr auto kVideoPatterns = "*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm";
#ifdef _WIN32
constexpr auto kImagePatterns = "*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp";
#else
constexpr auto kImagePatterns = "*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp";
#endif
// As OBS's own transition duration box.
constexpr int kDurationStep = 50;

QString filter(const QString &label, const char *patterns)
{
	return QStringLiteral("%1 (%2)").arg(label, QString::fromLatin1(patterns));
}

void addTransitions(QComboBox &combo, const std::string &picked, const std::vector<std::string> &transitions,
		    const TextLookup &text)
{
	combo.addItem(text("AirSettings.Fade"), QString());
	QStringList names;
	for (const std::string &name : transitions) {
		if (!name.empty()) {
			names.append(QString::fromStdString(name));
		}
	}
	std::stable_sort(names.begin(), names.end(), [](const QString &left, const QString &right) {
		return left.compare(right, Qt::CaseInsensitive) < 0;
	});
	for (const QString &name : names) {
		combo.addItem(name, name);
	}
	if (picked.empty()) {
		return;
	}
	const QString name = QString::fromStdString(picked);
	if (!names.contains(name)) {
		combo.addItem(text("AirSettings.MissingTransition").arg(name), name);
	}
	combo.setCurrentIndex(combo.findData(name));
}

} // namespace

AirSettingsDialog::AirSettingsDialog(const AirSettings &current, const std::vector<std::string> &transitions,
				     const TextLookup &text, QWidget *parent)
	: QDialog(parent),
	  videoFilters_{filter(text("AirSettings.VideoFiles"), kVideoPatterns),
			filter(text("AirSettings.AllFiles"), "*")},
	  imageFilters_{filter(text("AirSettings.ImageFiles"), kImagePatterns),
			filter(text("AirSettings.AllFiles"), "*")}
{
	setWindowTitle(text("AirSettings.Title"));

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(addPart(intro_, QStringLiteral("intro"), text("AirSettings.OnAir"), text("AirSettings.Intro"),
				  current.intro, current.into, transitions, text));
	layout->addWidget(addPart(outro_, QStringLiteral("outro"), text("AirSettings.BackToLive"),
				  text("AirSettings.Outro"), current.outro, current.back, transitions, text));
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

QGroupBox *AirSettingsDialog::addPart(Part &part, const QString &name, const QString &title, const QString &kindLabel,
				      const AirMedia &media, const AirTransition &transition,
				      const std::vector<std::string> &transitions, const TextLookup &text)
{
	auto *box = new QGroupBox(title, this);

	part.kind = new QComboBox(box);
	part.kind->setObjectName(name + "Kind");
	part.kind->addItem(text("AirSettings.None"), static_cast<int>(AirMediaKind::None));
	part.kind->addItem(text("AirSettings.Video"), static_cast<int>(AirMediaKind::Video));
	part.kind->addItem(text("AirSettings.Image"), static_cast<int>(AirMediaKind::Image));
	part.kind->setCurrentIndex(part.kind->findData(static_cast<int>(media.kind)));

	part.path = new QLineEdit(QString::fromStdString(media.path), box);
	part.path->setObjectName(name + "File");
	part.browse = new QPushButton(text("AirSettings.Browse"), box);
	part.browse->setObjectName(name + "Browse");

	part.imageTime = new QDoubleSpinBox(box);
	part.imageTime->setObjectName(name + "ImageTime");
	part.imageTime->setDecimals(1);
	part.imageTime->setSingleStep(0.5);
	part.imageTime->setRange(std::chrono::duration<double>(kMinImageTime).count(),
				 std::chrono::duration<double>(kMaxImageTime).count());
	part.imageTime->setSuffix(text("Dock.SecondsSuffix"));
	part.imageTime->setValue(std::chrono::duration<double>(media.imageTime).count());
	part.savedImageTime = media.imageTime;
	part.shownImageTime = std::chrono::milliseconds(std::lround(part.imageTime->value() * 1000));

	part.transition = new QComboBox(box);
	part.transition->setObjectName(name + "Transition");
	addTransitions(*part.transition, transition.name, transitions, text);

	part.duration = new QSpinBox(box);
	part.duration->setObjectName(name + "Duration");
	part.duration->setRange(static_cast<int>(kMinAirTransition.count()),
				static_cast<int>(kMaxAirTransition.count()));
	part.duration->setSingleStep(kDurationStep);
	part.duration->setSuffix(text("AirSettings.MillisecondsSuffix"));
	part.duration->setValue(static_cast<int>(transition.duration.count()));

	const auto followKind = [&part] {
		const auto kind = static_cast<AirMediaKind>(part.kind->currentData().toInt());
		part.path->setEnabled(kind != AirMediaKind::None);
		part.browse->setEnabled(kind != AirMediaKind::None);
		part.imageTime->setEnabled(kind == AirMediaKind::Image);
	};
	followKind();
	connect(part.kind, &QComboBox::currentIndexChanged, this, followKind);
	connect(part.browse, &QPushButton::clicked, this, [this, &part] {
		// Qt cannot let an exception through its event loop; the box simply does not open.
		try {
			browse(part);
		} catch (...) {
		}
	});

	auto *file = new QHBoxLayout;
	file->addWidget(part.path, 1);
	file->addWidget(part.browse);
	auto *animation = new QHBoxLayout;
	animation->addWidget(part.transition, 1);
	animation->addWidget(part.duration);
	// addRow gives a label its buddy only when the row is a single widget.
	auto *fileLabel = new QLabel(text("AirSettings.File"), box);
	fileLabel->setBuddy(part.path);
	auto *transitionLabel = new QLabel(text("AirSettings.Transition"), box);
	transitionLabel->setBuddy(part.transition);
	auto *form = new QFormLayout(box);
	form->addRow(kindLabel, part.kind);
	form->addRow(fileLabel, file);
	form->addRow(text("AirSettings.ImageTime"), part.imageTime);
	form->addRow(transitionLabel, animation);
	return box;
}

void AirSettingsDialog::browse(const Part &part)
{
	// Not run with exec(): its nested event loop could outlive this dialog. As a child,
	// the box goes with it.
	auto *files = new QFileDialog(this);
	files->setObjectName("airFile");
	files->setAttribute(Qt::WA_DeleteOnClose);
	files->setFileMode(QFileDialog::ExistingFile);
	const bool image = static_cast<AirMediaKind>(part.kind->currentData().toInt()) == AirMediaKind::Image;
	files->setNameFilters(image ? imageFilters_ : videoFilters_);
	if (!part.path->text().isEmpty()) {
		const QFileInfo file(part.path->text());
		files->setDirectory(file.absolutePath());
		files->selectFile(file.fileName());
	}
	connect(files, &QFileDialog::fileSelected, part.path, &QLineEdit::setText);
	files->open();
}

AirMedia AirSettingsDialog::mediaOf(const Part &part)
{
	AirMedia media;
	media.kind = static_cast<AirMediaKind>(part.kind->currentData().toInt());
	media.path = part.path->text().toStdString();
	const std::chrono::milliseconds shown(std::lround(part.imageTime->value() * 1000));
	media.imageTime = shown == part.shownImageTime ? part.savedImageTime : shown;
	return media;
}

AirTransition AirSettingsDialog::transitionOf(const Part &part)
{
	AirTransition transition;
	transition.name = part.transition->currentData().toString().toStdString();
	transition.duration = std::chrono::milliseconds(part.duration->value());
	return transition;
}

AirSettings AirSettingsDialog::chosen() const
{
	AirSettings settings;
	settings.intro = mediaOf(intro_);
	settings.into = transitionOf(intro_);
	settings.outro = mediaOf(outro_);
	settings.back = transitionOf(outro_);
	return settings;
}

} // namespace tapeloop::ui
