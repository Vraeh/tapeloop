// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/OverlayDialog.hpp"

#include "ui/Guarded.hpp"
#include "ui/OverlayPreview.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace tapeloop::ui {
namespace {

// A box anchored to one side of the largest canvas and a distance away can start that
// far beyond it.
constexpr int kMaxBoxPosition = kMaxOverlayExtent + kMaxCanvasSize;

// The preset combo's first entry stands for a layout no preset gives.
constexpr int kCustomPreset = -1;

QSpinBox *pixelBox(QWidget *parent, const char *name, int minimum, int maximum, const TextLookup &text)
{
	auto *box = new QSpinBox(parent);
	box->setObjectName(name);
	box->setRange(minimum, maximum);
	box->setSuffix(text("Overlays.PixelSuffix"));
	// A number being typed is set once it is complete, not digit by digit.
	box->setKeyboardTracking(false);
	return box;
}

void showValue(QSpinBox &box, int value)
{
	if (box.value() != value) {
		box.setValue(value);
	}
}

void selectData(QComboBox &combo, int data)
{
	combo.setCurrentIndex(std::max(combo.findData(data), 0));
}

} // namespace

OverlayDialog::OverlayDialog(DockBackend &backend, TextLookup text, QWidget *parent)
	: QDialog(parent),
	  backend_(backend),
	  text_(std::move(text)),
	  opened_(backend_.settings().overlays),
	  layout_(opened_),
	  overlays_(new QListWidget(this)),
	  add_(new QPushButton(text_("Overlays.Add"), this)),
	  remove_(new QPushButton(text_("Overlays.Remove"), this)),
	  moveUp_(new QPushButton(text_("Overlays.MoveUp"), this)),
	  moveDown_(new QPushButton(text_("Overlays.MoveDown"), this)),
	  properties_(new QPushButton(text_("Overlays.Properties"), this)),
	  sceneNote_(new QLabel(this)),
	  fields_(new QWidget(this)),
	  preset_(new QComboBox(fields_)),
	  x_(pixelBox(fields_, "overlayX", -kMaxBoxPosition, kMaxBoxPosition, text_)),
	  y_(pixelBox(fields_, "overlayY", -kMaxBoxPosition, kMaxBoxPosition, text_)),
	  width_(pixelBox(fields_, "overlayWidth", kMinOverlaySize, kMaxOverlayExtent, text_)),
	  height_(pixelBox(fields_, "overlayHeight", kMinOverlaySize, kMaxOverlayExtent, text_)),
	  opacitySlider_(new QSlider(Qt::Horizontal, fields_)),
	  opacity_(new QSpinBox(fields_)),
	  advancedFields_(new QWidget(fields_)),
	  anchor_(new QComboBox(advancedFields_)),
	  distanceX_(pixelBox(advancedFields_, "distanceX", -kMaxOverlayExtent, kMaxOverlayExtent, text_)),
	  distanceY_(pixelBox(advancedFields_, "distanceY", -kMaxOverlayExtent, kMaxOverlayExtent, text_)),
	  fit_(new QComboBox(advancedFields_)),
	  cropLeft_(pixelBox(advancedFields_, "cropLeft", 0, kMaxOverlayExtent, text_)),
	  cropTop_(pixelBox(advancedFields_, "cropTop", 0, kMaxOverlayExtent, text_)),
	  cropRight_(pixelBox(advancedFields_, "cropRight", 0, kMaxOverlayExtent, text_)),
	  cropBottom_(pixelBox(advancedFields_, "cropBottom", 0, kMaxOverlayExtent, text_)),
	  blend_(new QComboBox(advancedFields_)),
	  sceneBox_(new QGroupBox(text_("Overlays.ReplayScene"), this)),
	  scene_(new QComboBox(sceneBox_)),
	  sceneWarning_(new QLabel(sceneBox_)),
	  preview_(new OverlayPreview(
		  backend_, layout_, [this] { followPreview(); }, this)),
	  buttons_(
		  new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this))
{
	setWindowTitle(text_("Overlays.Title"));
	// The overlays are laid out on the canvas as it is now; ones made on another are
	// scaled to it, which alone is no change to write.
	layout_.setCanvas(backend_.canvas());
	start_ = layout_.settings();
	for (const OverlaySource &source : backend_.overlaySources()) {
		knownSources_.push_back(source.uuid);
	}
	scenes_ = backend_.scenes();

	overlays_->setObjectName("overlays");
	overlays_->setToolTip(text_("Overlays.List.Tooltip"));
	overlays_->setSelectionMode(QAbstractItemView::SingleSelection);
	overlays_->installEventFilter(this);
	add_->setObjectName("addOverlay");
	remove_->setObjectName("removeOverlay");
	moveUp_->setObjectName("moveOverlayUp");
	moveDown_->setObjectName("moveOverlayDown");
	properties_->setObjectName("overlayProperties");
	properties_->setToolTip(text_("Overlays.Properties.Tooltip"));
	sceneNote_->setObjectName("sceneNote");
	sceneNote_->setWordWrap(true);
	sceneNote_->setTextFormat(Qt::PlainText);
	fields_->setObjectName("overlayFields");

	preset_->setObjectName("preset");
	preset_->addItem(text_("Overlays.Preset.Custom"), kCustomPreset);
	preset_->addItem(text_("Overlays.Preset.TopLeft"), static_cast<int>(OverlayPreset::TopLeft));
	preset_->addItem(text_("Overlays.Preset.TopRight"), static_cast<int>(OverlayPreset::TopRight));
	preset_->addItem(text_("Overlays.Preset.BottomLeft"), static_cast<int>(OverlayPreset::BottomLeft));
	preset_->addItem(text_("Overlays.Preset.BottomRight"), static_cast<int>(OverlayPreset::BottomRight));
	preset_->addItem(text_("Overlays.Preset.LowerThird"), static_cast<int>(OverlayPreset::LowerThird));
	preset_->addItem(text_("Overlays.Preset.FullScreen"), static_cast<int>(OverlayPreset::FullScreen));
	x_->setToolTip(text_("Overlays.Position.Tooltip"));
	y_->setToolTip(text_("Overlays.Position.Tooltip"));
	opacitySlider_->setObjectName("opacitySlider");
	opacitySlider_->setRange(0, 100);
	opacity_->setObjectName("opacity");
	opacity_->setRange(0, 100);
	opacity_->setSuffix(text_("Overlays.PercentSuffix"));
	opacity_->setKeyboardTracking(false);

	advancedFields_->setObjectName("advancedFields");
	anchor_->setObjectName("anchor");
	anchor_->setToolTip(text_("Overlays.Anchor.Tooltip"));
	const char *const anchorKeys[] = {"Overlays.Anchor.TopLeft",    "Overlays.Anchor.Top",
					  "Overlays.Anchor.TopRight",   "Overlays.Anchor.Left",
					  "Overlays.Anchor.Center",     "Overlays.Anchor.Right",
					  "Overlays.Anchor.BottomLeft", "Overlays.Anchor.Bottom",
					  "Overlays.Anchor.BottomRight"};
	for (int anchor = 0; anchor <= static_cast<int>(OverlayAnchor::BottomRight); ++anchor) {
		anchor_->addItem(text_(anchorKeys[anchor]), anchor);
	}
	distanceX_->setToolTip(text_("Overlays.Distance.Tooltip"));
	distanceY_->setToolTip(text_("Overlays.Distance.Tooltip"));
	fit_->setObjectName("fit");
	fit_->addItem(text_("Overlays.Fit.Inner"), static_cast<int>(OverlayFit::Inner));
	fit_->addItem(text_("Overlays.Fit.Stretch"), static_cast<int>(OverlayFit::Stretch));
	fit_->addItem(text_("Overlays.Fit.Fill"), static_cast<int>(OverlayFit::Fill));
	cropLeft_->setToolTip(text_("Overlays.Crop.Tooltip"));
	cropTop_->setToolTip(text_("Overlays.Crop.Tooltip"));
	cropRight_->setToolTip(text_("Overlays.Crop.Tooltip"));
	cropBottom_->setToolTip(text_("Overlays.Crop.Tooltip"));
	blend_->setObjectName("blend");
	const char *const blendKeys[] = {"Overlays.Blend.Normal", "Overlays.Blend.Additive", "Overlays.Blend.Subtract",
					 "Overlays.Blend.Screen", "Overlays.Blend.Multiply", "Overlays.Blend.Lighten",
					 "Overlays.Blend.Darken"};
	for (int blend = 0; blend <= static_cast<int>(OverlayBlend::Darken); ++blend) {
		blend_->addItem(text_(blendKeys[blend]), blend);
	}

	sceneBox_->setObjectName("sceneBox");
	scene_->setObjectName("replayScene");
	scene_->setToolTip(text_("Overlays.ReplayScene.Tooltip"));
	sceneWarning_->setObjectName("sceneWarning");
	sceneWarning_->setWordWrap(true);
	// Scene names are the user's and shown as they are.
	sceneWarning_->setTextFormat(Qt::PlainText);
	preview_->setObjectName("overlayPreview");
	buttons_->setObjectName("overlayButtons");

	auto *listButtons = new QVBoxLayout;
	listButtons->addWidget(add_);
	listButtons->addWidget(remove_);
	listButtons->addWidget(moveUp_);
	listButtons->addWidget(moveDown_);
	listButtons->addWidget(properties_);
	listButtons->addStretch();
	auto *list = new QHBoxLayout;
	list->addWidget(overlays_, 1);
	list->addLayout(listButtons);

	auto *opacityRow = new QHBoxLayout;
	opacityRow->addWidget(opacitySlider_, 1);
	opacityRow->addWidget(opacity_);
	auto *form = new QFormLayout(fields_);
	form->setContentsMargins(0, 0, 0, 0);
	form->addRow(text_("Overlays.Preset"), preset_);
	form->addRow(text_("Overlays.X"), x_);
	form->addRow(text_("Overlays.Y"), y_);
	form->addRow(text_("Overlays.Width"), width_);
	form->addRow(text_("Overlays.Height"), height_);
	form->addRow(text_("Overlays.Opacity"), opacityRow);
	form->addRow(advancedFields_);

	auto *distance = new QHBoxLayout;
	distance->addWidget(distanceX_);
	distance->addWidget(distanceY_);
	auto *crop = new QHBoxLayout;
	crop->addWidget(cropLeft_);
	crop->addWidget(cropTop_);
	crop->addWidget(cropRight_);
	crop->addWidget(cropBottom_);
	auto *advancedForm = new QFormLayout(advancedFields_);
	advancedForm->setContentsMargins(0, 0, 0, 0);
	advancedForm->addRow(text_("Overlays.Anchor"), anchor_);
	advancedForm->addRow(text_("Overlays.Distance"), distance);
	advancedForm->addRow(text_("Overlays.Fit"), fit_);
	advancedForm->addRow(text_("Overlays.Crop"), crop);
	advancedForm->addRow(text_("Overlays.Blend"), blend_);

	auto *sceneLayout = new QVBoxLayout(sceneBox_);
	sceneLayout->addWidget(scene_);
	sceneLayout->addWidget(sceneWarning_);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(list, 1);
	layout->addWidget(sceneNote_);
	layout->addWidget(fields_);
	layout->addWidget(preview_, 1);
	layout->addWidget(sceneBox_);
	layout->addWidget(buttons_);

	setTabOrder(overlays_, add_);
	setTabOrder(add_, remove_);
	setTabOrder(remove_, moveUp_);
	setTabOrder(moveUp_, moveDown_);
	setTabOrder(moveDown_, properties_);
	setTabOrder(properties_, preset_);
	setTabOrder(preset_, x_);
	setTabOrder(x_, y_);
	setTabOrder(y_, width_);
	setTabOrder(width_, height_);
	setTabOrder(height_, opacitySlider_);
	setTabOrder(opacitySlider_, opacity_);
	setTabOrder(opacity_, anchor_);
	setTabOrder(anchor_, distanceX_);
	setTabOrder(distanceX_, distanceY_);
	setTabOrder(distanceY_, fit_);
	setTabOrder(fit_, cropLeft_);
	setTabOrder(cropLeft_, cropTop_);
	setTabOrder(cropTop_, cropRight_);
	setTabOrder(cropRight_, cropBottom_);
	setTabOrder(cropBottom_, blend_);
	setTabOrder(blend_, preview_);
	setTabOrder(preview_, scene_);

	connect(overlays_, &QListWidget::currentRowChanged, this, [this](int row) {
		guarded([&] {
			layout_.select(indexOfRow(row));
			showSelected();
		});
	});
	connect(overlays_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
		guarded([&] {
			if (const std::optional<size_t> index = indexOfRow(overlays_->row(item))) {
				layout_.setVisible(*index, item->checkState() == Qt::Checked);
				showSelected();
			}
		});
	});
	connect(add_, &QPushButton::clicked, this, [this] { guarded([this] { openPicker(); }); });
	connect(remove_, &QPushButton::clicked, this, [this] { guarded([this] { removeSelected(); }); });
	connect(moveUp_, &QPushButton::clicked, this, [this] { guarded([this] { moveSelected(true); }); });
	connect(moveDown_, &QPushButton::clicked, this, [this] { guarded([this] { moveSelected(false); }); });
	connect(properties_, &QPushButton::clicked, this, [this] { guarded([this] { openProperties(); }); });

	connect(preset_, &QComboBox::currentIndexChanged, this, [this] {
		const int preset = preset_->currentData().toInt();
		if (preset == kCustomPreset) {
			guarded([this] { showSelected(); });
			return;
		}
		editSelected([&](size_t index) { layout_.applyPreset(index, static_cast<OverlayPreset>(preset)); });
	});
	const auto connectBox = [this](QSpinBox *box, void (*change)(OverlayBox &, int)) {
		connect(box, &QSpinBox::valueChanged, this, [this, change](int value) {
			editSelected([&](size_t index) {
				OverlayBox moved = boxOf(layout_.items()[index], layout_.canvas());
				change(moved, value);
				layout_.setBox(index, moved);
			});
		});
	};
	connectBox(x_, [](OverlayBox &box, int value) { box.left = value; });
	connectBox(y_, [](OverlayBox &box, int value) { box.top = value; });
	connectBox(width_, [](OverlayBox &box, int value) { box.width = value; });
	connectBox(height_, [](OverlayBox &box, int value) { box.height = value; });
	const auto connectItem = [this](QSpinBox *box, void (*change)(OverlayItem &, int)) {
		connect(box, &QSpinBox::valueChanged, this,
			[this, change](int value) { changeSelected(change, value); });
	};
	connectItem(opacity_, [](OverlayItem &item, int value) { item.opacity = value; });
	connectItem(distanceX_, [](OverlayItem &item, int value) { item.x = value; });
	connectItem(distanceY_, [](OverlayItem &item, int value) { item.y = value; });
	connectItem(cropLeft_, [](OverlayItem &item, int value) { item.crop.left = value; });
	connectItem(cropTop_, [](OverlayItem &item, int value) { item.crop.top = value; });
	connectItem(cropRight_, [](OverlayItem &item, int value) { item.crop.right = value; });
	connectItem(cropBottom_, [](OverlayItem &item, int value) { item.crop.bottom = value; });
	connect(opacitySlider_, &QSlider::valueChanged, this, [this](int value) {
		changeSelected([](OverlayItem &item, int opacity) { item.opacity = opacity; }, value);
	});
	connect(anchor_, &QComboBox::currentIndexChanged, this, [this] {
		const auto anchor = static_cast<OverlayAnchor>(anchor_->currentData().toInt());
		editSelected([&](size_t index) { layout_.setAnchor(index, anchor); });
	});
	connect(fit_, &QComboBox::currentIndexChanged, this, [this] {
		changeSelected([](OverlayItem &item, int fit) { item.fit = static_cast<OverlayFit>(fit); },
			       fit_->currentData().toInt());
	});
	connect(blend_, &QComboBox::currentIndexChanged, this, [this] {
		changeSelected([](OverlayItem &item, int blend) { item.blend = static_cast<OverlayBlend>(blend); },
			       blend_->currentData().toInt());
	});
	connect(scene_, &QComboBox::currentIndexChanged, this,
		[this](int index) { guarded([&] { chooseScene(index); }); });

	connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
		guarded([this] { apply(); });
		accept();
	});
	connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
		bool applied = true;
		guarded([&] { applied = apply(); });
		// What the dialog shows belongs to the scene collection that was open.
		if (!applied) {
			reject();
		}
	});

	buildScenes();
	if (!layout_.items().empty()) {
		layout_.select(layout_.items().size() - 1);
	}
	showItems();
	const bool advanced = backend_.settings().showAdvanced;
	advancedFields_->setVisible(advanced);
	sceneBox_->setVisible(advanced);
	showScene();
}

void OverlayDialog::showAdvanced(bool advanced)
{
	// The dock asks every second; the scene is looked at again only on a change.
	if (sceneBox_->isHidden() != advanced) {
		return;
	}
	advancedFields_->setVisible(advanced);
	sceneBox_->setVisible(advanced);
	showScene();
}

bool OverlayDialog::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != overlays_ || event->type() != QEvent::KeyPress) {
		return QDialog::eventFilter(watched, event);
	}
	const auto *key = static_cast<QKeyEvent *>(event);
	const Qt::KeyboardModifiers modifiers = key->modifiers() & ~Qt::KeypadModifier;
	if (key->key() == Qt::Key_Delete && modifiers == Qt::NoModifier) {
		guarded([this] { removeSelected(); });
		return true;
	}
	if ((key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) && modifiers == Qt::AltModifier) {
		guarded([&] { moveSelected(key->key() == Qt::Key_Up); });
		return true;
	}
	return QDialog::eventFilter(watched, event);
}

void OverlayDialog::buildScenes()
{
	const QSignalBlocker block(scene_);
	scene_->clear();
	scene_->addItem(text_("Overlays.ReplayScene.None"), QString());
	for (const OverlayScene &scene : scenes_) {
		scene_->addItem(QString::fromStdString(scene.name), QString::fromStdString(scene.uuid));
	}
	const OverlaySettings &settings = layout_.settings();
	if (settings.mode != OverlayMode::Scene) {
		return;
	}
	const QString uuid = QString::fromStdString(settings.scene.uuid);
	int index = scene_->findData(uuid);
	if (index < 0) {
		const std::string &name = settings.scene.name.empty() ? settings.scene.uuid : settings.scene.name;
		scene_->addItem(text_("Overlays.ReplayScene.Missing").arg(QString::fromStdString(name)), uuid);
		index = scene_->count() - 1;
	}
	scene_->setCurrentIndex(index);
}

void OverlayDialog::showItems()
{
	{
		const QSignalBlocker block(overlays_);
		overlays_->clear();
		const std::vector<OverlayItem> &items = layout_.items();
		for (size_t index = items.size(); index-- > 0;) {
			const OverlayItem &item = items[index];
			QString name = QString::fromStdString(item.name);
			if (std::find(knownSources_.begin(), knownSources_.end(), item.uuid) == knownSources_.end()) {
				name = text_("Overlays.Missing").arg(name);
			}
			auto *row = new QListWidgetItem(name, overlays_);
			row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
			row->setCheckState(item.visible ? Qt::Checked : Qt::Unchecked);
		}
		const std::optional<size_t> selected = layout_.selected();
		overlays_->setCurrentRow(selected ? rowOfIndex(*selected) : -1);
	}
	add_->setEnabled(layout_.items().size() < kMaxOverlays);
	add_->setToolTip(add_->isEnabled() ? QString() : text_("Overlays.Full"));
	showSelected();
}

void OverlayDialog::showSelected()
{
	preview_->refresh();
	const std::optional<size_t> selected = layout_.selected();
	const size_t count = layout_.items().size();
	remove_->setEnabled(selected.has_value());
	moveUp_->setEnabled(selected && *selected + 1 < count);
	moveDown_->setEnabled(selected && *selected > 0);
	properties_->setEnabled(selected && std::find(knownSources_.begin(), knownSources_.end(),
						      layout_.items()[*selected].uuid) != knownSources_.end());
	fields_->setEnabled(selected.has_value());
	buttons_->button(QDialogButtonBox::Apply)->setEnabled(layout_.settings() != start_);
	if (!selected) {
		return;
	}

	const OverlayItem &item = layout_.items()[*selected];
	const OverlayBox box = boxOf(item, layout_.canvas());
	const std::optional<OverlayPreset> preset = matchingPreset(item, layout_.canvas());
	const QSignalBlocker blocks[] = {QSignalBlocker(preset_),     QSignalBlocker(x_),
					 QSignalBlocker(y_),          QSignalBlocker(width_),
					 QSignalBlocker(height_),     QSignalBlocker(opacitySlider_),
					 QSignalBlocker(opacity_),    QSignalBlocker(anchor_),
					 QSignalBlocker(distanceX_),  QSignalBlocker(distanceY_),
					 QSignalBlocker(fit_),        QSignalBlocker(cropLeft_),
					 QSignalBlocker(cropTop_),    QSignalBlocker(cropRight_),
					 QSignalBlocker(cropBottom_), QSignalBlocker(blend_)};
	selectData(*preset_, preset ? static_cast<int>(*preset) : kCustomPreset);
	showValue(*x_, box.left);
	showValue(*y_, box.top);
	showValue(*width_, box.width);
	showValue(*height_, box.height);
	opacitySlider_->setValue(item.opacity);
	showValue(*opacity_, item.opacity);
	selectData(*anchor_, static_cast<int>(item.anchor));
	showValue(*distanceX_, item.x);
	showValue(*distanceY_, item.y);
	selectData(*fit_, static_cast<int>(item.fit));
	showValue(*cropLeft_, item.crop.left);
	showValue(*cropTop_, item.crop.top);
	showValue(*cropRight_, item.crop.right);
	showValue(*cropBottom_, item.crop.bottom);
	selectData(*blend_, static_cast<int>(item.blend));
}

void OverlayDialog::followPreview()
{
	{
		const QSignalBlocker block(overlays_);
		const std::optional<size_t> selected = layout_.selected();
		overlays_->setCurrentRow(selected ? rowOfIndex(*selected) : -1);
	}
	showSelected();
}

void OverlayDialog::showScene()
{
	const OverlaySettings &settings = layout_.settings();
	const bool sceneMode = settings.mode == OverlayMode::Scene;
	const QString name =
		QString::fromStdString(settings.scene.name.empty() ? settings.scene.uuid : settings.scene.name);
	const bool found = std::any_of(scenes_.begin(), scenes_.end(),
				       [&](const OverlayScene &scene) { return scene.uuid == settings.scene.uuid; });

	// The scene is an advanced setting, but the list says why it may not be what shows.
	sceneNote_->setVisible(sceneMode && !sceneBox_->isVisibleTo(this));
	sceneNote_->setText(text_("Overlays.SceneNote").arg(name));

	QString warning;
	if (sceneMode && !found) {
		warning = text_("Overlays.ReplayScene.MissingWarning").arg(name);
	} else if (sceneMode && !backend_.sceneHoldsReplay(settings.scene.uuid)) {
		warning = text_("Overlays.ReplayScene.NoReplayView").arg(name);
	}
	sceneWarning_->setText(warning);
	sceneWarning_->setVisible(!warning.isEmpty());
}

bool OverlayDialog::apply()
{
	// Every write saves the scene collection, so a dialog left as it was writes nothing.
	if (layout_.settings() == start_) {
		return true;
	}
	BufferSettings settings = backend_.settings();
	if (settings.overlays != opened_) {
		return false;
	}
	settings.overlays = layout_.settings();
	backend_.setSettings(settings);
	opened_ = backend_.settings().overlays;
	start_ = layout_.settings();
	showSelected();
	return true;
}

void OverlayDialog::openPicker()
{
	const OverlaySettings &settings = layout_.settings();
	std::vector<OverlaySource> sources = backend_.overlaySources();
	// The replay's own scene inside itself would be a loop OBS refuses.
	if (settings.mode == OverlayMode::Scene) {
		std::erase_if(sources, [&](const OverlaySource &source) { return source.uuid == settings.scene.uuid; });
	}

	auto *picker = new QDialog(this);
	picker->setObjectName("overlayPicker");
	picker->setWindowTitle(text_("Overlays.Picker.Title"));
	picker->setAttribute(Qt::WA_DeleteOnClose);
	auto *choices = new QListWidget(picker);
	choices->setObjectName("overlayChoices");
	for (const OverlaySource &source : sources) {
		const QString name = QString::fromStdString(source.name);
		choices->addItem(source.scene ? text_("Overlays.Picker.Scene").arg(name) : name);
	}
	choices->setCurrentRow(sources.empty() ? -1 : 0);
	auto *empty = new QLabel(text_("Overlays.Picker.Empty"), picker);
	empty->setObjectName("overlayChoicesEmpty");
	empty->setWordWrap(true);
	empty->setVisible(sources.empty());
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, picker);
	buttons->button(QDialogButtonBox::Ok)->setEnabled(!sources.empty());
	auto *layout = new QVBoxLayout(picker);
	layout->addWidget(choices, 1);
	layout->addWidget(empty);
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, picker, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, picker, &QDialog::reject);
	connect(choices, &QListWidget::itemActivated, picker, &QDialog::accept);
	connect(picker, &QDialog::accepted, this, [this, choices, sources = std::move(sources)] {
		const int row = choices->currentRow();
		if (row >= 0 && static_cast<size_t>(row) < sources.size()) {
			guarded([&] { addOverlay(sources[static_cast<size_t>(row)]); });
		}
	});
	picker->open();
}

void OverlayDialog::addOverlay(const OverlaySource &source)
{
	if (!layout_.add(newOverlay(source.uuid, source.name, source.width, source.height, layout_.canvas()))) {
		return;
	}
	if (std::find(knownSources_.begin(), knownSources_.end(), source.uuid) == knownSources_.end()) {
		knownSources_.push_back(source.uuid);
	}
	showItems();
	overlays_->setFocus();
}

void OverlayDialog::removeSelected()
{
	const int row = overlays_->currentRow();
	const std::optional<size_t> index = layout_.selected();
	if (!index) {
		return;
	}
	layout_.remove(*index);
	const int rows = static_cast<int>(layout_.items().size());
	// The row below takes the place of the one removed, or the one above at the bottom.
	layout_.select(rows > 0 ? indexOfRow(std::min(row, rows - 1)) : std::nullopt);
	showItems();
}

void OverlayDialog::moveSelected(bool up)
{
	const std::optional<size_t> index = layout_.selected();
	if (!index) {
		return;
	}
	// The list shows the topmost first, so up is toward the end of the items.
	if (up && *index + 1 < layout_.items().size()) {
		layout_.moveItem(*index, *index + 1);
		layout_.select(*index + 1);
	} else if (!up && *index > 0) {
		layout_.moveItem(*index, *index - 1);
		layout_.select(*index - 1);
	} else {
		return;
	}
	showItems();
}

void OverlayDialog::openProperties()
{
	if (const std::optional<size_t> index = layout_.selected()) {
		backend_.openProperties(layout_.items()[*index].uuid);
	}
}

void OverlayDialog::editSelected(const std::function<void(size_t index)> &edit)
{
	guarded([&] {
		if (const std::optional<size_t> index = layout_.selected()) {
			edit(*index);
		}
		showSelected();
	});
}

void OverlayDialog::changeSelected(void (*change)(OverlayItem &, int), int value)
{
	editSelected([&](size_t index) {
		OverlayItem item = layout_.items()[index];
		change(item, value);
		layout_.setItem(index, std::move(item));
	});
}

void OverlayDialog::chooseScene(int index)
{
	const std::string uuid = scene_->itemData(index).toString().toStdString();
	if (uuid.empty()) {
		layout_.setMode(OverlayMode::Simple);
		layout_.setScene({});
	} else if (uuid != layout_.settings().scene.uuid) {
		const auto found = std::find_if(scenes_.begin(), scenes_.end(),
						[&](const OverlayScene &scene) { return scene.uuid == uuid; });
		// The only entry not among the scenes is the missing one the dialog opened with.
		layout_.setMode(OverlayMode::Scene);
		layout_.setScene(found != scenes_.end() ? *found : start_.scene);
	} else {
		layout_.setMode(OverlayMode::Scene);
	}
	showScene();
	showSelected();
}

std::optional<size_t> OverlayDialog::indexOfRow(int row) const noexcept
{
	const size_t count = layout_.items().size();
	if (row < 0 || static_cast<size_t>(row) >= count) {
		return std::nullopt;
	}
	return count - 1 - static_cast<size_t>(row);
}

int OverlayDialog::rowOfIndex(size_t index) const noexcept
{
	return static_cast<int>(layout_.items().size() - 1 - index);
}

} // namespace tapeloop::ui
