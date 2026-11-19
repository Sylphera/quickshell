#include "window_motion.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>

#include <qcoreapplication.h>

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

namespace {

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
    std::function<void()> done
) {
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
	HTHUMBNAIL thumbnail = nullptr;
	if (cover == nullptr || FAILED(DwmRegisterThumbnail(cover, source, &thumbnail))) {
		if (cover != nullptr) DestroyWindow(cover);
		return false;
	}
	SetLayeredWindowAttributes(cover, COVER_KEY, 255, LWA_COLORKEY);
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(
	    cover,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);

	auto apply = [thumbnail, sourceRect, frame](double progress, bool in) {
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
	if (shown) shown();
	DwmFlush();

	// A short animation on its own thread, on the compositor clock; the cover
	// is released back on the thread that owns it.
	std::thread([cover, thumbnail, apply, in, done = std::move(done)]() {
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
		    [cover, thumbnail, done]() {
			    if (done) done();
			    DwmUnregisterThumbnail(thumbnail);
			    DestroyWindow(cover);
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

} // namespace qs::windows
