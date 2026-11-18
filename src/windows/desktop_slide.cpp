#include "desktop_slide.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qtimer.h>

#include "panel_window.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

constexpr auto* OVERLAY_CLASS = L"QuickshellDesktopSlide";
constexpr double SLIDE_MS = 700;
constexpr int WORKSPACE_GAP = 50;
constexpr int SETTLE_RETRY_MS = 30;
constexpr int SETTLE_RETRIES = 30;

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
	case WM_PAINT: {
		PAINTSTRUCT paint {};
		auto* dc = BeginPaint(hwnd, &paint);
		FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
		EndPaint(hwnd, &paint);
		return 0;
	}
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

bool desktopWindow(HWND hwnd) {
	wchar_t name[16] {};
	GetClassNameW(hwnd, name, 16);
	return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0;
}

bool ownWindow(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

// A tiled window can carry a region that clips it to its tile; the thumbnail
// ignores it, so the source is cut to the region's bounds instead.
QRect sourceOf(HWND hwnd, const QRect& rect) {
	RECT box {};
	auto kind = GetWindowRgnBox(hwnd, &box);
	if (kind == SIMPLEREGION || kind == COMPLEXREGION) {
		return {box.left, box.top, box.right - box.left, box.bottom - box.top};
	}
	return {0, 0, rect.width(), rect.height()};
}

RECT toRect(const QRect& rect) { return {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1}; }

} // namespace

DesktopSlide* DesktopSlide::instance() {
	static QPointer<DesktopSlide> slide; // NOLINT
	if (slide.isNull()) slide = new DesktopSlide(QCoreApplication::instance());
	return slide.data();
}

DesktopSlide::DesktopSlide(QObject* parent): QObject(parent) {}

DesktopSlide::~DesktopSlide() { this->finish(); }

void DesktopSlide::begin(qsizetype from, qsizetype to) {
	if (to < 0) return;
	if (this->overlay != nullptr) this->retarget(to);
	else if (from >= 0 && from != to) this->start(from, to);
}

bool DesktopSlide::start(qsizetype from, qsizetype to) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr) return false;

	auto any = false;
	for (auto* window: tracker->windows()) {
		auto desktop = window->desktop();
		if (!window->minimized() && (desktop == from || desktop == to)) any = true;
	}
	if (!any) return false;

	POINT cursor {};
	GetCursorPos(&cursor);
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) return false;
	const auto& m = info.rcMonitor;
	this->monitor = QRect(m.left, m.top, m.right - m.left, m.bottom - m.top);

	this->overlay = createOverlay(this->monitor);
	if (this->overlay == nullptr) return false;
	auto dpr = GetDpiForWindow(this->overlay) / 96.0;
	this->travel = this->monitor.width() + static_cast<int>(std::lround(WORKSPACE_GAP * dpr));

	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == this->overlay || topmost(hwnd)) continue;
		if (!desktopWindow(hwnd) && !ownWindow(hwnd)) continue;
		auto rect = rectOf(hwnd);
		if (!rect.contains(this->monitor)) continue;

		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;
		auto local = rect.translated(-this->monitor.topLeft());
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.fVisible = TRUE;
		props.opacity = 255;
		props.rcDestination = toRect(local);
		DwmUpdateThumbnailProperties(thumbnail, &props);
		this->thumbs.append({thumbnail, local, QRect(), -1});
	}

	this->bases.insert(from, 0);
	this->target = from;
	this->cameraFrom = 0;
	this->cameraTo = 0;
	this->addDesktop(from);
	this->retarget(to);

	WinPanelWindow::setCoverActive(true);
	ShowWindow(this->overlay, SW_SHOWNOACTIVATE);
	SetWindowPos(this->overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	this->raisePanels();
	DwmFlush();
	DwmFlush();

	this->stop.store(false);
	this->settleRetries = 0;
	this->worker = std::thread(&DesktopSlide::run, this);
	return true;
}

void DesktopSlide::retarget(qsizetype to) {
	{
		auto lock = std::lock_guard(this->mutex);
		if (to == this->target) return;

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration<double, std::milli>(now - this->startedAt).count();
		auto progress = this->cameraFrom == this->cameraTo ? 1.0 : std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		if (!this->bases.contains(to)) {
			auto base = this->bases.value(this->target);
			this->bases.insert(to, base + (to > this->target ? this->travel : -this->travel));
		}

		this->cameraFrom = camera;
		this->cameraTo = this->bases.value(to);
		this->startedAt = now;
		this->target = to;
		this->settledSent = false;
	}
	this->addDesktop(to);
}

void DesktopSlide::addDesktop(qsizetype desktop) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr || this->overlay == nullptr) return;

	{
		auto lock = std::lock_guard(this->mutex);
		for (const auto& thumb: this->thumbs) {
			if (thumb.desktop == desktop) return;
		}
	}

	QList<Thumb> added;
	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (ownWindow(hwnd)) continue;
		auto* window = tracker->windowFor(hwnd);
		if (window == nullptr || window->minimized() || window->desktop() != desktop) continue;
		auto rect = rectOf(hwnd);
		if (!rect.intersects(this->monitor)) continue;

		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;
		auto source = sourceOf(hwnd, rect);
		auto local = source.translated(rect.topLeft() - this->monitor.topLeft());

		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE;
		props.rcSource = toRect(source);
		props.opacity = 255;
		props.fVisible = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		added.append({thumbnail, local, source, desktop});
	}

	auto lock = std::lock_guard(this->mutex);
	this->thumbs.append(added);
}

void DesktopSlide::run() {
	while (!this->stop.load()) {
		DwmFlush();
		auto lock = std::lock_guard(this->mutex);
		auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - this->startedAt).count();
		auto progress = std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		for (const auto& thumb: this->thumbs) {
			if (thumb.desktop < 0) continue;
			auto offset = static_cast<int>(std::lround(this->bases.value(thumb.desktop) - camera));
			auto rect = thumb.rect.translated(offset, 0);
			DWM_THUMBNAIL_PROPERTIES props {};
			props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
			props.fVisible = rect.intersects(QRect(QPoint(), this->monitor.size())) ? TRUE : FALSE;
			props.rcDestination = toRect(rect);
			DwmUpdateThumbnailProperties(thumb.thumbnail, &props);
		}

		if (progress >= 1.0 && !this->settledSent) {
			this->settledSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onSettled, Qt::QueuedConnection);
		}
	}
}

void DesktopSlide::onSettled() {
	qsizetype target = -1;
	{
		auto lock = std::lock_guard(this->mutex);
		if (!this->settledSent) return;
		target = this->target;
	}

	auto* desktops = VirtualDesktops::instance();
	if (desktops->currentIndex() != target && this->settleRetries++ < SETTLE_RETRIES) {
		QTimer::singleShot(SETTLE_RETRY_MS, this, &DesktopSlide::onSettled);
		return;
	}
	this->finish();
}

void DesktopSlide::raisePanels() {
	auto above = WinPanelWindow::aboveWindowHandles();
	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == this->overlay || !ownWindow(hwnd)) continue;
		if (!topmost(hwnd) && !above.contains(hwnd)) continue;
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}
}

void DesktopSlide::finish() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	for (const auto& thumb: this->thumbs) DwmUnregisterThumbnail(thumb.thumbnail);
	this->thumbs.clear();
	this->bases.clear();
	this->target = -1;
	this->settledSent = false;
	if (this->overlay == nullptr) return;
	DestroyWindow(this->overlay);
	this->overlay = nullptr;
	WinPanelWindow::setCoverActive(false);
	WinPanelWindow::restackAll();
}

} // namespace qs::windows
