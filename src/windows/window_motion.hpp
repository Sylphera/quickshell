#pragma once
#include <functional>

#include <dwmapi.h>
#include <qt_windows.h>

namespace qs::windows {

// end-4's windowsIn and windowsOut (popin 80% / 90% with a fade) for the
// shell's own windows, such as settings. The window is cloaked (it is ours, so
// DWMWA_CLOAK is allowed) while a thumbnail of it scales and fades on a small
// cover above it; popping in, the window shows again once the thumbnail lands.
class WindowPop {
public:
	// Hides the window now; pops it in once it has painted, or after a moment.
	static void popIn(HWND hwnd);
	// Pops the window out and leaves it cloaked; returns the duration in ms.
	static int popOut(HWND hwnd);
	// Starts the pop-in of a window cloaked by popIn.
	static void start(HWND hwnd, bool in);
	// end-4's windowsOut for an app window: a still of it pops out over it while
	// `close` closes it. False if it couldn't, and then `close` hasn't run.
	static bool closeOut(HWND hwnd, const std::function<void()>& close);
};

} // namespace qs::windows
