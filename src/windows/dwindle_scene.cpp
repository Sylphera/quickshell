#include "dwindle_scene.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>

#include <d3d11.h>
#include <dcomp.h>
#include <dcompanimation.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <qimage.h>
#include <qloggingcategory.h>
#include <qtimer.h>

#include "dwindle_scene_geometry.hpp"
#include "panel_window.hpp"

namespace qs::windows {

namespace {

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logDwindleScene, "quickshell.windows.dwindle.scene", QtWarningMsg);

constexpr double Duration = 0.16;
constexpr auto SettleMs = 55;
constexpr auto OverlayClass = L"Quickshell.DwindleComposition";
constexpr WORD SharedThumbnailOrdinal = 147;
constexpr DWORD SharedThumbnailKind = 2;
constexpr DWORD CompositionThumbnail = 0x04000000;

using CreateThumbnailVisual = HRESULT(WINAPI*)(
    HWND, HWND, DWORD, DWM_THUMBNAIL_PROPERTIES*, void*, void**, HTHUMBNAIL*
);

HRESULT createThumbnail(
    HWND host,
    HWND source,
    IDCompositionDesktopDevice* device,
    DWM_THUMBNAIL_PROPERTIES* properties,
    IDCompositionVisual** visual,
    HTHUMBNAIL* thumbnail
) {
	static auto create = reinterpret_cast<CreateThumbnailVisual>(
	    GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), MAKEINTRESOURCEA(SharedThumbnailOrdinal))
	);
	if (create == nullptr) return E_NOTIMPL;
	__try {
		return create(host, source, SharedThumbnailKind, properties, device,
		              reinterpret_cast<void**>(visual), thumbnail);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return E_FAIL;
	}
}

bool succeeded(HRESULT result, const char* operation) {
	if (SUCCEEDED(result)) return true;
	qCDebug(logDwindleScene) << operation << Qt::hex << result;
	return false;
}

QRect frame(HWND window) {
	RECT r {};
	if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
		return {};
	return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool cloaked(HWND window) {
	DWORD value = 0;
	return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &value, sizeof(value)))
	    && value != 0;
}

float windowRadius(HWND hwnd) {
	auto region = CreateRectRgn(0, 0, 0, 0);
	if (region != nullptr) {
		RECT box {};
		const auto shaped = GetWindowRgn(hwnd, region) == COMPLEXREGION
		                 && GetRgnBox(region, &box) != ERROR;
		int inset = 0;
		if (shaped) {
			const auto limit = std::min(64L, (box.right - box.left) / 2);
			while (inset < limit && !PtInRegion(region, box.left + inset, box.top)) ++inset;
		}
		DeleteObject(region);
		if (inset > 0) return static_cast<float>(inset);
	}
	DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_DEFAULT;
	DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
	if (preference == DWMWCP_DONOTROUND) return 0;
	const auto smallCorner = preference == DWMWCP_ROUNDSMALL;
	bool configured = false;
	const auto radius = qEnvironmentVariableIntValue(
	    smallCorner ? "II_DWINDLE_SMALL_CORNER_RADIUS" : "II_DWINDLE_CORNER_RADIUS", &configured
	);
	const auto dip = configured && radius >= 0 && radius <= 64 ? radius : (smallCorner ? 4 : 8);
	const auto dpi = GetDpiForWindow(hwnd);
	return dip * (dpi == 0 ? 1.0f : dpi / 96.0f);
}

QImage captureWorkspace(const QRect& work) {
	if (qint64(work.width()) * work.height() > 3840LL * 2160) return {};
	auto* screen = GetDC(nullptr);
	if (screen == nullptr) return {};
	auto* memory = CreateCompatibleDC(screen);
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = work.width();
	info.bmiHeader.biHeight = -work.height();
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* pixels = nullptr;
	auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
	QImage image;
	if (memory != nullptr && bitmap != nullptr) {
		auto* previous = SelectObject(memory, bitmap);
		if (previous != nullptr && BitBlt(memory, 0, 0, work.width(), work.height(), screen,
		                                 work.x(), work.y(), SRCCOPY)) {
			GdiFlush();
			image = QImage(static_cast<uchar*>(pixels), work.width(), work.height(), QImage::Format_RGB32)
			            .copy();
		}
		if (previous != nullptr) SelectObject(memory, previous);
	}
	if (bitmap != nullptr) DeleteObject(bitmap);
	if (memory != nullptr) DeleteDC(memory);
	ReleaseDC(nullptr, screen);
	return image;
}

bool isUncovered(HWND window, const QRect& bounds, HWND host) {
	DWORD affinity = 0;
	if (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE) return false;
	for (auto* above = GetWindow(window, GW_HWNDPREV); above != nullptr; above = GetWindow(above, GW_HWNDPREV)) {
		if (above != host && IsWindowVisible(above) && !IsIconic(above)
		    && !cloaked(above) && frame(above).intersects(bounds))
			return false;
	}
	return true;
}

qint64 counter() {
	LARGE_INTEGER value {};
	QueryPerformanceCounter(&value);
	return value.QuadPart;
}

qint64 frequency() {
	static const auto value = [] {
		LARGE_INTEGER result {};
		QueryPerformanceFrequency(&result);
		return result.QuadPart;
	}();
	return value;
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
	switch (message) {
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_NCHITTEST: return HTTRANSPARENT;
	default: return DefWindowProcW(hwnd, message, wParam, lParam);
	}
}

HWND createHost(const QRect& r) {
	auto* module = GetModuleHandleW(nullptr);
	static const bool registered = [module] {
		WNDCLASSW windowClass {};
		windowClass.hInstance = module;
		windowClass.lpfnWndProc = overlayProc;
		windowClass.lpszClassName = OverlayClass;
		return RegisterClassW(&windowClass) != 0;
	}();
	if (!registered) return nullptr;
	auto* window = CreateWindowExW(
	    WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
	    OverlayClass, L"", WS_POPUP, r.x(), r.y(), r.width(), r.height(),
	    nullptr, nullptr, module, nullptr
	);
	BOOL disable = TRUE;
	if (window != nullptr)
		DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
	return window;
}

struct Thumbnail {
	ComPtr<IDCompositionVisual> visual;
	HTHUMBNAIL handle = nullptr;
	QRect sourceCrop;
	QRect sourceRect;
	QPoint contentOffset;

	~Thumbnail() {
		if (this->handle != nullptr) DwmUnregisterThumbnail(this->handle);
	}
};

struct Tile {
	Thumbnail source;
	ComPtr<IDCompositionVisual> container;
	ComPtr<IDCompositionRectangleClip> clip;
	ComPtr<IDCompositionVisual> liveFrame;
	ComPtr<IDCompositionVisual> livePixels;
	ComPtr<IDCompositionRectangleClip> liveClip;
	QSize liveSize;
	QPoint liveOffset;
	ComPtr<IDCompositionVisual> placeholder;
	ComPtr<IDCompositionVisual> retained;
	QSize retainedSize;
	float radius = 0;
	bool showingRetained = false;
	QRectF from;
	QRect target;
	qint64 start = 0;
	bool waitingForSize = false;

	[[nodiscard]] QRectF at(qint64 now) const {
		auto elapsed = static_cast<double>(now - this->start) / frequency();
		return dwindle::interpolate(this->from, this->target, std::clamp(elapsed / Duration, 0.0, 1.0));
	}
};

struct Graphics {
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<ID3D11DeviceContext> context;

	Graphics() {
		ComPtr<ID3D11Device> device;
		if (succeeded(D3D11CreateDevice(
		        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		        nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, this->context.GetAddressOf()
		    ), "D3D11CreateDevice"))
			succeeded(device.As(&this->dxgi), "IDXGIDevice");
	}
};

Graphics& graphics() {
	static Graphics shared;
	return shared;
}

class Scene {
public:
	explicit Scene(const QRect& work): work(work) {
		this->idle.setSingleShot(true);
		QObject::connect(&this->idle, &QTimer::timeout, &this->idle, [this] { this->settle(); });
		this->sample.setTimerType(Qt::PreciseTimer);
		this->sample.setInterval(9);
		QObject::connect(&this->sample, &QTimer::timeout, &this->sample, [this] { this->refresh(); });
	}

	~Scene() { this->hide(); }

	bool show(const QList<DwindleTile>& plan);
	void hide();
	[[nodiscard]] bool visible() const { return this->host != nullptr; }
	[[nodiscard]] bool matches(const QList<DwindleTile>& plan) const;

	QRect work;

private:
	bool prepare();
	bool makeTileColor();
	ComPtr<IDCompositionSurface> upload(const QImage& image);
	bool addTile(Tile& tile, const DwindleTile& placement, const QImage& before);
	void retain(Tile& tile, const DwindleTile& placement, const QImage& before);
	bool retainedVisible(Tile& tile, bool visible);
	bool resizeLiveClip(Tile& tile);
	bool update(Tile& tile, const QRectF& from, const QRect& target, qint64 now);
	bool addSource(Thumbnail& thumb, HWND window, const QRect& crop = {});
	bool setNativeSize(Thumbnail& thumb, HWND window);
	void refresh();
	void settle();
	bool animate(float from, float to, qint64 time, auto setter);

	HWND host = nullptr;
	HWND backdropWindow = nullptr;
	ComPtr<IDCompositionDesktopDevice> desktopDevice;
	ComPtr<IDCompositionDevice> device;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> root;
	ComPtr<IDCompositionSurface> tileColor;
	Thumbnail backdrop;
	std::map<HWND, std::unique_ptr<Tile>> tiles;
	QTimer idle;
	QTimer sample;
	int retries = 0;
};

HWND backdropFor(const QRect& work, HWND host) {
	HWND found = nullptr;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (hwnd == host || !IsWindowVisible(hwnd) || cloaked(hwnd) || !frame(hwnd).contains(work))
			continue;
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == GetCurrentProcessId() && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
			return hwnd;
		if (hwnd == GetShellWindow()) found = hwnd;
	}
	return found;
}

bool Scene::prepare() {
	if (this->host != nullptr) return true;
	this->host = createHost(this->work);
	if (this->host == nullptr) {
		qCDebug(logDwindleScene) << "Overlay creation failed" << GetLastError();
		return false;
	}
	this->backdropWindow = backdropFor(this->work, this->host);
	if (this->backdropWindow == nullptr) {
		qCDebug(logDwindleScene) << "No desktop background source for" << this->work;
		return false;
	}

	auto& graphics = qs::windows::graphics();
	if (graphics.dxgi == nullptr
	    || !succeeded(DCompositionCreateDevice3(graphics.dxgi.Get(),
	                                            IID_PPV_ARGS(this->desktopDevice.GetAddressOf())),
	                  "DCompositionCreateDevice3")
	    || !succeeded(this->desktopDevice.As(&this->device), "IDCompositionDevice")
	    || !succeeded(this->device->CreateTargetForHwnd(this->host, TRUE, this->target.GetAddressOf()),
	                  "CreateTargetForHwnd")
	    || !succeeded(this->device->CreateVisual(this->root.GetAddressOf()), "CreateVisual")
	    || !succeeded(this->target->SetRoot(this->root.Get()), "SetRoot")
	    || !this->makeTileColor())
		return false;

	const auto crop = this->work.translated(-frame(this->backdropWindow).topLeft());
	if (!this->addSource(this->backdrop, this->backdropWindow, crop)) {
		qCDebug(logDwindleScene) << "Could not attach desktop background" << crop;
		return false;
	}
	return succeeded(this->root->AddVisual(this->backdrop.visual.Get(), TRUE, nullptr), "Add backdrop");
}

ComPtr<IDCompositionSurface> Scene::upload(const QImage& image) {
	ComPtr<IDCompositionSurface> content;
	if (image.isNull() || FAILED(this->device->CreateSurface(
	        image.width(), image.height(), DXGI_FORMAT_B8G8R8A8_UNORM,
	        DXGI_ALPHA_MODE_IGNORE, content.GetAddressOf()
	    ))) return {};
	ComPtr<IDXGISurface> surface;
	POINT offset {};
	if (!succeeded(content->BeginDraw(nullptr, IID_PPV_ARGS(surface.GetAddressOf()), &offset),
	               "BeginDraw")) return {};

	ComPtr<ID3D11Texture2D> texture;
	auto copied = succeeded(surface.As(&texture), "Workspace texture");
	if (copied) {
		const D3D11_BOX region {
		    static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
		    static_cast<UINT>(offset.x + image.width()), static_cast<UINT>(offset.y + image.height()), 1,
		};
		graphics().context->UpdateSubresource(texture.Get(), 0, &region, image.constBits(),
		                                      static_cast<UINT>(image.bytesPerLine()), 0);
	}
	if (!succeeded(content->EndDraw(), "EndDraw") || !copied) return {};
	return content;
}

bool Scene::makeTileColor() {
	auto image = QImage(1, 1, QImage::Format_RGB32);
	image.fill(0xff202020);
	this->tileColor = this->upload(image);
	return this->tileColor != nullptr;
}

bool Scene::addSource(Thumbnail& thumb, HWND window, const QRect& crop) {
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION
	             | DWM_TNP_RECTSOURCE | CompositionThumbnail;
	props.fSourceClientAreaOnly = FALSE;
	props.fVisible = TRUE;
	props.opacity = 255;
	const auto source = crop.isEmpty() ? QRect(QPoint(), frame(window).size()) : crop;
	if (source.isEmpty() || source.x() < 0 || source.y() < 0) return false;
	props.rcSource = {source.x(), source.y(), source.x() + source.width(), source.y() + source.height()};
	props.rcDestination = {0, 0, source.width(), source.height()};
	auto result = createThumbnail(this->host, window, this->desktopDevice.Get(), &props,
	                              thumb.visual.GetAddressOf(), &thumb.handle);
	if (FAILED(result) || thumb.visual == nullptr || thumb.handle == nullptr) {
		qCDebug(logDwindleScene) << "Shared thumbnail unavailable" << source << Qt::hex << result;
		return false;
	}
	return !crop.isEmpty() || this->setNativeSize(thumb, window);
}

bool Scene::setNativeSize(Thumbnail& thumb, HWND window) {
	SIZE size {};
	if (!succeeded(DwmQueryThumbnailSourceSize(thumb.handle, &size), "DwmQueryThumbnailSourceSize")
	    || size.cx <= 0 || size.cy <= 0)
		return false;
	auto crop = QRect(0, 0, size.cx, size.cy);
	auto source = crop;
	RECT raw {};
	const auto visible = frame(window);
	thumb.contentOffset = {};
	if (!visible.isEmpty() && GetWindowRect(window, &raw)) {
		const auto inner = visible.translated(-raw.left, -raw.top);
		const auto rawSize = QSize(raw.right - raw.left, raw.bottom - raw.top);
		if (crop.size() == rawSize && crop.contains(inner)) {
			crop = inner;
		} else if (crop.size() == visible.size() && QRect(QPoint(), rawSize).contains(inner)) {
			thumb.contentOffset = -inner.topLeft();
			source = QRect(QPoint(), rawSize);
		}
	}
	if (crop == thumb.sourceCrop && source == thumb.sourceRect) return true;
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION
	             | DWM_TNP_RECTSOURCE | CompositionThumbnail;
	props.fVisible = TRUE;
	props.opacity = 255;
	props.rcDestination = {0, 0, source.width(), source.height()};
	props.rcSource = {source.x(), source.y(), source.x() + source.width(), source.y() + source.height()};
	if (source != crop && FAILED(DwmUpdateThumbnailProperties(thumb.handle, &props))) {
		source = crop;
		props.rcDestination = {0, 0, crop.width(), crop.height()};
		props.rcSource = {crop.x(), crop.y(), crop.x() + crop.width(), crop.y() + crop.height()};
	}
	if (!succeeded(DwmUpdateThumbnailProperties(thumb.handle, &props), "DwmUpdateThumbnailProperties"))
		return false;
	thumb.sourceCrop = crop;
	thumb.sourceRect = source;
	return true;
}

bool Scene::animate(float from, float to, qint64 time, auto setter) {
	if (from == to) return SUCCEEDED(setter(to));
	ComPtr<IDCompositionAnimation> animation;
	const auto distance = static_cast<double>(to) - from;
	LARGE_INTEGER begin {};
	begin.QuadPart = time;
	return SUCCEEDED(this->device->CreateAnimation(animation.GetAddressOf()))
	    && SUCCEEDED(animation->SetAbsoluteBeginTime(begin))
	    && SUCCEEDED(animation->AddCubic(
	        0, from, static_cast<float>(3 * distance / Duration),
	        static_cast<float>(-3 * distance / (Duration * Duration)),
	        static_cast<float>(distance / (Duration * Duration * Duration))
	    ))
	    && SUCCEEDED(animation->End(Duration, to))
	    && SUCCEEDED(setter(animation.Get()));
}

bool Scene::addTile(Tile& tile, const DwindleTile& placement, const QImage& before) {
	tile.radius = windowRadius(placement.window);
	tile.waitingForSize = placement.previous.isEmpty();
	if (!this->addSource(tile.source, placement.window)
	    || FAILED(this->device->CreateVisual(tile.container.GetAddressOf()))
	    || FAILED(this->device->CreateVisual(tile.liveFrame.GetAddressOf()))
	    || FAILED(this->device->CreateVisual(tile.livePixels.GetAddressOf()))
	    || FAILED(this->device->CreateRectangleClip(tile.liveClip.GetAddressOf()))
	    || FAILED(tile.liveClip->SetLeft(0.0f))
	    || FAILED(tile.liveClip->SetTop(0.0f))
	    || !this->resizeLiveClip(tile)
	    || FAILED(tile.liveFrame->SetClip(tile.liveClip.Get()))
	    || FAILED(tile.livePixels->AddVisual(tile.source.visual.Get(), FALSE, nullptr))
	    || FAILED(tile.liveFrame->AddVisual(tile.livePixels.Get(), FALSE, nullptr))
	    || FAILED(this->device->CreateRectangleClip(tile.clip.GetAddressOf()))
	    || FAILED(tile.clip->SetLeft(0.0f))
	    || FAILED(tile.clip->SetTop(0.0f))
	    || FAILED(tile.container->SetClip(tile.clip.Get()))
	    || FAILED(this->root->AddVisual(tile.container.Get(), FALSE, nullptr)))
		return false;

	if (!tile.waitingForSize) {
		this->retain(tile, placement, before);
		return SUCCEEDED(tile.container->AddVisual(tile.liveFrame.Get(), FALSE, nullptr));
	}

	D2D_MATRIX_3X2_F stretch {};
	stretch._11 = placement.target.width();
	stretch._22 = placement.target.height();
	return SUCCEEDED(this->device->CreateVisual(tile.placeholder.GetAddressOf()))
	    && SUCCEEDED(tile.placeholder->SetContent(this->tileColor.Get()))
	    && SUCCEEDED(tile.placeholder->SetTransform(stretch))
	    && SUCCEEDED(tile.container->AddVisual(tile.placeholder.Get(), TRUE, nullptr));
}

bool Scene::resizeLiveClip(Tile& tile) {
	const auto size = tile.source.sourceCrop.size();
	const auto radius = std::min({tile.radius, size.width() / 2.0f, size.height() / 2.0f});
	if (FAILED(tile.livePixels->SetOffsetX(static_cast<float>(tile.source.contentOffset.x())))
	    || FAILED(tile.livePixels->SetOffsetY(static_cast<float>(tile.source.contentOffset.y())))
	    || FAILED(tile.liveClip->SetRight(static_cast<float>(size.width())))
	    || FAILED(tile.liveClip->SetBottom(static_cast<float>(size.height())))
	    || FAILED(tile.liveClip->SetTopLeftRadiusX(radius))
	    || FAILED(tile.liveClip->SetTopLeftRadiusY(radius))
	    || FAILED(tile.liveClip->SetTopRightRadiusX(radius))
	    || FAILED(tile.liveClip->SetTopRightRadiusY(radius))
	    || FAILED(tile.liveClip->SetBottomLeftRadiusX(radius))
	    || FAILED(tile.liveClip->SetBottomLeftRadiusY(radius))
	    || FAILED(tile.liveClip->SetBottomRightRadiusX(radius))
	    || FAILED(tile.liveClip->SetBottomRightRadiusY(radius)))
		return false;
	tile.liveSize = size;
	tile.liveOffset = tile.source.contentOffset;
	return true;
}

void Scene::retain(Tile& tile, const DwindleTile& placement, const QImage& before) {
	if (before.isNull() || placement.previous.isEmpty()
	    || placement.previous.size() == placement.target.size()
	    || !this->work.contains(placement.previous) || frame(placement.window) != placement.previous
	    || !isUncovered(placement.window, placement.previous, this->host))
		return;

	const auto old = placement.previous.translated(-this->work.topLeft());
	const auto surface = this->upload(before.copy(old));
	ComPtr<IDCompositionVisual> visual;
	if (surface != nullptr && SUCCEEDED(this->device->CreateVisual(visual.GetAddressOf()))
	    && SUCCEEDED(visual->SetContent(surface.Get()))) {
		tile.retained = std::move(visual);
		tile.retainedSize = old.size();
	}
}

bool Scene::retainedVisible(Tile& tile, bool visible) {
	if (tile.showingRetained == visible || tile.retained == nullptr) return false;
	const auto hr = visible ? tile.container->AddVisual(tile.retained.Get(), FALSE, nullptr)
	                        : tile.container->RemoveVisual(tile.retained.Get());
	if (FAILED(hr)) return false;
	tile.showingRetained = visible;
	return true;
}

bool Scene::update(Tile& tile, const QRectF& origin, const QRect& target, qint64 now) {
	tile.from = origin;
	tile.target = target;
	tile.start = now;
	if (tile.retainedSize.width() >= origin.width() - 1
	    && tile.retainedSize.height() >= origin.height() - 1
	    && (tile.target.width() < origin.width() - 1 || tile.target.height() < origin.height() - 1))
		this->retainedVisible(tile, true);
	const auto from = tile.from.translated(-this->work.topLeft());
	const auto to = tile.target.translated(-this->work.topLeft());
	const auto radius = [&tile](const QRectF& box) {
		return static_cast<float>(std::min({double(tile.radius), box.width() / 2, box.height() / 2}));
	};
	const auto corner = [this, fromRadius = radius(from), toRadius = radius(to), now](auto setter) {
		return this->animate(fromRadius, toRadius, now, setter);
	};
	return corner([&tile](auto value) { return tile.clip->SetTopLeftRadiusX(value); })
	    && corner([&tile](auto value) { return tile.clip->SetTopLeftRadiusY(value); })
	    && corner([&tile](auto value) { return tile.clip->SetTopRightRadiusX(value); })
	    && corner([&tile](auto value) { return tile.clip->SetTopRightRadiusY(value); })
	    && corner([&tile](auto value) { return tile.clip->SetBottomLeftRadiusX(value); })
	    && corner([&tile](auto value) { return tile.clip->SetBottomLeftRadiusY(value); })
	    && corner([&tile](auto value) { return tile.clip->SetBottomRightRadiusX(value); })
	    && corner([&tile](auto value) { return tile.clip->SetBottomRightRadiusY(value); })
	    && this->animate(from.x(), to.x(), now, [&tile](auto value) {
		    return tile.container->SetOffsetX(value);
	    })
	    && this->animate(from.y(), to.y(), now, [&tile](auto value) {
		    return tile.container->SetOffsetY(value);
	    })
	    && this->animate(from.width(), to.width(), now, [&tile](auto value) {
		    return tile.clip->SetRight(value);
	    })
	    && this->animate(from.height(), to.height(), now, [&tile](auto value) {
		    return tile.clip->SetBottom(value);
	    });
}

bool Scene::matches(const QList<DwindleTile>& plan) const {
	if (plan.size() != static_cast<qsizetype>(this->tiles.size())) return false;
	return std::all_of(plan.begin(), plan.end(), [this](const auto& placement) {
		auto it = this->tiles.find(placement.window);
		return it != this->tiles.end() && it->second->target == placement.target;
	});
}

bool Scene::show(const QList<DwindleTile>& plan) {
	if (!this->prepare()) return false;
	for (auto it = this->tiles.begin(); it != this->tiles.end();) {
		auto present = std::any_of(plan.begin(), plan.end(), [hwnd = it->first](const auto& tile) {
			return tile.window == hwnd;
		});
		if (present) {
			++it;
			continue;
		}
		if (FAILED(this->root->RemoveVisual(it->second->container.Get()))) return false;
		it = this->tiles.erase(it);
	}
	const auto needFrame = this->tiles.empty() && std::ranges::any_of(plan, [](const auto& tile) {
		return !tile.previous.isEmpty() && tile.previous.size() != tile.target.size();
	});
	const auto before = needFrame ? captureWorkspace(this->work) : QImage();
	for (const auto& placement: plan) {
		auto [it, added] = this->tiles.try_emplace(placement.window);
		if (!added) continue;
		it->second = std::make_unique<Tile>();
		if (!this->addTile(*it->second, placement, before)) {
			qCDebug(logDwindleScene) << "Could not attach tile" << placement.window;
			return false;
		}
	}
	auto start = counter();
	DCOMPOSITION_FRAME_STATISTICS stats {};
	if (SUCCEEDED(this->device->GetFrameStatistics(&stats)))
		start = std::max(start, stats.nextEstimatedFrameTime.QuadPart);
	QList<QRect> targets;
	targets.reserve(plan.size());
	QList<std::optional<QRectF>> origins(plan.size());
	for (qsizetype i = 0; i < plan.size(); ++i) {
		const auto& placement = plan[i];
		const auto& tile = *this->tiles.at(placement.window);
		targets.append(placement.target);
		if (!tile.target.isEmpty()) origins[i] = tile.at(start);
		else if (!placement.previous.isEmpty()) origins[i] = placement.previous;
	}
	if (!dwindle::planOrigins(targets, origins)) {
		qCDebug(logDwindleScene) << "No non-overlapping transition for" << targets.size() << "tiles";
		return false;
	}
	for (qsizetype i = 0; i < plan.size(); ++i) {
		const auto& placement = plan[i];
		if (!this->update(*this->tiles.at(placement.window), *origins[i], placement.target, start)) {
			qCDebug(logDwindleScene) << "Could not schedule tile" << placement.window;
			return false;
		}
	}
	if (!succeeded(this->device->Commit(), "Commit")) return false;
	if (!IsWindowVisible(this->host)) {
		auto panels = WinPanelWindow::aboveWindowHandles();
		auto above = HWND_TOPMOST;
		for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
			if (panels.contains(hwnd) && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
				above = hwnd;
		}
		if (!SetWindowPos(this->host, above, 0, 0, 0, 0,
		                  SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW))
			return false;
	}
	this->retries = 0;
	this->idle.start(static_cast<int>(Duration * 1000) + SettleMs);
	this->sample.start();
	qCDebug(logDwindleScene) << "Animating" << plan.size() << "tiles in" << this->work;
	return true;
}

void Scene::refresh() {
	if (this->host == nullptr || !IsWindow(this->backdropWindow)) {
		this->hide();
		return;
	}
	if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0
	    || GetAsyncKeyState(VK_MBUTTON) < 0)
	{
		this->hide();
		return;
	}
	const auto foreground = GetForegroundWindow();
	const auto intersecting = this->work.intersects(frame(foreground));
	if (foreground != nullptr && intersecting && !this->tiles.contains(foreground)) {
		this->hide();
		return;
	}
	bool changed = false;
	const auto now = counter();
	for (auto& [hwnd, tile]: this->tiles) {
		if (!IsWindow(hwnd) || !this->setNativeSize(tile->source, hwnd)) continue;
		if ((tile->liveSize != tile->source.sourceCrop.size()
		     || tile->liveOffset != tile->source.contentOffset)
		    && this->resizeLiveClip(*tile))
			changed = true;
		if (tile->waitingForSize) {
			const auto size = tile->source.sourceCrop.size();
			if (std::abs(size.width() - tile->target.width()) > 4
			    || std::abs(size.height() - tile->target.height()) > 4)
				continue;
			if (SUCCEEDED(tile->container->AddVisual(tile->liveFrame.Get(), FALSE, nullptr))) {
				tile->waitingForSize = false;
				tile->container->RemoveVisual(tile->placeholder.Get());
				tile->placeholder.Reset();
				changed = true;
			}
		}
		const auto moving = tile->at(now);
		const auto fills = [&moving](const QSize& size) {
			return size.width() >= moving.width() - 1 && size.height() >= moving.height() - 1;
		};
		changed |= this->retainedVisible(*tile, !fills(tile->source.sourceCrop.size())
		                                     && fills(tile->retainedSize));
	}
	if (changed && FAILED(this->device->Commit())) this->hide();
}

void Scene::settle() {
	const auto placed = std::all_of(this->tiles.begin(), this->tiles.end(), [](const auto& pair) {
		const auto& [hwnd, tile] = pair;
		if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return true;
		const auto actual = frame(hwnd);
		const auto target = tile->target.adjusted(-2, -2, 2, 2);
		return !actual.isEmpty() && target.contains(actual)
		    && std::abs(actual.width() - tile->target.width()) <= 4
		    && std::abs(actual.height() - tile->target.height()) <= 4;
	});
	if (!placed && this->retries++ < 3) {
		this->idle.start(16);
		return;
	}
	this->hide();
}

void Scene::hide() {
	this->idle.stop();
	this->sample.stop();
	if (this->host != nullptr) ShowWindow(this->host, SW_HIDE);
	if (this->target != nullptr) this->target->SetRoot(nullptr);
	if (this->device != nullptr) this->device->Commit();
	this->tiles.clear();
	if (this->backdrop.handle != nullptr) {
		DwmUnregisterThumbnail(this->backdrop.handle);
		this->backdrop.handle = nullptr;
	}
	this->backdrop.visual.Reset();
	this->root.Reset();
	this->tileColor.Reset();
	this->target.Reset();
	this->device.Reset();
	this->desktopDevice.Reset();
	if (this->host != nullptr) DestroyWindow(this->host);
	this->host = nullptr;
	this->backdropWindow = nullptr;
}

auto& scenes() {
	static auto* byMonitor = new std::map<HMONITOR, std::unique_ptr<Scene>>();
	return *byMonitor;
}

}

bool DwindleScene::enabled() {
	static const auto enabled = qEnvironmentVariableIntValue("II_DWINDLE_SCENE") == 1;
	return enabled;
}

void DwindleScene::warmUp() { (void)graphics(); }

void DwindleScene::present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed) {
	if (tiles.isEmpty() || work.isEmpty()) {
		DwindleScene::dismiss(monitor);
		return;
	}
	auto found = scenes().find(monitor);
	if (!changed && (found == scenes().end() || !found->second->visible())) return;
	if (found != scenes().end() && found->second->visible()
	    && found->second->work == work && found->second->matches(tiles)) return;
	auto& scene = scenes()[monitor];
	if (scene == nullptr || scene->work != work) scene = std::make_unique<Scene>(work);
	if (!scene->show(tiles)) {
		qCDebug(logDwindleScene) << "Using immediate placement";
		DwindleScene::dismiss(monitor);
	}
}

void DwindleScene::dismiss(HMONITOR monitor) { scenes().erase(monitor); }

void DwindleScene::dismissAll() { scenes().clear(); }

}
