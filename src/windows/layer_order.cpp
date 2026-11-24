#include "layer_order.hpp"
#include <functional>

#include <qt_windows.h>
#include <dwmapi.h>

#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qtypes.h>

#include "layer_plan.hpp"
#include "panel_window.hpp"
#include "util.hpp"

namespace qs::windows::layers {

namespace {

Q_LOGGING_CATEGORY(logLayers, "quickshell.windows.layers", QtWarningMsg);

constexpr int MINIMUM_SIZE = 40;

bool topmost(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

quintptr idOf(HWND hwnd) { return reinterpret_cast<quintptr>(hwnd); }

HWND windowOf(quintptr id) {
	return reinterpret_cast<HWND>(id); // NOLINT(performance-no-int-to-ptr)
}

QList<LayerWindow> currentOrder() {
	QList<LayerWindow> order;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		order.append({.id = idOf(hwnd), .topmost = topmost(hwnd)});
	}
	return order;
}

}

bool staysAbove(HWND hwnd) {
	if (!topmost(hwnd) || isOwnProcessWindow(hwnd)) return false;
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
	if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) return false;
	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	RECT rect {};
	return cloaked == 0 && GetWindowRect(hwnd, &rect) && rect.right - rect.left >= MINIMUM_SIZE
	    && rect.bottom - rect.top >= MINIMUM_SIZE;
}

HWND anchor(const std::function<bool(HWND)>& skip) {
	const auto panels = WinPanelWindow::aboveWindowHandles();
	HWND lowest = nullptr;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (!topmost(hwnd)) continue;
		if (skip && skip(hwnd)) continue;
		if (panels.contains(hwnd) || staysAbove(hwnd)) lowest = hwnd;
	}
	return lowest;
}

bool apply(const QList<HWND>& chain) {
	QList<quintptr> ids;
	for (auto* hwnd: chain) {
		if (hwnd != nullptr && IsWindow(hwnd) && !IsHungAppWindow(hwnd)) ids.append(idOf(hwnd));
	}
	auto* top = anchor([&chain](HWND hwnd) { return chain.contains(hwnd); });
	auto moves = planLayers(currentOrder(), idOf(top), ids);

	constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
	for (const auto& move: moves) {
		auto* hwnd = windowOf(move.id);
		auto* after = move.after == 0 ? HWND_TOPMOST : windowOf(move.after);
		auto moved = SetWindowPos(hwnd, after, 0, 0, 0, 0, flags) != FALSE;
		qCDebug(logLayers) << "Moved" << hwnd << "after" << after << moved;
	}
	return !moves.isEmpty();
}

}
