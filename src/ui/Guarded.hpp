// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

#pragma once

namespace tapeloop::ui {

// Qt cannot let an exception through its event loop; a failed change is dropped and the
// next look at the backend shows the state as it is.
template<typename Function> void guarded(Function &&function) noexcept
{
	try {
		function();
	} catch (...) {
	}
}

} // namespace tapeloop::ui
