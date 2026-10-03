// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/TapeloopDock.hpp"

#include "ui/SourceSettingsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <utility>

namespace tapeloop::ui {
namespace {

constexpr int kSourceColumn = 0;
constexpr int kStatusColumn = 1;

int seconds(Nanoseconds length)
{
	return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(length).count());
}

// Qt cannot let an exception through its event loop; a failed change is dropped and the
// next refresh shows the state as it is.
template<typename Function> void guarded(Function &&function) noexcept
{
	try {
		function();
	} catch (...) {
	}
}

} // namespace

TapeloopDock::TapeloopDock(DockBackend &backend, TextLookup text, QWidget *parent)
	: QWidget(parent),
	  backend_(backend),
	  text_(std::move(text)),
	  sources_(new QTableWidget(0, 2, this)),
	  sourceSettings_(new QPushButton(text_("Dock.SourceSettings"), this)),
	  length_(new QSpinBox(this)),
	  resolution_(new QComboBox(this)),
	  startWithOutputs_(new QCheckBox(text_("Dock.StartWithOutputs"), this)),
	  note_(new QLabel(text_("Dock.ApplyNote"), this)),
	  startStop_(new QPushButton(this))
{
	sources_->setObjectName("sources");
	sources_->setHorizontalHeaderLabels({text_("Dock.Source"), text_("Dock.Status")});
	sources_->horizontalHeader()->setSectionResizeMode(kSourceColumn, QHeaderView::Stretch);
	sources_->horizontalHeader()->setSectionResizeMode(kStatusColumn, QHeaderView::ResizeToContents);
	sources_->verticalHeader()->hide();
	sources_->setSelectionBehavior(QAbstractItemView::SelectRows);
	sources_->setSelectionMode(QAbstractItemView::SingleSelection);
	sources_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	sources_->setTabKeyNavigation(false);

	sourceSettings_->setObjectName("sourceSettings");
	sourceSettings_->setEnabled(false);
	length_->setObjectName("length");
	length_->setRange(seconds(kMinBufferLength), seconds(kMaxBufferLength));
	length_->setSuffix(text_("Dock.SecondsSuffix"));
	resolution_->setObjectName("resolution");
	addResolutions(*resolution_, text_);
	startWithOutputs_->setObjectName("startWithOutputs");
	note_->setObjectName("note");
	note_->setWordWrap(true);
	startStop_->setObjectName("startStop");

	auto *form = new QFormLayout;
	form->addRow(text_("Dock.Length"), length_);
	form->addRow(text_("Dock.Resolution"), resolution_);
	form->addRow(startWithOutputs_);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(sources_, 1);
	layout->addWidget(sourceSettings_);
	layout->addLayout(form);
	layout->addWidget(note_);
	layout->addWidget(startStop_);

	connect(sources_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (item->column() != kSourceColumn)
			return;
		guarded([&] {
			BufferSettings settings = backend_.settings();
			settings.sources[item->data(Qt::UserRole).toString().toStdString()].selected =
				item->checkState() == Qt::Checked;
			backend_.setSettings(settings);
		});
		refresh();
	});
	connect(sources_, &QTableWidget::currentCellChanged, this,
		[this](int row) { sourceSettings_->setEnabled(row >= 0); });
	connect(sources_, &QTableWidget::cellDoubleClicked, this, [this](int row) { editSourceSettings(row); });
	connect(sourceSettings_, &QPushButton::clicked, this, [this] { editSourceSettings(sources_->currentRow()); });
	connect(length_, &QSpinBox::valueChanged, this, [this](int value) {
		changeSettings([](BufferSettings &settings,
				  int seconds) { settings.length = std::chrono::seconds(seconds); },
			       value);
	});
	connect(resolution_, &QComboBox::currentIndexChanged, this, [this](int index) {
		changeSettings([](BufferSettings &settings, int chosen) { settings.resolution = resolutionAt(chosen); },
			       index);
	});
	connect(startWithOutputs_, &QCheckBox::toggled, this, [this](bool checked) {
		changeSettings([](BufferSettings &settings, int on) { settings.startWithOutputs = on != 0; },
			       checked ? 1 : 0);
	});
	connect(startStop_, &QPushButton::clicked, this, [this] {
		guarded([this] { backend_.toggleRunning(); });
		refresh();
	});

	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &TapeloopDock::refresh);
	timer->start(1000);
	refresh();
}

void TapeloopDock::refresh()
{
	guarded([this] {
		const std::vector<DockSource> sources = backend_.sources();
		std::vector<std::string> uuids;
		std::vector<std::string> names;
		for (const DockSource &source : sources) {
			uuids.push_back(source.uuid);
			names.push_back(source.name);
		}
		if (uuids != shownUuids_ || names != shownNames_) {
			rebuildSources(sources);
			shownUuids_ = std::move(uuids);
			shownNames_ = std::move(names);
		} else {
			updateSources(sources);
		}

		const BufferSettings settings = backend_.settings();
		const QSignalBlocker blockLength(length_);
		const QSignalBlocker blockResolution(resolution_);
		const QSignalBlocker blockStart(startWithOutputs_);
		// A value being typed is not overwritten.
		if (!length_->hasFocus())
			length_->setValue(seconds(settings.length));
		resolution_->setCurrentIndex(indexOfResolution(settings.resolution));
		startWithOutputs_->setChecked(settings.startWithOutputs);

		startStop_->setText(backend_.running() ? text_("Dock.Stop") : text_("Dock.Start"));
		const bool enabled = backend_.manualControlEnabled();
		startStop_->setEnabled(enabled);
		startStop_->setToolTip(enabled ? QString() : text_("Dock.FollowsOutputs"));
	});
}

void TapeloopDock::rebuildSources(const std::vector<DockSource> &sources)
{
	const QSignalBlocker block(sources_);
	sources_->setRowCount(static_cast<int>(sources.size()));
	for (size_t i = 0; i < sources.size(); ++i) {
		const int row = static_cast<int>(i);
		auto *name = new QTableWidgetItem(QString::fromStdString(sources[i].name));
		name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
		name->setData(Qt::UserRole, QString::fromStdString(sources[i].uuid));
		sources_->setItem(row, kSourceColumn, name);
		auto *status = new QTableWidgetItem;
		status->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		sources_->setItem(row, kStatusColumn, status);
	}
	updateSources(sources);
	sourceSettings_->setEnabled(sources_->currentRow() >= 0);
}

void TapeloopDock::updateSources(const std::vector<DockSource> &sources)
{
	const QSignalBlocker block(sources_);
	for (size_t i = 0; i < sources.size() && static_cast<int>(i) < sources_->rowCount(); ++i) {
		const int row = static_cast<int>(i);
		sources_->item(row, kSourceColumn)->setCheckState(sources[i].selected ? Qt::Checked : Qt::Unchecked);
		sources_->item(row, kStatusColumn)->setText(statusText(sources[i]));
	}
}

QString TapeloopDock::statusText(const DockSource &source) const
{
	if (!source.selected)
		return {};
	switch (source.state) {
	case SourceState::Running: {
		const double buffered = std::chrono::duration<double>(source.buffered).count();
		const double megabytes = static_cast<double>(source.bytes) / 1'000'000.0;
		return text_("Dock.Status.Running").arg(buffered, 0, 'f', 1).arg(megabytes, 0, 'f', 1);
	}
	case SourceState::Failed:
		return text_("Dock.Status.Failed");
	case SourceState::Stopped:
		break;
	}
	return text_("Dock.Status.Stopped");
}

void TapeloopDock::changeSettings(void (*change)(BufferSettings &, int), int value)
{
	guarded([&] {
		BufferSettings settings = backend_.settings();
		change(settings, value);
		backend_.setSettings(settings);
	});
	refresh();
}

void TapeloopDock::editSourceSettings(int row)
{
	if (row < 0 || row >= sources_->rowCount())
		return;
	guarded([&] {
		const std::string uuid =
			sources_->item(row, kSourceColumn)->data(Qt::UserRole).toString().toStdString();
		const BufferSettings settings = backend_.settings();
		const auto found = settings.sources.find(uuid);
		const SourceSettings current = found != settings.sources.end() ? found->second : SourceSettings{};
		SourceSettingsDialog dialog(sources_->item(row, kSourceColumn)->text(), current, settings, text_, this);
		if (dialog.exec() != QDialog::Accepted)
			return;
		BufferSettings changed = backend_.settings();
		changed.sources[uuid] = dialog.result();
		backend_.setSettings(changed);
	});
	refresh();
}

} // namespace tapeloop::ui
