#include "native_corners.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <windowsx.h>

#include "util.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logCorners, "quickshell.windows.corners", QtWarningMsg);

constexpr auto* CORNER_CLASS = L"QuickshellScreenCorner";
constexpr auto* CORNER_PROP = L"QuickshellScreenCornerIndex";
constexpr int TOPMOST_MS = 2000;

bool cornerIsRight(int corner) { return corner == 1 || corner == 3; }
bool cornerIsBottom(int corner) { return corner >= 2; }

} // namespace

NativeScreenCorners::NativeScreenCorners(QObject* parent): QObject(parent) {
	this->rebuildTimer.setSingleShot(true);
	this->rebuildTimer.setInterval(0);
	QObject::connect(&this->rebuildTimer, &QTimer::timeout, this, &NativeScreenCorners::rebuild);

	// Other topmost windows (the shell's own panels among them) come and go;
	// the corners stay over them, like end-4's overlay layer.
	this->topmostTimer.setInterval(TOPMOST_MS);
	QObject::connect(&this->topmostTimer, &QTimer::timeout, this, [this]() {
		for (const auto& corner: this->corners) {
			if (corner.hwnd == nullptr) continue;
			SetWindowPos(
			    corner.hwnd,
			    HWND_TOPMOST,
			    0,
			    0,
			    0,
			    0,
			    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
			);
		}
	});
}

NativeScreenCorners::~NativeScreenCorners() { this->destroyWindows(); }

#define CORNER_SETTER(name, member, type)                                                          \
	void NativeScreenCorners::name(type value) {                                                     \
		if (value == this->member) return;                                                             \
		this->member = value;                                                                          \
		emit this->changed();                                                                          \
		this->scheduleRebuild();                                                                       \
	}

CORNER_SETTER(setRadius, mRadius, qint32)
CORNER_SETTER(setShown, mShown, bool)
CORNER_SETTER(setHotEnabled, mHotEnabled, bool)
CORNER_SETTER(setHotBottom, mHotBottom, bool)
CORNER_SETTER(setHotWidth, mHotWidth, qint32)
CORNER_SETTER(setHotHeight, mHotHeight, qint32)
CORNER_SETTER(setDeadPixel, mDeadPixel, bool)

#undef CORNER_SETTER

void NativeScreenCorners::setColor(const QColor& color) {
	if (color == this->mColor) return;
	this->mColor = color;
	emit this->changed();
	this->scheduleRebuild();
}

void NativeScreenCorners::setScreenName(const QString& name) {
	if (name == this->mScreenName) return;
	this->mScreenName = name;
	emit this->changed();
	this->scheduleRebuild();
}

void NativeScreenCorners::scheduleRebuild() { this->rebuildTimer.start(); }

bool NativeScreenCorners::isHot(int corner) const {
	return this->mHotEnabled && cornerIsBottom(corner) == this->mHotBottom && this->mHotWidth > 0
	    && this->mHotHeight > 0;
}

void NativeScreenCorners::destroyWindows() {
	this->topmostTimer.stop();
	for (auto& corner: this->corners) {
		if (corner.hwnd != nullptr) DestroyWindow(corner.hwnd);
		corner = Corner();
	}
}

void NativeScreenCorners::rebuild() {
	this->destroyWindows();

	QScreen* screen = nullptr;
	for (auto* candidate: QGuiApplication::screens()) {
		if (candidate->name() == this->mScreenName) screen = candidate;
	}
	if (screen != this->screen) {
		if (this->screen != nullptr) QObject::disconnect(this->screen, nullptr, this, nullptr);
		this->screen = screen;
		if (screen != nullptr) {
			QObject::connect(
			    screen,
			    &QScreen::geometryChanged,
			    this,
			    &NativeScreenCorners::scheduleRebuild
			);
			QObject::connect(
			    screen,
			    &QScreen::physicalDotsPerInchChanged,
			    this,
			    &NativeScreenCorners::scheduleRebuild
			);
		}
	}
	if (screen == nullptr) return;

	auto rects = monitorRects(monitorForScreen(screen));
	if (!rects.valid) return;
	this->scale = screen->devicePixelRatio();

	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &NativeScreenCorners::windowProc;
		cls.hInstance = instance;
		cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		cls.lpszClassName = CORNER_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}

	auto radius = static_cast<int>(std::lround(this->mRadius * this->scale));
	auto hotWidth = static_cast<int>(std::lround(this->mHotWidth * this->scale));
	auto hotHeight = std::max(1, static_cast<int>(std::lround(this->mHotHeight * this->scale)));
	auto drawn = this->mShown && radius > 0 && this->mColor.alpha() > 0;
	const auto& monitor = rects.monitor;

	auto* screenDc = GetDC(nullptr);
	for (auto index = 0; index < 4; index++) {
		auto hot = this->isHot(index);
		if (!drawn && !hot) continue;

		auto right = cornerIsRight(index);
		auto bottom = cornerIsBottom(index);
		auto width = std::max(drawn ? radius : 0, hot ? hotWidth : 0);
		auto height = std::max(drawn ? radius : 0, hot ? hotHeight : 0);
		auto x = right ? monitor.left() + monitor.width() - width : monitor.left();
		auto y = bottom ? monitor.top() + monitor.height() - height : monitor.top();
		if (this->mDeadPixel && right) x += 1;
		if (this->mDeadPixel && bottom) y += 1;

		auto* hwnd = CreateWindowExW(
		    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
		    CORNER_CLASS,
		    L"",
		    WS_POPUP,
		    x,
		    y,
		    width,
		    height,
		    nullptr,
		    nullptr,
		    instance,
		    nullptr
		);
		if (hwnd == nullptr) {
			qCWarning(logCorners) << "Couldn't create a corner window:" << GetLastError();
			continue;
		}
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		SetPropW(hwnd, CORNER_PROP, reinterpret_cast<HANDLE>(static_cast<quintptr>(index + 1)));

		auto& corner = this->corners.at(index);
		corner.hwnd = hwnd;
		if (hot) {
			corner.hot =
			    QRect(right ? width - hotWidth : 0, bottom ? height - hotHeight : 0, hotWidth, hotHeight);
		}

		// Premultiplied BGRA: the fill outside a quarter circle centred on the
		// far corner, antialiased, plus a near-invisible hot zone so it can
		// be hit-tested.
		BITMAPINFO info {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void* bits = nullptr;
		auto* bitmap = CreateDIBSection(screenDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) DeleteObject(bitmap);
			continue;
		}
		auto* pixels = static_cast<std::uint32_t*>(bits);
		auto color = this->mColor;
		for (auto py = 0; py < height; py++) {
			for (auto px = 0; px < width; px++) {
				auto cx = right ? width - 1 - px : px;
				auto cy = bottom ? height - 1 - py : py;
				auto alpha = 0.0;
				if (drawn && cx < radius && cy < radius) {
					auto dx = radius - (cx + 0.5);
					auto dy = radius - (cy + 0.5);
					alpha = std::clamp(std::hypot(dx, dy) - radius + 0.5, 0.0, 1.0) * color.alphaF();
				}
				auto a = static_cast<int>(std::lround(alpha * 255));
				if (hot && corner.hot.contains(px, py)) a = std::max(a, 1);
				auto premultiply = [a](int channel) { return channel * a / 255; };
				pixels[py * width + px] = (static_cast<std::uint32_t>(a) << 24)
				                        | (static_cast<std::uint32_t>(premultiply(color.red())) << 16)
				                        | (static_cast<std::uint32_t>(premultiply(color.green())) << 8)
				                        | static_cast<std::uint32_t>(premultiply(color.blue()));
			}
		}

		auto* memDc = CreateCompatibleDC(screenDc);
		auto* old = SelectObject(memDc, bitmap);
		POINT position {x, y};
		SIZE size {width, height};
		POINT origin {0, 0};
		BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		UpdateLayeredWindow(hwnd, screenDc, &position, &size, memDc, &origin, 0, &blend, ULW_ALPHA);
		SelectObject(memDc, old);
		DeleteDC(memDc);
		DeleteObject(bitmap);

		ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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
	ReleaseDC(nullptr, screenDc);

	if (std::ranges::any_of(this->corners, [](const Corner& c) { return c.hwnd != nullptr; })) {
		this->topmostTimer.start();
	}
}

LRESULT CALLBACK
NativeScreenCorners::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto* self = reinterpret_cast<NativeScreenCorners*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	auto index = static_cast<int>(reinterpret_cast<quintptr>(GetPropW(hwnd, CORNER_PROP))) - 1;
	if (self == nullptr || index < 0 || index > 3) return DefWindowProcW(hwnd, msg, wParam, lParam);
	return self->handle(index, msg, wParam, lParam);
}

LRESULT NativeScreenCorners::handle(int index, UINT msg, WPARAM wParam, LPARAM lParam) {
	auto& corner = this->corners.at(index);
	auto* hwnd = corner.hwnd;

	switch (msg) {
	case WM_NCHITTEST: {
		POINT point {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
		ScreenToClient(hwnd, &point);
		return corner.hot.contains(point.x, point.y) ? HTCLIENT : HTTRANSPARENT;
	}
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_MOUSEMOVE: {
		if (!corner.tracking) {
			TRACKMOUSEEVENT track {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
			TrackMouseEvent(&track);
			corner.tracking = true;
			emit this->entered(index);
		}
		auto x = GET_X_LPARAM(lParam) - corner.hot.left();
		auto y = GET_Y_LPARAM(lParam) - corner.hot.top();
		emit this->moved(
		    index,
		    x / this->scale,
		    y / this->scale,
		    corner.hot.width() / this->scale,
		    corner.hot.height() / this->scale
		);
		return 0;
	}
	case WM_LBUTTONDOWN: emit this->pressed(index); return 0;
	case WM_MOUSEWHEEL: {
		auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
		auto steps = delta / WHEEL_DELTA;
		if (steps == 0) steps = delta > 0 ? 1 : -1;
		emit this->scrolled(index, steps);
		return 0;
	}
	case WM_MOUSELEAVE:
		corner.tracking = false;
		emit this->left(index);
		return 0;
	default: return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

} // namespace qs::windows
