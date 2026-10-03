// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "ui/DockText.hpp"

#include <QComboBox>

#include <array>

namespace tapeloop::ui {
namespace {

constexpr std::array<uint32_t, 5> kFixedHeights = {360, 480, 720, 1080, 2160};

} // namespace

const std::vector<const char *> &dockTextKeys()
{
	static const std::vector<const char *> keys = {
		"Dock.Title",
		"Dock.Source",
		"Dock.Status",
		"Dock.Status.Stopped",
		"Dock.Status.Running",
		"Dock.Status.Failed",
		"Dock.SourceSettings",
		"Dock.Length",
		"Dock.SecondsSuffix",
		"Dock.Resolution",
		"Dock.Resolution.Canvas",
		"Dock.Resolution.Output",
		"Dock.Resolution.Fixed",
		"Dock.StartWithOutputs",
		"Dock.Start",
		"Dock.Stop",
		"Dock.FollowsOutputs",
		"Dock.ApplyNote",
		"SourceSettings.Title",
		"SourceSettings.OwnLength",
		"SourceSettings.OwnResolution",
	};
	return keys;
}

void addResolutions(QComboBox &combo, const TextLookup &text)
{
	combo.addItem(text("Dock.Resolution.Canvas"));
	combo.addItem(text("Dock.Resolution.Output"));
	for (const uint32_t height : kFixedHeights)
		combo.addItem(text("Dock.Resolution.Fixed").arg(height));
}

ReplayResolution resolutionAt(int index)
{
	if (index == 1)
		return {ResolutionMode::Output, ReplayResolution{}.height};
	if (index >= 2 && static_cast<size_t>(index - 2) < kFixedHeights.size())
		return {ResolutionMode::Fixed, kFixedHeights[static_cast<size_t>(index - 2)]};
	return {};
}

int indexOfResolution(ReplayResolution resolution)
{
	switch (resolution.mode) {
	case ResolutionMode::Output:
		return 1;
	case ResolutionMode::Fixed:
		for (size_t i = 0; i < kFixedHeights.size(); ++i) {
			if (kFixedHeights[i] == resolution.height)
				return static_cast<int>(i) + 2;
		}
		break;
	case ResolutionMode::Canvas:
		break;
	}
	return 0;
}

} // namespace tapeloop::ui
