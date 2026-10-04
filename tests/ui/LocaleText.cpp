// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#include "LocaleText.hpp"

#include <fstream>

namespace tapeloop::test {

std::map<std::string, QString> localeStrings()
{
	std::map<std::string, QString> strings;
	std::ifstream file(TAPELOOP_LOCALE_FILE);
	std::string line;
	while (std::getline(file, line)) {
		const size_t equals = line.find('=');
		if (equals == std::string::npos || line.size() < equals + 3 || line[equals + 1] != '"' ||
		    line.back() != '"')
			continue;
		strings[line.substr(0, equals)] =
			QString::fromStdString(line.substr(equals + 2, line.size() - equals - 3));
	}
	return strings;
}

tapeloop::ui::TextLookup recordingLocaleText(std::set<std::string> &missing)
{
	return [strings = localeStrings(), &missing](const char *key) {
		const auto found = strings.find(key);
		if (found != strings.end())
			return found->second;
		missing.insert(key);
		return QString(key);
	};
}

tapeloop::ui::TextLookup localeText()
{
	return [strings = localeStrings()](const char *key) {
		const auto found = strings.find(key);
		return found != strings.end() ? found->second : QString(key);
	};
}

} // namespace tapeloop::test
