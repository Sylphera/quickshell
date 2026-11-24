#include "window_motion.hpp"
#include <algorithm>
#include <cmath>
#include <functional>

#include <qcoreapplication.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qtimer.h>

#include "desktop_slide.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logMotion, "quickshell.windows.motion", QtWarningMsg);

constexpr double MOVE_MS = 300;
constexpr double CURVE_X1 = 0.05;
constexpr double CURVE_Y1 = 0.7;
constexpr double CURVE_X2 = 0.1;
constexpr double CURVE_Y2 = 1.0;
constexpr auto* COVER_CLASS = L"QuickshellWindowMotion";
constexpr COLORREF COVER_KEY = RGB(255, 0, 255);
constexpr double TAKEOFF_TIMEOUT_MS = 60;
constexpr int REPAINT_SETTLE_MS = 33;
constexpr double FIRST_FRAME_MS = 300;
constexpr double HOLD_MS = 50;
constexpr int LINGER_MS = 400;
constexpr int STILL_MS = 100;

double bezierAt(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

bool paintsOnlyOnScreen(HWND hwnd) {
	wchar_t cls[64] {};
	GetClassNameW(hwnd, cls, 64);
	return wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0;
}

HWND snapshotOf(
    const QRect& frame,
    HWND window = nullptr,
    const QRect& visible = QRect(),
    float cornerRadius = -1
) {
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
	if (window != nullptr) {
		if (!PrintWindow(window, dc, PW_RENDERFULLCONTENT)) {
			SelectObject(dc, old);
			DeleteDC(dc);
			DeleteObject(bitmap);
			ReleaseDC(nullptr, screen);
			DestroyWindow(host);
			return nullptr;
		}
	} else {
		BitBlt(dc, 0, 0, width, height, screen, frame.x(), frame.y(), SRCCOPY);
	}
	GdiFlush();
	auto* bits = static_cast<quint32*>(raw);
	auto count = static_cast<qsizetype>(width) * height;
	auto translucent = false;
	if (window != nullptr) {
		for (qsizetype i = 0; i < count && !translucent; i += 7) {
			auto alpha = bits[i] >> 24; // NOLINT
			translucent = alpha != 0 && alpha != 0xff;
		}
	}
	if (!translucent) {
		for (qsizetype i = 0; i < count; i++) bits[i] |= 0xff000000; // NOLINT
	}
	if (cornerRadius >= 0) {
		const auto shown = visible.isEmpty() ? QRect(0, 0, width, height) : visible;
		const auto radius = static_cast<double>(cornerRadius);
		const auto reach = static_cast<int>(std::ceil(radius));
		for (auto cy = 0; cy < reach; cy++) {
			for (auto cx = 0; cx < reach; cx++) {
				const auto dx = radius - cx - 0.5;
				const auto dy = radius - cy - 0.5;
				const auto cover = std::clamp(radius + 0.5 - std::sqrt((dx * dx) + (dy * dy)), 0.0, 1.0);
				if (cover >= 1) continue;
				for (auto corner = 0; corner < 4; corner++) {
					auto x = shown.left() + (corner % 2 == 0 ? cx : shown.width() - 1 - cx);
					auto y = shown.top() + (corner < 2 ? cy : shown.height() - 1 - cy);
					if (x < 0 || y < 0 || x >= width || y >= height) continue;
					auto& pixel = bits[(y * width) + x]; // NOLINT
					quint32 faded = 0;
					for (auto shift = 0; shift < 32; shift += 8)
						faded |= static_cast<quint32>(std::lround(((pixel >> shift) & 0xff) * cover)) << shift;
					pixel = faded;
				}
			}
		}
	} else if (window != nullptr && !visible.isEmpty()) {
		auto dpi = GetDpiForWindow(window);
		auto radius = static_cast<int>(std::lround(8.0 * (dpi != 0 ? dpi : 96) / 96.0));
		auto clearOutside = [&](int cx, int cy, int x0, int y0) {
			for (auto y = y0; y < y0 + radius; y++) {
				for (auto x = x0; x < x0 + radius; x++) {
					if (x < 0 || y < 0 || x >= width || y >= height) continue;
					auto dx = x + 0.5 - cx;
					auto dy = y + 0.5 - cy;
					if (dx * dx + dy * dy > static_cast<double>(radius) * radius) bits[(y * width) + x] = 0; // NOLINT
				}
			}
		};
		auto l = visible.left();
		auto t = visible.top();
		auto r = visible.left() + visible.width();
		auto b = visible.top() + visible.height();
		clearOutside(l + radius, t + radius, l, t);
		clearOutside(r - radius, t + radius, r - radius, t);
		clearOutside(l + radius, b - radius, l, b - radius);
		clearOutside(r - radius, b - radius, r - radius, b - radius);
	}

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

}

namespace {

bool hasPainted(HWND hwnd) {
	RECT raw {};
	if (!GetWindowRect(hwnd, &raw)) return true;
	RECT bounds {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) bounds = raw;
	auto width = static_cast<int>(raw.right - raw.left);
	auto height = static_cast<int>(raw.bottom - raw.top);
	auto left = static_cast<int>(bounds.left - raw.left) + 8;
	auto top = static_cast<int>(bounds.top - raw.top) + 8;
	auto right = static_cast<int>(bounds.right - raw.left) - 8;
	auto bottom = static_cast<int>(bounds.bottom - raw.top) - 8;
	if (width <= 0 || height <= 0 || right <= left || bottom <= top) return true;
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	auto* screen = GetDC(nullptr);
	auto* dc = CreateCompatibleDC(screen);
	auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, screen);
	if (dc == nullptr || bitmap == nullptr || bits == nullptr) {
		if (bitmap != nullptr) DeleteObject(bitmap);
		if (dc != nullptr) DeleteDC(dc);
		return true;
	}
	auto* old = SelectObject(dc, bitmap);
	auto painted = true;
	if (PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT)) {
		GdiFlush();
		const auto* pixels = static_cast<const quint32*>(bits);
		auto first = pixels[(top * width) + left];
		auto differs = [first](quint32 pixel) {
			for (auto shift = 0; shift < 24; shift += 8) {
				auto a = static_cast<int>((first >> shift) & 0xff);
				auto b = static_cast<int>((pixel >> shift) & 0xff);
				if (std::abs(a - b) > 6) return true;
			}
			return false;
		};
		constexpr int STEPS = 24;
		painted = false;
		for (auto y = 0; y < STEPS && !painted; y++) {
			auto py = top + ((bottom - top - 1) * y / (STEPS - 1));
			for (auto x = 0; x < STEPS; x++) {
				auto px = left + ((right - left - 1) * x / (STEPS - 1));
				if (differs(pixels[(py * width) + px])) {
					painted = true;
					break;
				}
			}
		}
	}
	SelectObject(dc, old);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return painted;
}

}

HWND windowStill(HWND window, const QRect& raw, const QRect& visible, float radius) {
	const auto frame = visible.translated(raw.topLeft());
	auto covered = false;
	for (auto* above = GetWindow(window, GW_HWNDPREV); above != nullptr && !covered;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		if (!IsWindowVisible(above) || IsIconic(above)) continue;
		if ((GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0) continue;
		DWORD cloaked = 0;
		DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		RECT rect {};
		if (cloaked != 0 || !GetWindowRect(above, &rect)) continue;
		covered = QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top).intersects(frame);
	}
	const QRect screen(
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN),
	    GetSystemMetrics(SM_CXVIRTUALSCREEN),
	    GetSystemMetrics(SM_CYVIRTUALSCREEN)
	);
	if (covered || !screen.contains(frame)) return snapshotOf(raw, window, visible, radius);

	auto* still = snapshotOf(frame, nullptr, QRect(), radius);
	return still != nullptr ? still : snapshotOf(raw, window, visible, radius);
}

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

constexpr int CLOSE_GIVE_BACK_MS = 1500;

QRect lerp(const QRect& a, const QRect& b, double t) {
	auto mix = [t](int x, int y) { return static_cast<int>(std::lround(x + (y - x) * t)); };
	return QRect(
	    QPoint(mix(a.left(), b.left()), mix(a.top(), b.top())),
	    QSize(std::max(1, mix(a.width(), b.width())), std::max(1, mix(a.height(), b.height())))
	);
}

bool isAbove(HWND window, HWND reference) {
	for (auto* hwnd = GetWindow(reference, GW_HWNDPREV); hwnd != nullptr;
	     hwnd = GetWindow(hwnd, GW_HWNDPREV))
	{
		if (hwnd == window) return true;
	}
	return false;
}

QPointer<WindowMotion> MOTION; // NOLINT

}

WindowMotion* WindowMotion::instance() {
	if (MOTION.isNull()) MOTION = new WindowMotion(QCoreApplication::instance());
	return MOTION.data();
}

WindowMotion::WindowMotion(QObject* parent): QObject(parent) {}

WindowMotion::~WindowMotion() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	for (const auto& entry: this->entries) {
		if (!IsWindow(entry.hwnd)) continue;
		moveTo(entry.hwnd, entry.to, true);
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

	this->coverRect = QRect(
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN) + 1,
	    std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)),
	    std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1)
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
	DwmSetWindowAttribute(
	    this->cover,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);
	return true;
}

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

void WindowMotion::slideTo(HWND hwnd, const QPoint& topLeft) {
	RECT raw {};
	RECT bounds {};
	if (!GetWindowRect(hwnd, &raw)) return;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
		bounds = raw;
	SetWindowPos(
	    hwnd,
	    nullptr,
	    topLeft.x() - (bounds.left - raw.left),
	    topLeft.y() - (bounds.top - raw.top),
	    0,
	    0,
	    SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_ASYNCWINDOWPOS
	);
}

void WindowMotion::clipTo(HWND hwnd, const QSize& visible) {
	if (IsHungAppWindow(hwnd)) return;
	if (visible.isEmpty()) {
		SetWindowRgn(hwnd, nullptr, TRUE);
		return;
	}
	RECT raw {};
	RECT bounds {};
	if (!GetWindowRect(hwnd, &raw)) return;
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
		bounds = raw;
	auto left = static_cast<int>(bounds.left - raw.left);
	auto top = static_cast<int>(bounds.top - raw.top);
	auto* region = CreateRectRgn(left, top, left + visible.width(), top + visible.height());
	if (region == nullptr) return;
	if (SetWindowRgn(hwnd, region, FALSE) == 0) DeleteObject(region);
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

}

void WindowMotion::layout(Entry& entry, double progress) {
	entry.progress = progress;
	auto eased = ease(progress);
	entry.current = lerp(entry.from, entry.to, eased);
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
	if (entry.live && !entry.overlay) props.fVisible = FALSE;
	DwmUpdateThumbnailProperties(entry.thumbnail, &props);

	if (entry.holdThumbnail != nullptr) {
		auto since = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry.start).count();
		DWM_THUMBNAIL_PROPERTIES hold {};
		hold.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
		auto off = entry.holdOffset;
		hold.rcSource = {off.x(), off.y(), off.x() + entry.holdSize.width(), off.y() + entry.holdSize.height()};
		hold.rcDestination = props.rcDestination;
		hold.opacity = props.opacity;
		if (!entry.waiting && since > 0 && std::abs(frame.width() - entry.to.width()) <= 2
		    && std::abs(frame.height() - entry.to.height()) <= 2)
		{
			entry.caughtUp = true;
		}
		hold.fVisible = entry.caughtUp ? FALSE : TRUE;
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
}

void WindowMotion::restack() {
	std::ranges::stable_sort(this->entries, [](const Entry& a, const Entry& b) { return a.depth > b.depth; });
	QList<HTHUMBNAIL> old;
	for (auto& entry: this->entries) {
		HTHUMBNAIL thumbnail = nullptr;
		auto* source = entry.snapshot != nullptr ? entry.snapshot : entry.hwnd;
		if (FAILED(DwmRegisterThumbnail(this->cover, source, &thumbnail))) continue;
		old.append(entry.thumbnail);
		entry.thumbnail = thumbnail;
		if (entry.holdThumbnail != nullptr) {
			HTHUMBNAIL hold = nullptr;
			if (SUCCEEDED(DwmRegisterThumbnail(this->cover, entry.hold, &hold))) {
				DwmUnregisterThumbnail(entry.holdThumbnail);
				entry.holdThumbnail = hold;
			}
		}
		this->layout(entry, entry.progress);
	}
	if (old.isEmpty()) return;
	DwmFlush();
	for (auto* thumbnail: old) DwmUnregisterThumbnail(thumbnail);
}

bool WindowMotion::begin(
    HWND hwnd,
    const QRect& from,
    const QRect& to,
    const QRect& parked,
    bool pop,
    bool still
) {
	for (auto i = this->stills.size() - 1; i >= 0; i--) {
		if (this->stills[i].hwnd != hwnd) continue;
		release(this->stills[i]);
		this->stills.removeAt(i);
	}
	if (this->moving(hwnd)) {
		this->retarget(hwnd, to, parked);
		return true;
	}
	if (!this->ensureCover()) return false;

	HWND snapshot = !pop && still ? snapshotOf(from) : nullptr;
	auto snapshotSize = from.size();
	HWND hold = nullptr;
	QPoint holdOffset;
	QSize holdSize;
	HTHUMBNAIL thumbnail = nullptr;
	if (FAILED(DwmRegisterThumbnail(this->cover, snapshot != nullptr ? snapshot : hwnd, &thumbnail)))
	{
		if (snapshot != nullptr) DestroyWindow(snapshot);
		if (hold != nullptr) DestroyWindow(hold);
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
		    .waiting = snapshot == nullptr && pop,
		    .depth = depthOf(hwnd),
		    .hold = hold,
		    .holdOffset = holdOffset,
		    .holdSize = holdSize,
		});
		auto& added = this->entries.last();
		if (added.hold != nullptr && FAILED(DwmRegisterThumbnail(this->cover, added.hold, &added.holdThumbnail))) {
			added.holdThumbnail = nullptr;
		}
		this->layout(added, 0);
		if (this->entries.size() > 1) this->restack();
	}

	this->placeCover(hwnd);
	if (!IsWindowVisible(this->cover)) {
		ShowWindow(this->cover, SW_SHOWNOACTIVATE);
		DwmFlush();
	}
	if (!pop && snapshot == nullptr) {
		auto lock = std::lock_guard(this->mutex);
		auto& added = this->entries.last();
		added.live = true;
		added.realSize = to.size();
		added.overlay = false;
		this->layout(added, 0);
	} else {
		moveTo(hwnd, parked, false);
	}

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
		if (entry->holdThumbnail != nullptr) {
			DwmUnregisterThumbnail(entry->holdThumbnail);
			entry->holdThumbnail = nullptr;
		}
		if (entry->pop && !entry->done) {
			entry->from = to;
			entry->to = to;
		} else {
			entry->pop = false;
			entry->from = entry->current;
			entry->to = to;
			entry->start = std::chrono::steady_clock::now();
		}
		entry->done = false;
		if (entry->live) {
			return;
		}
	}
	moveTo(hwnd, parked, false);
}

HWND WindowMotion::windowAt(POINT point) {
	auto lock = std::lock_guard(this->mutex);
	HWND found = nullptr;
	auto depth = 0;
	for (const auto& entry: this->entries) {
		if (entry.done) continue;
		auto shown = entry.pop ? entry.to : entry.current;
		if (!shown.contains(QPoint(point.x, point.y))) continue;
		if (found == nullptr || entry.depth < depth) {
			found = entry.hwnd;
			depth = entry.depth;
		}
	}
	return found;
}

QRect WindowMotion::stopAt(HWND hwnd) {
	QRect shown;
	auto empty = false;
	{
		auto lock = std::lock_guard(this->mutex);
		for (auto i = this->entries.size() - 1; i >= 0; i--) {
			if (this->entries[i].hwnd != hwnd) continue;
			const auto& entry = this->entries[i];
			shown = entry.pop ? entry.to : entry.current;
			release(entry);
			this->entries.removeAt(i);
		}
		empty = this->entries.isEmpty();
	}
	if (empty) this->closeCover();
	return shown;
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

		QList<HWND> unpainted;
		{
			auto lock = std::lock_guard(this->mutex);
			for (const auto& entry: this->entries) {
				if (!entry.done && entry.waiting && entry.pop && !entry.painted) unpainted.append(entry.hwnd);
			}
		}
		QList<HWND> paintedNow;
		for (auto* hwnd: unpainted) {
			if (hasPainted(hwnd)) paintedNow.append(hwnd);
		}

		auto finished = false;
		{
			auto lock = std::lock_guard(this->mutex);
			auto holding = false;
			auto longestWait = 0.0;
			for (const auto& entry: this->entries) {
				if (!entry.done && entry.waiting) {
					longestWait = std::max(longestWait, std::chrono::duration<double, std::milli>(last - entry.start).count());
				}
			}
			for (auto& entry: this->entries) {
				if (entry.done || !entry.waiting || !entry.pop || entry.painted) continue;
				if (paintedNow.contains(entry.hwnd) || longestWait >= FIRST_FRAME_MS) entry.painted = true;
				else holding = true;
			}
			for (auto& entry: this->entries) {
				if (entry.done || !entry.waiting) continue;
				RECT raw {};
				RECT bounds {};
				auto left = GetWindowRect(entry.hwnd, &raw) && raw.left >= entry.parked.left() - 64;
				if (left && !entry.pop
				    && SUCCEEDED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
				{
					left = bounds.right - bounds.left >= entry.parked.width() - 2
					    && bounds.bottom - bounds.top >= entry.parked.height() - 2;
				}
				auto waited = std::chrono::duration<double, std::milli>(last - entry.start).count();
				if (!left && waited < TAKEOFF_TIMEOUT_MS) {
					holding = true;
					if (!entry.pop && waited > 10 && entry.resent < 3) {
						entry.resent++;
						qCWarning(logMotion).noquote() << "[park] resend" << entry.resent << entry.hwnd << "after" << waited << "ms";
						moveTo(entry.hwnd, entry.parked, false);
					}
				}
			}
			for (auto& entry: this->entries) {
				if (entry.done) continue;
				if (entry.waiting) {
					if (holding) continue;
					entry.waiting = false;
					entry.start = last + std::chrono::milliseconds(REPAINT_SETTLE_MS);
				}
				auto elapsed = std::chrono::duration<double, std::milli>(last - entry.start).count();
				auto progress = std::clamp(elapsed / MOVE_MS, 0.0, 1.0);
				this->layout(entry, progress);
				if (entry.live) {
					moveTo(entry.hwnd, entry.current, false);
					if (entry.overlay) {
						clipTo(entry.hwnd, QSize(
						    std::min(entry.current.width(), entry.realSize.width()),
						    std::min(entry.current.height(), entry.realSize.height())
						));
					}
				}
				if (entry.snapshot == nullptr && !entry.pop) {
					RECT bounds {};
					entry.frames++;
					if (SUCCEEDED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
					    && (std::abs(static_cast<int>(bounds.right - bounds.left) - entry.to.width()) > 2
					        || std::abs(static_cast<int>(bounds.bottom - bounds.top) - entry.to.height()) > 2))
					{
						entry.lateFrames++;
					}
				}
				if (progress >= 1.0) {
					entry.done = true;
					finished = true;
					if (entry.lateFrames > 0) {
						qCWarning(logMotion).noquote() << "[stretch]" << entry.hwnd << "app at its old size for" << entry.lateFrames << "of" << entry.frames << "frames, from" << entry.from << "to" << entry.to;
					}
				}
			}
		}
		if (finished && !this->donePending.exchange(true)) {
			QMetaObject::invokeMethod(this, &WindowMotion::onFrameDone, Qt::QueuedConnection);
		}
	}
}

void WindowMotion::onFrameDone() {
	this->donePending.store(false);
	QList<Entry> landing;
	auto empty = false;
	{
		auto lock = std::lock_guard(this->mutex);
		for (auto i = this->entries.size() - 1; i >= 0; i--) {
			if (!this->entries[i].done) continue;
			if (this->entries[i].snapshot == nullptr) {
				auto& entry = this->entries[i];
				RECT raw {};
				RECT bounds {};
				if (paintsOnlyOnScreen(entry.hwnd) && !entry.live
				    && entry.holdThumbnail == nullptr && GetWindowRect(entry.hwnd, &raw)
				    && SUCCEEDED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
				{
					auto* still = snapshotOf(
					    QRect(raw.left, raw.top, raw.right - raw.left, raw.bottom - raw.top),
					    entry.hwnd,
					    QRect(bounds.left - raw.left, bounds.top - raw.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
					);
					HTHUMBNAIL thumbnail = nullptr;
					if (still != nullptr && SUCCEEDED(DwmRegisterThumbnail(this->cover, still, &thumbnail))) {
						auto dest = entry.to.translated(-this->coverRect.topLeft());
						DWM_THUMBNAIL_PROPERTIES shown {};
						shown.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
						shown.rcSource = {
						    bounds.left - raw.left,
						    bounds.top - raw.top,
						    bounds.right - raw.left,
						    bounds.bottom - raw.top
						};
						shown.rcDestination = {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
						shown.opacity = 255;
						shown.fVisible = TRUE;
						DwmUpdateThumbnailProperties(thumbnail, &shown);
						entry.hold = still;
						entry.holdThumbnail = thumbnail;
					} else if (still != nullptr) {
						DestroyWindow(still);
					}
				}
			}
			landing.append(this->entries.takeAt(i));
		}
		empty = this->entries.isEmpty();
	}
	for (const auto& entry: landing) {
		if (IsWindow(entry.hwnd)) {
			if (entry.live && entry.overlay) clipTo(entry.hwnd, QSize());
			moveTo(entry.hwnd, entry.to, true);
		}
	}
	for (const auto& entry: landing) {
		if (entry.snapshot != nullptr) this->lingering.append(entry);
		else this->stills.append(entry);
		emit this->landed(entry.hwnd, entry.to);
	}
	if (!this->stills.isEmpty()) {
		QTimer::singleShot(STILL_MS, this, [this]() {
			for (const auto& entry: this->stills) release(entry);
			this->stills.clear();
			auto lock = std::unique_lock(this->mutex);
			auto none = this->entries.isEmpty();
			lock.unlock();
			if (none && this->lingering.isEmpty()) this->closeCover();
		});
	}
	if (!this->lingering.isEmpty()) {
		QTimer::singleShot(LINGER_MS, this, [this]() {
			for (const auto& entry: this->lingering) release(entry);
			this->lingering.clear();
			auto lock = std::unique_lock(this->mutex);
			auto none = this->entries.isEmpty();
			lock.unlock();
			if (none && this->stills.isEmpty()) this->closeCover();
		});
	}
	if (empty && this->lingering.isEmpty() && this->stills.isEmpty()) this->closeCover();
}

void WindowMotion::release(const Entry& entry) {
	if (entry.live && entry.overlay && IsWindow(entry.hwnd)) clipTo(entry.hwnd, QSize());
	DwmUnregisterThumbnail(entry.thumbnail);
	if (entry.snapshot != nullptr) DestroyWindow(entry.snapshot);
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
		for (const auto& entry: this->stills) release(entry);
		this->stills.clear();
	}
	if (this->cover != nullptr) {
		DestroyWindow(this->cover);
		this->cover = nullptr;
	}
}

namespace {

constexpr double POP_IN_MS = 300;
constexpr double POP_OUT_MS = 200;
constexpr double POP_IN_SCALE = 0.8;
constexpr double POP_OUT_SCALE = 0.9;
constexpr auto* POP_CLASS = L"QuickshellWindowPop";

void setCloaked(HWND hwnd, bool cloaked) {
	BOOL value = cloaked ? TRUE : FALSE;
	DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
}

}

void WindowPop::popIn(HWND hwnd) {
	if (hwnd == nullptr) return;
	setCloaked(hwnd, true);
}

int WindowPop::popOut(HWND hwnd) {
	if (hwnd == nullptr) return 0;
	WindowPop::start(hwnd, false);
	return static_cast<int>(POP_OUT_MS);
}

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

}

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
	auto* still = snapshotOf(frame);
	if (still == nullptr) return false;
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
}

void WindowPop::popInAt(HWND hwnd, const QRect& frame, const std::function<void()>& land) {
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

}
