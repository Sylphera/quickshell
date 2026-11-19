#include "window_motion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include <qcoreapplication.h>
#include <qpointer.h>
#include <qtimer.h>

#include "desktop_slide.hpp"

namespace qs::windows {

namespace {

// end-4's emphasizedDecel.
constexpr double CURVE_X1 = 0.05;
constexpr double CURVE_Y1 = 0.7;
constexpr double CURVE_X2 = 0.1;
constexpr double CURVE_Y2 = 1.0;
constexpr COLORREF COVER_KEY = RGB(255, 0, 255);

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

// How long a window that was asked to close may take before it comes back.
constexpr int CLOSE_GIVE_BACK_MS = 1500;

// A still of what is on screen in `frame`, in a window of its own off screen.
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
