// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/TapeloopDock.hpp"

#include "ui/SourceSettingsDialog.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
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
	  activateOffAir_(new QCheckBox(text_("Dock.ActivateOffAir"), this)),
	  forceH264_(new QCheckBox(text_("Dock.ForceH264"), this)),
	  note_(new QLabel(text_("Dock.ApplyNote"), this)),
	  startStop_(new QPushButton(this)),
	  followsOutputs_(new QLabel(text_("Dock.FollowsOutputs"), this)),
	  captureReplay_(new QPushButton(text_("Dock.CaptureReplay"), this)),
	  tagFilter_(new QComboBox(this)),
	  replays_(new QListWidget(this)),
	  tagName_(new QLineEdit(this)),
	  addTag_(new QPushButton(text_("Dock.AddTag"), this))
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
	sources_->installEventFilter(this);

	sourceSettings_->setObjectName("sourceSettings");
	sourceSettings_->setEnabled(false);
	length_->setObjectName("length");
	length_->setRange(seconds(kMinBufferLength), seconds(kMaxBufferLength));
	length_->setSuffix(text_("Dock.SecondsSuffix"));
	length_->setKeyboardTracking(false);
	resolution_->setObjectName("resolution");
	addResolutions(*resolution_, text_);
	startWithOutputs_->setObjectName("startWithOutputs");
	activateOffAir_->setObjectName("activateOffAir");
	activateOffAir_->setToolTip(text_("Dock.ActivateOffAir.Tooltip"));
	forceH264_->setObjectName("forceH264");
	forceH264_->setToolTip(text_("Dock.ForceH264.Tooltip"));
	note_->setObjectName("note");
	note_->setWordWrap(true);
	startStop_->setObjectName("startStop");
	captureReplay_->setObjectName("captureReplay");
	captureReplay_->setToolTip(text_("Dock.CaptureReplay.Tooltip"));
	replays_->setObjectName("replays");
	replays_->setToolTip(text_("Dock.Replays.Tooltip"));
	replays_->setSelectionMode(QAbstractItemView::SingleSelection);
	tagFilter_->setObjectName("tagFilter");
	tagFilter_->setToolTip(text_("Dock.TagFilter.Tooltip"));
	tagFilter_->addItem(text_("Dock.TagFilter.All"), QString());
	tagName_->setObjectName("tagName");
	tagName_->setPlaceholderText(text_("Dock.TagName"));
	addTag_->setObjectName("addTag");
	addTag_->setToolTip(text_("Dock.AddTag.Tooltip"));
	followsOutputs_->setObjectName("followsOutputs");
	followsOutputs_->setWordWrap(true);
	followsOutputs_->hide();

	auto *form = new QFormLayout;
	form->addRow(text_("Dock.Length"), length_);
	form->addRow(text_("Dock.Resolution"), resolution_);
	form->addRow(startWithOutputs_);
	form->addRow(activateOffAir_);
	form->addRow(forceH264_);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(sources_, 1);
	layout->addWidget(sourceSettings_);
	layout->addLayout(form);
	layout->addWidget(note_);
	layout->addWidget(startStop_);
	layout->addWidget(followsOutputs_);
	layout->addWidget(captureReplay_);
	layout->addWidget(tagFilter_);
	layout->addWidget(replays_, 1);
	auto *tagging = new QHBoxLayout;
	tagging->addWidget(tagName_, 1);
	tagging->addWidget(addTag_);
	layout->addLayout(tagging);

	connect(sources_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
		if (item->column() != kSourceColumn) {
			return;
		}
		guarded([&] {
			BufferSettings settings = backend_.settings();
			settings.sources[item->data(Qt::UserRole).toString().toStdString()].selected =
				item->checkState() == Qt::Checked;
			backend_.setSettings(settings);
		});
		refresh();
	});
	// The current cell stays on the checkbox, so that Space always toggles it.
	connect(sources_, &QTableWidget::currentCellChanged, this, [this](int row, int column) {
		sourceSettings_->setEnabled(row >= 0);
		if (row >= 0 && column != kSourceColumn) {
			sources_->setCurrentCell(row, kSourceColumn);
		}
	});
	connect(sourceSettings_, &QPushButton::clicked, this, [this] { openSourceSettings(sources_->currentRow()); });
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
	connect(activateOffAir_, &QCheckBox::toggled, this, [this](bool checked) {
		changeSettings([](BufferSettings &settings, int on) { settings.activateOffAir = on != 0; },
			       checked ? 1 : 0);
	});
	connect(forceH264_, &QCheckBox::toggled, this, [this](bool checked) {
		changeSettings([](BufferSettings &settings, int on) { settings.forceH264 = on != 0; }, checked ? 1 : 0);
	});
	connect(captureReplay_, &QPushButton::clicked, this, [this] {
		guarded([this] { backend_.captureReplay(); });
		refresh();
	});
	connect(tagFilter_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
	const auto addTag = [this] {
		const std::string tag = tagName_->text().toStdString();
		bool added = false;
		guarded([&] { added = backend_.tagReplay(backend_.currentReplay(), tag); });
		if (added) {
			tagName_->clear();
		}
		refresh();
	};
	connect(addTag_, &QPushButton::clicked, this, addTag);
	connect(tagName_, &QLineEdit::returnPressed, this, addTag);
	connect(replays_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		guarded([&] { backend_.pickReplay(item->data(Qt::UserRole).toULongLong()); });
		refresh();
	});
	connect(startStop_, &QPushButton::clicked, this, [this] {
		guarded([this] { backend_.toggleRunning(); });
		refresh();
	});

	// OBS adds docks hidden; a hidden dock has nothing to show.
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] {
		if (isVisible()) {
			refresh();
		}
	});
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
		const QSignalBlocker blockActivate(activateOffAir_);
		const QSignalBlocker blockForceH264(forceH264_);
		// A value being typed is not overwritten.
		if (!length_->hasFocus()) {
			length_->setValue(seconds(settings.length));
		}
		resolution_->setCurrentIndex(indexOfResolution(settings.resolution));
		startWithOutputs_->setChecked(settings.startWithOutputs);
		activateOffAir_->setChecked(settings.activateOffAir);
		forceH264_->setChecked(settings.forceH264);

		updateReplays();

		startStop_->setText(backend_.running() ? text_("Dock.Stop") : text_("Dock.Start"));
		const bool enabled = backend_.manualControlEnabled();
		startStop_->setEnabled(enabled);
		startStop_->setToolTip(enabled ? QString() : text_("Dock.FollowsOutputs"));
		followsOutputs_->setVisible(!enabled);
	});
}

void TapeloopDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refresh();
}

bool TapeloopDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == sources_ && event->type() == QEvent::KeyPress) {
		const int key = static_cast<QKeyEvent *>(event)->key();
		if (key == Qt::Key_Return || key == Qt::Key_Enter) {
			openSourceSettings(sources_->currentRow());
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void TapeloopDock::rebuildSources(const std::vector<DockSource> &sources)
{
	const QSignalBlocker block(sources_);
	const int currentRow = sources_->currentRow();
	const std::string current = currentRow >= 0 && static_cast<size_t>(currentRow) < shownUuids_.size()
					    ? shownUuids_[static_cast<size_t>(currentRow)]
					    : std::string();

	sources_->setRowCount(static_cast<int>(sources.size()));
	int restored = -1;
	for (size_t i = 0; i < sources.size(); ++i) {
		const int row = static_cast<int>(i);
		auto *name = new QTableWidgetItem(QString::fromStdString(sources[i].name));
		name->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
		name->setData(Qt::UserRole, QString::fromStdString(sources[i].uuid));
		sources_->setItem(row, kSourceColumn, name);
		auto *status = new QTableWidgetItem;
		status->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
		sources_->setItem(row, kStatusColumn, status);
		if (!current.empty() && sources[i].uuid == current) {
			restored = row;
		}
	}
	updateSources(sources);

	// The current row follows its source, not its position.
	if (restored >= 0) {
		sources_->setCurrentCell(restored, kSourceColumn);
	} else {
		sources_->setCurrentCell(-1, -1);
	}
	sourceSettings_->setEnabled(restored >= 0);
}

void TapeloopDock::updateSources(const std::vector<DockSource> &sources)
{
	const QSignalBlocker block(sources_);
	for (size_t i = 0; i < sources.size() && static_cast<int>(i) < sources_->rowCount(); ++i) {
		const int row = static_cast<int>(i);
		sources_->item(row, kSourceColumn)->setCheckState(sources[i].selected ? Qt::Checked : Qt::Unchecked);
		QTableWidgetItem *status = sources_->item(row, kStatusColumn);
		status->setText(statusText(sources[i]));
		// Left out of activation explains waiting too, so it comes first.
		QString note;
		if (sources[i].selected && sources[i].activationLeftOut) {
			note = text_("Dock.Status.NotActivated.Tooltip");
		} else if (sources[i].selected && sources[i].state == SourceState::Waiting) {
			note = text_("Dock.Status.Waiting.Tooltip");
		}
		const char *path = nullptr;
		if (sources[i].selected && sources[i].state == SourceState::Running) {
			switch (sources[i].encoderPath) {
			case EncoderPath::Texture:
				break;
			case EncoderPath::Readback:
				path = "Dock.Status.Readback.Tooltip";
				break;
			case EncoderPath::Software:
				path = "Dock.Status.Software.Tooltip";
				break;
			}
		}
		if (path) {
			note += (note.isEmpty() ? QString() : QStringLiteral("\n\n")) + text_(path);
		}
		status->setIcon(note.isEmpty() ? QIcon() : style()->standardIcon(QStyle::SP_MessageBoxInformation));
		status->setToolTip(note);
	}
}

QString TapeloopDock::statusText(const DockSource &source) const
{
	if (!source.selected) {
		return {};
	}
	switch (source.state) {
	case SourceState::Running: {
		const double buffered = std::chrono::duration<double>(source.buffered).count();
		const double megabytes = static_cast<double>(source.bytes) / 1'000'000.0;
		return text_("Dock.Status.Running").arg(buffered, 0, 'f', 1).arg(megabytes, 0, 'f', 1);
	}
	case SourceState::Failed:
		return text_("Dock.Status.Failed");
	case SourceState::Waiting:
		return text_("Dock.Status.Waiting");
	case SourceState::Stopped:
		break;
	}
	return text_("Dock.Status.Stopped");
}

void TapeloopDock::updateReplays()
{
	// The filter offers every tag, rebuilt when they change and never while it is open.
	const std::vector<std::string> tags = backend_.replayTags();
	if (tags != shownTags_ && !tagFilter_->view()->isVisible()) {
		const QString chosen = tagFilter_->currentData().toString();
		const QSignalBlocker block(tagFilter_);
		tagFilter_->clear();
		tagFilter_->addItem(text_("Dock.TagFilter.All"), QString());
		for (const std::string &tag : tags) {
			tagFilter_->addItem(QString::fromStdString(tag), QString::fromStdString(tag));
		}
		tagFilter_->setCurrentIndex(std::max(tagFilter_->findData(chosen), 0));
		shownTags_ = tags;
	}

	const std::vector<DockReplay> replays = backend_.replays(tagFilter_->currentData().toString().toStdString());
	std::vector<uint64_t> ids;
	std::vector<QString> texts;
	for (const DockReplay &replay : replays) {
		const QDateTime captured = QDateTime::fromMSecsSinceEpoch(
			std::chrono::duration_cast<std::chrono::milliseconds>(replay.capturedAt.time_since_epoch())
				.count());
		QString text = text_("Dock.Replay")
				       .arg(captured.toString(QStringLiteral("HH:mm:ss")))
				       .arg(static_cast<qulonglong>(replay.sources));
		for (const std::string &tag : replay.tags) {
			text += QStringLiteral(" #") + QString::fromStdString(tag);
		}
		ids.push_back(replay.id);
		texts.push_back(std::move(text));
	}
	if (ids != shownReplays_ || texts != shownReplayTexts_) {
		const QSignalBlocker block(replays_);
		replays_->clear();
		for (size_t i = 0; i < ids.size(); ++i) {
			auto *item = new QListWidgetItem(texts[i], replays_);
			item->setData(Qt::UserRole, static_cast<qulonglong>(ids[i]));
		}
		shownReplays_ = std::move(ids);
		shownReplayTexts_ = std::move(texts);
	}
	// The replay that goes on air next is the one selected, and the one a tag goes on.
	const uint64_t current = backend_.currentReplay();
	const auto shown = std::find(shownReplays_.begin(), shownReplays_.end(), current);
	const QSignalBlocker block(replays_);
	replays_->setCurrentRow(shown != shownReplays_.end() ? static_cast<int>(shown - shownReplays_.begin()) : -1);
	addTag_->setEnabled(current != 0);
	tagName_->setEnabled(current != 0);
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

void TapeloopDock::openSourceSettings(int row)
{
	if (row < 0 || row >= sources_->rowCount()) {
		return;
	}
	guarded([&] {
		const QTableWidgetItem *item = sources_->item(row, kSourceColumn);
		const std::string uuid = item->data(Qt::UserRole).toString().toStdString();
		const BufferSettings settings = backend_.settings();
		const auto found = settings.sources.find(uuid);
		const SourceSettings current = found != settings.sources.end() ? found->second : SourceSettings{};

		// Not run with exec(): its nested event loop could outlive the dock if OBS closed it
		// meanwhile. As a child, the dialog goes with the dock.
		auto *dialog = new SourceSettingsDialog(item->text(), current, settings, text_, this);
		dialog->setObjectName("sourceSettingsDialog");
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		connect(dialog, &QDialog::accepted, this,
			[this, dialog, uuid] { applySourceSettings(uuid, dialog->result()); });
		dialog->open();
	});
}

void TapeloopDock::applySourceSettings(const std::string &uuid, const SourceSettings &chosen)
{
	// The scene collection can change while the dialog is open, even while the dock is
	// hidden and not refreshing.
	refresh();
	if (std::find(shownUuids_.begin(), shownUuids_.end(), uuid) == shownUuids_.end()) {
		return;
	}
	guarded([&] {
		BufferSettings settings = backend_.settings();
		SourceSettings &source = settings.sources[uuid];
		source.length = chosen.length;
		source.resolution = chosen.resolution;
		source.activateOffAir = chosen.activateOffAir;
		backend_.setSettings(settings);
	});
	refresh();
}

} // namespace tapeloop::ui
