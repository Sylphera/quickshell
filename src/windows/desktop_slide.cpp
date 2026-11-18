#include "desktop_slide.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qstringlist.h>
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
constexpr int HOLD_MS = 150;
constexpr auto* SNAPSHOT_CLASS = L"QuickshellDesktopSlideSnapshot";
constexpr int CORNER_ZONE = 40;
constexpr int CORNER_TOLERANCE = 6;

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
		cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		cls.lpszClassName = OVERLAY_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	return CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
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

// Wait for the next compositor frame, the way wiri paces its animations
// (MIT, jolionlands/wiri: src/animation/clock.rs): Windows 11's compositor
// clock when dcomp.dll exports it, DwmFlush otherwise.
void waitForFrame() {
	using WaitFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
	static auto wait = []() -> WaitFn {
		auto* dcomp = LoadLibraryW(L"dcomp.dll");
		if (dcomp == nullptr) return nullptr;
		return reinterpret_cast<WaitFn>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
	}();
	if (wait != nullptr && wait(0, nullptr, 100) != WAIT_FAILED) return;
	DwmFlush();
}

double refreshPeriodMs() {
	DWM_TIMING_INFO timing {.cbSize = sizeof(DWM_TIMING_INFO)};
	LARGE_INTEGER frequency {};
	if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0
	    && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
	{
		return static_cast<double>(timing.qpcRefreshPeriod) * 1000.0 / static_cast<double>(frequency.QuadPart);
	}
	return 1000.0 / 60.0;
}

// The desktop window repaints its wallpaper when the desktop changes and shows
// white for a frame, so the backdrop is a still copy put in place on show.
// Browsers and video players keep a video surface that makes their live
// thumbnail expensive for the compositor, even when nothing plays. They slide
// as still copies; everything else stays live.
bool heavyWindow(const TrackedWindow* window) {
	static const QStringList names = {
	    QStringLiteral("zen.exe"),      QStringLiteral("firefox.exe"),  QStringLiteral("librewolf.exe"),
	    QStringLiteral("floorp.exe"),   QStringLiteral("waterfox.exe"), QStringLiteral("chrome.exe"),
	    QStringLiteral("msedge.exe"),   QStringLiteral("brave.exe"),    QStringLiteral("opera.exe"),
	    QStringLiteral("vivaldi.exe"),  QStringLiteral("chromium.exe"), QStringLiteral("thorium.exe"),
	    QStringLiteral("mpv.exe"),      QStringLiteral("vlc.exe"),      QStringLiteral("mpc-hc64.exe"),
	    QStringLiteral("mpc-be64.exe"), QStringLiteral("potplayermini64.exe"),
	};
	auto path = window->exePath();
	auto name = path.mid(path.lastIndexOf(QLatin1Char('\\')) + 1).toLower();
	return names.contains(name);
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
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(this->overlay, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
	auto dpr = GetDpiForWindow(this->overlay) / 96.0;
	this->travel = this->monitor.width() + static_cast<int>(std::lround(WORKSPACE_GAP * dpr));

	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == this->overlay || topmost(hwnd)) continue;
		// The desktop hosts the wallpaper and the shell's embedded widgets: it
		// stays live over the still copy so neither freezes during the slide.
		if (!ownWindow(hwnd) && hwnd != GetShellWindow()) continue;
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

	this->pruneSnapshots();
	this->prepareBackdrop();
	this->bases.insert(from, 0);
	this->target = from;
	this->cameraFrom = 0;
	this->cameraTo = 0;
	this->addDesktop(from, true);
	this->retarget(to);

	WinPanelWindow::setCoverActive(true);
	this->raised = WinPanelWindow::raiseDemoted();
	{
		// The first frame on screen must already match what was there.
		auto lock = std::lock_guard(this->mutex);
		this->layoutThumbs(0);
	}
	this->placeUnderPanels();
	ShowWindow(this->overlay, SW_SHOWNOACTIVATE);
	DwmFlush();
	{
		// The slide starts on the first frame the cover is on screen; the setup
		// before it must not eat into end-4's front-loaded curve.
		auto lock = std::lock_guard(this->mutex);
		this->startedAt = std::chrono::steady_clock::now();
	}

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
			auto base = this->bases.value(this->target) + (to > this->target ? this->travel : -this->travel);
			// Like Hyprland, a desktop that held this slot leaves the strip.
			for (auto iter = this->bases.begin(); iter != this->bases.end();) {
				if (iter.value() != base) {
					++iter;
					continue;
				}
				auto gone = iter.key();
				for (auto i = this->thumbs.size() - 1; i >= 0; i--) {
					if (this->thumbs[i].desktop != gone) continue;
					DwmUnregisterThumbnail(this->thumbs[i].thumbnail);
					this->thumbs.removeAt(i);
				}
				iter = this->bases.erase(iter);
			}
			this->bases.insert(to, base);
		}

		this->cameraFrom = camera;
		this->cameraTo = this->bases.value(to);
		this->startedAt = now;
		this->target = to;
		this->settledSent = false;
	}
	this->settleRetries = 0;
	this->addDesktop(to, false);
}

void DesktopSlide::addDesktop(qsizetype desktop, bool capture) {
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

		auto source = sourceOf(hwnd, rect);
		auto local = source.translated(rect.topLeft() - this->monitor.topLeft());

		auto frame = local;
		RECT bounds {};
		if (source.size() == rect.size()
		    && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
		{
			frame = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
			            .translated(-this->monitor.topLeft());
		}
		auto heavy = heavyWindow(window);
		auto snapshot = capture && heavy ? this->takeSnapshot(hwnd, frame) : false;
		if (!snapshot && heavy) {
			auto known = this->snapshots.constFind(hwnd);
			snapshot = known != this->snapshots.constEnd() && known->frame == frame && IsWindow(known->host);
		}
		if (snapshot) {
			const auto& copy = this->snapshots.value(hwnd);
			HTHUMBNAIL thumbnail = nullptr;
			if (SUCCEEDED(DwmRegisterThumbnail(this->overlay, copy.host, &thumbnail))) {
				DWM_THUMBNAIL_PROPERTIES props {};
				props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE;
				props.rcSource = {0, 0, frame.width(), frame.height()};
				props.opacity = 255;
				props.fVisible = FALSE;
				DwmUpdateThumbnailProperties(thumbnail, &props);
				added.append({thumbnail, frame, QRect(QPoint(), frame.size()), desktop});
				continue;
			}
		}

		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;

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
	auto last = std::chrono::steady_clock::now();
	auto period = refreshPeriodMs();
	while (!this->stop.load()) {
		// The clock can wake early; never step faster than the display refreshes.
		do {
			waitForFrame();
		} while (!this->stop.load()
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();
		auto lock = std::lock_guard(this->mutex);
		auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - this->startedAt).count();
		auto progress = std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		this->layoutThumbs(camera);

		if (progress >= 1.0 && !this->settledSent) {
			this->settledSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onSettled, Qt::QueuedConnection);
		}
	}
}

// Called with the mutex held.
void DesktopSlide::layoutThumbs(double camera) {
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

	// Keystrokes a moment apart keep the same cover instead of rebuilding it.
	QTimer::singleShot(HOLD_MS, this, [this, target]() {
		{
			auto lock = std::lock_guard(this->mutex);
			if (!this->settledSent || this->target != target) return;
		}
		this->finish();
	});
}

// Panels are left alone: repainting their full-screen transparent windows to
// restack them can flash a frame. The cover slides in just under the lowest.
void DesktopSlide::placeUnderPanels() {
	HWND lowest = nullptr;
	for (auto* hwnd: topLevelWindows()) {
		if (hwnd == this->overlay || !ownWindow(hwnd) || !topmost(hwnd)) continue;
		lowest = hwnd;
	}
	SetWindowPos(
	    this->overlay,
	    lowest != nullptr ? lowest : HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
}

void DesktopSlide::prepareBackdrop() {
	auto* screen = GetDC(nullptr);
	auto width = this->monitor.width();
	auto height = this->monitor.height();
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	this->wallBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	this->wallBits = static_cast<quint32*>(bits);
	this->wallDc = CreateCompatibleDC(screen);
	SelectObject(this->wallDc, this->wallBitmap);

	RECT fill {0, 0, width, height};
	FillRect(this->wallDc, &fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
	if (auto* desktop = GetShellWindow()) {
		RECT r {};
		GetWindowRect(desktop, &r);
		auto* full = CreateCompatibleDC(screen);
		auto* fullBitmap = CreateCompatibleBitmap(screen, r.right - r.left, r.bottom - r.top);
		auto* fullPrevious = SelectObject(full, fullBitmap);
		if (PrintWindow(desktop, full, PW_RENDERFULLCONTENT)) {
			BitBlt(this->wallDc, 0, 0, width, height, full, this->monitor.x() - r.left, this->monitor.y() - r.top, SRCCOPY);
		}
		SelectObject(full, fullPrevious);
		DeleteObject(fullBitmap);
		DeleteDC(full);
	}
	GdiFlush();

	POINT origin {this->monitor.x(), this->monitor.y()};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, 0};
	UpdateLayeredWindow(this->overlay, screen, &origin, &size, this->wallDc, &source, 0, &blend, ULW_OPAQUE);
	ReleaseDC(nullptr, screen);
}

void DesktopSlide::releaseBackdrop() {
	if (this->wallDc != nullptr) DeleteDC(this->wallDc);
	if (this->wallBitmap != nullptr) DeleteObject(this->wallBitmap);
	this->wallDc = nullptr;
	this->wallBitmap = nullptr;
	this->wallBits = nullptr;
}

// A live thumbnail of a window that draws video, even paused, costs the
// compositor every frame. A window on screen when the slide starts is copied
// once instead, and the copy is kept to slide it back in next time if the
// window has not moved. Corners left showing the wallpaper are cut away by
// comparing with the backdrop, whatever radius rounds them.
bool DesktopSlide::takeSnapshot(HWND hwnd, const QRect& frame) {
	auto screenRect = QRect(QPoint(), this->monitor.size());
	if (this->wallBits == nullptr || !screenRect.contains(frame) || frame.isEmpty()) return false;

	auto width = frame.width();
	auto height = frame.height();
	auto& snapshot = this->snapshots[hwnd];
	if (snapshot.host != nullptr && snapshot.frame.size() != frame.size()) {
		DestroyWindow(snapshot.host);
		snapshot.host = nullptr;
	}
	auto parkX = GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 100;
	if (snapshot.host == nullptr) {
		static auto registered = false; // NOLINT
		auto* instance = GetModuleHandleW(nullptr);
		if (!registered) {
			WNDCLASSEXW cls {};
			cls.cbSize = sizeof(cls);
			cls.lpfnWndProc = &DefWindowProcW;
			cls.hInstance = instance;
			cls.lpszClassName = SNAPSHOT_CLASS;
			registered = RegisterClassExW(&cls) != 0;
		}
		snapshot.host = CreateWindowExW(
		    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
		    SNAPSHOT_CLASS,
		    L"",
		    WS_POPUP,
		    parkX,
		    0,
		    width,
		    height,
		    nullptr,
		    nullptr,
		    instance,
		    nullptr
		);
		if (snapshot.host == nullptr) {
			this->snapshots.remove(hwnd);
			return false;
		}
		BOOL noTransitions = TRUE;
		DwmSetWindowAttribute(snapshot.host, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
	}

	auto* screen = GetDC(nullptr);
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* raw = nullptr;
	auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
	auto* dc = CreateCompatibleDC(screen);
	SelectObject(dc, bitmap);
	BitBlt(dc, 0, 0, width, height, screen, this->monitor.x() + frame.x(), this->monitor.y() + frame.y(), SRCCOPY);
	GdiFlush();

	auto* bits = static_cast<quint32*>(raw);
	auto zone = std::min({CORNER_ZONE, width / 2, height / 2});
	auto wallWidth = this->monitor.width();
	auto close = [](quint32 a, quint32 b) {
		auto channel = [](quint32 v, int shift) { return static_cast<int>((v >> shift) & 0xff); };
		return std::abs(channel(a, 16) - channel(b, 16)) <= CORNER_TOLERANCE
		    && std::abs(channel(a, 8) - channel(b, 8)) <= CORNER_TOLERANCE
		    && std::abs(channel(a, 0) - channel(b, 0)) <= CORNER_TOLERANCE;
	};
	for (auto y = 0; y < height; y++) {
		auto cornerRow = y < zone || y >= height - zone;
		for (auto x = 0; x < width; x++) {
			auto& p = bits[y * width + x];
			auto cornerColumn = x < zone || x >= width - zone;
			if (cornerRow && cornerColumn && close(p, this->wallBits[(frame.y() + y) * wallWidth + frame.x() + x])) {
				p = 0;
			} else {
				p |= 0xff000000u;
			}
		}
	}

	POINT origin {parkX, 0};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(snapshot.host, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
	DeleteDC(dc);
	DeleteObject(bitmap);
	ReleaseDC(nullptr, screen);
	ShowWindow(snapshot.host, SW_SHOWNOACTIVATE);
	snapshot.frame = frame;
	return true;
}

void DesktopSlide::pruneSnapshots() {
	for (auto iter = this->snapshots.begin(); iter != this->snapshots.end();) {
		if (IsWindow(iter.key()) && IsWindow(iter->host)) {
			++iter;
			continue;
		}
		if (iter->host != nullptr) DestroyWindow(iter->host);
		iter = this->snapshots.erase(iter);
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
	this->releaseBackdrop();
	if (this->overlay == nullptr) return;
	DestroyWindow(this->overlay);
	this->overlay = nullptr;
	WinPanelWindow::setCoverActive(false);
	WinPanelWindow::restack(this->raised);
	this->raised.clear();
}

} // namespace qs::windows
