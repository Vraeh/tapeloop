// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "FakeBackend.hpp"
#include "LocaleText.hpp"

#include "ui/OverlayDialog.hpp"
#include "ui/TapeloopDock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>

#include <chrono>
#include <set>
#include <string>

using namespace std::chrono_literals;
using tapeloop::CanvasSize;
using tapeloop::OverlayMode;
using tapeloop::OverlayPreset;
using tapeloop::OverlayScene;
using tapeloop::test::FakeBackend;
using tapeloop::test::localeText;
using tapeloop::ui::DockSource;
using tapeloop::ui::OverlayDialog;
using tapeloop::ui::OverlaySource;
using tapeloop::ui::SourceState;
using tapeloop::ui::TapeloopDock;

namespace {

template<typename Widget> Widget *child(const QWidget &parent, const char *name)
{
	auto *widget = parent.findChild<Widget *>(name);
	REQUIRE(widget);
	return widget;
}

// Score at the bottom, then Logo, then Clock on top, all three sources OBS has.
void addOverlays(FakeBackend &backend)
{
	backend.overlayList = {{"uuid-clock", "Clock", false, 300, 100},
			       {"uuid-logo", "Logo", false, 400, 200},
			       {"uuid-score", "Score", false, 800, 120}};
	backend.current.overlays.items.clear();
	for (auto it = backend.overlayList.rbegin(); it != backend.overlayList.rend(); ++it) {
		backend.current.overlays.items.push_back(
			tapeloop::newOverlay(it->uuid, it->name, it->width, it->height, backend.canvasSize));
	}
}

QStringList rows(const QListWidget &list)
{
	QStringList names;
	for (int row = 0; row < list.count(); ++row) {
		names.append(list.item(row)->text());
	}
	return names;
}

void press(QWidget *widget, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString &text = {})
{
	QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
	QApplication::sendEvent(widget, &event);
}

QPushButton *dialogButton(const QWidget &dialog, QDialogButtonBox::StandardButton which)
{
	QPushButton *button = child<QDialogButtonBox>(dialog, "overlayButtons")->button(which);
	REQUIRE(button);
	return button;
}

OverlayDialog *openOverlays(TapeloopDock &dock)
{
	child<QPushButton>(dock, "overlaySettings")->click();
	// The dialog has no Q_OBJECT, so it is found as a QDialog.
	auto *dialog = static_cast<OverlayDialog *>(dock.findChild<QDialog *>("overlayDialog"));
	REQUIRE(dialog);
	return dialog;
}

QDialog *openPicker(const QWidget &dialog)
{
	child<QPushButton>(dialog, "addOverlay")->click();
	return child<QDialog>(dialog, "overlayPicker");
}

// Picks a row of the picker and closes it, which deletes it at once rather than later.
void pick(QDialog *picker, int row)
{
	child<QListWidget>(*picker, "overlayChoices")->setCurrentRow(row);
	picker->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
	delete picker;
}

} // namespace

TEST_CASE("the overlay dialog lists the overlays topmost first")
{
	FakeBackend backend;
	addOverlays(backend);
	backend.current.overlays.items[1].visible = false;
	OverlayDialog dialog(backend, localeText());

	auto *list = child<QListWidget>(dialog, "overlays");
	CHECK(rows(*list) == QStringList{"Clock", "Logo", "Score"});
	CHECK(list->item(0)->checkState() == Qt::Checked);
	CHECK(list->item(1)->checkState() == Qt::Unchecked);
	// The newest overlay is the one shown in the fields.
	CHECK(list->currentRow() == 0);
	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == 300);
	CHECK_FALSE(child<QPushButton>(dialog, "moveOverlayUp")->isEnabled());
	CHECK(child<QPushButton>(dialog, "moveOverlayDown")->isEnabled());
}

TEST_CASE("the overlay dialog marks an overlay whose source is gone")
{
	FakeBackend backend;
	addOverlays(backend);
	backend.overlayList.erase(backend.overlayList.begin());
	OverlayDialog dialog(backend, localeText());

	auto *list = child<QListWidget>(dialog, "overlays");
	CHECK(rows(*list) == QStringList{"Clock (not found)", "Logo", "Score"});
	auto *properties = child<QPushButton>(dialog, "overlayProperties");
	CHECK_FALSE(properties->isEnabled());

	list->setCurrentRow(1);
	CHECK(properties->isEnabled());
	properties->click();
	CHECK(backend.propertiesOpened == std::vector<std::string>{"uuid-logo"});
}

TEST_CASE("the overlay dialog writes its copy only on OK or Apply")
{
	FakeBackend backend;
	addOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	dialog.show();
	QPushButton *apply = dialogButton(dialog, QDialogButtonBox::Apply);
	CHECK_FALSE(apply->isEnabled());

	auto *opacity = child<QSpinBox>(dialog, "opacity");
	opacity->setValue(50);
	CHECK(child<QSlider>(dialog, "opacitySlider")->value() == 50);
	CHECK(dialog.editing().items().back().opacity == 50);
	CHECK(backend.settingsChanges == 0);
	CHECK(apply->isEnabled());

	apply->click();
	CHECK(backend.settingsChanges == 1);
	CHECK(backend.current.overlays.items.back().opacity == 50);
	CHECK_FALSE(apply->isEnabled());
	CHECK(dialog.isVisible());

	child<QSlider>(dialog, "opacitySlider")->setValue(20);
	CHECK(opacity->value() == 20);
	dialogButton(dialog, QDialogButtonBox::Cancel)->click();
	CHECK(backend.settingsChanges == 1);
	CHECK(backend.current.overlays.items.back().opacity == 50);
	CHECK_FALSE(dialog.isVisible());
}

TEST_CASE("OK writes the overlay dialog's copy once, and nothing when it is unchanged")
{
	FakeBackend backend;
	addOverlays(backend);
	{
		OverlayDialog dialog(backend, localeText());
		dialogButton(dialog, QDialogButtonBox::Ok)->click();
		CHECK(backend.settingsChanges == 0);
		CHECK(dialog.result() == QDialog::Accepted);
	}
	{
		OverlayDialog dialog(backend, localeText());
		child<QSpinBox>(dialog, "overlayX")->setValue(10);
		child<QSpinBox>(dialog, "overlayY")->setValue(20);
		dialogButton(dialog, QDialogButtonBox::Ok)->click();
		CHECK(backend.settingsChanges == 1);
		const tapeloop::OverlayBox box =
			tapeloop::boxOf(backend.current.overlays.items.back(), backend.current.overlays.canvas);
		CHECK(box.left == 10);
		CHECK(box.top == 20);
	}
}

TEST_CASE("overlays made on another canvas are shown on this one without a change to write")
{
	FakeBackend backend;
	backend.current.overlays.canvas = {1280, 720};
	backend.current.overlays.items = {tapeloop::newOverlay("uuid-logo", "Logo", 400, 200, {1280, 720})};
	backend.overlayList = {{"uuid-logo", "Logo", false, 400, 200}};
	OverlayDialog dialog(backend, localeText());

	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == 600);
	CHECK(child<QSpinBox>(dialog, "overlayHeight")->value() == 300);
	CHECK_FALSE(dialogButton(dialog, QDialogButtonBox::Apply)->isEnabled());
	dialogButton(dialog, QDialogButtonBox::Ok)->click();
	CHECK(backend.settingsChanges == 0);
}

TEST_CASE("overlays opened under another scene collection are dropped")
{
	FakeBackend backend;
	addOverlays(backend);
	SECTION("on Apply, which closes the dialog")
	{
		OverlayDialog dialog(backend, localeText());
		dialog.show();
		child<QSpinBox>(dialog, "opacity")->setValue(50);
		backend.current.overlays.items.pop_back();
		dialogButton(dialog, QDialogButtonBox::Apply)->click();
		CHECK(backend.settingsChanges == 0);
		CHECK(backend.current.overlays.items.size() == 2);
		CHECK_FALSE(dialog.isVisible());
		CHECK(dialog.result() == QDialog::Rejected);
	}
	SECTION("on OK")
	{
		OverlayDialog dialog(backend, localeText());
		child<QSpinBox>(dialog, "opacity")->setValue(50);
		backend.current.overlays.items.pop_back();
		dialogButton(dialog, QDialogButtonBox::Ok)->click();
		CHECK(backend.settingsChanges == 0);
		CHECK(backend.current.overlays.items.size() == 2);
	}
}

TEST_CASE("the overlay dialog writes the rest of the settings as they are now")
{
	FakeBackend backend;
	addOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	child<QSpinBox>(dialog, "opacity")->setValue(50);

	backend.current.length = 77s;
	dialogButton(dialog, QDialogButtonBox::Ok)->click();
	CHECK(backend.settingsChanges == 1);
	CHECK(backend.current.length == 77s);
	CHECK(backend.current.overlays.items.back().opacity == 50);
}

TEST_CASE("the overlay list works from the keyboard")
{
	FakeBackend backend;
	addOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	auto *list = child<QListWidget>(dialog, "overlays");
	REQUIRE(list->currentRow() == 0);

	press(list, Qt::Key_Space, Qt::NoModifier, " ");
	CHECK(list->item(0)->checkState() == Qt::Unchecked);
	CHECK_FALSE(dialog.editing().items()[2].visible);
	press(list, Qt::Key_Space, Qt::NoModifier, " ");
	CHECK(dialog.editing().items()[2].visible);

	press(list, Qt::Key_Down, Qt::AltModifier);
	CHECK(rows(*list) == QStringList{"Logo", "Clock", "Score"});
	CHECK(list->currentRow() == 1);
	press(list, Qt::Key_Down, Qt::AltModifier | Qt::KeypadModifier);
	CHECK(rows(*list) == QStringList{"Logo", "Score", "Clock"});
	press(list, Qt::Key_Down, Qt::AltModifier);
	CHECK(rows(*list) == QStringList{"Logo", "Score", "Clock"});
	press(list, Qt::Key_Up, Qt::AltModifier);
	CHECK(rows(*list) == QStringList{"Logo", "Clock", "Score"});
	CHECK(list->currentRow() == 1);
	CHECK(dialog.editing().items()[1].uuid == "uuid-clock");

	press(list, Qt::Key_Delete);
	CHECK(rows(*list) == QStringList{"Logo", "Score"});
	// The row below takes the place of the one removed.
	CHECK(list->currentRow() == 1);
	press(list, Qt::Key_Delete);
	CHECK(rows(*list) == QStringList{"Logo"});
	CHECK(list->currentRow() == 0);
	press(list, Qt::Key_Delete);
	CHECK(list->count() == 0);
	CHECK_FALSE(child<QWidget>(dialog, "overlayFields")->isEnabled());
	CHECK_FALSE(child<QPushButton>(dialog, "removeOverlay")->isEnabled());
	press(list, Qt::Key_Delete);

	CHECK(backend.settingsChanges == 0);
}

TEST_CASE("the overlay buttons move and remove the selected overlay")
{
	FakeBackend backend;
	addOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	auto *list = child<QListWidget>(dialog, "overlays");
	list->setCurrentRow(2);

	child<QPushButton>(dialog, "moveOverlayUp")->click();
	CHECK(rows(*list) == QStringList{"Clock", "Score", "Logo"});
	CHECK(list->currentRow() == 1);
	child<QPushButton>(dialog, "moveOverlayDown")->click();
	CHECK(rows(*list) == QStringList{"Clock", "Logo", "Score"});
	CHECK(list->currentRow() == 2);
	CHECK_FALSE(child<QPushButton>(dialog, "moveOverlayDown")->isEnabled());
	child<QPushButton>(dialog, "removeOverlay")->click();
	CHECK(rows(*list) == QStringList{"Clock", "Logo"});
	// At the bottom, the row above takes the selection.
	CHECK(list->currentRow() == 1);
}

TEST_CASE("the overlay picker adds a source or scene on top")
{
	FakeBackend backend;
	addOverlays(backend);
	backend.overlayList.push_back({"uuid-intro", "Intro", true, 0, 0});
	OverlayDialog dialog(backend, localeText());

	QDialog *picker = openPicker(dialog);
	auto *choices = child<QListWidget>(*picker, "overlayChoices");
	// Sources already shown can be added again, at another place or size.
	CHECK(rows(*choices) == QStringList{"Clock", "Logo", "Score", "Intro (scene)"});
	CHECK_FALSE(child<QLabel>(*picker, "overlayChoicesEmpty")->isVisibleTo(picker));
	pick(picker, 1);

	auto *list = child<QListWidget>(dialog, "overlays");
	CHECK(rows(*list) == QStringList{"Logo", "Clock", "Logo", "Score"});
	CHECK(list->currentRow() == 0);
	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == 400);
	CHECK(child<QSpinBox>(dialog, "overlayHeight")->value() == 200);

	pick(openPicker(dialog), 3);
	CHECK(rows(*list) == QStringList{"Intro", "Logo", "Clock", "Logo", "Score"});
	// A scene with no size yet starts at a quarter of the canvas.
	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == 480);
	CHECK(backend.settingsChanges == 0);
}

TEST_CASE("the overlay picker leaves out the scene replays go on air in")
{
	FakeBackend backend;
	backend.overlayList = {{"uuid-logo", "Logo", false, 400, 200},
			       {"uuid-replays", "Replays", true, 0, 0},
			       {"uuid-intro", "Intro", true, 0, 0}};
	backend.sceneList = {{"uuid-replays", "Replays"}, {"uuid-intro", "Intro"}};
	backend.current.overlays.mode = OverlayMode::Scene;
	backend.current.overlays.scene = {"uuid-replays", "Replays"};
	OverlayDialog dialog(backend, localeText());

	QDialog *picker = openPicker(dialog);
	CHECK(rows(*child<QListWidget>(*picker, "overlayChoices")) == QStringList{"Logo", "Intro (scene)"});
	delete picker;

	SECTION("and offers it once replays go on air with the overlays")
	{
		backend.current.showAdvanced = true;
		OverlayDialog advanced(backend, localeText());
		auto *scene = child<QComboBox>(advanced, "replayScene");
		scene->setCurrentIndex(0);
		picker = openPicker(advanced);
		CHECK(rows(*child<QListWidget>(*picker, "overlayChoices")) ==
		      QStringList{"Logo", "Replays (scene)", "Intro (scene)"});
	}
}

TEST_CASE("the overlay picker says when there is nothing to add")
{
	FakeBackend backend;
	OverlayDialog dialog(backend, localeText());
	QDialog *picker = openPicker(dialog);

	CHECK(child<QListWidget>(*picker, "overlayChoices")->count() == 0);
	CHECK(child<QLabel>(*picker, "overlayChoicesEmpty")->isVisibleTo(picker));
	CHECK_FALSE(picker->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
}

TEST_CASE("no overlay is added past the most there may be")
{
	FakeBackend backend;
	addOverlays(backend);
	while (backend.current.overlays.items.size() < tapeloop::kMaxOverlays) {
		backend.current.overlays.items.push_back(backend.current.overlays.items.front());
	}
	OverlayDialog dialog(backend, localeText());
	auto *add = child<QPushButton>(dialog, "addOverlay");
	CHECK_FALSE(add->isEnabled());
	CHECK(add->toolTip() == "No more overlays fit.");

	child<QPushButton>(dialog, "removeOverlay")->click();
	CHECK(add->isEnabled());
}

TEST_CASE("the overlay fields edit the selected overlay")
{
	FakeBackend backend;
	addOverlays(backend);
	OverlayDialog dialog(backend, localeText());
	child<QListWidget>(dialog, "overlays")->setCurrentRow(1);
	auto *preset = child<QComboBox>(dialog, "preset");
	CHECK(preset->currentText() == "Custom");

	preset->setCurrentIndex(preset->findData(static_cast<int>(OverlayPreset::FullScreen)));
	CHECK(child<QSpinBox>(dialog, "overlayX")->value() == 0);
	CHECK(child<QSpinBox>(dialog, "overlayY")->value() == 0);
	CHECK(child<QSpinBox>(dialog, "overlayWidth")->value() == 1920);
	CHECK(child<QSpinBox>(dialog, "overlayHeight")->value() == 1080);

	child<QSpinBox>(dialog, "overlayWidth")->setValue(1000);
	CHECK(preset->currentText() == "Custom");
	// Custom is what no preset gives, so picking it leaves the overlay as it is.
	preset->setCurrentIndex(preset->findData(static_cast<int>(OverlayPreset::FullScreen)));
	preset->setCurrentIndex(0);
	CHECK(preset->currentText() == "Full screen");

	child<QComboBox>(dialog, "anchor")->setCurrentIndex(8);
	CHECK(dialog.editing().items()[1].anchor == tapeloop::OverlayAnchor::BottomRight);
	CHECK(child<QSpinBox>(dialog, "overlayX")->value() == 0);
	child<QSpinBox>(dialog, "distanceX")->setValue(40);
	CHECK(child<QSpinBox>(dialog, "overlayX")->value() == -40);
	child<QSpinBox>(dialog, "cropTop")->setValue(12);
	child<QComboBox>(dialog, "fit")->setCurrentIndex(1);
	child<QComboBox>(dialog, "blend")->setCurrentIndex(4);
	const tapeloop::OverlayItem &item = dialog.editing().items()[1];
	CHECK(item.crop.top == 12);
	CHECK(item.fit == tapeloop::OverlayFit::Stretch);
	CHECK(item.blend == tapeloop::OverlayBlend::Multiply);
	CHECK(dialog.editing().items()[2].uuid == "uuid-clock");
	CHECK(dialog.editing().items()[2].opacity == 100);
}

TEST_CASE("the overlay dialog shows the advanced fields only with the advanced settings")
{
	FakeBackend backend;
	addOverlays(backend);
	TapeloopDock dock(backend, localeText());
	OverlayDialog *dialog = openOverlays(dock);
	CHECK(dialog->isVisible());
	CHECK(child<QWidget>(*dialog, "overlayFields")->isVisible());
	CHECK(child<QWidget>(*dialog, "overlayPreview")->isVisible());
	CHECK_FALSE(child<QWidget>(*dialog, "advancedFields")->isVisible());
	CHECK_FALSE(child<QWidget>(*dialog, "sceneBox")->isVisible());

	backend.current.showAdvanced = true;
	dock.refresh();
	CHECK(child<QWidget>(*dialog, "advancedFields")->isVisible());
	CHECK(child<QWidget>(*dialog, "sceneBox")->isVisible());

	backend.current.showAdvanced = false;
	dock.refresh();
	CHECK_FALSE(child<QWidget>(*dialog, "advancedFields")->isVisible());
	CHECK_FALSE(child<QWidget>(*dialog, "sceneBox")->isVisible());
}

TEST_CASE("the dock opens one overlay dialog at a time")
{
	FakeBackend backend;
	TapeloopDock dock(backend, localeText());
	const QPointer<OverlayDialog> dialog = openOverlays(dock);
	child<QPushButton>(dock, "overlaySettings")->click();
	CHECK(dock.findChildren<QDialog *>("overlayDialog").size() == 1);

	dialog->reject();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	CHECK(dialog.isNull());
	CHECK(openOverlays(dock) != nullptr);
}

TEST_CASE("the overlay dialog says when the replay scene will not show the replay")
{
	FakeBackend backend;
	backend.current.showAdvanced = true;
	backend.sceneList = {{"uuid-replays", "Replays"}, {"uuid-intro", "Intro"}};
	backend.sceneViews = {"uuid-intro"};
	backend.current.overlays.mode = OverlayMode::Scene;
	backend.current.overlays.scene = {"uuid-gone", "Old replays"};
	OverlayDialog dialog(backend, localeText());
	auto *scene = child<QComboBox>(dialog, "replayScene");
	auto *warning = child<QLabel>(dialog, "sceneWarning");

	CHECK(scene->currentText() == "Old replays (not found)");
	CHECK(warning->isVisibleTo(&dialog));
	CHECK(warning->text() == "The scene Old replays is not there, so replays go on air with the overlays above.");

	scene->setCurrentIndex(scene->findData(QString("uuid-replays")));
	CHECK(dialog.editing().settings().scene == OverlayScene{"uuid-replays", "Replays"});
	CHECK(warning->text() == "The scene Replays holds no Tapeloop Replay View, so the replay does not show in it.");

	scene->setCurrentIndex(scene->findData(QString("uuid-intro")));
	CHECK_FALSE(warning->isVisibleTo(&dialog));

	scene->setCurrentIndex(0);
	CHECK(dialog.editing().settings().mode == OverlayMode::Simple);
	CHECK(dialog.editing().settings().scene == OverlayScene{});
	CHECK_FALSE(warning->isVisibleTo(&dialog));

	scene->setCurrentIndex(scene->count() - 1);
	CHECK(dialog.editing().settings().mode == OverlayMode::Scene);
	CHECK(dialog.editing().settings().scene == OverlayScene{"uuid-gone", "Old replays"});
	CHECK_FALSE(dialogButton(dialog, QDialogButtonBox::Apply)->isEnabled());
}

TEST_CASE("without the advanced settings the overlay dialog says a scene of the user's goes on air")
{
	FakeBackend backend;
	backend.sceneList = {{"uuid-replays", "Replays"}};
	backend.current.overlays.mode = OverlayMode::Scene;
	backend.current.overlays.scene = {"uuid-replays", "Replays"};
	OverlayDialog dialog(backend, localeText());
	auto *note = child<QLabel>(dialog, "sceneNote");
	CHECK(note->isVisibleTo(&dialog));
	CHECK(note->text().contains("Replays"));

	dialog.showAdvanced(true);
	CHECK_FALSE(note->isVisibleTo(&dialog));
}

TEST_CASE("every control of the overlay dialog is reached with the keyboard in order")
{
	FakeBackend backend;
	backend.current.showAdvanced = true;
	const OverlayDialog dialog(backend, localeText());
	QStringList order;
	QWidget *widget = child<QListWidget>(dialog, "overlays");
	const QWidget *first = widget;
	do {
		if ((widget->focusPolicy() & Qt::TabFocus) != 0 && !widget->objectName().isEmpty() &&
		    !widget->objectName().startsWith("qt_")) {
			order.append(widget->objectName());
		}
		widget = widget->nextInFocusChain();
	} while (widget != first);
	CHECK(order ==
	      QStringList{"overlays",          "addOverlay",    "removeOverlay", "moveOverlayUp", "moveOverlayDown",
			  "overlayProperties", "preset",        "overlayX",      "overlayY",      "overlayWidth",
			  "overlayHeight",     "opacitySlider", "opacity",       "anchor",        "distanceX",
			  "distanceY",         "fit",           "cropLeft",      "cropTop",       "cropRight",
			  "cropBottom",        "blend",         "replayScene"});
}

TEST_CASE("the dock's stats show each selected buffer with the advanced settings")
{
	FakeBackend backend;
	backend.shown = {{"uuid-camera-1", "Camera 1", false, SourceState::Running, 30s, 120'000'000},
			 {"uuid-camera-2", "Camera 2", false, SourceState::Stopped, 0s, 0},
			 {"uuid-scoreboard", "Scoreboard", false, SourceState::Waiting, 0s, 0}};
	backend.shown[0].totalFrames = 3000;
	backend.shown[0].skippedFrames = 30;
	backend.current.sources["uuid-camera-1"].selected = true;
	backend.current.sources["uuid-scoreboard"].selected = true;
	backend.isRunning = true;
	TapeloopDock dock(backend, localeText());
	auto *stats = child<QTableWidget>(dock, "stats");
	CHECK_FALSE(stats->isVisibleTo(&dock));

	backend.current.showAdvanced = true;
	dock.refresh();
	CHECK(stats->isVisibleTo(&dock));
	REQUIRE(stats->rowCount() == 2);
	CHECK(stats->item(0, 0)->text() == "Camera 1");
	CHECK(stats->item(0, 1)->text() == "120.0 MB");
	CHECK(stats->item(0, 2)->text() == "Not reported");
	CHECK(stats->item(0, 3)->text() == "30 of 3000 (1.0%)");
	CHECK(stats->item(1, 0)->text() == "Scoreboard");
	CHECK(stats->item(1, 1)->text() == "-");
	CHECK(stats->item(1, 3)->text() == "-");

	backend.current.sources["uuid-camera-1"].selected = false;
	dock.refresh();
	REQUIRE(stats->rowCount() == 1);
	CHECK(stats->item(0, 0)->text() == "Scoreboard");
}

TEST_CASE("every string the overlay dialog asks for is in the locale file")
{
	FakeBackend backend;
	addOverlays(backend);
	backend.overlayList.push_back({"uuid-intro", "Intro", true, 0, 0});
	backend.overlayList.erase(backend.overlayList.begin());
	while (backend.current.overlays.items.size() < tapeloop::kMaxOverlays) {
		backend.current.overlays.items.push_back(backend.current.overlays.items.front());
	}
	backend.sceneList = {{"uuid-replays", "Replays"}};
	backend.current.overlays.mode = OverlayMode::Scene;
	backend.current.overlays.scene = {"uuid-gone", "Old replays"};
	backend.shown = {{"uuid-camera-1", "Camera 1", false, SourceState::Running, 30s, 120'000'000}};
	backend.shown[0].totalFrames = 3000;
	backend.current.sources["uuid-camera-1"].selected = true;
	backend.isRunning = true;

	std::set<std::string> missing;
	TapeloopDock dock(backend, tapeloop::test::recordingLocaleText(missing));
	OverlayDialog *dialog = openOverlays(dock);
	backend.current.showAdvanced = true;
	dock.refresh();
	auto *scene = child<QComboBox>(*dialog, "replayScene");
	scene->setCurrentIndex(scene->findData(QString("uuid-replays")));
	child<QPushButton>(*dialog, "removeOverlay")->click();
	delete openPicker(*dialog);
	backend.overlayList.clear();
	delete openPicker(*dialog);
	CHECK(missing.empty());
}
