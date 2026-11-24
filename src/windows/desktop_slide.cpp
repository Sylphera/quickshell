#include "desktop_slide.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <pdh.h>

#include <qcoreapplication.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qset.h>
#include <qstringlist.h>
#include <qtimer.h>

#include "dim_plan.hpp"
#include "dwindle_scene.hpp"
#include "inactive_dim.hpp"
#include "panel_window.hpp"
#include "scratchpad.hpp"
#include "tiling.hpp"
#include "touchpad_swipe.hpp"
#include "virtual_desktops.hpp"
#include "window_motion.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logSlide, "quickshell.windows.slide", QtWarningMsg);

constexpr auto* OVERLAY_CLASS = L"QuickshellDesktopSlide";
constexpr double SLIDE_MS = 700;
constexpr double SWIPE_CANCEL_RATIO = 0.2;
constexpr int WORKSPACE_GAP = 50;
constexpr int SETTLE_RETRY_MS = 30;
constexpr int SETTLE_RETRIES = 30;
constexpr int HOLD_MS = 150;
constexpr int COMMIT_MS = 400;

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
	case WM_NCHITTEST: return HTCLIENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
	case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
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
	auto* hwnd = CreateWindowExW(
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
	BOOL disableTransitions = TRUE;
	if (hwnd != nullptr)
		DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
	return hwnd;
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

QRect sourceOf(HWND hwnd, const QRect& rect) {
	RECT box {};
	auto kind = GetWindowRgnBox(hwnd, &box);
	if (kind == SIMPLEREGION || kind == COMPLEXREGION) {
		return {box.left, box.top, box.right - box.left, box.bottom - box.top};
	}
	return {0, 0, rect.width(), rect.height()};
}

// Adapted from wiri's src/animation/clock.rs, Copyright (c) 2026 jolionlands,
// MIT License (see third-party/wiri-LICENSE).
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
		return static_cast<double>(timing.qpcRefreshPeriod) * 1000.0
		     / static_cast<double>(frequency.QuadPart);
	}
	return 1000.0 / 60.0;
}

bool isChromium(HWND hwnd) {
	wchar_t cls[64] {};
	GetClassNameW(hwnd, cls, 64);
	return wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0;
}

RECT toRect(const QRect& rect) {
	return {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
}

QRect cursorMonitor() {
	POINT cursor {};
	GetCursorPos(&cursor);
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) return {};
	const auto& m = info.rcMonitor;
	return {m.left, m.top, m.right - m.left, m.bottom - m.top};
}

QList<HWND> backdropSources(const QRect& monitor, const QList<HWND>& windows, HWND overlay) {
	QList<HWND> sources;
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == overlay || topmost(hwnd)) continue;
		if (!ownWindow(hwnd) && hwnd != GetShellWindow()) continue;
		if (rectOf(hwnd).contains(monitor)) sources.append(hwnd);
	}
	return sources;
}

HTHUMBNAIL addBackdrop(HWND overlay, HWND source, const QRect& monitor) {
	HTHUMBNAIL thumbnail = nullptr;
	if (FAILED(DwmRegisterThumbnail(overlay, source, &thumbnail))) return nullptr;
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags =
	    DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
	props.fVisible = TRUE;
	props.opacity = 255;
	props.rcDestination = toRect(rectOf(source).translated(-monitor.topLeft()));
	if (SUCCEEDED(DwmUpdateThumbnailProperties(thumbnail, &props))) return thumbnail;
	DwmUnregisterThumbnail(thumbnail);
	return nullptr;
}

bool hideThumbnail(HTHUMBNAIL thumbnail) {
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE;
	props.fVisible = FALSE;
	return SUCCEEDED(DwmUpdateThumbnailProperties(thumbnail, &props));
}

}

DesktopSlide* DesktopSlide::instance() {
	static QPointer<DesktopSlide> slide; // NOLINT
	if (slide.isNull()) slide = new DesktopSlide(QCoreApplication::instance());
	return slide.data();
}

DesktopSlide::DesktopSlide(QObject* parent): QObject(parent) {
	this->liveWarmTimer.setSingleShot(true);
	QObject::connect(&this->liveWarmTimer, &QTimer::timeout, this, &DesktopSlide::warmLiveThumbs);
	QTimer::singleShot(0, this, [this]() {
		auto* tracker = WindowTracker::instance();
		if (tracker == nullptr) return;
		auto schedule = [this](TrackedWindow*) {
			this->liveWarmPending = true;
			if (this->overlay == nullptr) this->liveWarmTimer.start(0);
		};
		QObject::connect(tracker, &WindowTracker::windowAdded, this, schedule);
		QObject::connect(tracker, &WindowTracker::windowRemoved, this, schedule);
		this->warmLiveThumbs();
	});
}

DesktopSlide::~DesktopSlide() {
	this->finish();
	this->clearLiveOverlay();
}

bool DesktopSlide::begin(qsizetype from, qsizetype to) {
	if (to < 0) return false;
	if (this->overlay != nullptr) this->retarget(to);
	else if (from >= 0 && from != to) this->start(from, to);
	return this->overlay != nullptr;
}

bool DesktopSlide::beginOverJump(qsizetype from, qsizetype to) {
	this->hurried = true;
	auto started = this->begin(from, to);
	this->hurried = false;
	return started;
}

void DesktopSlide::goBack(qsizetype to, qsizetype from) {
	if (this->overlay != nullptr && !this->swiping && this->target == to) this->retarget(from);
}

void DesktopSlide::swipeUpdate(double offset) {
	auto* desktops = VirtualDesktops::instance();
	if (!this->swiping) {
		this->swiping = true;
		this->swipeFrom = desktops->shownIndex();
	}
	auto from = this->swipeFrom;
	auto way = offset > 0 ? 1 : (offset < 0 ? -1 : 0);
	auto to = way == 0 ? -1 : this->swipeTarget(way);
	if (to < 0) {
		to = from;
		offset = 0;
	}
	this->swipeOffset = offset;

	if (this->overlay == nullptr) {
		if (to == from) return;
		if (!this->start(from, to)) {
			qCDebug(logSlide) << "Swipe from" << from << "to" << to << "has no slide to drive";
			return;
		}
		qCDebug(logSlide) << "Swipe from" << from << "to" << to << "drives the slide";
	}
	{
		auto lock = std::lock_guard(this->mutex);
		this->manual = true;
		this->manualCamera = this->bases.value(from) + std::clamp(offset, -1.0, 1.0) * this->travel;
	}
	this->retarget(to);
}

bool DesktopSlide::swipeCanGo(int way) const {
	return this->swipeTarget(way) >= 0;
}

qsizetype DesktopSlide::swipeTarget(int way) const {
	auto* tracker = WindowTracker::instance();
	auto* desktops = VirtualDesktops::instance();
	if (tracker == nullptr) return -1;

	QSet<qsizetype> occupied;
	for (auto* window: tracker->windows()) {
		if (!Scratchpad::holds(window)) occupied.insert(window->desktop());
	}
	auto from = this->swiping ? this->swipeFrom : desktops->shownIndex();
	return swipeDestination(from, way, desktops->count(), [&occupied](qsizetype desktop) {
		return occupied.contains(desktop);
	});
}

void DesktopSlide::goTo(qsizetype to) {
	auto* desktops = VirtualDesktops::instance();
	if (to < desktops->count()) {
		desktops->switchToAsync(to);
		return;
	}
	qCDebug(logSlide) << "Swipe creates desktop" << to;
	desktops->ensureCount(to + 1, [this, desktops, to, from = this->swipeFrom](bool created) {
		if (this->swiping || (this->overlay != nullptr && this->target != to)) return;
		if (created) desktops->switchToAsync(to);
		else if (this->overlay != nullptr) this->retarget(from);
	});
}

void DesktopSlide::swipeEnd(SwipeRelease release) {
	if (!this->swiping) return;
	this->swiping = false;
	auto goesBy = [release](double offset) {
		if (release == SwipeRelease::Cancel) return false;
		return std::abs(offset) >= SWIPE_CANCEL_RATIO
		    || (release == SwipeRelease::Force && offset != 0);
	};
	if (this->overlay == nullptr) {
		if (this->swipeOffset != 0 && goesBy(this->swipeOffset)) {
			auto to = this->swipeTarget(this->swipeOffset > 0 ? 1 : -1);
			if (to >= 0) this->goTo(to);
		}
		return;
	}

	qsizetype to = -1;
	{
		auto lock = std::lock_guard(this->mutex);
		auto offset = (this->manualCamera - this->bases.value(this->swipeFrom)) / this->travel;
		auto goes = this->target != this->swipeFrom && goesBy(offset);
		to = goes ? this->target : this->swipeFrom;
	}
	qCDebug(logSlide) << "Swipe let go toward" << to << "from" << this->swipeFrom;
	this->retarget(to);
	{
		auto lock = std::lock_guard(this->mutex);
		this->cameraFrom = this->manualCamera;
		this->cameraTo = this->bases.value(to);
		this->startedAt = std::chrono::steady_clock::now();
		this->settledSent = false;
		this->commitSent = false;
		this->manual = false;
	}
	if (to != this->swipeFrom) this->goTo(to);
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

	this->monitor = cursorMonitor();
	if (this->monitor.isEmpty()) return false;

	if (!this->prepareLiveOverlay(this->monitor, topLevelWindows())) return false;
	this->overlay = this->liveOverlay;
	this->setClickThrough(false);
	auto dpr = GetDpiForWindow(this->overlay) / 96.0;
	this->travel = this->monitor.width() + static_cast<int>(std::lround(WORKSPACE_GAP * dpr));

	if (!this->hurried) this->findPinned();
	if (!this->hurried) this->captureDialogs(from);
	this->prepareBackdrop();
	WinPanelWindow::setCoverActive(true);
	this->raised = WinPanelWindow::raiseDemoted();
	this->placeUnderPanels();
	this->bases.insert(from, 0);
	this->target = from;
	this->cameraFrom = 0;
	this->cameraTo = 0;
	if (!this->addLiveDesktop(from)) {
		this->finish();
		return false;
	}
	this->retarget(to);

	this->carryPanels();
	this->layoutThumbs(0);
	ShowWindow(this->overlay, SW_SHOWNOACTIVATE);
	InactiveDim::instance()->stepAside();
	DwmFlush();
	if (this->panelsCarried) {
		WinPanelWindow::setCoverOverPanels(true);
		this->placeUnderPanels();
	}
	{
		auto lock = std::lock_guard(this->mutex);
		this->startedAt = std::chrono::steady_clock::now();
	}

	this->stop.store(false);
	this->settleRetries = 0;
	this->worker = std::thread(&DesktopSlide::run, this);
	return true;
}

void DesktopSlide::retarget(qsizetype to) {
	if (to == this->target) return;

	this->addLiveDesktop(to);
	if (this->panelsCarried) {
		this->liftFullscreen(to);
	} else if (this->worker.joinable() && this->carryPanels()) {
		DwmFlush();
		WinPanelWindow::setCoverOverPanels(true);
		this->placeUnderPanels();
	}
	{
		auto lock = std::lock_guard(this->mutex);

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration<double, std::milli>(now - this->startedAt).count();
		auto progress = std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->manual
		                ? this->manualCamera
		                : this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		auto here = this->target;
		auto nearest = std::numeric_limits<double>::max();
		for (auto iter = this->bases.cbegin(); iter != this->bases.cend(); ++iter) {
			auto distance = std::abs(iter.value() - camera);
			if (distance < nearest) {
				nearest = distance;
				here = iter.key();
			}
		}
		auto base = to == here ? this->bases.value(here)
		                       : this->bases.value(here) + (to > here ? this->travel : -this->travel);
		auto low = std::min(camera, static_cast<double>(base));
		auto high = std::max(camera, static_cast<double>(base));
		for (auto iter = this->bases.begin(); iter != this->bases.end();) {
			auto key = iter.key();
			auto value = static_cast<double>(iter.value());
			auto other = key != here && key != to;
			auto inTheWay = other && value >= low && value <= high;
			auto outOfSight = !this->manual && other
			               && std::abs(value - camera) >= this->monitor.width();
			if (!inTheWay && !outOfSight) {
				++iter;
				continue;
			}
			for (auto i = this->thumbs.size() - 1; i >= 0; i--) {
				if (this->thumbs[i].desktop != key) continue;
				this->releaseThumb(this->thumbs[i]);
				this->thumbs.removeAt(i);
			}
			iter = this->bases.erase(iter);
		}
		this->bases.insert(to, base);

		this->cameraFrom = camera;
		this->cameraTo = this->bases.value(to);
		this->startedAt = now;
		this->target = to;
		this->settledSent = false;
		this->commitSent = false;
	}
	this->setClickThrough(false);
	this->settleRetries = 0;
}

bool DesktopSlide::prepareLiveOverlay(const QRect& monitor, const QList<HWND>& windows) {
	auto backdrops = backdropSources(monitor, windows, this->liveOverlay);
	if (this->liveOverlay != nullptr && this->liveMonitor == monitor
	    && this->liveBackdropSources == backdrops)
		return true;

	this->clearLiveOverlay();
	this->liveOverlay = createOverlay(monitor);
	if (this->liveOverlay == nullptr) return false;
	this->liveMonitor = monitor;
	this->liveBackdropSources = backdrops;
	this->liveWarmPending = true;
	for (auto* source: backdrops) addBackdrop(this->liveOverlay, source, monitor);
	return true;
}

HTHUMBNAIL DesktopSlide::liveThumbnail(HWND window) {
	if (auto registered = this->liveThumbnails.value(window)) return registered;
	HTHUMBNAIL thumbnail = nullptr;
	if (FAILED(DwmRegisterThumbnail(this->liveOverlay, window, &thumbnail))) return nullptr;
	if (!hideThumbnail(thumbnail)) {
		DwmUnregisterThumbnail(thumbnail);
		return nullptr;
	}
	this->liveThumbnails.insert(window, thumbnail);
	this->liveThumbnailOrder.append(window);
	return thumbnail;
}

void DesktopSlide::warmLiveThumbs() {
	if (this->overlay != nullptr) return;
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr) return;
	const auto monitor = cursorMonitor();
	if (monitor.isEmpty()) return;
	const auto windows = topLevelWindows();
	if (!this->prepareLiveOverlay(monitor, windows)) return;
	this->findPinned();

	for (auto iter = this->liveThumbnails.begin(); iter != this->liveThumbnails.end();) {
		if (tracker->windowFor(iter.key()) != nullptr) {
			++iter;
			continue;
		}
		DwmUnregisterThumbnail(iter.value());
		this->liveThumbnailOrder.removeAll(iter.key());
		iter = this->liveThumbnails.erase(iter);
	}
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* window = tracker->windowFor(windows[i]);
		if (window == nullptr || window->minimized() || Scratchpad::holds(window)) continue;
		this->liveThumbnail(windows[i]);
	}
	this->liveWarmPending = false;
}

void DesktopSlide::clearLiveOverlay() {
	if (this->liveOverlay != nullptr) DestroyWindow(this->liveOverlay);
	this->liveOverlay = nullptr;
	this->liveMonitor = {};
	this->liveBackdropSources.clear();
	this->liveThumbnails.clear();
	this->liveThumbnailOrder.clear();
}

bool DesktopSlide::carryPanels() {
	{
		auto lock = std::lock_guard(this->mutex);
		auto containsFullscreen = std::ranges::any_of(this->thumbs, [](const Thumb& thumb) {
			return thumb.desktop >= 0 && thumb.fullscreen;
		});
		if (!containsFullscreen) return false;
	}

	QList<Thumb> added;
	for (auto* hwnd: WinPanelWindow::aboveWindowHandles()) {
		auto rect = rectOf(hwnd).translated(-this->monitor.topLeft());
		if (rect.isEmpty()) continue;
		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags =
		    DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
		props.fVisible = TRUE;
		props.opacity = 255;
		props.fSourceClientAreaOnly = FALSE;
		props.rcDestination = toRect(rect);
		if (FAILED(DwmUpdateThumbnailProperties(thumbnail, &props))) {
			DwmUnregisterThumbnail(thumbnail);
			continue;
		}
		added.append({.thumbnail = thumbnail, .rect = rect, .desktop = -1, .window = nullptr, .visible = rect});
	}
	if (added.isEmpty()) return false;
	{
		auto lock = std::lock_guard(this->mutex);
		this->thumbs.append(added);
	}
	this->panelsCarried = true;
	this->liftFullscreen();
	qCDebug(logSlide) << "Carrying" << added.size() << "panels under a fullscreen window";
	return true;
}

void DesktopSlide::liftFullscreen(qsizetype desktop) {
	auto lock = std::lock_guard(this->mutex);
	for (auto& thumb: this->thumbs) {
		if (thumb.window == nullptr || thumb.desktop < 0) continue;
		if (desktop >= 0 && thumb.desktop != desktop) continue;
		if (!thumb.fullscreen) continue;
		DwmUnregisterThumbnail(thumb.thumbnail);
		this->liveThumbnails.remove(thumb.window);
		this->liveThumbnailOrder.removeAll(thumb.window);
		auto thumbnail = this->liveThumbnail(thumb.window);
		if (thumbnail == nullptr) {
			thumb.thumbnail = nullptr;
			thumb.desktop = -2;
			continue;
		}
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE;
		props.rcSource = toRect(thumb.source);
		props.fVisible = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		thumb.thumbnail = thumbnail;
		thumb.visible = QRect();
	}
}

void DesktopSlide::releaseThumb(const Thumb& thumb) {
	if (thumb.thumbnail == nullptr) return;
	if (thumb.window == nullptr) {
		DwmUnregisterThumbnail(thumb.thumbnail);
		return;
	}
	if (hideThumbnail(thumb.thumbnail)) return;
	DwmUnregisterThumbnail(thumb.thumbnail);
	this->liveThumbnails.remove(thumb.window);
	this->liveThumbnailOrder.removeAll(thumb.window);
}

void DesktopSlide::captureDialogs(qsizetype desktop) {
	for (auto iter = this->stills.begin(); iter != this->stills.end();) {
		if (IsWindow(iter.key())) {
			++iter;
			continue;
		}
		DestroyWindow(iter->host);
		iter = this->stills.erase(iter);
	}

	auto* tracker = WindowTracker::instance();
	for (auto* hwnd: topLevelWindows()) {
		auto* owner = GetWindow(hwnd, GW_OWNER);
		auto* window = owner == nullptr ? nullptr : tracker->windowFor(hwnd);
		if (window == nullptr || window->desktop() != desktop || window->minimized() || IsHungAppWindow(hwnd)) continue;
		if (Scratchpad::holds(window) || this->pinned.contains(hwnd)) continue;
		auto raw = rectOf(hwnd);
		RECT bounds {};
		if (raw.isEmpty() || !raw.intersects(this->monitor)
		    || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
			continue;
		auto frame = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
		auto source = frame.translated(-raw.topLeft());
		auto* host = windowStill(hwnd, raw, source, DwindleScene::cornerRadius(hwnd));
		if (host == nullptr) continue;
		RECT size {};
		GetWindowRect(host, &size);
		if (size.right - size.left == frame.width() && size.bottom - size.top == frame.height())
			source.moveTo(0, 0);
		if (auto old = this->stills.value(hwnd); old.host != nullptr) DestroyWindow(old.host);
		this->stills.insert(hwnd, {.host = host, .source = source, .frame = frame});
	}
}

void DesktopSlide::addDialogs(qsizetype desktop, QList<Thumb>& added) {
	auto* tracker = WindowTracker::instance();
	const auto viewport = QRect(QPoint(), this->monitor.size());
	for (auto iter = this->stills.cbegin(); iter != this->stills.cend(); ++iter) {
		if (!IsWindow(iter.key())) continue;
		auto* owner = tracker->windowFor(GetAncestor(iter.key(), GA_ROOTOWNER));
		if (owner == nullptr || owner->desktop() != desktop || owner->minimized()) continue;
		auto local = iter->frame.translated(-this->monitor.topLeft());
		auto clipped = local.intersected(viewport);
		if (clipped.isEmpty()) continue;
		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, iter->host, &thumbnail))) continue;
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
		props.rcSource = toRect(
		    QRect(iter->source.topLeft() + clipped.topLeft() - local.topLeft(), clipped.size())
		);
		props.opacity = 255;
		props.fVisible = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		added.append({.thumbnail = thumbnail, .rect = clipped, .desktop = desktop, .source = iter->source});
	}
}

bool DesktopSlide::addLiveDesktop(qsizetype desktop) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr || this->overlay == nullptr) return false;

	QSet<HWND> existing;
	{
		auto lock = std::lock_guard(this->mutex);
		for (const auto& thumb: this->thumbs) {
			if (thumb.window != nullptr) existing.insert(thumb.window);
		}
	}

	QList<HWND> sources;
	auto* tiling = TilingManager::active();
	const auto viewport = QRect(QPoint(), this->monitor.size());
	const auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (ownWindow(hwnd)) continue;
		auto* window = tracker->windowFor(hwnd);
		if (Scratchpad::holds(window) || this->pinned.contains(hwnd)) continue;
		if (window == nullptr || window->minimized() || window->desktop() != desktop) continue;
		auto rect = rectOf(hwnd);
		if (!rect.intersects(this->monitor)) continue;
		if (this->stills.contains(hwnd)) continue;
		if (tiling != nullptr && tiling->covers(window)) {
			sources.removeIf([tracker, window](HWND under) {
				return tracker->windowFor(under)->screen() == window->screen();
			});
		}
		if (!existing.contains(hwnd)) sources.append(hwnd);
	}

	qsizetype previous = -1;
	bool outOfOrder = false;
	for (auto* hwnd: sources) {
		auto registered = this->liveThumbnailOrder.indexOf(hwnd);
		if (registered < 0) registered = this->liveThumbnailOrder.size();
		if (registered < previous) outOfOrder = true;
		previous = registered;
	}
	if (outOfOrder) {
		for (auto* hwnd: sources) {
			if (auto old = this->liveThumbnails.take(hwnd)) DwmUnregisterThumbnail(old);
			this->liveThumbnailOrder.removeAll(hwnd);
		}
	}

	QList<Thumb> added;
	bool complete = true;
	for (auto* hwnd: sources) {
		auto rect = rectOf(hwnd);
		auto source = sourceOf(hwnd, rect);
		auto local = source.translated(rect.topLeft() - this->monitor.topLeft());
		auto backdrop = hasWindowBackdrop(hwnd);
		RECT bounds {};
		if ((backdrop || isChromium(hwnd)) && source.size() == rect.size()
		    && SUCCEEDED(
		        DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))
		    ))
		{
			QRect frame(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
			if (!frame.isEmpty()) {
				source = frame.translated(-rect.topLeft());
				local = frame.translated(-this->monitor.topLeft());
			}
		}
		auto clipped = local.intersected(viewport);
		if (clipped.isEmpty()) continue;
		source = QRect(source.topLeft() + clipped.topLeft() - local.topLeft(), clipped.size());

		auto thumbnail = this->liveThumbnail(hwnd);
		if (thumbnail == nullptr) {
			complete = false;
			continue;
		}
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE;
		props.rcSource = toRect(source);
		props.fVisible = FALSE;
		if (FAILED(DwmUpdateThumbnailProperties(thumbnail, &props))) {
			DwmUnregisterThumbnail(thumbnail);
			this->liveThumbnails.remove(hwnd);
			this->liveThumbnailOrder.removeAll(hwnd);
			complete = false;
			continue;
		}
		auto* tracked = tracker->windowFor(hwnd);
		auto mark = reinterpret_cast<INT_PTR>(GetPropW(hwnd, L"qs.fullscreen"));
		auto fullscreen = mark == 1 || mark == 2
		               || (tracked != nullptr && tracked->borderlessFullscreen());
		added.append({
		    .thumbnail = thumbnail,
		    .rect = clipped,
		    .desktop = desktop,
		    .window = hwnd,
		    .source = source,
		    .fullscreen = fullscreen,
		});
	}
	QList<DimWindow> stack;
	for (auto i = added.size() - 1; i >= 0; i--)
	{
		RECT bounds {};
		auto rect = added[i].rect;
		if (SUCCEEDED(DwmGetWindowAttribute(
		        added[i].window,
		        DWMWA_EXTENDED_FRAME_BOUNDS,
		        &bounds,
		        sizeof(bounds)
		    )))
		{
			rect &= QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
			            .translated(-this->monitor.topLeft());
		}
		stack.append({.id = reinterpret_cast<quintptr>(added[i].window), .rect = rect});
	}
	for (const auto& area: planDim(stack, 0)) {
		auto* hwnd = reinterpret_cast<HWND>(area.id); // NOLINT(performance-no-int-to-ptr)
		auto level = InactiveDim::levelOf(hwnd).to;
		if (level == 0) continue;
		for (const auto& part: area.region) {
			auto thumbnail = InactiveDim::shadeThumbnail(this->overlay, level);
			if (thumbnail == nullptr) continue;
			added.append({.thumbnail = thumbnail, .rect = part, .desktop = desktop});
		}
	}
	auto known = std::ranges::any_of(this->thumbs, [desktop](const Thumb& thumb) {
		return thumb.desktop == desktop && thumb.window == nullptr && thumb.source.isValid();
	});
	if (!known) this->addDialogs(desktop, added);
	auto lock = std::lock_guard(this->mutex);
	this->thumbs.append(added);
	return complete;
}

void DesktopSlide::layoutThumbs(double camera) {
	const auto viewport = QRect(QPoint(), this->monitor.size());
	auto panelsLeft = 0;
	auto panelsRight = viewport.width();
	for (const auto& thumb: this->thumbs) {
		if (thumb.desktop < 0 || !thumb.fullscreen) continue;
		auto base = this->bases.constFind(thumb.desktop);
		if (base == this->bases.cend()) continue;
		auto left = static_cast<int>(std::lround(base.value() - camera));
		auto right = left + viewport.width();
		if (right <= 0 || left >= viewport.width()) continue;
		if (left <= 0) panelsLeft = std::max(panelsLeft, right);
		else panelsRight = std::min(panelsRight, left);
	}
	const auto panelsArea = QRect(panelsLeft, 0, std::max(0, panelsRight - panelsLeft), viewport.height());
	for (auto& thumb: this->thumbs) {
		if (thumb.desktop == -1 && thumb.window == nullptr) {
			auto visible = thumb.rect.intersected(panelsArea);
			if (visible == thumb.visible) continue;
			DWM_THUMBNAIL_PROPERTIES props {};
			props.dwFlags = DWM_TNP_VISIBLE;
			props.fVisible = visible.isEmpty() ? FALSE : TRUE;
			if (props.fVisible) {
				props.dwFlags |= DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE;
				props.rcDestination = toRect(visible);
				props.rcSource = toRect(visible.translated(-thumb.rect.topLeft()));
			}
			if (SUCCEEDED(DwmUpdateThumbnailProperties(thumb.thumbnail, &props))) thumb.visible = visible;
			continue;
		}
		if (thumb.desktop < 0) continue;
		auto base = this->bases.constFind(thumb.desktop);
		if (base == this->bases.cend()) continue;
		auto offset = static_cast<int>(std::lround(base.value() - camera));
		auto rect = thumb.rect.translated(offset, 0);
		auto visible = rect.intersects(viewport) ? rect : QRect();
		if (visible == thumb.visible) continue;

		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_VISIBLE;
		props.fVisible = visible.isEmpty() ? FALSE : TRUE;
		if (props.fVisible) {
			props.dwFlags |= DWM_TNP_RECTDESTINATION;
			props.rcDestination = toRect(rect);
		}
		if (SUCCEEDED(DwmUpdateThumbnailProperties(thumb.thumbnail, &props))) thumb.visible = visible;
	}
}

void DesktopSlide::run() {
	auto last = std::chrono::steady_clock::now();
	auto period = refreshPeriodMs();
	while (!this->stop.load()) {
		do {
			waitForFrame();
		} while (!this->stop.load()
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last)
		                    .count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();
		auto lock = std::lock_guard(this->mutex);
		auto elapsed = std::chrono::duration<double, std::milli>(last - this->startedAt).count();
		auto progress = std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);
		this->layoutThumbs(this->manual ? this->manualCamera : camera);
		if (this->manual) continue;

		if (elapsed >= COMMIT_MS && !this->commitSent) {
			this->commitSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onCommit, Qt::QueuedConnection);
		}
		if (progress >= 1.0 && !this->settledSent) {
			this->settledSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onSettled, Qt::QueuedConnection);
		}
	}
}

void DesktopSlide::onCommit() {
	{
		auto lock = std::lock_guard(this->mutex);
		if (!this->commitSent || this->manual) return;
	}
	VirtualDesktops::instance()->commitSwitch();
	this->setClickThrough(true);
}

void DesktopSlide::setClickThrough(bool through) {
	if (this->overlay == nullptr) return;
	auto style = GetWindowLongPtrW(this->overlay, GWL_EXSTYLE);
	auto wanted =
	    through ? style | WS_EX_TRANSPARENT : style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
	if (wanted != style) SetWindowLongPtrW(this->overlay, GWL_EXSTYLE, wanted);
}

void DesktopSlide::onSettled() {
	qsizetype target = -1;
	std::chrono::steady_clock::time_point started;
	{
		auto lock = std::lock_guard(this->mutex);
		if (!this->settledSent) return;
		target = this->target;
		started = this->startedAt;
	}
	VirtualDesktops::instance()->commitSwitch();

	auto* desktops = VirtualDesktops::instance();
	if (desktops->currentIndex() != target && this->settleRetries++ < SETTLE_RETRIES) {
		QTimer::singleShot(SETTLE_RETRY_MS, this, &DesktopSlide::onSettled);
		return;
	}

	QTimer::singleShot(HOLD_MS, this, [this, target, started]() {
		{
			auto lock = std::lock_guard(this->mutex);
			if (!this->settledSent || this->target != target || this->startedAt != started) return;
		}
		this->finish();
	});
}

void DesktopSlide::findPinned() {
	this->pinned.clear();
	auto* desktops = VirtualDesktops::instance();
	auto* tracker = WindowTracker::instance();
	for (auto* hwnd: topLevelWindows()) {
		if (ownWindow(hwnd) || !topmost(hwnd)) continue;
		auto* window = tracker->windowFor(hwnd);
		if (window == nullptr || Scratchpad::holds(window) || window->minimized()) continue;
		if (desktops->isWindowPinned(hwnd)) this->pinned.insert(hwnd);
	}
}

void DesktopSlide::placeUnderPanels() {
	HWND lowest = nullptr;
	for (auto* hwnd: topLevelWindows()) {
		if (hwnd == this->overlay || !topmost(hwnd)) continue;
		if (ownWindow(hwnd) || this->pinned.contains(hwnd)) lowest = hwnd;
	}
	SetWindowPos(
	    this->overlay,
	    HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
	if (lowest != nullptr) {
		SetWindowPos(
		    this->overlay,
		    lowest,
		    0,
		    0,
		    0,
		    0,
		    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
		);
	}

	wchar_t lowestClass[64] {};
	if (lowest != nullptr) GetClassNameW(lowest, lowestClass, 64);
	HWND over = nullptr;
	for (auto* hwnd = GetWindow(this->overlay, GW_HWNDPREV); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDPREV)) {
		if (IsWindowVisible(hwnd) && !ownWindow(hwnd) && !topmost(hwnd)) {
			over = hwnd;
			break;
		}
	}
	qCDebug(logSlide) << "Cover placed under" << lowest << QString::fromWCharArray(lowestClass) << "topmost"
	                  << topmost(this->overlay) << "plain window over it" << over;
	if (over != nullptr) {
		SetWindowPos(
		    this->overlay,
		    HWND_TOPMOST,
		    0,
		    0,
		    0,
		    0,
		    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
		);
		qCInfo(logSlide) << "A window was over the cover; put the cover over everything";
	}
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
	this->wallDc = CreateCompatibleDC(screen);
	SelectObject(this->wallDc, this->wallBitmap);

	RECT fill {0, 0, width, height};
	FillRect(this->wallDc, &fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
	auto covered = this->hurried && !this->liveBackdropSources.isEmpty();
	if (auto* desktop = covered ? nullptr : GetShellWindow()) {
		RECT r {};
		GetWindowRect(desktop, &r);
		auto* full = CreateCompatibleDC(screen);
		auto* fullBitmap = CreateCompatibleBitmap(screen, r.right - r.left, r.bottom - r.top);
		auto* fullPrevious = SelectObject(full, fullBitmap);
		if (PrintWindow(desktop, full, PW_RENDERFULLCONTENT)) {
			BitBlt(
			    this->wallDc,
			    0,
			    0,
			    width,
			    height,
			    full,
			    this->monitor.x() - r.left,
			    this->monitor.y() - r.top,
			    SRCCOPY
			);
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
	UpdateLayeredWindow(
	    this->overlay,
	    screen,
	    &origin,
	    &size,
	    this->wallDc,
	    &source,
	    0,
	    &blend,
	    ULW_OPAQUE
	);
	ReleaseDC(nullptr, screen);
}

void DesktopSlide::releaseBackdrop() {
	if (this->wallDc != nullptr) DeleteDC(this->wallDc);
	if (this->wallBitmap != nullptr) DeleteObject(this->wallBitmap);
	this->wallDc = nullptr;
	this->wallBitmap = nullptr;
}

void DesktopSlide::finish() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	if (this->panelsCarried) {
		WinPanelWindow::setCoverOverPanels(false);
		this->panelsCarried = false;
	}
	if (this->overlay != nullptr) {
		ShowWindow(this->overlay, SW_HIDE);
		this->overlay = nullptr;
		InactiveDim::instance()->comeBack();
		this->overlay = this->liveOverlay;
		DwmFlush();
	}
	for (const auto& thumb: this->thumbs) this->releaseThumb(thumb);
	this->thumbs.clear();
	this->bases.clear();
	this->target = -1;
	this->settledSent = false;
	this->commitSent = false;
	this->releaseBackdrop();
	if (this->overlay == nullptr) return;
	this->overlay = nullptr;
	WinPanelWindow::setCoverActive(false);
	WinPanelWindow::restack(this->raised);
	this->raised.clear();
	if (this->liveWarmPending) this->liveWarmTimer.start(0);
}

void waitForCompositorFrame() { waitForFrame(); }
double compositorRefreshMs() { return refreshPeriodMs(); }

}
