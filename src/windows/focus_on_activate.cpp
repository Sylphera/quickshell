#include "focus_on_activate.hpp"

#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>

#include "input_mask.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

#include <windows.h>

namespace qs::windows::focus_on_activate {

namespace {

Q_LOGGING_CATEGORY(logFocusOnActivate, "quickshell.windows.focusonactivate", QtDebugMsg);

constexpr DWORD ACTIVATION_WINDOW_MS = 1500;
constexpr DWORD DESKTOP_SETTLE_MS = 1500;
constexpr DWORD REPEAT_FLASH_MS = 10000;
constexpr auto* CLASS_NAME = L"QuickshellFocusOnActivate";

HWND gWindow = nullptr;      // NOLINT
UINT gShellMessage = 0;      // NOLINT
DWORD gHandledPress = 0;     // NOLINT
bool gHoldsMouseHook = false; // NOLINT
DWORD gDesktopChanged = 0;    // NOLINT
QList<QMetaObject::Connection> gConnections; // NOLINT
QHash<HWND, DWORD> gLastFlash;               // NOLINT

TrackedWindow* trackedFor(HWND hwnd) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr || hwnd == nullptr) return nullptr;
	for (auto* candidate: {hwnd, GetAncestor(hwnd, GA_ROOT), GetAncestor(hwnd, GA_ROOTOWNER)}) {
		if (candidate == nullptr) continue;
		if (auto* window = tracker->windowFor(candidate)) return window;
	}
	return nullptr;
}

void onFlash(HWND hwnd) {
	const auto now = GetTickCount();
	const auto previous = gLastFlash.value(hwnd, 0);
	gLastFlash.insert(hwnd, now);
	if (gLastFlash.size() > 256) gLastFlash.clear();
	qCDebug(logFocusOnActivate) << "Flash from" << hwnd << (previous == 0 ? 0 : now - previous) << "ms after its last";
	if (previous != 0 && now - previous < REPEAT_FLASH_MS) return;

	const auto press = lastPressTime();
	if (press == 0 || press == gHandledPress) return;
	const auto age = GetTickCount() - press;
	if (age > ACTIVATION_WINDOW_MS) return;
	if (gDesktopChanged != 0 && GetTickCount() - gDesktopChanged < DESKTOP_SETTLE_MS) return;

	auto* window = trackedFor(hwnd);
	if (window == nullptr) return;
	auto* foreground = GetForegroundWindow();
	if (foreground == nullptr || foreground != lastPressForeground()) return;
	auto* source = trackedFor(foreground);
	if (source == nullptr || source == window) return;
	auto* desktops = WindowTracker::instance()->desktops();
	if (desktops == nullptr || !desktops->isWindowOnCurrent(source->hwnd())) return;

	gHandledPress = press;
	qCInfo(logFocusOnActivate) << "Focusing" << window->appId() << "after an activation request" << age
	                           << "ms after input";
	window->activate(true);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	if (gShellMessage != 0 && message == gShellMessage) {
		if (wParam == HSHELL_FLASH) onFlash(reinterpret_cast<HWND>(lParam)); // NOLINT(performance-no-int-to-ptr)
		return 0;
	}
	return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool start() {
	auto* instance = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = windowProc;
	wc.hInstance = instance;
	wc.lpszClassName = CLASS_NAME;
	if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

	gShellMessage = RegisterWindowMessageW(L"SHELLHOOK");
	gWindow = CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    CLASS_NAME,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (gWindow == nullptr || gShellMessage == 0) return false;
	if (!RegisterShellHookWindow(gWindow)) {
		qCWarning(logFocusOnActivate) << "Could not register the shell hook window:" << GetLastError();
		DestroyWindow(gWindow);
		gWindow = nullptr;
		return false;
	}
	return true;
}

} // namespace

void setEnabled(bool enabled) {
	if (enabled == (gWindow != nullptr)) return;
	if (enabled) {
		if (!start()) return;
		if (auto* tracker = InputMaskTracker::instance()) {
			tracker->acquireHook();
			gHoldsMouseHook = true;
		}
		if (auto* desktops = WindowTracker::instance()->desktops()) {
			auto stamp = []() { gDesktopChanged = GetTickCount(); };
			gConnections.append(QObject::connect(desktops, &VirtualDesktops::currentChanged, stamp));
			gConnections.append(QObject::connect(desktops, &VirtualDesktops::shownChanged, stamp));
		}
		return;
	}
	for (const auto& connection: gConnections) QObject::disconnect(connection);
	gConnections.clear();
	gLastFlash.clear();
	DeregisterShellHookWindow(gWindow);
	DestroyWindow(gWindow);
	gWindow = nullptr;
	if (gHoldsMouseHook) {
		InputMaskTracker::instance()->releaseHook();
		gHoldsMouseHook = false;
	}
}

} // namespace qs::windows::focus_on_activate
