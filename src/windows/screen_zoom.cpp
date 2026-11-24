#include "screen_zoom.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

#include <qcoreapplication.h>
#include <qeasingcurve.h>
#include <qelapsedtimer.h>
#include <qlogging.h>
#include <qpointer.h>
#include <qthread.h>
#include <qtimer.h>

#include <qt_windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace qs::windows {

namespace {

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using DxgiInterface = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

std::mutex viewMutex;
ScreenZoomView visibleView;

void setVisibleView(const ScreenZoomView& view) {
	const auto lock = std::scoped_lock(viewMutex);
	visibleView = view;
}

class ZoomSurface {
public:
	~ZoomSurface() {
		if (this->host) DestroyWindow(this->host);
		try {
			if (this->session) this->session.Close();
			if (this->pool) this->pool.Close();
		} catch (const winrt::hresult_error& error) {
			qWarning() << "Screen zoom cleanup failed:" << Qt::hex << error.code().value;
		}
	}

	void initialize(POINT cursor) {
		this->monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info {};
		info.cbSize = sizeof(info);
		if (!GetMonitorInfoW(this->monitor, &info)) winrt::throw_last_error();
		this->bounds = info.rcMonitor;
		this->createWindow();

		winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
		    D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, this->device.put(),
		    nullptr, this->context.put()));
		auto dxgi = this->device.as<IDXGIDevice>();
		winrt::com_ptr<::IInspectable> inspectable;
		winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));

		auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
		GraphicsCaptureItem item {nullptr};
		winrt::check_hresult(factory->CreateForMonitor(
		    this->monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
		this->size = item.Size();
		if (this->size.Width != this->bounds.right - this->bounds.left
		    || this->size.Height != this->bounds.bottom - this->bounds.top)
			winrt::throw_hresult(E_UNEXPECTED);

		this->createComposition(dxgi);
		this->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(inspectable.as<IDirect3DDevice>(),
		    DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, this->size);
		this->itemClosed = item.Closed(winrt::auto_revoke, [closed = this->closed](auto const&, auto const&) {
			closed->store(true);
		});
		this->session = this->pool.CreateCaptureSession(item);
		this->session.IsCursorCaptureEnabled(false);
		try {
			this->session.IsBorderRequired(false);
		} catch (const winrt::hresult_error&) {}
		this->session.StartCapture();
		this->captureTime.start();
	}

	bool invalid() const { return this->displayChanged || this->closed->load(); }

	void render(float level, POINT cursor) {
		if (auto frame = this->pool.TryGetNextFrame()) {
			const auto frameSize = frame.ContentSize();
			if (frameSize.Width != this->size.Width || frameSize.Height != this->size.Height)
				winrt::throw_hresult(E_UNEXPECTED);

			auto source = frame.Surface().as<DxgiInterface>();
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::check_hresult(source->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
			winrt::com_ptr<ID3D11Texture2D> backBuffer;
			winrt::check_hresult(this->swapChain->GetBuffer(
			    0, winrt::guid_of<ID3D11Texture2D>(), backBuffer.put_void()));
			this->context->CopyResource(backBuffer.get(), texture.get());
			const auto present = this->swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
			if (present != DXGI_ERROR_WAS_STILL_DRAWING) {
				winrt::check_hresult(present);
				this->hasFrame = true;
				this->captureTime.restart();
			}
		}

		if (!this->hasFrame && this->captureTime.hasExpired(1500))
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
		if (!this->hasFrame || level <= 1.0f || !PtInRect(&this->bounds, cursor)) {
			if (IsWindowVisible(this->host)) {
				ShowWindow(this->host, SW_HIDE);
				setVisibleView({});
			}
			return;
		}

		const auto transformChanged = level != this->lastLevel || cursor.x != this->lastCursor.x
		    || cursor.y != this->lastCursor.y;
		if (transformChanged) {
			D2D_MATRIX_3X2_F transform {};
			transform._11 = level;
			transform._22 = level;
			transform._31 = (cursor.x - this->bounds.left) * (1.0f - level);
			transform._32 = (cursor.y - this->bounds.top) * (1.0f - level);
			winrt::check_hresult(this->visual->SetTransform(transform));
			winrt::check_hresult(this->composition->Commit());
			this->lastLevel = level;
			this->lastCursor = cursor;
			this->offset = {transform._31, transform._32};
		}

		const auto visible = IsWindowVisible(this->host) != FALSE;
		if (!visible || this->hasShellWindowAbove()) {
			const auto flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
			    | (visible ? 0 : SWP_SHOWWINDOW);
			if (!SetWindowPos(this->host, HWND_TOPMOST, 0, 0, 0, 0, flags))
				winrt::throw_last_error();
		}
		if (!visible || transformChanged) setVisibleView({this->monitor, level, this->offset});
	}

private:
	bool hasShellWindowAbove() const {
		const auto shellProcess = GetCurrentProcessId();
		for (auto* window = GetWindow(this->host, GW_HWNDPREV); window;
		     window = GetWindow(window, GW_HWNDPREV)) {
			if (!IsWindowVisible(window)) continue;
			DWORD process = 0;
			GetWindowThreadProcessId(window, &process);
			if (process != shellProcess) continue;

			DWORD cloaked = 0;
			DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) continue;

			RECT rect {};
			RECT intersection {};
			if (GetWindowRect(window, &rect) && IntersectRect(&intersection, &rect, &this->bounds))
				return true;
		}
		return false;
	}

	void createWindow() {
		const auto module = GetModuleHandleW(nullptr);
		WNDCLASSW cls {};
		cls.hInstance = module;
		cls.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
			switch (message) {
			case WM_NCCREATE: {
				const auto* creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
				SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
				break;
			}
			case WM_NCHITTEST: return HTTRANSPARENT;
			case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
			case WM_DISPLAYCHANGE:
			case WM_CLOSE:
				if (auto* surface = reinterpret_cast<ZoomSurface*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
					surface->displayChanged = true;
					ShowWindow(window, SW_HIDE);
				}
				return 0;
			default: break;
			}
			return DefWindowProcW(window, message, wparam, lparam);
		};
		cls.lpszClassName = L"QuickshellScreenZoom";
		if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			winrt::throw_last_error();

		this->host = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
		        | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
		    cls.lpszClassName, L"", WS_POPUP, this->bounds.left, this->bounds.top,
		    this->bounds.right - this->bounds.left, this->bounds.bottom - this->bounds.top,
		    nullptr, nullptr, module, this);
		if (!this->host || !SetLayeredWindowAttributes(this->host, 0, 255, LWA_ALPHA)
		    || !SetWindowDisplayAffinity(this->host, WDA_EXCLUDEFROMCAPTURE))
			winrt::throw_last_error();

		BOOL enabled = TRUE;
		DwmSetWindowAttribute(this->host, DWMWA_TRANSITIONS_FORCEDISABLED, &enabled, sizeof(enabled));
		DwmSetWindowAttribute(this->host, DWMWA_EXCLUDED_FROM_PEEK, &enabled, sizeof(enabled));
	}

	void createComposition(const winrt::com_ptr<IDXGIDevice>& dxgi) {
		winrt::com_ptr<IDXGIAdapter> adapter;
		winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
		winrt::com_ptr<IDXGIFactory2> factory;
		winrt::check_hresult(adapter->GetParent(winrt::guid_of<IDXGIFactory2>(), factory.put_void()));

		DXGI_SWAP_CHAIN_DESC1 description {};
		description.Width = static_cast<UINT>(this->size.Width);
		description.Height = static_cast<UINT>(this->size.Height);
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.BufferCount = 2;
		description.Scaling = DXGI_SCALING_STRETCH;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		winrt::check_hresult(factory->CreateSwapChainForComposition(
		    this->device.get(), &description, nullptr, this->swapChain.put()));

		winrt::check_hresult(DCompositionCreateDevice(dxgi.get(),
		    winrt::guid_of<IDCompositionDevice>(), this->composition.put_void()));
		winrt::check_hresult(this->composition->CreateTargetForHwnd(this->host, TRUE, this->target.put()));
		winrt::check_hresult(this->composition->CreateVisual(this->visual.put()));
		winrt::check_hresult(this->visual->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR));
		winrt::check_hresult(this->visual->SetContent(this->swapChain.get()));
		winrt::check_hresult(this->target->SetRoot(this->visual.get()));
		winrt::check_hresult(this->composition->Commit());
	}

	HWND host = nullptr;
	HMONITOR monitor = nullptr;
	QPointF offset;
	RECT bounds {};
	winrt::Windows::Graphics::SizeInt32 size {};
	winrt::com_ptr<ID3D11Device> device;
	winrt::com_ptr<ID3D11DeviceContext> context;
	winrt::com_ptr<IDXGISwapChain1> swapChain;
	winrt::com_ptr<IDCompositionDevice> composition;
	winrt::com_ptr<IDCompositionTarget> target;
	winrt::com_ptr<IDCompositionVisual> visual;
	Direct3D11CaptureFramePool pool {nullptr};
	GraphicsCaptureSession session {nullptr};
	std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
	GraphicsCaptureItem::Closed_revoker itemClosed;
	QElapsedTimer captureTime;
	POINT lastCursor {};
	float lastLevel = 1.0f;
	bool hasFrame = false;
	bool displayChanged = false;
};

float animationLevel(float from, float to, qint64 elapsed) {
	constexpr auto duration = 300;
	if (elapsed >= duration) return to;
	static const auto curve = [] {
		auto easing = QEasingCurve(QEasingCurve::BezierSpline);
		easing.addCubicBezierSegment({0, 0}, {0, 1}, {1, 1});
		return easing;
	}();
	const auto progress = curve.valueForProgress(static_cast<qreal>(elapsed) / duration);
	return from + (to - from) * static_cast<float>(progress);
}

class ScreenZoom: public QObject {
public:
	ScreenZoom() {
		this->frames.setTimerType(Qt::PreciseTimer);
		this->frames.setInterval(8);
		QObject::connect(&this->frames, &QTimer::timeout, this, [this] { this->frame(); });
	}

	~ScreenZoom() override {
		this->reset();
		if (this->apartment) winrt::uninit_apartment();
	}

	void adjust(int steps) {
		const auto next = std::clamp(this->target + steps * 3, 10, 30);
		if (next == this->target) return;
		if (!this->surface) {
			POINT cursor {};
			if (!GetCursorPos(&cursor)) return;
			try {
				if (!this->apartment) {
					winrt::init_apartment(winrt::apartment_type::multi_threaded);
					this->apartment = true;
				}
				auto newSurface = std::make_unique<ZoomSurface>();
				newSurface->initialize(cursor);
				this->surface = std::move(newSurface);
			} catch (const winrt::hresult_error& error) {
				qWarning() << "Screen zoom initialization failed:" << Qt::hex << error.code().value;
				return;
			}
		}

		this->from = this->animation.isValid() ? this->level() : 1.0f;
		this->target = next;
		this->animation.start();
		if (!this->frames.isActive()) this->frames.start();
	}

	void reset() {
		this->frames.stop();
		this->frozen.invalidate();
		setVisibleView({});
		this->surface.reset();
		this->animation.invalidate();
		this->target = 10;
	}

	void freeze() {
		if (this->surface) this->frozen.start();
	}

private:
	float level() const {
		return animationLevel(this->from, this->target / 10.0f, this->animation.elapsed());
	}

	void frame() {
		if ((GetAsyncKeyState(VK_ESCAPE) < 0
		     && (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0))
		    || this->surface->invalid()) {
			this->reset();
			return;
		}

		if (this->frozen.isValid()) {
			if (!this->frozen.hasExpired(2000)) return;
			this->frozen.invalidate();
		}

		const auto zoom = this->level();
		POINT cursor {};
		if ((zoom <= 1.0f && this->target == 10) || !GetCursorPos(&cursor)) {
			this->reset();
			return;
		}
		try {
			this->surface->render(zoom, cursor);
		} catch (const winrt::hresult_error& error) {
			qWarning() << "Screen zoom rendering failed:" << Qt::hex << error.code().value;
			this->reset();
		}
	}

	QTimer frames {this};
	QElapsedTimer animation;
	QElapsedTimer frozen;
	std::unique_ptr<ZoomSurface> surface;
	float from = 1.0f;
	int target = 10;
	bool apartment = false;
};

class ZoomThread: public QObject {
public:
	explicit ZoomThread(QObject* parent): QObject(parent), zoom(new ScreenZoom) {
		this->zoom->moveToThread(&this->thread);
		QObject::connect(&this->thread, &QThread::finished, this->zoom, &QObject::deleteLater);
		this->thread.start();
	}

	~ZoomThread() override {
		this->thread.quit();
		this->thread.wait();
	}

	void adjust(bool increase) {
		QMetaObject::invokeMethod(this->zoom, [worker = this->zoom, increase] {
			worker->adjust(increase ? 1 : -1);
		}, Qt::QueuedConnection);
	}

	void freeze() {
		QMetaObject::invokeMethod(this->zoom, [worker = this->zoom] { worker->freeze(); },
		    Qt::QueuedConnection);
	}

	void reset() {
		QMetaObject::invokeMethod(this->zoom, [worker = this->zoom] { worker->reset(); },
		    Qt::QueuedConnection);
	}

private:
	QThread thread;
	ScreenZoom* zoom;
};

ZoomThread* zoomThread(bool create) {
	static QPointer<ZoomThread> thread;
	if (!thread && create) thread = new ZoomThread(QCoreApplication::instance());
	return thread;
}

}

ScreenZoomView screenZoomView() {
	const auto lock = std::scoped_lock(viewMutex);
	return visibleView;
}

void adjustScreenZoom(bool increase) {
	if (auto* thread = zoomThread(increase)) thread->adjust(increase);
}

void freezeScreenZoom() {
	if (auto* thread = zoomThread(false)) thread->freeze();
}

void resetScreenZoom() {
	if (auto* thread = zoomThread(false)) thread->reset();
}

}
