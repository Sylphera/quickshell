#include "window_motion.hpp"
#include "util.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

#include <qcoreapplication.h>
#include <qpointer.h>
#include <qtimer.h>

#include "desktop_slide.hpp"

namespace qs::windows {

namespace {

// end-4: windowsMove, speed 3 (300 ms), emphasizedDecel.
constexpr double MOVE_MS = 300;
// How long a resized window shows how it looked before its live picture.
constexpr double HOLD_MS = 50;
constexpr double CURVE_X1 = 0.05;
constexpr double CURVE_Y1 = 0.7;
constexpr double CURVE_X2 = 0.1;
constexpr double CURVE_Y2 = 1.0;
constexpr auto* COVER_CLASS = L"QuickshellWindowMotion";
constexpr COLORREF COVER_KEY = RGB(255, 0, 255);
// An app that takes longer than this to leave slides anyway.
constexpr double TAKEOFF_TIMEOUT_MS = 0;
// Two compositor frames for an app to repaint at its new size before sliding.
constexpr int REPAINT_SETTLE_MS = 33;
// How long a still stays over a window that landed, while it paints again.
constexpr int LINGER_MS = 400;
// How long a live picture stays over its landed window: about a frame.
constexpr int LAND_HANDOFF_MS = 20;
// How long the scene stays once everything has landed.
constexpr int COVER_LINGER_MS = 0;

double bezierAt(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

// Chromium and Electron stop painting a window that is off screen, and drop
// its picture a moment later: a thumbnail of it goes blank mid-slide. Like the
// desktop slide, they travel as a still taken before they leave.
bool paintsOnlyOnScreen(HWND hwnd) {
	wchar_t cls[64] {};
	GetClassNameW(hwnd, cls, 64);
	return wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0;
}

HWND snapshotOf(const QRect& frame) {
	auto width = frame.width();
	auto height = frame.height();
	if (width <= 0 || height <= 0) return nullptr;

	static const auto* className = []() {
		WNDCLASSW wc {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"QuickshellMotionSnapshot";
		RegisterClassW(&wc);
		return L"QuickshellMotionSnapshot";
	}();
	auto x = GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 100;
	auto* host = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
	    className,
	    L"",
	    WS_POPUP,
	    x,
	    frame.top(),
	    width,
	    height,
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
	if (host == nullptr) return nullptr;

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
	auto* old = SelectObject(dc, bitmap);
	BitBlt(dc, 0, 0, width, height, screen, frame.x(), frame.y(), SRCCOPY);
	GdiFlush();
	auto* bits = static_cast<quint32*>(raw);
	for (qsizetype i = 0; i < static_cast<qsizetype>(width) * height; i++)
		bits[i] |= 0xff000000; // NOLINT

	POINT origin {x, frame.top()};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(host, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
	SelectObject(dc, old);
	DeleteDC(dc);
	DeleteObject(bitmap);
	ReleaseDC(nullptr, screen);
	ShowWindow(host, SW_SHOWNOACTIVATE);
	return host;
}

} // namespace

bool WindowMotion::travelsAsStill(HWND hwnd) { return paintsOnlyOnScreen(hwnd); }

namespace {

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

// How long a window that was asked to close may take before it comes back.
constexpr int CLOSE_GIVE_BACK_MS = 1500;

QRect lerp(const QRect& a, const QRect& b, double t) {
	auto mix = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
	return QRect(
	    QPoint(mix(a.left(), b.left()), mix(a.top(), b.top())),
	    QSize(std::max(1, mix(a.width(), b.width())), std::max(1, mix(a.height(), b.height())))
	);
}

// Whether `window` is above `reference` in the z-order.
bool isAbove(HWND window, HWND reference) {
	for (auto* hwnd = GetWindow(reference, GW_HWNDPREV); hwnd != nullptr;
	     hwnd = GetWindow(hwnd, GW_HWNDPREV))
	{
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
		if (!IsWindow(entry.hwnd)) continue;
		moveTo(entry.hwnd, entry.to, true);
		// Chromium resized for a second target while nearly all off screen took
		// itself for hidden and came back grey. A frame change, without a
		// resize (that showed an empty frame), makes it lay out and paint.
		if (entry.retargeted && travelsAsStill(entry.hwnd)) {
			SetWindowPos(entry.hwnd, nullptr, 0, 0, 0, 0,
			    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			RedrawWindow(entry.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
		}
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

	// One pixel short of the screens at the top: a window covering a monitor
	// exactly is taken by Explorer for a fullscreen app, and the shell's
	// panels step down behind everything for as long as it is there.
	this->coverRect = QRect(
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN) + 1,
	    std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
	    std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1)
	);
	this->cover = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
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
	DwmSetWindowAttribute(
	    this->cover,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);
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
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
		bounds = raw;
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

namespace {

// How much of a picture of `source` to show in `frame` without stretching it,
// as Winland does: one scale for both sides, just enough to cover the frame,
// with the picture held at its top left and cut off on the right and bottom.
QSize coveringPart(const QSize& source, const QSize& frame) {
	if (source.isEmpty() || frame.isEmpty()) return source;
	auto scale = std::max(
	    static_cast<double>(frame.width()) / source.width(),
	    static_cast<double>(frame.height()) / source.height()
	);
	return {
	    std::min(source.width(), static_cast<int>(std::lround(frame.width() / scale))),
	    std::min(source.height(), static_cast<int>(std::lround(frame.height() / scale)))
	};
}

} // namespace

// Called with the mutex held. The source follows the window's own frame, so it
// shows the window at whatever size it has reached.
void WindowMotion::layout(Entry& entry, double progress) {
	entry.progress = progress;
	auto eased = ease(progress);
	entry.current = lerp(entry.from, entry.to, eased);
	// windowsIn: popin 80% and fadeIn, both on the same curve.
	auto opacity = 255.0;
	auto shown = entry.current;
	if (entry.pop) {
		auto scale = 0.8 + 0.2 * eased;
		auto size = QSizeF(entry.to.size()) * scale;
		auto center = QRectF(entry.to).center();
		shown = QRectF(
		            center.x() - size.width() / 2,
		            center.y() - size.height() / 2,
		            size.width(),
		            size.height()
		)
		            .toRect();
		entry.current = entry.to;
		opacity = 255.0 * eased;
	}
	if (entry.snapshot != nullptr) {
		// The whole still, stretched to where the window is on its way.
		auto dest = shown.translated(-this->coverRect.topLeft());
		auto part = entry.snapshotSize;
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
		props.rcSource = {0, 0, part.width(), part.height()};
		props.rcDestination =
		    {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
		props.opacity = static_cast<BYTE>(std::clamp(std::lround(opacity), 0L, 255L));
		props.fVisible = TRUE;
		DwmUpdateThumbnailProperties(entry.thumbnail, &props);
		return;
	}
	RECT raw {};
	RECT bounds {};
	if (!GetWindowRect(entry.hwnd, &raw)
	    || FAILED(
	        DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))
	    ))
	{
		return;
	}
	auto dest = shown.translated(-this->coverRect.topLeft());
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY
	              | DWM_TNP_SOURCECLIENTAREAONLY;
	auto frame = QSize(bounds.right - bounds.left, bounds.bottom - bounds.top);
	// The whole window, stretched to where it is on its way, as Hyprland
	// stretches its buffer: it has its new size already, so it lands at 1:1.
	auto part = frame;
	props.rcSource = {
	    bounds.left - raw.left,
	    bounds.top - raw.top,
	    bounds.left - raw.left + part.width(),
	    bounds.top - raw.top + part.height()
	};
	props.rcDestination =
	    {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
	props.opacity = static_cast<BYTE>(std::clamp(std::lround(opacity), 0L, 255L));
	props.fVisible = TRUE;
	props.fSourceClientAreaOnly = FALSE;
	DwmUpdateThumbnailProperties(entry.thumbnail, &props);

	if (entry.holdThumbnail != nullptr) {
		auto since = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry.holdStart).count();
		DWM_THUMBNAIL_PROPERTIES hold {};
		hold.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
		hold.rcSource = {0, 0, entry.snapshotSize.width(), entry.snapshotSize.height()};
		hold.rcDestination = props.rcDestination;
		hold.opacity = 255;
		hold.fVisible = since < HOLD_MS ? TRUE : FALSE;
		DwmUpdateThumbnailProperties(entry.holdThumbnail, &hold);
	}
}

namespace {
int depthOf(HWND hwnd) {
	auto depth = 0;
	for (auto* above = GetWindow(hwnd, GW_HWNDPREV); above != nullptr; above = GetWindow(above, GW_HWNDPREV))
		depth++;
	return depth;
}
} // namespace

// Called with the mutex held. DWM draws thumbnails in the order they were
// registered, which was the order the windows set off in: one sliding over
// another that sits above it. They are registered again, the deepest first.
// Called with the mutex held. A window wholly under the cover was taken by
// Windows for hidden and stopped painting, then showed blank for a frame as
// the cover went. The cover leaves one pixel of each window uncovered.
void WindowMotion::updateHoles() {
	if (this->cover == nullptr) return;
	auto* region = CreateRectRgn(0, 0, this->coverRect.width(), this->coverRect.height());
	auto hole = [this, region](const QRect& frame) {
		if (frame.width() < 24 || frame.height() < 8) return;
		auto x = frame.left() + 12 - this->coverRect.left();
		auto y = frame.bottom() - 1 - this->coverRect.top();
		auto* dot = CreateRectRgn(x, y, x + 1, y + 1);
		CombineRgn(region, region, dot, RGN_DIFF);
		DeleteObject(dot);
	};
	for (const auto& entry: this->entries) hole(entry.to);
	for (auto it = this->scene.constBegin(); it != this->scene.constEnd(); ++it) {
		RECT bounds {};
		if (SUCCEEDED(DwmGetWindowAttribute(it.key(), DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
			hole(QRect(QPoint(bounds.left, bounds.top), QPoint(bounds.right - 1, bounds.bottom - 1)));
		}
	}
	SetWindowRgn(this->cover, region, FALSE); // the window owns the region now
}

void WindowMotion::placeCoverUnderPanels() {
	HWND lowest = nullptr;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (hwnd == this->cover || !IsWindowVisible(hwnd)) continue;
		if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) break;
		if (isOwnProcessWindow(hwnd)) lowest = hwnd;
	}
	// Straight under the lowest panel: put at the very top first, a cover
	// already on screen hid the bar and the panels for a frame.
	constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
	SetWindowPos(this->cover, lowest != nullptr ? lowest : HWND_TOPMOST, 0, 0, 0, 0, flags);
}

// Called with the mutex held. The cover draws the whole scene while windows
// move: the wallpaper and every visible window, bottom to top as they are
// stacked, the moving ones sliding and the rest where they are. The real
// windows meanwhile sit in their new tiles under it, on screen, so they paint
// at their new size (moved off screen, apps stopped painting: grey Chromium,
// an empty File Explorer). New thumbnails go up before the old ones go, so
// no frame is left without one.
void WindowMotion::restack() {
	QList<HWND> order;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
		order.prepend(hwnd);
	auto screen = this->coverRect.adjusted(0, -1, 0, 0);
	QHash<HWND, HTHUMBNAIL> scene;
	QList<HTHUMBNAIL> old;
	for (auto& entry: this->entries) old.append(entry.thumbnail);
	for (auto* thumbnail: std::as_const(this->scene)) old.append(thumbnail);

	for (auto* hwnd: order) {
		auto entry = std::ranges::find_if(this->entries, [hwnd](const Entry& e) { return e.hwnd == hwnd; });
		if (entry != this->entries.end()) {
			HTHUMBNAIL thumbnail = nullptr;
			auto* source = entry->snapshot != nullptr ? entry->snapshot : entry->hwnd;
			if (FAILED(DwmRegisterThumbnail(this->cover, source, &thumbnail))) continue;
			entry->thumbnail = thumbnail;
			this->layout(*entry, entry->progress);
			continue;
		}
		if (hwnd == this->cover || !IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
		if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 && isOwnProcessWindow(hwnd)) continue;
		DWORD cloaked = 0;
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
			continue;
		wchar_t cls[64] {};
		GetClassNameW(hwnd, cls, 64);
		if (wcsncmp(cls, L"QuickshellMotion", 16) == 0 || wcscmp(cls, L"QuickshellWindowPop") == 0) continue;
		RECT raw {};
		if (!GetWindowRect(hwnd, &raw)) continue;
		// The visible frame, without the invisible resize borders: drawn into
		// the raw rect, the picture was stretched past its tile.
		RECT bounds {};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) bounds = raw;
		auto rect = QRect(QPoint(bounds.left, bounds.top), QPoint(bounds.right - 1, bounds.bottom - 1));
		if (rect.isEmpty() || !rect.intersects(screen)) continue;
		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->cover, hwnd, &thumbnail))) continue;
		auto dest = rect.translated(-this->coverRect.topLeft());
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.rcSource = {
		    bounds.left - raw.left,
		    bounds.top - raw.top,
		    bounds.right - raw.left,
		    bounds.bottom - raw.top
		};
		props.rcDestination = {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
		props.opacity = 255;
		props.fVisible = TRUE;
		props.fSourceClientAreaOnly = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		scene.insert(hwnd, thumbnail);
	}
	for (auto* thumbnail: old) DwmUnregisterThumbnail(thumbnail);
	this->scene = scene;
}

bool WindowMotion::begin(
    HWND hwnd,
    const QRect& from,
    const QRect& to,
    const QRect& parked,
    bool pop,
    bool still
) {
	if (this->moving(hwnd)) {
		this->retarget(hwnd, to, parked);
		return true;
	}
	if (!this->ensureCover()) return false;

	HWND snapshot = !pop && still ? snapshotOf(from) : nullptr;
	auto snapshotSize = from.size();
	// With the scene on screen it isn't built again: registered anew from
	// the bottom, the new wallpaper went over every old window thumbnail and
	// the whole layout vanished for a frame. A window already in the scene
	// starts moving with its own thumbnail, in its place in the stack.
	auto onScreen = IsWindowVisible(this->cover) != FALSE;
	// Only a window that changes size gets the empty frame.
	HWND hold = !pop && snapshot == nullptr && from.size() != to.size() ? snapshotOf(from) : nullptr;
	HTHUMBNAIL thumbnail = nullptr;
	if (onScreen && snapshot == nullptr) {
		auto lock = std::lock_guard(this->mutex);
		thumbnail = this->scene.take(hwnd);
		// Landed a moment ago, its picture still over it: that one moves on,
		// or a second picture of it was made and the first stayed behind.
		if (thumbnail == nullptr) {
			auto pending = std::ranges::find_if(this->handoff, [hwnd](const Entry& e) { return e.hwnd == hwnd; });
			if (pending != this->handoff.end()) {
				thumbnail = pending->thumbnail;
				releaseHold(*pending);
				this->handoff.erase(pending);
			}
		}
	}
	if (thumbnail == nullptr
	    && FAILED(DwmRegisterThumbnail(this->cover, snapshot != nullptr ? snapshot : hwnd, &thumbnail)))
	{
		if (snapshot != nullptr) DestroyWindow(snapshot);
		if (this->entries.isEmpty()) this->closeCover();
		return false;
	}

	{
		auto lock = std::lock_guard(this->mutex);
		this->entries.append(Entry {
		    .hwnd = hwnd,
		    .thumbnail = thumbnail,
		    .snapshot = snapshot,
		    .snapshotSize = snapshotSize,
		    .from = from,
		    .to = to,
		    .current = from,
		    .parked = parked,
		    .pop = pop,
		    .start = std::chrono::steady_clock::now(),
		    // A still covers the window already: no need to wait for it to leave,
		    // which a busy app (Chromium repainting after a landing) takes long to.
		    .waiting = snapshot == nullptr,
		    .depth = depthOf(hwnd),
		    .hold = hold,
		    .holdStart = std::chrono::steady_clock::now(),
		});
		this->layout(this->entries.last(), 0);
		if (!onScreen) this->restack();
		this->updateHoles();
		// Registered last, so over its live picture.
		auto& added = this->entries.last();
		if (added.hold != nullptr && FAILED(DwmRegisterThumbnail(this->cover, added.hold, &added.holdThumbnail))) {
			added.holdThumbnail = nullptr;
		}
		this->layout(added, 0);
	}

	// The scene is up, over every app window and under the shell's panels,
	// before the window moves to its new tile. In the topmost band: placed
	// only over the plain windows, the moving ones rose over it as tiling
	// placed them, and slid behind a frozen scene.
	if (!IsWindowVisible(this->cover)) {
		this->placeCoverUnderPanels();
		ShowWindow(this->cover, SW_SHOWNOACTIVATE);
	}
	// The scene reaches the screen before any window moves under it: DWM can
	// show the wallpaper a frame before the windows' thumbnails, and a window
	// that had already left its place was gone for that frame.
	DwmFlush();
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
		auto entry =
		    std::ranges::find_if(this->entries, [hwnd](const Entry& e) { return e.hwnd == hwnd; });
		if (entry == this->entries.end()) return;
		if (entry->to == to && !entry->done) return;
		entry->parked = parked;
		entry->retargeted = true;
		if (entry->pop && !entry->done) {
			// Still popping in: keep growing, toward the new tile.
			entry->from = to;
			entry->to = to;
		} else {
			entry->pop = false;
			entry->from = entry->current;
			entry->to = to;
			entry->start = std::chrono::steady_clock::now();
		}
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
			release(this->entries[i]);
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
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last)
		                    .count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();

		auto finished = false;
		{
			auto lock = std::lock_guard(this->mutex);
			// Windows that move together set off together: each one starting
			// once its own app had left put them out of step, one sliding over
			// another that hadn't moved yet. They wait for the slowest (up to
			// the timeout) and start in the same frame.
			auto holding = false;
			for (auto& entry: this->entries) {
				if (entry.done || !entry.waiting) continue;
				RECT raw {};
				RECT bounds {};
				auto left = GetWindowRect(entry.hwnd, &raw) && raw.left >= entry.parked.left() - 64;
				// And has taken the size it waits at: before that, its picture
				// is too small for the frame and leaves a gap.
				if (left && !entry.pop
				    && SUCCEEDED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
				{
					left = bounds.right - bounds.left >= entry.parked.width() - 2
					    && bounds.bottom - bounds.top >= entry.parked.height() - 2;
				}
				auto waited = std::chrono::duration<double, std::milli>(last - entry.start).count();
				if (!left && waited < TAKEOFF_TIMEOUT_MS) holding = true;
			}
			for (auto& entry: this->entries) {
				if (entry.done) continue;
				if (entry.waiting) {
					if (holding) continue;
					entry.waiting = false;
					// Hold still a moment longer: the app repaints at its new size
					// while in place, instead of during the curve's fast start.
					entry.start = last + std::chrono::milliseconds(REPAINT_SETTLE_MS);
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
			landing.append(this->entries.takeAt(i));
		}
		empty = this->entries.isEmpty();
	}
	for (const auto& entry: landing) {
		if (IsWindow(entry.hwnd)) moveTo(entry.hwnd, entry.to, true);
	}
	// The windows are in place before their pictures go, a frame later:
	// hiding the picture first left a frame with neither (the window
	// vanished for a frame). The picture ends at 1:1 over its window.
	QList<Entry> live;
	for (const auto& entry: landing) {
		// An app that stopped painting while away comes back blank for a few
		// frames: its still stays over it, in place, until it has painted.
		if (entry.snapshot != nullptr) this->lingering.append(entry);
		else live.append(entry);
		emit this->landed(entry.hwnd, entry.to);
	}
	if (!live.isEmpty()) {
		this->landingLive += static_cast<int>(live.size());
		{
			auto lock = std::lock_guard(this->mutex);
			this->handoff.append(live);
		}
		QTimer::singleShot(LAND_HANDOFF_MS, this, [this, live]() {
			QList<Entry> done;
			{
				// Others still moving: the cover stays, so its picture stays too,
				// at rest in its tile with the rest of the scene. A window that
				// set off again meanwhile took its picture along already.
				auto lock = std::lock_guard(this->mutex);
				for (const auto& entry: live) {
					auto pending = std::ranges::find_if(this->handoff, [&entry](const Entry& e) {
						return e.hwnd == entry.hwnd && e.thumbnail == entry.thumbnail;
					});
					if (pending == this->handoff.end()) continue;
					this->handoff.erase(pending);
					if (!this->entries.isEmpty() && entry.snapshot == nullptr && !this->scene.contains(entry.hwnd)) {
						this->scene.insert(entry.hwnd, entry.thumbnail);
					} else {
						done.append(entry);
					}
				}
			}
			this->landingLive -= static_cast<int>(live.size());
			auto lock = std::unique_lock(this->mutex);
			auto none = this->entries.isEmpty();
			lock.unlock();
			// The cover goes before the moved windows' pictures: dropped first,
			// the wallpaper showed in their place for a frame, all at once.
			if (none && this->lingering.isEmpty() && this->landingLive == 0) {
				// The scene stays a little after everything landed, pictures at
				// 1:1 over their windows: closed at once, a window that had moved
				// under it hadn't painted there yet and the wallpaper showed in
				// its place for a frame.
				QTimer::singleShot(COVER_LINGER_MS, this, [this, done]() {
					auto lock = std::unique_lock(this->mutex);
					auto still = this->entries.isEmpty();
					lock.unlock();
					if (still && this->lingering.isEmpty() && this->landingLive == 0) this->closeCover();
					for (const auto& entry: done) release(entry);
				});
				return;
			}
			for (const auto& entry: done) release(entry);
		});
	}
	if (!this->lingering.isEmpty()) {
		QTimer::singleShot(LINGER_MS, this, [this]() {
			for (const auto& entry: this->lingering) release(entry);
			this->lingering.clear();
			auto lock = std::unique_lock(this->mutex);
			auto none = this->entries.isEmpty();
			lock.unlock();
			if (none && this->landingLive == 0) this->closeCover();
		});
	}
	if (empty && this->lingering.isEmpty() && this->landingLive == 0) this->closeCover();
}

void WindowMotion::release(const Entry& entry) {
	DwmUnregisterThumbnail(entry.thumbnail);
	if (entry.snapshot != nullptr) DestroyWindow(entry.snapshot);
	releaseHold(entry);
}

void WindowMotion::releaseHold(const Entry& entry) {
	if (entry.holdThumbnail != nullptr) DwmUnregisterThumbnail(entry.holdThumbnail);
	if (entry.hold != nullptr) DestroyWindow(entry.hold);
}

void WindowMotion::closeCover() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	{
		auto lock = std::lock_guard(this->mutex);
		for (const auto& entry: this->entries) release(entry);
		this->entries.clear();
		for (const auto& entry: this->lingering) release(entry);
		this->lingering.clear();
	}
	for (auto* thumbnail: std::as_const(this->scene)) DwmUnregisterThumbnail(thumbnail);
	this->scene.clear();
	if (this->cover != nullptr) {
		DestroyWindow(this->cover);
		this->cover = nullptr;
	}
}

namespace {

// end-4: windowsIn speed 3 (300 ms), windowsOut speed 2 (200 ms), both
// emphasizedDecel; popin 80% in, 90% out; fadeIn/fadeOut alongside.
constexpr double POP_IN_MS = 300;
constexpr double POP_OUT_MS = 200;
constexpr double POP_IN_SCALE = 0.8;
constexpr double POP_OUT_SCALE = 0.9;
constexpr auto* POP_CLASS = L"QuickshellWindowPop";

void setCloaked(HWND hwnd, bool cloaked) {
	BOOL value = cloaked ? TRUE : FALSE;
	DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
}

} // namespace

void WindowPop::popIn(HWND hwnd) {
	if (hwnd == nullptr) return;
	setCloaked(hwnd, true);
}

int WindowPop::popOut(HWND hwnd) {
	if (hwnd == nullptr) return 0;
	WindowPop::start(hwnd, false);
	return static_cast<int>(POP_OUT_MS);
}

// Acrylic glass for a window with a see-through backdrop (Windows Terminal's
// acrylic, Windows 11's mica): a thumbnail carries the app's content but not
// the backdrop DWM draws behind it, which then showed up only once the window
// landed. This sits under the cover and follows the thumbnail's rect.
struct AccentPolicy {
	DWORD accentState;
	DWORD accentFlags;
	DWORD gradientColor;
	DWORD animationId;
};
struct CompositionData {
	DWORD attribute;
	PVOID data;
	SIZE_T size;
};
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

bool hasWindowBackdrop(HWND hwnd) {
	wchar_t cls[64] {};
	GetClassNameW(hwnd, cls, 64);
	if (wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) return true;
	DWORD backdrop = 0;
	constexpr DWORD SYSTEMBACKDROP_TYPE = 38;
	return SUCCEEDED(DwmGetWindowAttribute(hwnd, SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))
	    && backdrop > 1;
}

namespace {

HWND createWindowGlass(const QRect& rect) {
	static const auto setComposition = []() -> SetCompositionFn {
		auto* user32 = GetModuleHandleW(L"user32.dll");
		return user32 == nullptr ? nullptr
		                         : reinterpret_cast<SetCompositionFn>( // NOLINT
		                               GetProcAddress(user32, "SetWindowCompositionAttribute")
		                           );
	}();
	if (setComposition == nullptr) return nullptr;
	static const auto* className = []() {
		WNDCLASSW wc {};
		wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) -> LRESULT {
			if (msg == WM_NCHITTEST) return HTTRANSPARENT;
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		};
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"QuickshellPopGlass";
		RegisterClassW(&wc);
		return L"QuickshellPopGlass";
	}();
	auto* glass = CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
	    className,
	    L"",
	    WS_POPUP,
	    rect.x(),
	    rect.y(),
	    rect.width(),
	    rect.height(),
	    nullptr,
	    nullptr,
	    GetModuleHandleW(nullptr),
	    nullptr
	);
	if (glass == nullptr) return nullptr;
	constexpr DWORD ACCENT_ACRYLIC = 4;
	constexpr DWORD ACCENT_GRADIENT = 2;
	constexpr DWORD WCA_ACCENT = 19;
	AccentPolicy policy {ACCENT_ACRYLIC, ACCENT_GRADIENT, 0x01000000, 0};
	CompositionData data {WCA_ACCENT, &policy, sizeof(policy)};
	if (!setComposition(glass, &data)) {
		DestroyWindow(glass);
		return nullptr;
	}
	auto corners = DWMWCP_ROUND;
	DwmSetWindowAttribute(glass, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
	COLORREF border = DWMWA_COLOR_NONE;
	DwmSetWindowAttribute(glass, DWMWA_BORDER_COLOR, &border, sizeof(border));
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(
	    glass,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);
	return glass;
}

HWND createMotionCover(const QRect& frame) {
	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &DefWindowProcW;
		cls.hInstance = instance;
		cls.hbrBackground = CreateSolidBrush(COVER_KEY);
		cls.lpszClassName = POP_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	auto* cover = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    POP_CLASS,
	    L"",
	    WS_POPUP,
	    frame.x(),
	    frame.y(),
	    frame.width(),
	    frame.height(),
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (cover == nullptr) return nullptr;
	if (!SetLayeredWindowAttributes(cover, COVER_KEY, 255, LWA_COLORKEY)) {
		DestroyWindow(cover);
		return nullptr;
	}
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(
	    cover,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);
	return cover;
}

// A cover in `frame` that shows `source` (its `sourceRect`) popping in or out,
// stacked just above `below`. `shown` runs once the cover is up and `done`
// on the GUI thread when the animation ends.
bool popCover(
    HWND source,
    const QRect& frame,
    RECT sourceRect,
    HWND below,
    bool in,
    std::function<void()> shown,
    std::function<void()> done,
    int lingerMs = 0,
    HWND glass = nullptr
) {
	auto* cover = createMotionCover(frame);
	HTHUMBNAIL thumbnail = nullptr;
	if (cover == nullptr || FAILED(DwmRegisterThumbnail(cover, source, &thumbnail))) {
		if (cover != nullptr) DestroyWindow(cover);
		return false;
	}

	auto apply = [thumbnail, sourceRect, frame, glass](double progress, bool in) {
		auto eased = ease(progress);
		auto scale =
		    in ? POP_IN_SCALE + (1.0 - POP_IN_SCALE) * eased : 1.0 - (1.0 - POP_OUT_SCALE) * eased;
		auto alpha = in ? eased : 1.0 - eased;
		auto width = frame.width() * scale;
		auto height = frame.height() * scale;
		auto left = (frame.width() - width) / 2;
		auto top = (frame.height() - height) / 2;
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.rcSource = sourceRect;
		props.rcDestination = {
		    static_cast<LONG>(std::lround(left)),
		    static_cast<LONG>(std::lround(top)),
		    static_cast<LONG>(std::lround(left + width)),
		    static_cast<LONG>(std::lround(top + height))
		};
		props.opacity = static_cast<BYTE>(std::clamp(std::lround(alpha * 255), 0L, 255L));
		props.fVisible = TRUE;
		props.fSourceClientAreaOnly = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		if (glass != nullptr) {
			SetWindowPos(
			    glass,
			    nullptr,
			    frame.x() + props.rcDestination.left,
			    frame.y() + props.rcDestination.top,
			    props.rcDestination.right - props.rcDestination.left,
			    props.rcDestination.bottom - props.rcDestination.top,
			    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS
			);
		}
	};

	apply(0, in);
	auto* above = GetWindow(below, GW_HWNDPREV);
	SetWindowPos(
	    cover,
	    above != nullptr ? above : HWND_TOP,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
	);
	ShowWindow(cover, SW_SHOWNOACTIVATE);
	if (glass != nullptr) {
		SetWindowPos(glass, cover, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	if (shown) shown();
	DwmFlush();

	// A short animation on its own thread, on the compositor clock; the cover
	// is released back on the thread that owns it.
	std::thread([cover, thumbnail, apply, in, lingerMs, glass, done = std::move(done)]() {
		auto duration = in ? POP_IN_MS : POP_OUT_MS;
		auto start = std::chrono::steady_clock::now();
		auto period = compositorRefreshMs();
		auto last = start;
		while (true) {
			do {
				waitForCompositorFrame();
			} while (
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count()
			    < period * 0.75
			);
			last = std::chrono::steady_clock::now();
			auto progress = std::clamp(
			    std::chrono::duration<double, std::milli>(last - start).count() / duration,
			    0.0,
			    1.0
			);
			apply(progress, in);
			if (progress >= 1.0) break;
		}
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [cover, thumbnail, done, lingerMs, glass]() {
			    if (done) done();
			    // Over a window that just moved into place, the cover stays a
			    // moment, until the window is there.
			    QTimer::singleShot(lingerMs, QCoreApplication::instance(), [cover, thumbnail, glass]() {
				    DwmUnregisterThumbnail(thumbnail);
				    DestroyWindow(cover);
				    if (glass != nullptr) DestroyWindow(glass);
			    });
		    },
		    Qt::QueuedConnection
		);
	}).detach();
	return true;
}

} // namespace

void WindowPop::start(HWND hwnd, bool in) {
	RECT raw {};
	RECT bounds {};
	if (!IsWindow(hwnd) || !GetWindowRect(hwnd, &raw)
	    || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
	{
		setCloaked(hwnd, false);
		return;
	}
	auto frame =
	    QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
	RECT source {
	    bounds.left - raw.left,
	    bounds.top - raw.top,
	    bounds.right - raw.left,
	    bounds.bottom - raw.top
	};
	// The thumbnail takes the window's place in the same frame it hides.
	auto popped = popCover(
	    hwnd,
	    frame,
	    source,
	    hwnd,
	    in,
	    [hwnd]() { setCloaked(hwnd, true); },
	    [hwnd, in]() {
		    if (in && IsWindow(hwnd)) setCloaked(hwnd, false);
	    }
	);
	if (!popped) setCloaked(hwnd, !in);
}

bool WindowPop::closeOut(HWND hwnd, const std::function<void()>& close) {
	RECT bounds {};
	if (!IsWindow(hwnd) || IsIconic(hwnd)
	    || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
	{
		return false;
	}
	auto frame =
	    QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
	// A still of it as it looks now: the app is gone a moment later.
	auto* still = snapshotOf(frame);
	if (still == nullptr) return false;
	// The real window leaves the screen as its still starts popping out, or it
	// showed whole behind the still until the app was done closing. An app
	// that doesn't close (it asks to save, say) gets it back.
	RECT raw {};
	GetWindowRect(hwnd, &raw);
	auto away = [hwnd, raw, close]() {
		auto right = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
		SetWindowPos(hwnd, nullptr, right + 100, raw.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
		close();
		QTimer::singleShot(CLOSE_GIVE_BACK_MS, QCoreApplication::instance(), [hwnd, raw]() {
			if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return;
			SetWindowPos(hwnd, nullptr, raw.left, raw.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
		});
	};
	auto popped = popCover(
	    still,
	    frame,
	    RECT {0, 0, frame.width(), frame.height()},
	    hwnd,
	    false,
	    away,
	    [still]() { DestroyWindow(still); }
	);
	if (!popped) DestroyWindow(still);
	return popped;
}

namespace {
// How long a new window may take to reach its tile's size before popping in
// anyway, and how long the cover stays once the window is in place.
constexpr int SIZE_WAIT_MS = 300;
constexpr int SIZE_POLL_MS = 16;
constexpr int LAND_LINGER_MS = 60;

QRect frameOf(HWND hwnd, RECT* raw = nullptr) {
	RECT bounds {};
	RECT rect {};
	if (!GetWindowRect(hwnd, &rect)
	    || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
	{
		return {};
	}
	if (raw != nullptr) *raw = rect;
	return {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
}

void popInNow(HWND hwnd, const QRect& frame, const std::function<void()>& land) {
	RECT raw {};
	auto current = frameOf(hwnd, &raw);
	if (current.isEmpty()) {
		land();
		return;
	}
	auto left = current.left() - raw.left;
	auto top = current.top() - raw.top;
	RECT source {left, top, left + current.width(), top + current.height()};
	auto* glass = hasWindowBackdrop(hwnd) ? createWindowGlass(frame) : nullptr;
	auto popped = popCover(hwnd, frame, source, nullptr, true, nullptr, land, LAND_LINGER_MS, glass);
	if (!popped) {
		if (glass != nullptr) DestroyWindow(glass);
		land();
	}
}
} // namespace

void WindowPop::popInAt(HWND hwnd, const QRect& frame, const std::function<void()>& land) {
	// The app may still be at the size it opened with: popping that in blew its
	// content up. Wait (briefly) for it to take its tile's size first.
	auto* timer = new QTimer(QCoreApplication::instance());
	auto started = std::chrono::steady_clock::now();
	timer->setInterval(SIZE_POLL_MS);
	QObject::connect(timer, &QTimer::timeout, [timer, hwnd, frame, land, started]() {
		auto waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		auto current = frameOf(hwnd);
		if (IsWindow(hwnd) && current.size() != frame.size() && waited < SIZE_WAIT_MS) return;
		timer->stop();
		timer->deleteLater();
		if (!IsWindow(hwnd)) return;
		popInNow(hwnd, frame, land);
	});
	timer->start();
}

} // namespace qs::windows
