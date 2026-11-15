// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockBackend.hpp"
#include "ui/DockText.hpp"

#include <QWidget>

#include <optional>
#include <vector>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace tapeloop::ui {

class PlayerPicture;

// The replay player as the dock offers it: its picture, its controls, or both. The
// controls act through the backend as the player's hotkeys do, the scrub buttons for as
// long as they are held, and are disabled while the player holds no replay. While the
// controls are shown they read the player ten times a second, for the position to move
// smoothly.
class MiniPlayer : public QWidget {
public:
	MiniPlayer(DockBackend &backend, const TextLookup &text, PlayerShows shows, QWidget *parent = nullptr);
	~MiniPlayer() override;

	MiniPlayer(const MiniPlayer &) = delete;
	MiniPlayer &operator=(const MiniPlayer &) = delete;
	MiniPlayer(MiniPlayer &&) = delete;
	MiniPlayer &operator=(MiniPlayer &&) = delete;

	PlayerShows shows() const noexcept { return shows_; }
	// Reads the player again.
	void refresh();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QPushButton *newButton(const char *name, const char *label, const char *accessible);
	QPushButton *addButton(const char *name, const char *label, const char *accessible, PlayerControl control);
	QPushButton *addScrubButton(const char *name, const char *label, const char *accessible, PlayerControl control);
	void control(PlayerControl control, bool pressed) noexcept;
	// Lets go of the dock's scrub still held, as when its button lets go.
	void releaseScrub() noexcept;

	DockBackend &backend_;
	TextLookup text_;
	PlayerShows shows_;
	PlayerPicture *picture_ = nullptr;
	QWidget *controls_ = nullptr;
	std::vector<QPushButton *> buttons_;
	QPushButton *playPause_ = nullptr;
	QSlider *position_ = nullptr;
	QLabel *state_ = nullptr;
	QTimer *timer_ = nullptr;
	std::optional<PlayerControl> scrubbing_;
};

} // namespace tapeloop::ui
