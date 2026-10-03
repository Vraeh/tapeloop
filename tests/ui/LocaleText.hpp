// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

#include "ui/DockText.hpp"

#include <QString>

#include <map>
#include <string>

namespace tapeloop::test {

// The strings of data/locale/en-US.ini, read the way OBS reads them: Key="value".
std::map<std::string, QString> localeStrings();

// Looks keys up in localeStrings(); a missing key comes back as itself.
tapeloop::ui::TextLookup localeText();

} // namespace tapeloop::test
