// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/MiniPlayer.hpp"

#include "ui/PlayerPicture.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>

namespace tapeloop::ui {
namespace {

constexpr int kRefreshMs = 100;
constexpr int kSeekStepMs = 100;
constexpr int kSeekPageMs = 1000;

template<typename Function> void guarded(Function &&function) noexcept
{
	try {
		function();
	} catch (...) {
	}
}

int milliseconds(Nanoseconds time)
{
	const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(time).count();
	return static_cast<int>(std::clamp<int64_t>(ms, 0, std::numeric_limits<int>::max()));
}

QString seconds(Nanoseconds time)
{
	return QString::number(static_cast<double>(milliseconds(time)) / 1000.0, 'f', 1);
}

// Keeps the picture at 16:9 of the width it is given.
class PictureFrame : public QWidget {
public:
	explicit PictureFrame(QWidget *parent) : QWidget(parent)
	{
		QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
		policy.setHeightForWidth(true);
		setSizePolicy(policy);
	}

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override { return width * 9 / 16; }
	QSize sizeHint() const override { return {320, 180}; }
};

} // namespace

MiniPlayer::MiniPlayer(DockBackend &backend, const TextLookup &text, PlayerShows shows, QWidget *parent)
	: QWidget(parent),
	  backend_(backend),
	  text_(text),
	  shows_(shows)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	if (shows_ != PlayerShows::Controls) {
		auto *frame = new PictureFrame(this);
		frame->setObjectName("pictureFrame");
		auto *frameLayout = new QVBoxLayout(frame);
		frameLayout->setContentsMargins(0, 0, 0, 0);
		picture_ = new PlayerPicture(backend_.pictureSurface(), frame);
		picture_->setObjectName("picture");
		frameLayout->addWidget(picture_);
		layout->addWidget(frame, 1);
	}
	if (shows_ == PlayerShows::Picture) {
		return;
	}

	controls_ = new QWidget(this);
	controls_->setObjectName("playerControls");
	position_ = new QSlider(Qt::Horizontal, controls_);
	position_->setObjectName("position");
	position_->setAccessibleName(text_("Dock.Player.Position"));
	position_->setToolTip(text_("Dock.Player.Position"));
	position_->setRange(0, 0);
	position_->setSingleStep(kSeekStepMs);
	position_->setPageStep(kSeekPageMs);
	state_ = new QLabel(controls_);
	state_->setObjectName("playerState");
	state_->setTextFormat(Qt::PlainText);

	auto *moving = new QHBoxLayout;
	moving->addWidget(addScrubButton("scrubBackward", "Dock.Player.ScrubBackward", "Dock.Player.ScrubBackward.Name",
					 PlayerControl::ScrubBackward));
	moving->addWidget(addButton("previousFrame", "Dock.Player.PreviousFrame", "Dock.Player.PreviousFrame.Name",
				    PlayerControl::PreviousFrame));
	playPause_ = addButton("playPause", "Dock.Player.Play", "Dock.Player.PlayPause", PlayerControl::PlayPause);
	moving->addWidget(playPause_);
	moving->addWidget(addButton("nextFrame", "Dock.Player.NextFrame", "Dock.Player.NextFrame.Name",
				    PlayerControl::NextFrame));
	moving->addWidget(addScrubButton("scrubForward", "Dock.Player.ScrubForward", "Dock.Player.ScrubForward.Name",
					 PlayerControl::ScrubForward));
	auto *speed = new QHBoxLayout;
	speed->addWidget(
		addButton("reverse", "Dock.Player.Reverse", "Dock.Player.Reverse.Name", PlayerControl::Reverse));
	speed->addWidget(addButton("slower", "Dock.Player.Slower", "Dock.Player.Slower.Name", PlayerControl::Slower));
	speed->addWidget(addButton("normalSpeed", "Dock.Player.NormalSpeed", "Dock.Player.NormalSpeed.Name",
				   PlayerControl::NormalSpeed));
	speed->addWidget(addButton("faster", "Dock.Player.Faster", "Dock.Player.Faster.Name", PlayerControl::Faster));

	auto *controlsLayout = new QVBoxLayout(controls_);
	controlsLayout->setContentsMargins(0, 0, 0, 0);
	controlsLayout->addWidget(position_);
	controlsLayout->addWidget(state_);
	controlsLayout->addLayout(moving);
	controlsLayout->addLayout(speed);
	layout->addWidget(controls_);

	// The slider first, then the buttons as they read.
	QWidget *previous = position_;
	for (QPushButton *button : buttons_) {
		setTabOrder(previous, button);
		previous = button;
	}

	connect(position_, &QSlider::valueChanged, this,
		[this](int ms) { guarded([&] { backend_.seekPlayer(std::chrono::milliseconds(ms)); }); });
	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
	refresh();
}

MiniPlayer::~MiniPlayer()
{
	releaseScrub();
}

QPushButton *MiniPlayer::newButton(const char *name, const char *label, const char *accessible)
{
	auto *button = new QPushButton(text_(label), controls_);
	button->setObjectName(name);
	button->setAccessibleName(text_(accessible));
	button->setToolTip(text_(accessible));
	buttons_.push_back(button);
	return button;
}

QPushButton *MiniPlayer::addButton(const char *name, const char *label, const char *accessible, PlayerControl control)
{
	QPushButton *button = newButton(name, label, accessible);
	connect(button, &QPushButton::clicked, this, [this, control] { this->control(control, true); });
	return button;
}

QPushButton *MiniPlayer::addScrubButton(const char *name, const char *label, const char *accessible,
					PlayerControl control)
{
	QPushButton *button = newButton(name, label, accessible);
	// Qt lets go of a held button when the pointer leaves it or it is disabled, but not
	// when it loses focus to another window while Space holds it.
	button->installEventFilter(this);
	connect(button, &QPushButton::pressed, this, [this, control] {
		releaseScrub();
		scrubbing_ = control;
		this->control(control, true);
	});
	connect(button, &QPushButton::released, this, [this, control] {
		if (scrubbing_ == control) {
			releaseScrub();
		}
	});
	return button;
}

void MiniPlayer::control(PlayerControl control, bool pressed) noexcept
{
	guarded([&] { backend_.controlPlayer(control, pressed); });
	refresh();
}

void MiniPlayer::releaseScrub() noexcept
{
	if (!scrubbing_) {
		return;
	}
	const PlayerControl held = *scrubbing_;
	scrubbing_.reset();
	guarded([&] { backend_.controlPlayer(held, false); });
}

void MiniPlayer::refresh()
{
	if (!controls_) {
		return;
	}
	guarded([this] {
		const DockPlayer player = backend_.player();
		if (!player.loaded) {
			releaseScrub();
		}
		for (QPushButton *button : buttons_) {
			button->setEnabled(player.loaded);
		}
		position_->setEnabled(player.loaded);
		playPause_->setText(text_(player.playing ? "Dock.Player.Pause" : "Dock.Player.Play"));
		{
			const QSignalBlocker block(position_);
			position_->setRange(0, milliseconds(player.length));
			// A drag in progress is not pulled back to where the player was.
			if (!position_->isSliderDown()) {
				position_->setValue(milliseconds(player.position));
			}
		}
		if (!player.loaded) {
			state_->setText(text_("Dock.Player.Empty"));
			return;
		}
		const QString speed = QString::number(static_cast<double>(std::abs(player.rate)) / 1000.0, 'g', 3);
		state_->setText(text_("Dock.Player.State")
					.arg(text_(player.onAir ? "Dock.Player.OnAir" : "Dock.Player.Cued"),
					     text_(player.playing ? "Dock.Player.Playing" : "Dock.Player.Paused"),
					     text_(player.rate < 0 ? "Dock.Player.SpeedReverse" : "Dock.Player.Speed")
						     .arg(speed),
					     seconds(player.position), seconds(player.length)));
	});
}

void MiniPlayer::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (timer_) {
		refresh();
		timer_->start(kRefreshMs);
	}
}

void MiniPlayer::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	releaseScrub();
	if (timer_) {
		timer_->stop();
	}
}

bool MiniPlayer::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::FocusOut && scrubbing_) {
		auto *button = qobject_cast<QPushButton *>(watched);
		if (button && button->isDown()) {
			button->setDown(false);
		}
		releaseScrub();
	}
	return QWidget::eventFilter(watched, event);
}

} // namespace tapeloop::ui
