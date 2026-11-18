#include "window_motion.hpp"
#include <algorithm>
#include <cmath>

#include <qcoreapplication.h>
#include <qpointer.h>

#include "desktop_slide.hpp"

namespace qs::windows {

namespace {

// end-4: windowsMove, speed 3 (300 ms), emphasizedDecel.
constexpr double MOVE_MS = 300;
constexpr double CURVE_X1 = 0.05;
constexpr double CURVE_Y1 = 0.7;
constexpr double CURVE_X2 = 0.1;
constexpr double CURVE_Y2 = 1.0;
constexpr auto* COVER_CLASS = L"QuickshellWindowMotion";
constexpr COLORREF COVER_KEY = RGB(255, 0, 255);
// An app that takes longer than this to leave slides anyway.
constexpr double TAKEOFF_TIMEOUT_MS = 250;

double bezierAt(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double ease(double x) {
	auto lo = 0.0;
	auto hi = 1.0;
	for (auto i = 0; i < 40; i++) {
		auto mid = (lo + hi) / 2;
		if (bezierAt(CURVE_X1, CURVE_X2, mid) < x) lo = mid;
		else hi = mid;
	}
	return bezierAt(CURVE_Y1, CURVE_Y2, (lo + hi) / 2);
}

QRect lerp(const QRect& a, const QRect& b, double t) {
	auto mix = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
	return QRect(
	    QPoint(mix(a.left(), b.left()), mix(a.top(), b.top())),
	    QSize(std::max(1, mix(a.width(), b.width())), std::max(1, mix(a.height(), b.height())))
	);
}

// Whether `window` is above `reference` in the z-order.
bool isAbove(HWND window, HWND reference) {
	for (auto* hwnd = GetWindow(reference, GW_HWNDPREV); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDPREV)) {
		if (hwnd == window) return true;
	}
	return false;
}

QPointer<WindowMotion> MOTION; // NOLINT

} // namespace

WindowMotion* WindowMotion::instance() {
	if (MOTION.isNull()) MOTION = new WindowMotion(QCoreApplication::instance());
	return MOTION.data();
}

WindowMotion::WindowMotion(QObject* parent): QObject(parent) {}

WindowMotion::~WindowMotion() {
	// Never leave a window waiting off screen.
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	for (const auto& entry: this->entries) {
		if (IsWindow(entry.hwnd)) moveTo(entry.hwnd, entry.to, true);
	}
	this->closeCover();
}

bool WindowMotion::moving(HWND hwnd) const {
	auto lock = std::lock_guard(this->mutex);
	return std::ranges::any_of(this->entries, [hwnd](const Entry& e) { return e.hwnd == hwnd; });
}

bool WindowMotion::ensureCover() {
	if (this->cover != nullptr) return true;

	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &DefWindowProcW;
		cls.hInstance = instance;
		cls.hbrBackground = CreateSolidBrush(COVER_KEY);
		cls.lpszClassName = COVER_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}

	this->coverRect = QRect(
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN),
	    std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
	    std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN))
	);
	this->cover = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    COVER_CLASS,
	    L"",
	    WS_POPUP,
	    this->coverRect.x(),
	    this->coverRect.y(),
	    this->coverRect.width(),
	    this->coverRect.height(),
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (this->cover == nullptr) return false;
	SetLayeredWindowAttributes(this->cover, COVER_KEY, 255, LWA_COLORKEY);
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(this->cover, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
	return true;
}

// Just above the highest moving window: windows over it (floating ones, the
// shell's panels) stay over its thumbnail.
void WindowMotion::placeCover(HWND above) {
	if (this->cover == nullptr || !IsWindowVisible(this->cover) || isAbove(above, this->cover)) {
		auto* previous = GetWindow(above, GW_HWNDPREV);
		if (previous == this->cover) return;
		SetWindowPos(
		    this->cover,
		    previous != nullptr ? previous : HWND_TOP,
		    0,
		    0,
		    0,
		    0,
		    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
		);
	}
}

void WindowMotion::moveTo(HWND hwnd, const QRect& frame, bool wait) {
	RECT raw {};
	RECT bounds {};
	if (!GetWindowRect(hwnd, &raw)) return;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) bounds = raw;
	auto left = bounds.left - raw.left;
	auto top = bounds.top - raw.top;
	auto right = raw.right - bounds.right;
	auto bottom = raw.bottom - bounds.bottom;
	auto flags = UINT(SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
	// A hung app would hold the shell up; it moves when it wakes instead.
	if (!wait || IsHungAppWindow(hwnd)) flags |= SWP_ASYNCWINDOWPOS;
	SetWindowPos(
	    hwnd,
	    nullptr,
	    frame.left() - left,
	    frame.top() - top,
	    frame.width() + left + right,
	    frame.height() + top + bottom,
	    flags
	);
}

// Called with the mutex held. The source follows the window's own frame, so it
// shows the window at whatever size it has reached.
void WindowMotion::layout(Entry& entry, double progress) {
	entry.current = lerp(entry.from, entry.to, ease(progress));
	RECT raw {};
	RECT bounds {};
	if (!GetWindowRect(entry.hwnd, &raw)
	    || FAILED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
	{
		return;
	}
	auto dest = entry.current.translated(-this->coverRect.topLeft());
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY
	              | DWM_TNP_SOURCECLIENTAREAONLY;
	props.rcSource = {bounds.left - raw.left, bounds.top - raw.top, bounds.right - raw.left, bounds.bottom - raw.top};
	props.rcDestination = {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
	props.opacity = 255;
	props.fVisible = TRUE;
	props.fSourceClientAreaOnly = FALSE;
	DwmUpdateThumbnailProperties(entry.thumbnail, &props);
}

bool WindowMotion::begin(HWND hwnd, const QRect& from, const QRect& to, const QRect& parked) {
	if (this->moving(hwnd)) {
		this->retarget(hwnd, to, parked);
		return true;
	}
	if (!this->ensureCover()) return false;

	HTHUMBNAIL thumbnail = nullptr;
	if (FAILED(DwmRegisterThumbnail(this->cover, hwnd, &thumbnail))) {
		if (this->entries.isEmpty()) this->closeCover();
		return false;
	}

	{
		auto lock = std::lock_guard(this->mutex);
		this->entries.append(Entry {
		    .hwnd = hwnd,
		    .thumbnail = thumbnail,
		    .from = from,
		    .to = to,
		    .current = from,
		    .parked = parked,
		    .start = std::chrono::steady_clock::now(),
		});
		this->layout(this->entries.last(), 0);
	}

	// The thumbnail is in the window's place before the window leaves it, in
	// the same compositor frame.
	this->placeCover(hwnd);
	if (!IsWindowVisible(this->cover)) ShowWindow(this->cover, SW_SHOWNOACTIVATE);
	moveTo(hwnd, parked, false);

	if (!this->worker.joinable()) {
		this->stop.store(false);
		this->worker = std::thread(&WindowMotion::run, this);
	}
	return true;
}

void WindowMotion::retarget(HWND hwnd, const QRect& to, const QRect& parked) {
	{
		auto lock = std::lock_guard(this->mutex);
		auto entry = std::ranges::find_if(this->entries, [hwnd](const Entry& e) { return e.hwnd == hwnd; });
		if (entry == this->entries.end()) return;
		if (entry->to == to && !entry->done) return;
		entry->from = entry->current;
		entry->to = to;
		entry->parked = parked;
		entry->start = std::chrono::steady_clock::now();
		entry->done = false;
	}
	moveTo(hwnd, parked, false);
}

void WindowMotion::cancel(HWND hwnd) {
	auto empty = false;
	{
		auto lock = std::lock_guard(this->mutex);
		for (auto i = this->entries.size() - 1; i >= 0; i--) {
			if (this->entries[i].hwnd != hwnd) continue;
			DwmUnregisterThumbnail(this->entries[i].thumbnail);
			this->entries.removeAt(i);
		}
		empty = this->entries.isEmpty();
	}
	if (empty) this->closeCover();
}

void WindowMotion::run() {
	auto last = std::chrono::steady_clock::now();
	auto period = compositorRefreshMs();
	while (!this->stop.load()) {
		do {
			waitForCompositorFrame();
		} while (!this->stop.load()
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();

		auto finished = false;
		{
			auto lock = std::lock_guard(this->mutex);
			for (auto& entry: this->entries) {
				if (entry.done) continue;
				if (entry.waiting) {
					RECT raw {};
					auto left = GetWindowRect(entry.hwnd, &raw) && raw.left >= entry.parked.left() - 64;
					auto waited = std::chrono::duration<double, std::milli>(last - entry.start).count();
					if (!left && waited < TAKEOFF_TIMEOUT_MS) continue;
					entry.waiting = false;
					entry.start = last;
				}
				auto elapsed = std::chrono::duration<double, std::milli>(last - entry.start).count();
				auto progress = std::clamp(elapsed / MOVE_MS, 0.0, 1.0);
				this->layout(entry, progress);
				if (progress >= 1.0) {
					entry.done = true;
					finished = true;
				}
			}
		}
		if (finished && !this->donePending.exchange(true)) {
			QMetaObject::invokeMethod(this, &WindowMotion::onFrameDone, Qt::QueuedConnection);
		}
	}
}

// Thumbnail visibility reaches the screen right away, a window move a frame
// later at most: thumbnails go first, then the windows jump into place.
void WindowMotion::onFrameDone() {
	this->donePending.store(false);
	QList<Entry> landing;
	auto empty = false;
	{
		auto lock = std::lock_guard(this->mutex);
		for (auto i = this->entries.size() - 1; i >= 0; i--) {
			if (!this->entries[i].done) continue;
			DWM_THUMBNAIL_PROPERTIES props {};
			props.dwFlags = DWM_TNP_VISIBLE;
			props.fVisible = FALSE;
			DwmUpdateThumbnailProperties(this->entries[i].thumbnail, &props);
			landing.append(this->entries.takeAt(i));
		}
		empty = this->entries.isEmpty();
	}
	for (const auto& entry: landing) {
		if (IsWindow(entry.hwnd)) moveTo(entry.hwnd, entry.to, true);
	}
	for (const auto& entry: landing) {
		DwmUnregisterThumbnail(entry.thumbnail);
		emit this->landed(entry.hwnd, entry.to);
	}
	if (empty) this->closeCover();
}

void WindowMotion::closeCover() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	{
		auto lock = std::lock_guard(this->mutex);
		for (const auto& entry: this->entries) DwmUnregisterThumbnail(entry.thumbnail);
		this->entries.clear();
	}
	if (this->cover != nullptr) {
		DestroyWindow(this->cover);
		this->cover = nullptr;
	}
}

} // namespace qs::windows
