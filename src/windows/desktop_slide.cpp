#include "desktop_slide.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>

#include "window_tracker.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logSlide, "quickshell.windows.desktopslide", QtWarningMsg);

constexpr auto* OVERLAY_CLASS = L"QuickshellDesktopSlide";
constexpr qint64 SLIDE_MS = 700;
constexpr int WORKSPACE_GAP = 50;

// end-4's menu_decel curve for the workspaces animation.
constexpr double CURVE_X1 = 0.1;
constexpr double CURVE_Y1 = 1.0;
constexpr double CURVE_X2 = 0.0;
constexpr double CURVE_Y2 = 1.0;

double bezier(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double ease(double x) {
	auto lo = 0.0;
	auto hi = 1.0;
	for (auto i = 0; i < 40; i++) {
		auto mid = (lo + hi) / 2;
		if (bezier(CURVE_X1, CURVE_X2, mid) < x) lo = mid;
		else hi = mid;
	}
	return bezier(CURVE_Y1, CURVE_Y2, (lo + hi) / 2);
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;
	default: return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

HWND createOverlay(const QRect& rect) {
	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &overlayProc;
		cls.hInstance = instance;
		cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		cls.lpszClassName = OVERLAY_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	return CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
	    OVERLAY_CLASS,
	    L"",
	    WS_POPUP,
	    rect.x(),
	    rect.y(),
	    rect.width(),
	    rect.height(),
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
}

QRect rectOf(HWND hwnd) {
	RECT r {};
	if (!GetWindowRect(hwnd, &r)) return {};
	return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool topmost(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

QList<HWND> topLevelWindows() {
	QList<HWND> list;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (IsWindowVisible(hwnd)) reinterpret_cast<QList<HWND>*>(param)->append(hwnd);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&list)
	);
	return list;
}

bool ownWindow(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

} // namespace

DesktopSlide* DesktopSlide::instance() {
	static QPointer<DesktopSlide> slide; // NOLINT
	if (slide.isNull()) slide = new DesktopSlide(QCoreApplication::instance());
	return slide.data();
}

DesktopSlide::DesktopSlide(QObject* parent): QObject(parent) {}

DesktopSlide::~DesktopSlide() {
	if (this->worker.joinable()) this->worker.join();
	this->finish();
}

bool DesktopSlide::begin(qsizetype from, qsizetype to) {
	if (this->mRunning.load() || from < 0 || to < 0 || from == to) return false;
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr) return false;

	POINT cursor {};
	GetCursorPos(&cursor);
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) return false;
	const auto& m = info.rcMonitor;
	this->monitor = QRect(m.left, m.top, m.right - m.left, m.bottom - m.top);

	struct Source {
		HWND hwnd;
		QRect rect;
		int lane;
	};
	QList<Source> sources;
	auto moving = 0;
	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		auto rect = rectOf(hwnd);
		if (!rect.intersects(this->monitor)) continue;

		if (ownWindow(hwnd)) {
			if (!topmost(hwnd) && rect.contains(this->monitor)) sources.append({hwnd, rect, 0});
			continue;
		}

		auto* window = tracker->windowFor(hwnd);
		if (window == nullptr || window->minimized()) continue;
		if (window->desktop() == from) sources.append({hwnd, rect, 1});
		else if (window->desktop() == to) sources.append({hwnd, rect, 2});
		else continue;
		moving++;
	}
	if (moving == 0) return false;

	this->overlay = createOverlay(this->monitor);
	if (this->overlay == nullptr) return false;

	auto direction = to > from ? -1 : 1;
	auto dpr = GetDpiForWindow(this->overlay) / 96.0;
	auto travel = this->monitor.width() + static_cast<int>(std::lround(WORKSPACE_GAP * dpr));

	for (const auto& source: sources) {
		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, source.hwnd, &thumbnail))) continue;
		auto rect = source.rect.translated(-this->monitor.topLeft());
		if (source.lane == 2) rect.translate(-direction * travel, 0);

		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.fVisible = TRUE;
		props.opacity = 255;
		props.fSourceClientAreaOnly = FALSE;
		props.rcDestination = {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
		DwmUpdateThumbnailProperties(thumbnail, &props);
		this->thumbs.append({thumbnail, source.rect.translated(-this->monitor.topLeft()), source.lane});
	}

	ShowWindow(this->overlay, SW_SHOWNOACTIVATE);
	SetWindowPos(this->overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	this->raisePanels();
	DwmFlush();
	DwmFlush();

	this->mRunning.store(true);
	if (this->worker.joinable()) this->worker.join();
	this->worker = std::thread(&DesktopSlide::run, this, direction, travel, SLIDE_MS);
	return true;
}

void DesktopSlide::raisePanels() {
	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == this->overlay || !ownWindow(hwnd) || !topmost(hwnd)) continue;
		SetWindowPos(
		    hwnd,
		    HWND_TOPMOST,
		    0,
		    0,
		    0,
		    0,
		    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
		);
	}
}

void DesktopSlide::run(int direction, int travel, qint64 durationMs) {
	auto start = std::chrono::steady_clock::now();
	while (true) {
		DwmFlush();
		auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
		auto progress = std::clamp(elapsed.count() / static_cast<double>(durationMs), 0.0, 1.0);
		auto eased = ease(progress);

		for (const auto& thumb: this->thumbs) {
			if (thumb.lane == 0) continue;
			auto offset = thumb.lane == 1 ? direction * travel * eased
			                              : direction * travel * (eased - 1.0);
			auto rect = thumb.rect.translated(static_cast<int>(std::lround(offset)), 0);
			DWM_THUMBNAIL_PROPERTIES props {};
			props.dwFlags = DWM_TNP_RECTDESTINATION;
			props.rcDestination = {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
			DwmUpdateThumbnailProperties(thumb.thumbnail, &props);
		}

		if (progress >= 1.0) break;
	}
	DwmFlush();
	QMetaObject::invokeMethod(this, &DesktopSlide::finish, Qt::QueuedConnection);
}

void DesktopSlide::finish() {
	if (this->worker.joinable() && this->worker.get_id() != std::this_thread::get_id()) {
		this->worker.join();
	}
	for (const auto& thumb: this->thumbs) DwmUnregisterThumbnail(thumb.thumbnail);
	this->thumbs.clear();
	if (this->overlay != nullptr) DestroyWindow(this->overlay);
	this->overlay = nullptr;
	this->mRunning.store(false);
}

} // namespace qs::windows
