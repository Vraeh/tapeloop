// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "FakeBackend.hpp"
#include "LocaleText.hpp"

#include "ui/SourceSettingsDialog.hpp"
#include "ui/TapeloopDock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QPushButton>
#include <QListWidget>
#include <QSpinBox>
#include <QTableWidget>

#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>

using namespace std::chrono_literals;
using tapeloop::BufferSettings;
using tapeloop::ReplayResolution;
using tapeloop::ResolutionMode;
using tapeloop::SourceSettings;
using tapeloop::test::FakeBackend;
using tapeloop::test::localeText;
using tapeloop::ui::DockSource;
using tapeloop::ui::SourceState;
using tapeloop::ui::TapeloopDock;

namespace {

FakeBackend backendWithSources()
{
	FakeBackend backend;
	backend.shown = {
		{"uuid-camera-1", "Camera 1", false, SourceState::Stopped, 0s, 0},
		{"uuid-camera-2", "Camera 2", false, SourceState::Stopped, 0s, 0},
		{"uuid-scoreboard", "Scoreboard", false, SourceState::Stopped, 0s, 0},
	};
	return backend;
}

// The dialog has no Q_OBJECT, so it is found as a QDialog.
tapeloop::ui::SourceSettingsDialog *sourceDialog(const QWidget &dock)
{
	return static_cast<tapeloop::ui::SourceSettingsDialog *>(dock.findChild<QDialog *>("sourceSettingsDialog"));
}

template<typename Widget> Widget *child(const QWidget &parent, const char *name)
{
	auto *widget = parent.findChild<Widget *>(name);
	REQUIRE(widget);
	return widget;
}

} // namespace

TEST_CASE("the dock lists every source with its selection")
{
	FakeBackend backend = backendWithSources();
	backend.current.sources["uuid-camera-2"].selected = true;
	TapeloopDock dock(backend, localeText());

	auto *table = child<QTableWidget>(dock, "sources");
	REQUIRE(table->rowCount() == 3);
	CHECK(table->item(0, 0)->text() == "Camera 1");
	CHECK(table->item(0, 0)->checkState() == Qt::Unchecked);
	CHECK(table->item(1, 0)->checkState() == Qt::Checked);
	CHECK(table->item(2, 0)->text() == "Scoreboard");
}

TEST_CASE("checking a source in the dock selects it")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");

	table->item(0, 0)->setCheckState(Qt::Checked);
	CHECK(backend.current.sources.at("uuid-camera-1").selected);
	table->item(0, 0)->setCheckState(Qt::Unchecked);
	CHECK_FALSE(backend.current.sources.at("uuid-camera-1").selected);
}

TEST_CASE("selected sources show the state of their buffer")
{
	FakeBackend backend = backendWithSources();
	backend.shown[0].state = SourceState::Running;
	backend.shown[0].buffered = 42'500ms;
	backend.shown[0].bytes = 50'000'000;
	backend.shown[1].state = SourceState::Failed;
	backend.current.sources["uuid-camera-1"].selected = true;
	backend.current.sources["uuid-camera-2"].selected = true;
	backend.current.sources["uuid-scoreboard"].selected = true;
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");

	CHECK(table->item(0, 1)->text() == "42.5 s, 50.0 MB");
	CHECK(table->item(1, 1)->text() == "Encoder failed");
	CHECK(table->item(2, 1)->text() == "Stopped");

	backend.current.sources["uuid-scoreboard"].selected = false;
	dock.refresh();
	CHECK(table->item(2, 1)->text().isEmpty());
}

TEST_CASE("the advanced settings choose the replay encoder and whether other cards may encode")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	dock.show();
	auto *advanced = child<QCheckBox>(dock, "advanced");
	auto *settings = child<QWidget>(dock, "advancedSettings");
	CHECK_FALSE(advanced->isChecked());
	CHECK_FALSE(settings->isVisible());
	advanced->setChecked(true);
	CHECK(settings->isVisible());
	// Showing them is not a change of the settings.
	CHECK(backend.settingsChanges == 0);

	auto *encoder = child<QComboBox>(dock, "replayEncoder");
	REQUIRE(encoder->count() == 3);
	CHECK(encoder->currentIndex() == 0);
	CHECK(encoder->itemData(0).toString().isEmpty());
	CHECK(encoder->itemText(1) == "NVIDIA NVENC HEVC");
	encoder->setCurrentIndex(2);
	CHECK(backend.current.replayEncoder == "obs_x264");
	CHECK_FALSE(child<QCheckBox>(dock, "forceH264")->isEnabled());
	// Another offered encoder in the settings, as after a scene collection switch.
	backend.current.replayEncoder = "obs_nvenc_hevc_tex";
	dock.refresh();
	CHECK(encoder->currentData().toString() == "obs_nvenc_hevc_tex");
	encoder->setCurrentIndex(0);
	CHECK(backend.current.replayEncoder.empty());

	auto *otherAdapters = child<QCheckBox>(dock, "otherAdapters");
	CHECK(otherAdapters->isChecked());
	otherAdapters->setChecked(false);
	CHECK_FALSE(backend.current.allowOtherAdapters);

	// A choice that is no longer offered stays shown until another is picked, and keeping
	// replays in H.264 waits while an encoder is chosen.
	auto *forceH264 = child<QCheckBox>(dock, "forceH264");
	CHECK(forceH264->isEnabled());
	backend.current.replayEncoder = "obs_qsv11_v2";
	dock.refresh();
	CHECK(encoder->currentData().toString() == "obs_qsv11_v2");
	CHECK(encoder->count() == 4);
	CHECK(forceH264->parentWidget() == settings);
	CHECK_FALSE(forceH264->isEnabled());

	// The list follows what OBS offers.
	backend.choices.push_back({"obs_qsv11_v2", "QuickSync H.264"});
	dock.refresh();
	CHECK(encoder->count() == 4);
	CHECK(encoder->currentText() == "QuickSync H.264");

	// Tab goes through the advanced settings in the order they show.
	const auto nextFocus = [](QWidget *from) {
		QWidget *next = from->nextInFocusChain();
		while (next != from && (!(next->focusPolicy() & Qt::TabFocus) || next->objectName().isEmpty())) {
			next = next->nextInFocusChain();
		}
		return next->objectName();
	};
	CHECK(nextFocus(advanced) == "replayEncoder");
	CHECK(nextFocus(encoder) == "otherAdapters");
	CHECK(nextFocus(otherAdapters) == "forceH264");
	CHECK(nextFocus(forceH264) == "startStop");
}

TEST_CASE("the encoder list holds still while it is open")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	dock.show();
	child<QCheckBox>(dock, "advanced")->setChecked(true);
	auto *encoder = child<QComboBox>(dock, "replayEncoder");
	encoder->showPopup();
	REQUIRE(encoder->view()->isVisible());
	encoder->view()->setCurrentIndex(encoder->model()->index(2, 0));
	backend.choices.push_back({"obs_qsv11_v2", "QuickSync H.264"});
	dock.refresh();
	CHECK(encoder->view()->currentIndex().row() == 2);
	CHECK(encoder->count() == 3);
	encoder->hidePopup();
	dock.refresh();
	CHECK(encoder->count() == 4);
}

TEST_CASE("a source on an encoder path that is not the optimal one says so")
{
	FakeBackend backend = backendWithSources();
	backend.shown[0].state = SourceState::Running;
	backend.shown[0].encoderPath = tapeloop::EncoderPath::Software;
	backend.shown[1].state = SourceState::Running;
	backend.shown[1].encoderPath = tapeloop::EncoderPath::Readback;
	backend.shown[2].state = SourceState::Running;
	for (const auto &source : backend.shown) {
		backend.current.sources[source.uuid].selected = true;
	}
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");
	CHECK_FALSE(table->item(0, 1)->icon().isNull());
	CHECK(table->item(0, 1)->toolTip().contains("x264"));
	CHECK_FALSE(table->item(1, 1)->icon().isNull());
	CHECK(table->item(1, 1)->toolTip().contains("read back through memory"));
	CHECK(table->item(2, 1)->icon().isNull());
	CHECK(table->item(2, 1)->toolTip().isEmpty());

	// With another note, both show, a paragraph each.
	backend.shown[0].activationLeftOut = true;
	dock.refresh();
	CHECK(table->item(0, 1)->toolTip().contains("restarts when it becomes active"));
	CHECK(table->item(0, 1)->toolTip().contains("x264"));
	CHECK(table->item(0, 1)->toolTip().contains("\n\n"));

	// A buffer that is stopped, failed or waiting has no encoder at work to speak of, and
	// an unselected source no buffer.
	for (const SourceState state : {SourceState::Stopped, SourceState::Failed, SourceState::Waiting}) {
		backend.shown[1].state = state;
		dock.refresh();
		CHECK_FALSE(table->item(1, 1)->toolTip().contains("read back through memory"));
	}
	backend.shown[1].state = SourceState::Running;
	backend.current.sources[backend.shown[1].uuid].selected = false;
	dock.refresh();
	CHECK(table->item(1, 1)->toolTip().isEmpty());
}

TEST_CASE("the dock captures replays and picks the one that goes on air")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *capture = child<QPushButton>(dock, "captureReplay");
	auto *list = child<QListWidget>(dock, "replays");
	CHECK(list->count() == 0);

	capture->click();
	capture->click();
	REQUIRE(backend.captures == 2);
	REQUIRE(list->count() == 2);
	// Newest first, and the newest goes on air next.
	CHECK(list->item(0)->data(Qt::UserRole).toULongLong() == 2u);
	CHECK(list->item(0)->text().contains("2 sources"));
	CHECK(list->currentRow() == 0);

	// Picking an older one makes it the one on air next, and selects it.
	Q_EMIT list->itemClicked(list->item(1));
	CHECK(backend.picked == 1u);
	dock.refresh();
	CHECK(list->currentRow() == 1);
	// A tag goes on the selected replay, and the list can show only the replays with it.
	auto *name = child<QLineEdit>(dock, "tagName");
	name->setText("goal");
	child<QPushButton>(dock, "addTag")->click();
	CHECK(name->text().isEmpty());
	REQUIRE(list->count() == 2);
	CHECK(list->item(1)->text().endsWith("#goal"));
	CHECK_FALSE(list->item(0)->text().contains("#goal"));
	auto *filter = child<QComboBox>(dock, "tagFilter");
	REQUIRE(filter->count() == 2);
	filter->setCurrentIndex(1);
	REQUIRE(list->count() == 1);
	CHECK(list->item(0)->data(Qt::UserRole).toULongLong() == 1u);
	filter->setCurrentIndex(0);
	CHECK(list->count() == 2);
	// A blank name tags nothing and stays to be fixed.
	name->setText(" ");
	Q_EMIT name->returnPressed();
	CHECK(name->text() == " ");

	// Buffers and settings are left alone.
	CHECK(backend.settingsChanges == 0);
	CHECK(backend.toggles == 0);
}

TEST_CASE("a media source left out of activation says why")
{
	FakeBackend backend = backendWithSources();
	backend.shown[0].state = SourceState::Waiting;
	backend.shown[0].activationLeftOut = true;
	backend.current.sources["uuid-camera-1"].selected = true;
	TapeloopDock dock(backend, localeText());
	const QTableWidgetItem *status = child<QTableWidget>(dock, "sources")->item(0, 1);
	CHECK_FALSE(status->icon().isNull());
	CHECK(status->toolTip().contains("restarts when it becomes active"));

	// Unselected, it has nothing to explain.
	backend.current.sources["uuid-camera-1"].selected = false;
	dock.refresh();
	const QTableWidgetItem *refreshed = child<QTableWidget>(dock, "sources")->item(0, 1);
	CHECK(refreshed->icon().isNull());
	CHECK(refreshed->toolTip().isEmpty());
}

TEST_CASE("the activation checkbox follows the settings")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *activate = child<QCheckBox>(dock, "activateOffAir");
	CHECK_FALSE(activate->isChecked());
	backend.current.activateOffAir = true;
	dock.refresh();
	CHECK(activate->isChecked());
	CHECK(activate->toolTip().contains("monitor"));
	CHECK(backend.settingsChanges == 0);
}

TEST_CASE("a source waiting for a picture says so with an icon and a tooltip")
{
	FakeBackend backend = backendWithSources();
	backend.shown[0].state = SourceState::Waiting;
	backend.current.sources["uuid-camera-1"].selected = true;
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");

	const QTableWidgetItem *status = table->item(0, 1);
	CHECK(status->text() == "Waiting for a picture");
	CHECK_FALSE(status->icon().isNull());
	CHECK(status->toolTip().contains("starts its buffer as soon as it has one"));

	backend.shown[0].state = SourceState::Running;
	dock.refresh();
	CHECK(table->item(0, 1)->icon().isNull());
	CHECK(table->item(0, 1)->toolTip().isEmpty());
}

TEST_CASE("the dock picks up sources that come and go")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");

	backend.shown.pop_back();
	dock.refresh();
	CHECK(table->rowCount() == 2);
	backend.shown.push_back({"uuid-replay", "Replay", false, SourceState::Stopped, 0s, 0});
	dock.refresh();
	REQUIRE(table->rowCount() == 3);
	CHECK(table->item(2, 0)->text() == "Replay");
}

TEST_CASE("the start and stop button follows the buffer lifecycle")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *button = child<QPushButton>(dock, "startStop");

	CHECK(button->text() == "Start buffers");
	CHECK(button->isEnabled());
	button->click();
	CHECK(backend.toggles == 1);
	CHECK(button->text() == "Stop buffers");

	backend.manualEnabled = false;
	dock.refresh();
	CHECK_FALSE(button->isEnabled());
	CHECK_FALSE(button->toolTip().isEmpty());
}

TEST_CASE("the global settings in the dock reach the backend")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());

	auto *length = child<QSpinBox>(dock, "length");
	CHECK(length->value() == 60);
	CHECK(length->minimum() == 10);
	CHECK(length->maximum() == 300);
	length->setValue(90);
	CHECK(backend.current.length == 90s);

	auto *resolution = child<QComboBox>(dock, "resolution");
	REQUIRE(resolution->count() == 7);
	resolution->setCurrentIndex(4);
	CHECK(backend.current.resolution == ReplayResolution{ResolutionMode::Fixed, 720});
	resolution->setCurrentIndex(1);
	CHECK(backend.current.resolution.mode == ResolutionMode::Output);

	auto *startWithOutputs = child<QCheckBox>(dock, "startWithOutputs");
	CHECK(startWithOutputs->isChecked());
	startWithOutputs->setChecked(false);
	CHECK_FALSE(backend.current.startWithOutputs);

	auto *activate = child<QCheckBox>(dock, "activateOffAir");
	CHECK_FALSE(activate->isChecked());
	CHECK_FALSE(activate->toolTip().isEmpty());
	activate->setChecked(true);
	CHECK(backend.current.activateOffAir);
	auto *forceH264 = child<QCheckBox>(dock, "forceH264");
	CHECK_FALSE(forceH264->isChecked());
	CHECK_FALSE(forceH264->toolTip().isEmpty());
	forceH264->setChecked(true);
	CHECK(backend.current.forceH264);
}

TEST_CASE("the source settings dialog sets and clears a source's own settings")
{
	BufferSettings global;
	global.length = 90s;
	SourceSettings current;
	current.selected = true;
	tapeloop::ui::SourceSettingsDialog dialog("Camera 1", current, global, localeText());

	CHECK(dialog.windowTitle() == "Settings for Camera 1");
	auto *length = child<QSpinBox>(dialog, "length");
	CHECK(length->value() == 90);
	CHECK_FALSE(length->isEnabled());
	child<QCheckBox>(dialog, "ownLength")->setChecked(true);
	CHECK(length->isEnabled());
	length->setValue(30);
	child<QCheckBox>(dialog, "ownResolution")->setChecked(true);
	child<QComboBox>(dialog, "resolution")->setCurrentIndex(6);

	auto *activate = child<QCheckBox>(dialog, "activate");
	CHECK_FALSE(activate->isEnabled());
	child<QCheckBox>(dialog, "ownActivation")->setChecked(true);
	CHECK(activate->isEnabled());
	activate->setChecked(true);

	SourceSettings result = dialog.result();
	CHECK(result.length == 30s);
	CHECK(result.resolution == ReplayResolution{ResolutionMode::Fixed, 2160});
	CHECK(result.activateOffAir == true);

	child<QCheckBox>(dialog, "ownLength")->setChecked(false);
	child<QCheckBox>(dialog, "ownResolution")->setChecked(false);
	child<QCheckBox>(dialog, "ownActivation")->setChecked(false);
	result = dialog.result();
	CHECK_FALSE(result.length);
	CHECK_FALSE(result.resolution);
	CHECK_FALSE(result.activateOffAir);
}

TEST_CASE("every control of the dock can be reached with the keyboard")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	size_t controls = 0;
	for (QWidget *widget : dock.findChildren<QWidget *>()) {
		const bool control = qobject_cast<QAbstractButton *>(widget) || qobject_cast<QSpinBox *>(widget) ||
				     qobject_cast<QComboBox *>(widget) || qobject_cast<QTableWidget *>(widget);
		// Qt's own parts, such as the table's corner button, have no name.
		if (!control || widget->objectName().isEmpty()) {
			continue;
		}
		++controls;
		CAPTURE(widget->objectName().toStdString());
		CHECK((widget->focusPolicy() & Qt::TabFocus) != 0);
	}
	CHECK(controls == 14);
}

TEST_CASE("every string the dock asks for is in the locale file")
{
	FakeBackend backend = backendWithSources();
	backend.shown[0].state = SourceState::Running;
	backend.shown[1].state = SourceState::Failed;
	backend.shown[2].state = SourceState::Waiting;
	for (const auto &source : backend.shown) {
		backend.current.sources[source.uuid].selected = true;
	}
	backend.manualEnabled = false;

	std::set<std::string> missing;
	TapeloopDock dock(backend, tapeloop::test::recordingLocaleText(missing));
	auto *table = child<QTableWidget>(dock, "sources");
	table->setCurrentCell(0, 0);
	child<QPushButton>(dock, "sourceSettings")->click();
	REQUIRE(sourceDialog(dock));
	backend.isRunning = true;
	dock.refresh();
	CHECK(missing.empty());

	// The plugin looks the dock's title up itself, when it adds the dock.
	CHECK(tapeloop::test::localeStrings().contains("Dock.Title"));
}

TEST_CASE("refreshing the dock changes nothing")
{
	FakeBackend backend = backendWithSources();
	backend.current.sources["uuid-camera-1"].selected = true;
	backend.current.length = 120s;
	backend.current.resolution = {ResolutionMode::Fixed, 480};
	backend.current.startWithOutputs = false;
	backend.current.forceH264 = true;
	TapeloopDock dock(backend, localeText());
	dock.refresh();
	dock.refresh();
	CHECK(backend.settingsChanges == 0);
	CHECK(child<QCheckBox>(dock, "forceH264")->isChecked());
	CHECK(child<QSpinBox>(dock, "length")->value() == 120);
	CHECK(child<QComboBox>(dock, "resolution")->currentIndex() == 3);
}

TEST_CASE("the dock's source settings change only that source's own settings")
{
	FakeBackend backend = backendWithSources();
	backend.current.sources["uuid-scoreboard"].selected = true;
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");
	auto *button = child<QPushButton>(dock, "sourceSettings");
	CHECK_FALSE(button->isEnabled());
	table->setCurrentCell(2, 0);
	REQUIRE(button->isEnabled());
	button->click();

	auto *dialog = sourceDialog(dock);
	REQUIRE(dialog);
	CHECK(dialog->windowTitle() == "Settings for Scoreboard");
	child<QCheckBox>(*dialog, "ownLength")->setChecked(true);
	child<QSpinBox>(*dialog, "length")->setValue(30);
	child<QCheckBox>(*dialog, "ownActivation")->setChecked(true);
	child<QCheckBox>(*dialog, "activate")->setChecked(true);
	// A change made elsewhere while the dialog is open is not undone.
	backend.current.sources["uuid-scoreboard"].selected = false;
	dialog->accept();

	const SourceSettings &scoreboard = backend.current.sources.at("uuid-scoreboard");
	CHECK(scoreboard.length == 30s);
	CHECK_FALSE(scoreboard.resolution);
	CHECK(scoreboard.activateOffAir == true);
	CHECK_FALSE(scoreboard.selected);
}

TEST_CASE("the dock drops a source's settings once the source is gone")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	child<QTableWidget>(dock, "sources")->setCurrentCell(1, 0);
	child<QPushButton>(dock, "sourceSettings")->click();
	auto *dialog = sourceDialog(dock);
	REQUIRE(dialog);
	child<QCheckBox>(*dialog, "ownLength")->setChecked(true);

	// Not refreshed in between, as when the dock is hidden.
	backend.shown.erase(backend.shown.begin() + 1);
	const int changes = backend.settingsChanges;
	dialog->accept();
	CHECK(backend.settingsChanges == changes);
	CHECK_FALSE(backend.current.sources.contains("uuid-camera-2"));
}

TEST_CASE("the current row of the dock follows its source")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");
	table->setCurrentCell(2, 0);

	backend.shown.insert(backend.shown.begin(), {"uuid-aerial", "Aerial", false, SourceState::Stopped, 0s, 0});
	dock.refresh();
	REQUIRE(table->currentRow() == 3);
	CHECK(table->item(table->currentRow(), 0)->text() == "Scoreboard");

	backend.shown.pop_back();
	dock.refresh();
	CHECK(table->currentRow() == -1);
	CHECK_FALSE(child<QPushButton>(dock, "sourceSettings")->isEnabled());
}

TEST_CASE("the source list works from the keyboard")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");
	table->setCurrentCell(0, 0);

	QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
	QApplication::sendEvent(table, &right);
	CHECK(table->currentColumn() == 0);

	QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ");
	QApplication::sendEvent(table, &space);
	CHECK(backend.current.sources["uuid-camera-1"].selected);

	QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(table, &enter);
	CHECK(sourceDialog(dock));
}

TEST_CASE("the dock says why the manual control is off")
{
	FakeBackend backend = backendWithSources();
	backend.manualEnabled = false;
	TapeloopDock dock(backend, localeText());
	dock.show();
	auto *reason = child<QLabel>(dock, "followsOutputs");
	CHECK(reason->isVisible());
	CHECK(reason->text() == "The buffers follow streaming and recording while either runs.");

	backend.manualEnabled = true;
	dock.refresh();
	CHECK_FALSE(reason->isVisible());
}

TEST_CASE("a length being typed in the dock is not overwritten")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	dock.show();
	dock.activateWindow();
	auto *length = child<QSpinBox>(dock, "length");
	CHECK_FALSE(length->keyboardTracking());
	length->setFocus();
	QApplication::processEvents();
	REQUIRE(length->hasFocus());

	length->setValue(200);
	backend.current.length = 45s;
	dock.refresh();
	CHECK(length->value() == 200);
}

// Renders the dock in a few states into TAPELOOP_SCREENSHOT_DIR, for review by eye.
TEST_CASE("dock screenshots", "[.screenshots]")
{
	const char *directory = std::getenv("TAPELOOP_SCREENSHOT_DIR");
	REQUIRE(directory);
	const QDir out(directory);

	FakeBackend empty;
	{
		TapeloopDock dock(empty, localeText());
		dock.resize(380, 520);
		CHECK(dock.grab().save(out.filePath("dock-empty.png")));
	}

	FakeBackend backend = backendWithSources();
	backend.current.sources["uuid-camera-1"].selected = true;
	backend.current.sources["uuid-scoreboard"].selected = true;
	{
		TapeloopDock dock(backend, localeText());
		dock.resize(380, 520);
		CHECK(dock.grab().save(out.filePath("dock-selected.png")));
	}

	backend.isRunning = true;
	backend.manualEnabled = false;
	backend.shown[0].state = SourceState::Running;
	backend.shown[0].buffered = 58'300ms;
	backend.shown[0].bytes = 214'000'000;
	backend.shown[2].state = SourceState::Running;
	backend.shown[2].buffered = 31'000ms;
	backend.shown[2].bytes = 57'000'000;
	{
		TapeloopDock dock(backend, localeText());
		dock.resize(380, 520);
		CHECK(dock.grab().save(out.filePath("dock-running.png")));
	}

	SourceSettings current;
	current.selected = true;
	current.length = 30s;
	tapeloop::ui::SourceSettingsDialog dialog("Camera 1", current, backend.current, localeText());
	dialog.adjustSize();
	CHECK(dialog.grab().save(out.filePath("dock-source-settings.png")));
}

TEST_CASE("every kind of edit in the dock writes the settings once")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *table = child<QTableWidget>(dock, "sources");

	const auto writesOnce = [&](const auto &edit) {
		const int before = backend.settingsChanges;
		edit();
		return backend.settingsChanges == before + 1;
	};
	CHECK(writesOnce([&] { table->item(0, 0)->setCheckState(Qt::Checked); }));
	CHECK(writesOnce([&] { child<QSpinBox>(dock, "length")->setValue(45); }));
	CHECK(writesOnce([&] { child<QComboBox>(dock, "resolution")->setCurrentIndex(4); }));
	CHECK(writesOnce([&] { child<QCheckBox>(dock, "startWithOutputs")->setChecked(false); }));
	CHECK(writesOnce([&] { child<QCheckBox>(dock, "activateOffAir")->setChecked(true); }));
	CHECK(writesOnce([&] { child<QCheckBox>(dock, "forceH264")->setChecked(true); }));
	child<QCheckBox>(dock, "advanced")->setChecked(true);
	CHECK(writesOnce([&] { child<QComboBox>(dock, "replayEncoder")->setCurrentIndex(1); }));
	CHECK(writesOnce([&] { child<QCheckBox>(dock, "otherAdapters")->setChecked(false); }));
	CHECK(writesOnce([&] {
		table->setCurrentCell(0, 0);
		child<QPushButton>(dock, "sourceSettings")->click();
		auto *dialog = sourceDialog(dock);
		child<QCheckBox>(*dialog, "ownLength")->setChecked(true);
		dialog->accept();
	}));
}

TEST_CASE("typing a length in the dock writes the settings once, when it is entered")
{
	FakeBackend backend = backendWithSources();
	TapeloopDock dock(backend, localeText());
	auto *length = child<QSpinBox>(dock, "length");
	const int before = backend.settingsChanges;

	// Each keystroke would otherwise write, and save the scene collection, once.
	length->selectAll();
	for (const char digit : {'1', '2', '0'}) {
		QKeyEvent key(QEvent::KeyPress, Qt::Key_0 + (digit - '0'), Qt::NoModifier, QString(QChar(digit)));
		QApplication::sendEvent(length, &key);
	}
	CHECK(backend.settingsChanges == before);
	QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	QApplication::sendEvent(length, &enter);
	CHECK(length->value() == 120);
	CHECK(backend.settingsChanges == before + 1);
}
