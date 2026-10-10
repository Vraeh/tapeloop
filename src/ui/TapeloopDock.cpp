// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/TapeloopDock.hpp"

#include "ui/SourceSettingsDialog.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
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

// Qt goes past the rows that cannot be current on the arrow keys only, and ignores Home,
// End or a page key that stops on a heading or a damaged replay; such a move ends on the
// nearest row that can be current instead, back toward where it came from.
class ReplayList : public QListWidget {
public:
	using QListWidget::QListWidget;

protected:
	QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers modifiers) override
	{
		const QModelIndex target = QListWidget::moveCursor(action, modifiers);
		if (!target.isValid() || (target.flags() & Qt::ItemIsEnabled)) {
			return target;
		}
		const bool upward = action == MoveHome || action == MovePageUp || action == MoveUp ||
				    action == MoveLeft || action == MovePrevious;
		const int step = upward ? 1 : -1;
		for (int row = target.row() + step; row >= 0 && row < count(); row += step) {
			if (item(row)->flags() & Qt::ItemIsEnabled) {
				return model()->index(row, 0);
			}
		}
		return target;
	}
};

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
	  advanced_(new QCheckBox(text_("Dock.Advanced"), this)),
	  advancedSettings_(new QWidget(this)),
	  replayEncoder_(new QComboBox(this)),
	  otherAdapters_(new QCheckBox(text_("Dock.OtherAdapters"), this)),
	  note_(new QLabel(text_("Dock.ApplyNote"), this)),
	  startStop_(new QPushButton(this)),
	  followsOutputs_(new QLabel(text_("Dock.FollowsOutputs"), this)),
	  captureReplay_(new QPushButton(text_("Dock.CaptureReplay"), this)),
	  tagFilter_(new QComboBox(this)),
	  replays_(new ReplayList(this)),
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
	replays_->installEventFilter(this);
	replays_->viewport()->installEventFilter(this);
	// Moves without a button reach the filter only with tracking on.
	replays_->viewport()->setMouseTracking(true);

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
	advanced_->setObjectName("advanced");
	advanced_->setToolTip(text_("Dock.Advanced.Tooltip"));
	advancedSettings_->setObjectName("advancedSettings");
	advancedSettings_->hide();
	replayEncoder_->setObjectName("replayEncoder");
	replayEncoder_->setToolTip(text_("Dock.ReplayEncoder.Tooltip"));
	otherAdapters_->setObjectName("otherAdapters");
	otherAdapters_->setToolTip(text_("Dock.OtherAdapters.Tooltip"));
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
	form->addRow(advanced_);

	// The settings few need, shown by the switch above.
	auto *advanced = new QFormLayout(advancedSettings_);
	advanced->setContentsMargins(0, 0, 0, 0);
	advanced->addRow(text_("Dock.ReplayEncoder"), replayEncoder_);
	advanced->addRow(otherAdapters_);
	advanced->addRow(forceH264_);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(sources_, 1);
	layout->addWidget(sourceSettings_);
	layout->addLayout(form);
	layout->addWidget(advancedSettings_);
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
	connect(advanced_, &QCheckBox::toggled, this, [this](bool checked) {
		advancedSettings_->setVisible(checked);
		refresh();
	});
	connect(replayEncoder_, &QComboBox::currentIndexChanged, this, [this](int index) {
		guarded([&] {
			BufferSettings settings = backend_.settings();
			settings.replayEncoder = replayEncoder_->itemData(index).toString().toStdString();
			backend_.setSettings(settings);
		});
		refresh();
	});
	// The advanced settings follow the switch, before the controls after them.
	setTabOrder(advanced_, replayEncoder_);
	setTabOrder(replayEncoder_, otherAdapters_);
	setTabOrder(otherAdapters_, forceH264_);
	setTabOrder(forceH264_, startStop_);
	connect(otherAdapters_, &QCheckBox::toggled, this, [this](bool checked) {
		changeSettings([](BufferSettings &settings, int on) { settings.allowOtherAdapters = on != 0; },
			       checked ? 1 : 0);
	});
	connect(forceH264_, &QCheckBox::toggled, this, [this](bool checked) {
		changeSettings([](BufferSettings &settings, int on) { settings.forceH264 = on != 0; }, checked ? 1 : 0);
	});
	connect(captureReplay_, &QPushButton::clicked, this, [this] {
		guarded([this] { backend_.captureReplay(); });
		refresh();
	});
	connect(tagFilter_, &QComboBox::currentIndexChanged, this, [this] {
		// A filter chosen after a capture the list has not shown yet is the user's choice
		// over that capture.
		guarded([this] { lastCaptureSeen_ = std::max(lastCaptureSeen_, backend_.lastCapture()); });
		refresh();
	});
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
	// Moving through the list with the keyboard picks as a click does, and a click on the
	// row already current picks it again after a capture the list has not shown yet. The
	// list is refreshed once the view is done with the event, since a rebuild inside it
	// would leave the view selecting the row now under the pointer.
	const auto pick = [this](QListWidgetItem *item, bool clicked) {
		if (!item) {
			return;
		}
		guarded([&] {
			const uint64_t id = item->data(Qt::UserRole).toULongLong();
			const uint64_t current = backend_.currentReplay();
			// A replay captured during the press is the one to show next, over the click
			// that ends it.
			const bool capturedDuringPress = clicked && currentAtPress_ && current != *currentAtPress_;
			if (id != 0 && id != current && !capturedDuringPress) {
				backend_.pickReplay(id);
			}
		});
		if (clicked) {
			currentAtPress_.reset();
		}
		if (!refreshQueued_) {
			refreshQueued_ = true;
			QMetaObject::invokeMethod(
				this,
				[this] {
					refreshQueued_ = false;
					refresh();
				},
				Qt::QueuedConnection);
		}
	};
	connect(replays_, &QListWidget::currentItemChanged, this, [pick](QListWidgetItem *item) { pick(item, false); });
	connect(replays_, &QListWidget::itemClicked, this, [pick](QListWidgetItem *item) { pick(item, true); });
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
		const QSignalBlocker blockEncoder(replayEncoder_);
		const QSignalBlocker blockOtherAdapters(otherAdapters_);
		// A value being typed is not overwritten.
		if (!length_->hasFocus()) {
			length_->setValue(seconds(settings.length));
		}
		resolution_->setCurrentIndex(indexOfResolution(settings.resolution));
		startWithOutputs_->setChecked(settings.startWithOutputs);
		activateOffAir_->setChecked(settings.activateOffAir);
		forceH264_->setChecked(settings.forceH264);
		// A chosen encoder decides the codec itself.
		forceH264_->setEnabled(settings.replayEncoder.empty());
		otherAdapters_->setChecked(settings.allowOtherAdapters);
		// The encoders are looked up only while the advanced settings show them.
		if (advanced_->isChecked()) {
			updateEncoders(settings.replayEncoder);
		}

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
	// A dialog that opens during a press takes its release, and the pointer may never come
	// back over the list to end it.
	if (watched == replays_ && event->type() == QEvent::FocusOut) {
		leftButtonHeld_ = false;
	}
	if (watched == replays_->viewport()) {
		const QEvent::Type type = event->type();
		if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick ||
		    type == QEvent::MouseButtonRelease) {
			// Qt makes the row under any button current, and the list has no menu, so only
			// the left button picks.
			if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton) {
				return true;
			}
			leftButtonHeld_ = type != QEvent::MouseButtonRelease;
			// The second press of a double click ends in a click too.
			if (type != QEvent::MouseButtonRelease) {
				currentAtPress_.reset();
				guarded([this] { currentAtPress_ = backend_.currentReplay(); });
			}
			// The second press of a double click comes after the first click's refresh,
			// which can have put another replay under the pointer; the first click
			// picked already.
			if (type == QEvent::MouseButtonDblClick) {
				return true;
			}
		}
		if (type == QEvent::MouseMove) {
			if (static_cast<QMouseEvent *>(event)->buttons() & Qt::LeftButton) {
				// Dragging would make the row under the pointer current, which another one
				// is once Qt scrolls a row pressed at the edge into view.
				return true;
			}
			// A release that never came, as when a dialog opened during the press.
			leftButtonHeld_ = false;
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
		if (sources[i].selected && sources[i].hevcFailed) {
			note += (note.isEmpty() ? QString() : QStringLiteral("\n\n")) +
				text_(sources[i].state == SourceState::Running ? "Dock.Status.HevcFallback.Tooltip"
									       : "Dock.Status.HevcFailed.Tooltip");
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
	// A replay just captured is the one to show next and carries no tag yet, so a filter
	// would hide it; unless a replay was picked since, which is the one to show then.
	const uint64_t current = backend_.currentReplay();
	const uint64_t lastCapture = backend_.lastCapture();
	if (lastCapture > lastCaptureSeen_) {
		lastCaptureSeen_ = lastCapture;
		if (current == lastCapture) {
			const QSignalBlocker block(tagFilter_);
			tagFilter_->setCurrentIndex(0);
		}
	}

	const std::vector<DockReplay> replays = backend_.replays(tagFilter_->currentData().toString().toStdString());
	// A row per replay, after a row naming its broadcast; those rows hold id 0.
	std::vector<uint64_t> ids;
	std::vector<QString> texts;
	std::vector<QString> tips;
	std::vector<bool> damaged;
	for (size_t i = 0; i < replays.size(); ++i) {
		const DockReplay &replay = replays[i];
		if (!replay.broadcast.empty() && (i == 0 || replays[i - 1].broadcast != replay.broadcast)) {
			ids.push_back(0);
			texts.push_back(QString::fromStdString(replay.broadcast));
			tips.emplace_back();
			damaged.push_back(false);
		}
		const QDateTime captured = QDateTime::fromMSecsSinceEpoch(
			std::chrono::duration_cast<std::chrono::milliseconds>(replay.capturedAt.time_since_epoch())
				.count());
		const QString time = captured.toString(QStringLiteral("HH:mm:ss"));
		const auto sources = static_cast<qulonglong>(replay.sources);
		QString text;
		QString tip;
		switch (replay.state) {
		case ReplayState::Writing:
			text = text_("Dock.Replay.Saving").arg(time).arg(sources);
			break;
		case ReplayState::Stored:
			text = text_("Dock.Replay").arg(time).arg(sources);
			break;
		case ReplayState::NotSaved:
			text = text_("Dock.Replay.NotSaved").arg(time).arg(sources);
			tip = text_("Dock.Replay.NotSaved.Tooltip");
			break;
		case ReplayState::Damaged:
			text = text_("Dock.Replay.Damaged").arg(QString::fromStdString(replay.fileName));
			tip = text_("Dock.Replay.Damaged.Tooltip");
			break;
		}
		for (const std::string &tag : replay.tags) {
			text += QStringLiteral(" #") + QString::fromStdString(tag);
		}
		ids.push_back(replay.id);
		texts.push_back(std::move(text));
		tips.push_back(std::move(tip));
		damaged.push_back(replay.state == ReplayState::Damaged);
	}
	// Never while the left button is held on the list: moving the pointer then makes the
	// row under it current, which after a rebuild can be another replay.
	if (!leftButtonHeld_ && (ids != shownReplays_ || texts != shownReplayTexts_ || tips != shownReplayTips_)) {
		const QSignalBlocker block(replays_);
		replays_->clear();
		for (size_t i = 0; i < ids.size(); ++i) {
			auto *item = new QListWidgetItem(texts[i], replays_);
			item->setData(Qt::UserRole, static_cast<qulonglong>(ids[i]));
			if (ids[i] == 0) {
				// A heading, which can be neither selected nor made current.
				item->setFlags(Qt::NoItemFlags);
				QFont font = item->font();
				font.setBold(true);
				item->setFont(font);
			}
			if (!tips[i].isEmpty()) {
				item->setToolTip(tips[i]);
			}
			// A damaged replay cannot be picked, and the arrow keys go past it as they go
			// past a heading.
			if (damaged[i]) {
				item->setFlags(Qt::NoItemFlags);
			}
		}
		shownReplays_ = std::move(ids);
		shownReplayTexts_ = std::move(texts);
		shownReplayTips_ = std::move(tips);
	}
	// The replay that goes on air next is the one selected, and the one a tag goes on,
	// which has to be in sight.
	const auto shown = current == 0 ? shownReplays_.end()
					: std::find(shownReplays_.begin(), shownReplays_.end(), current);
	const bool listed = shown != shownReplays_.end();
	const QSignalBlocker block(replays_);
	// Cleared through the view rather than with setCurrentRow(-1), so that the view keeps
	// no current row when it gains focus instead of making its first row current, which
	// would pick that replay.
	replays_->setCurrentIndex(listed ? replays_->model()->index(static_cast<int>(shown - shownReplays_.begin()), 0)
					 : QModelIndex());
	addTag_->setEnabled(listed);
	tagName_->setEnabled(listed);
}

void TapeloopDock::updateEncoders(const std::string &chosen)
{
	// Not while its list is open, which would move the user off the row they are on.
	if (replayEncoder_->view()->isVisible()) {
		return;
	}
	// OBS may load more encoders, so the list is checked each time and rebuilt when it
	// changes; a choice no longer offered stays, under its id, until another is picked.
	std::vector<std::pair<std::string, std::string>> encoders;
	for (EncoderChoice &choice : backend_.encoderChoices()) {
		encoders.emplace_back(std::move(choice.id), std::move(choice.name));
	}
	const bool offered = chosen.empty() || std::any_of(encoders.begin(), encoders.end(), [&](const auto &encoder) {
				     return encoder.first == chosen;
			     });
	if (!offered) {
		encoders.emplace_back(chosen, chosen);
	}
	if (encoders != shownEncoders_) {
		replayEncoder_->clear();
		replayEncoder_->addItem(text_("Dock.ReplayEncoder.Automatic"), QString());
		for (const auto &[id, name] : encoders) {
			replayEncoder_->addItem(QString::fromStdString(name), QString::fromStdString(id));
		}
		shownEncoders_ = std::move(encoders);
	}
	replayEncoder_->setCurrentIndex(replayEncoder_->findData(QString::fromStdString(chosen)));
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
