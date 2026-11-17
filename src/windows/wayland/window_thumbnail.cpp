#include "window_thumbnail.hpp"
#include <cmath>
#include <cstring>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qquickitem.h>
#include <qquickwindow.h>
#include <qrect.h>

#include "../../core/logcat.hpp"
#include "../window_tracker.hpp"
#include "toplevel.hpp"

namespace qs::wayland::screencopy {

namespace {
QS_LOGGING_CATEGORY(logThumbnail, "quickshell.windows.thumbnail", QtWarningMsg);

LONG rounded(qreal value) { return static_cast<LONG>(std::lround(value)); }

constexpr auto* HOST_CLASS = L"QuickshellThumbnailHost";

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;
	default: return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

HWND createHost(HWND owner) {
	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &hostProc;
		cls.hInstance = instance;
		cls.lpszClassName = HOST_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	return CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
	    HOST_CLASS,
	    L"",
	    WS_POPUP,
	    0,
	    0,
	    0,
	    0,
	    owner,
	    nullptr,
	    instance,
	    nullptr
	);
}
} // namespace

WindowThumbnail::WindowThumbnail(QQuickItem* parent): QQuickItem(parent) {}

WindowThumbnail::~WindowThumbnail() { this->unregister(); }

void WindowThumbnail::setCaptureSource(QObject* source) {
	if (source == this->mCaptureSource) return;
	if (this->mCaptureSource) QObject::disconnect(this->mCaptureSource, nullptr, this, nullptr);

	this->mCaptureSource = source;
	if (source != nullptr) {
		QObject::connect(source, &QObject::destroyed, this, &WindowThumbnail::refresh);
	}

	emit this->captureSourceChanged();
	this->refresh();
}

void WindowThumbnail::setConstraintSize(QSizeF size) {
	if (size == this->mConstraintSize) return;
	this->mConstraintSize = size;
	emit this->constraintSizeChanged();
	this->updateImplicitSize();
}

void WindowThumbnail::componentComplete() {
	QQuickItem::componentComplete();
	this->mCompleted = true;
	this->refresh();
}

void WindowThumbnail::itemChange(ItemChange change, const ItemChangeData& data) {
	QQuickItem::itemChange(change, data);
	if (change == ItemSceneChange) this->refresh();
	else if (change == ItemVisibleHasChanged || change == ItemOpacityHasChanged) this->sync();
}

HWND WindowThumbnail::sourceWindow() const {
	auto* toplevel = qobject_cast<toplevel::Toplevel*>(this->mCaptureSource);
	if (toplevel == nullptr || toplevel->window() == nullptr) return nullptr;
	auto* hwnd = toplevel->window()->hwnd();
	return IsWindow(hwnd) ? hwnd : nullptr;
}

void WindowThumbnail::refresh() {
	auto* window = this->mCompleted ? this->window() : nullptr;
	auto* destination = window != nullptr ? reinterpret_cast<HWND>(window->winId()) : nullptr; // NOLINT
	auto* source = this->sourceWindow();

	if (this->mThumbnail != nullptr && destination == this->mDestination && source == this->mSource) {
		this->sync();
		return;
	}

	auto had = this->mThumbnail != nullptr;
	this->unregister();

	if (destination != nullptr && source != nullptr) {
		this->mHost = createHost(destination);
		HTHUMBNAIL thumbnail = nullptr;
		auto result = this->mHost == nullptr ? E_FAIL : DwmRegisterThumbnail(this->mHost, source, &thumbnail);
		if (FAILED(result)) {
			qCWarning(logThumbnail) << "Couldn't register a thumbnail of" << source << "result" << Qt::hex << result;
		} else {
			this->mThumbnail = thumbnail;
			this->mDestination = destination;
			this->mSource = source;
			this->mFrameConnection =
			    QObject::connect(window, &QQuickWindow::afterAnimating, this, &WindowThumbnail::sync);
			this->mPlaceConnection = QObject::connect(
			    window,
			    &QQuickWindow::beforeSynchronizing,
			    this,
			    &WindowThumbnail::place,
			    Qt::DirectConnection
			);

			DWM_THUMBNAIL_PROPERTIES hidden {};
			hidden.dwFlags = DWM_TNP_VISIBLE;
			hidden.fVisible = FALSE;
			DwmUpdateThumbnailProperties(thumbnail, &hidden);
			this->mApplied = hidden;
		}
	}

	if (had != (this->mThumbnail != nullptr)) emit this->hasContentChanged();
	if (this->mThumbnail == nullptr) {
		this->setSourceSize(QSize());
		return;
	}

	this->sync();
	window->update();
}

void WindowThumbnail::unregister() {
	if (this->mFrameConnection) QObject::disconnect(this->mFrameConnection);
	if (this->mPlaceConnection) QObject::disconnect(this->mPlaceConnection);
	if (this->mThumbnail != nullptr) DwmUnregisterThumbnail(this->mThumbnail);
	if (this->mHost != nullptr) DestroyWindow(this->mHost);
	this->mHost = nullptr;
	this->mHostShape = QRect();
	this->mHostRadius = -1;
	this->mHostSize = QSize();
	this->mThumbnail = nullptr;
	this->mDestination = nullptr;
	this->mSource = nullptr;
	this->mApplied = {};
	this->mSourcePixels = {};
}

void WindowThumbnail::sync() {
	if (this->mThumbnail == nullptr) return;
	auto* window = this->window();
	if (window == nullptr) return;

	SIZE source {};
	if (FAILED(DwmQueryThumbnailSourceSize(this->mThumbnail, &source))) {
		this->unregister();
		emit this->hasContentChanged();
		this->setSourceSize(QSize());
		return;
	}
	this->mSourcePixels = source;
	this->setSourceSize(QSize(static_cast<int>(source.cx), static_cast<int>(source.cy)));
}

void WindowThumbnail::place() {
	if (this->mThumbnail == nullptr) return;
	auto* window = this->window();
	if (window == nullptr) return;
	auto source = this->mSourcePixels;

	qreal opacity = 1;
	for (const QQuickItem* item = this; item != nullptr; item = item->parentItem()) {
		opacity *= item->opacity();
	}

	auto rect = this->mapRectToScene(QRectF(0, 0, this->width(), this->height()));
	auto shown = rect.intersected(QRectF(0, 0, window->width(), window->height()));
	for (const QQuickItem* item = this->parentItem(); item != nullptr; item = item->parentItem()) {
		if (item->clip()) shown = shown.intersected(item->mapRectToScene(item->boundingRect()));
	}
	auto visible = this->isVisible() && window->isVisible() && !shown.isEmpty() && opacity > 0
	            && source.cx > 0 && source.cy > 0;

	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY
	              | DWM_TNP_SOURCECLIENTAREAONLY;
	props.fVisible = visible ? TRUE : FALSE;
	props.fSourceClientAreaOnly = FALSE;
	props.opacity = static_cast<BYTE>(std::lround(qBound(0.0, opacity, 1.0) * 255));

	auto dpr = window->devicePixelRatio();
	auto host = QRect();
	if (visible) {
		POINT origin {rounded(shown.left() * dpr), rounded(shown.top() * dpr)};
		ClientToScreen(this->mDestination, &origin);
		host = QRect(
		    origin.x,
		    origin.y,
		    rounded(shown.right() * dpr) - rounded(shown.left() * dpr),
		    rounded(shown.bottom() * dpr) - rounded(shown.top() * dpr)
		);
		visible = host.width() > 0 && host.height() > 0;
		props.fVisible = visible ? TRUE : FALSE;
	}

	if (visible) {
		props.rcDestination = {0, 0, host.width(), host.height()};

		auto sx = static_cast<qreal>(source.cx) / rect.width();
		auto sy = static_cast<qreal>(source.cy) / rect.height();
		props.rcSource = {
		    rounded((shown.left() - rect.left()) * sx),
		    rounded((shown.top() - rect.top()) * sy),
		    rounded((shown.right() - rect.left()) * sx),
		    rounded((shown.bottom() - rect.top()) * sy),
		};
	}

	if (visible) {
		SetWindowPos(
		    this->mHost,
		    HWND_TOPMOST,
		    host.x(),
		    host.y(),
		    host.width(),
		    host.height(),
		    SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS
		);
		auto corner = QPoint(rounded((rect.left() - shown.left()) * dpr), rounded((rect.top() - shown.top()) * dpr));
		auto full = QSize(rounded(rect.width() * dpr), rounded(rect.height() * dpr));
		auto radius = static_cast<int>(std::lround(this->mRadius * dpr));
		auto shape = QRect(corner, full);
		if (shape != this->mHostShape || radius != this->mHostRadius || host.size() != this->mHostSize) {
			this->mHostShape = shape;
			this->mHostRadius = radius;
			this->mHostSize = host.size();
			QMetaObject::invokeMethod(
			    this,
			    [this, size = host.size(), corner, full, radius]() { this->shapeHost(size, corner, full, radius); },
			    Qt::QueuedConnection
			);
		}
	} else if (this->mHost != nullptr && this->mApplied.fVisible) {
		ShowWindowAsync(this->mHost, SW_HIDE);
	}

	if (std::memcmp(&props, &this->mApplied, sizeof(props)) == 0) return;
	if (FAILED(DwmUpdateThumbnailProperties(this->mThumbnail, &props))) {
		QMetaObject::invokeMethod(
		    this,
		    [this]() {
			    this->unregister();
			    emit this->hasContentChanged();
		    },
		    Qt::QueuedConnection
		);
		return;
	}
	this->mApplied = props;
}

void WindowThumbnail::shapeHost(QSize size, QPoint corner, QSize full, int radius) {
	if (this->mHost == nullptr) return;
	if (radius <= 0) {
		SetWindowRgn(this->mHost, nullptr, TRUE);
		return;
	}
	auto* region = CreateRoundRectRgn(
	    corner.x(),
	    corner.y(),
	    corner.x() + full.width() + 1,
	    corner.y() + full.height() + 1,
	    radius * 2,
	    radius * 2
	);
	auto* clip = CreateRectRgn(0, 0, size.width(), size.height());
	CombineRgn(region, region, clip, RGN_AND);
	DeleteObject(clip);
	if (SetWindowRgn(this->mHost, region, TRUE) == 0) DeleteObject(region);
}

void WindowThumbnail::setRadius(qreal radius) {
	if (qFuzzyCompare(radius, this->mRadius)) return;
	this->mRadius = radius;
	emit this->radiusChanged();
	if (auto* window = this->window()) window->update();
}

void WindowThumbnail::setSourceSize(QSize size) {
	if (size == this->mSourceSize) return;
	this->mSourceSize = size;
	emit this->sourceSizeChanged();
	this->updateImplicitSize();
}

void WindowThumbnail::updateImplicitSize() {
	auto size = this->mSourceSize.toSizeF();
	auto constraint = this->mConstraintSize;
	if (!size.isEmpty() && constraint.width() > 0 && constraint.height() > 0) {
		size.scale(constraint, Qt::KeepAspectRatio);
	}
	this->setImplicitSize(size.width(), size.height());
}

} // namespace qs::wayland::screencopy
