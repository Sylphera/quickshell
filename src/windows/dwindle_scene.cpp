#include "dwindle_scene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <d3d11.h>
#include <dcomp.h>
#include <dcompanimation.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <qimage.h>
#include <qloggingcategory.h>
#include <qset.h>
#include <qtimer.h>

#include "dwindle_scene_geometry.hpp"
#include "panel_window.hpp"

namespace qs::windows {

namespace {

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logDwindleScene, "quickshell.windows.dwindle.scene", QtWarningMsg);

constexpr double Duration = 0.3;
constexpr float PopInScale = 0.8f;
constexpr float PopOutScale = 0.9f;
constexpr double PopOutDuration = 0.2;
constexpr double CloseFrameLifetime = 1.0;
constexpr double CachedCloseLifetime = 4.0;
constexpr qint64 CloseCacheBudget = 32 * 1024 * 1024;
constexpr auto SettleMs = 55;
constexpr double JoinGrace = 0.12;
constexpr double CoverHold = 0.7;
constexpr int ReleaseRetries = 12;
constexpr int ArrivalRetries = 90;
constexpr double OcclusionMoveGrace = 0.05;
constexpr double OcclusionStageLimit = 0.33;

std::function<bool(HWND)>& occlusionStager() {
	static auto* stage = new std::function<bool(HWND)>();
	return *stage;
}

std::function<void(const QList<HWND>&)>& releaser() {
	static auto* release = new std::function<void(const QList<HWND>&)>();
	return *release;
}
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

QRect sceneViewport(HMONITOR monitor, const QRect& work) {
	MONITORINFO info {};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info)) return work;
	const auto& bounds = info.rcMonitor;
	const QRect viewport(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
	return viewport.contains(work) ? viewport : work;
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

QImage captureWorkspace(const QRect& work, HWND window = nullptr) {
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
		const auto copied = previous != nullptr
		                 && (window != nullptr
		                         ? PrintWindow(window, memory, PW_RENDERFULLCONTENT)
		                         : BitBlt(memory, 0, 0, work.width(), work.height(), screen,
		                                  work.x(), work.y(), SRCCOPY));
		if (copied) {
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

std::atomic_bool& closeCaptureBusy() {
	static auto* busy = new std::atomic_bool(false);
	return *busy;
}

QImage captureWindowPixels(HWND window, const QRect& raw, const QRect& visible) {
	auto image = captureWorkspace(raw, window);
	if (image.isNull()) return {};

	image = image.copy(visible.translated(-raw.topLeft()));
	const auto sample = [&image](int x, int y) {
		return image.pixel(image.width() * x / 8, image.height() * y / 8) & 0xffffff;
	};
	if (sample(4, 4) == 0 && sample(2, 2) == 0 && sample(6, 2) == 0
	    && sample(2, 6) == 0 && sample(6, 6) == 0)
		return {};
	return image;
}

template<typename Work>
bool startCloseCapture(Work work) {
	if (closeCaptureBusy().exchange(true)) return false;
	try {
		std::thread([work = std::move(work)]() mutable {
			work();
			closeCaptureBusy().store(false);
		}).detach();
	} catch (const std::system_error&) {
		closeCaptureBusy().store(false);
		return false;
	}
	return true;
}

QImage captureWindowForClose(HWND window, const QSize& expectedSize) {
	DWORD affinity = 0;
	if (IsHungAppWindow(window) || (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE))
		return {};

	RECT rect {};
	const auto visible = frame(window);
	if (visible.size() != expectedSize || !GetWindowRect(window, &rect)) return {};
	const QRect raw(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
	if (!raw.contains(visible)) return {};

	std::promise<QImage> result;
	auto future = result.get_future();
	if (!startCloseCapture([window, raw, visible, result = std::move(result)]() mutable {
		    result.set_value(captureWindowPixels(window, raw, visible));
	    })) return {};

	if (future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) return {};
	return future.get();
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

bool diagnosticsEnabled() {
	static const auto enabled = qEnvironmentVariableIntValue("II_DWINDLE_DIAGNOSTICS") == 1;
	return enabled;
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

struct AnimatedFrame {
	QRectF from;
	QRect target;
	qint64 start = 0;
	qint64 popStart = 0;

	[[nodiscard]] QRectF at(qint64 now) const {
		auto elapsed = static_cast<double>(now - this->start) / frequency();
		return dwindle::interpolate(this->from, this->target, std::clamp(elapsed / Duration, 0.0, 1.0));
	}

	[[nodiscard]] QRectF shownAt(qint64 now) const {
		auto bounds = this->at(now);
		if (this->popStart == 0) return bounds;
		const auto elapsed = static_cast<double>(now - this->popStart) / frequency();
		const auto scale = PopInScale + (1.0 - PopInScale) * dwindle::ease(elapsed / Duration);
		const auto center = bounds.center();
		bounds.setSize(bounds.size() * scale);
		bounds.moveCenter(center);
		return bounds;
	}
};

struct DragTile {
	HWND window;
	AnimatedFrame frame;
	int order;
};

struct DragScene {
	QRect work;
	QList<DragTile> tiles;
};

struct DragScenes {
	std::mutex mutex;
	std::map<HWND, DragScene> byHost;
};

DragScenes& dragScenes() {
	static DragScenes scenes;
	return scenes;
}

struct Tile: AnimatedFrame {
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
	int order = 0;
	bool waitingForSize = false;
	ComPtr<IDCompositionScaleTransform> pop;
	ComPtr<IDCompositionEffectGroup> fade;

	ComPtr<IDCompositionVisual> diagnosticMarker;
	qint64 occlusionRequested = 0;
	int diagnosticState = -1;
	QSize diagnosticSize;
	QPoint diagnosticOffset;
};

struct CloseFrame {
	QImage pixels;
	QRect bounds;
	float radius;
	qint64 captured;
	bool destroyedOnly = false;

	[[nodiscard]] bool ready(HWND hwnd) const {
		return !IsWindow(hwnd) || (!this->destroyedOnly && !IsWindowVisible(hwnd));
	}
};

struct CachedCloseFrame {
	DWORD pid;
	CloseFrame frame;
};

struct CloseCache {
	std::mutex mutex;
	std::map<HWND, CachedCloseFrame> frames;
	qint64 bytes = 0;
	quint64 generation = 0;
};

CloseCache& closeCache() {
	static auto* cache = new CloseCache;
	return *cache;
}

void forgetCachedFrame(CloseCache& cache, std::map<HWND, CachedCloseFrame>::iterator it) {
	cache.bytes -= it->second.frame.pixels.sizeInBytes();
	cache.frames.erase(it);
}

struct ClosingTile {
	ComPtr<IDCompositionVisual> visual;
	QRect bounds;
	qint64 end;
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
		this->closeExpiry.setSingleShot(true);
		QObject::connect(&this->closeExpiry, &QTimer::timeout, &this->closeExpiry, [this] {
			this->closeFrames.clear();
		});
		this->sample.setTimerType(Qt::PreciseTimer);
		this->sample.setInterval(9);
		QObject::connect(&this->sample, &QTimer::timeout, &this->sample, [this] { this->refresh(); });
	}

	~Scene() { this->hide(); }

	bool show(const QList<DwindleTile>& plan, const QRect& layoutWork);
	void snapshotClose(HWND window);
	void acceptCachedClose(HWND window, CloseFrame frame);
	[[nodiscard]] bool hasClosingTile() const;
	void hold();
	void hide();
	[[nodiscard]] bool visible() const { return this->host != nullptr; }
	[[nodiscard]] bool matches(const QList<DwindleTile>& plan) const;
	[[nodiscard]] QRect frameForDrag(HWND window) const;

	QRect work;

private:
	QRect visibleWork;
	bool prepare();
	void publishDragScene() const;
	bool makeTileColor();
	bool addDiagnosticMarker(Tile& tile);
	void inspectParticipants(qint64 now);
	ComPtr<IDCompositionSurface> upload(const QImage& image);
	bool addTile(Tile& tile, const DwindleTile& placement, const QImage& before);
	bool awaitSize(Tile& tile, const QRect& target);
	void retain(Tile& tile, const DwindleTile& placement, const QImage& before);
	bool retainedVisible(Tile& tile, bool visible);
	bool resizeLiveClip(Tile& tile);
	bool update(Tile& tile, const QRectF& from, const QRect& target, qint64 now);
	bool pop(Tile& tile, const QRect& target, qint64 now, bool closing, bool fadeIn);
	bool addClosingTiles(qint64 now);
	bool removeFinishedClosingTiles(qint64 now);
	bool addSource(Thumbnail& thumb, HWND window, const QRect& crop = {});
	bool setNativeSize(Thumbnail& thumb, HWND window);
	void refresh();
	void settle();
	bool raise();
	[[nodiscard]] HWND coveringHost() const;
	[[nodiscard]] QList<HWND> occludingTiles() const;
	bool parkOccluders(qint64 now);
	bool animate(float from, float to, qint64 time, auto setter, double duration = Duration);

	HWND host = nullptr;
	HWND backdropWindow = nullptr;
	HMONITOR barMonitor = nullptr;
	ComPtr<IDCompositionDesktopDevice> desktopDevice;
	ComPtr<IDCompositionDevice> device;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> root;
	ComPtr<IDCompositionSurface> tileColor;
	ComPtr<IDCompositionSurface> diagnosticColor;
	Thumbnail backdrop;
	std::map<HWND, std::unique_ptr<Tile>> tiles;
	int lastTileOrder = 0;
	std::map<HWND, CloseFrame> closeFrames;
	QList<ClosingTile> closingTiles;
	QTimer closeExpiry;
	QTimer idle;
	QTimer sample;
	int retries = 0;
	HWND stranger = nullptr;
	qint64 strangerSince = 0;
	qint64 holdUntil = 0;
	qint64 occlusionStarted = 0;
	QImage coverFrame;
	bool released = false;
	bool finishing = false;
	bool hurrying = false;
	void release();

public:
	void finish(bool hurry = false);
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

bool Scene::animate(float from, float to, qint64 time, auto setter, double duration) {
	if (from == to) return SUCCEEDED(setter(to));
	ComPtr<IDCompositionAnimation> animation;
	LARGE_INTEGER begin {};
	begin.QuadPart = time;
	if (FAILED(this->device->CreateAnimation(animation.GetAddressOf()))
	    || FAILED(animation->SetAbsoluteBeginTime(begin)))
		return false;
	for (const auto& segment: dwindle::curveSegments(from, to, duration)) {
		if (FAILED(animation->AddCubic(
		        segment.begin, static_cast<float>(segment.value), static_cast<float>(segment.linear),
		        static_cast<float>(segment.quadratic), static_cast<float>(segment.cubic)
		    )))
			return false;
	}
	return SUCCEEDED(animation->End(duration, to)) && SUCCEEDED(setter(animation.Get()));
}

bool Scene::addDiagnosticMarker(Tile& tile) {
	if (!diagnosticsEnabled()) return true;
	if (this->diagnosticColor == nullptr) {
		auto color = QImage(12, 12, QImage::Format_RGB32);
		color.fill(0xffff00ff);
		this->diagnosticColor = this->upload(color);
		if (this->diagnosticColor == nullptr) return false;
	}
	return SUCCEEDED(this->device->CreateVisual(tile.diagnosticMarker.GetAddressOf()))
	    && SUCCEEDED(tile.diagnosticMarker->SetContent(this->diagnosticColor.Get()))
	    && SUCCEEDED(tile.diagnosticMarker->SetOffsetX(24.0f))
	    && SUCCEEDED(tile.diagnosticMarker->SetOffsetY(24.0f))
	    && SUCCEEDED(tile.container->AddVisual(tile.diagnosticMarker.Get(), FALSE, nullptr));
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
	return this->awaitSize(tile, placement.target);
}

bool Scene::awaitSize(Tile& tile, const QRect& target) {
	if (!tile.waitingForSize) {
		this->retainedVisible(tile, false);
		tile.retained.Reset();
		tile.retainedSize = {};
		if (FAILED(tile.container->RemoveVisual(tile.liveFrame.Get()))) return false;
		tile.waitingForSize = true;
	}
	D2D_MATRIX_3X2_F stretch {};
	stretch._11 = target.width();
	stretch._22 = target.height();
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
	const auto hr = visible ? tile.container->AddVisual(tile.retained.Get(), FALSE, tile.diagnosticMarker.Get())
	                        : tile.container->RemoveVisual(tile.retained.Get());
	if (FAILED(hr)) return false;
	tile.showingRetained = visible;
	return true;
}

bool Scene::pop(Tile& tile, const QRect& target, qint64 now, bool closing, bool fadeIn) {
	tile.from = target;
	tile.target = target;
	tile.start = now;
	tile.popStart = closing ? 0 : now;
	const auto local = target.translated(-this->work.topLeft());
	const auto radius = static_cast<float>(
	    std::min({double(tile.radius), target.width() / 2.0, target.height() / 2.0})
	);
	if (FAILED(tile.container->SetOffsetX(static_cast<float>(local.x())))
	    || FAILED(tile.container->SetOffsetY(static_cast<float>(local.y())))
	    || FAILED(tile.clip->SetRight(static_cast<float>(target.width())))
	    || FAILED(tile.clip->SetBottom(static_cast<float>(target.height())))
	    || FAILED(tile.clip->SetTopLeftRadiusX(radius)) || FAILED(tile.clip->SetTopLeftRadiusY(radius))
	    || FAILED(tile.clip->SetTopRightRadiusX(radius)) || FAILED(tile.clip->SetTopRightRadiusY(radius))
	    || FAILED(tile.clip->SetBottomLeftRadiusX(radius)) || FAILED(tile.clip->SetBottomLeftRadiusY(radius))
	    || FAILED(tile.clip->SetBottomRightRadiusX(radius))
	    || FAILED(tile.clip->SetBottomRightRadiusY(radius))
	    || FAILED(this->device->CreateScaleTransform(tile.pop.GetAddressOf()))
	    || FAILED(tile.pop->SetCenterX(target.width() / 2.0f))
	    || FAILED(tile.pop->SetCenterY(target.height() / 2.0f))
	    || FAILED(this->device->CreateEffectGroup(tile.fade.GetAddressOf())))
		return false;
	const auto duration = closing ? PopOutDuration : Duration;
	const auto fromScale = closing ? 1.0f : PopInScale;
	const auto toScale = closing ? PopOutScale : 1.0f;
	return this->animate(fromScale, toScale, now,
	                     [&tile](auto value) { return tile.pop->SetScaleX(value); }, duration)
	    && this->animate(fromScale, toScale, now,
	                     [&tile](auto value) { return tile.pop->SetScaleY(value); }, duration)
	    && this->animate(fadeIn ? 0.0f : 1.0f, closing ? 0.0f : 1.0f, now,
	                     [&tile](auto value) { return tile.fade->SetOpacity(value); }, duration)
	    && SUCCEEDED(tile.container->SetTransform(tile.pop.Get()))
	    && SUCCEEDED(tile.container->SetEffect(tile.fade.Get()));
}

bool Scene::update(Tile& tile, const QRectF& origin, const QRect& target, qint64 now) {
	if (tile.pop != nullptr && tile.target.size() != target.size()
	    && (!this->animate(tile.target.width() / 2.0f, target.width() / 2.0f, now,
	                       [&tile](auto value) { return tile.pop->SetCenterX(value); })
	        || !this->animate(tile.target.height() / 2.0f, target.height() / 2.0f, now,
	                          [&tile](auto value) { return tile.pop->SetCenterY(value); })))
		return false;
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

void Scene::snapshotClose(HWND window) {
	if (!IsWindowVisible(window) || IsIconic(window) || cloaked(window)) return;

	auto bounds = frame(window);
	auto radius = windowRadius(window);
	qCDebug(logDwindleScene) << "Dwindle close capture" << window << bounds << this->work;
	const auto now = counter();
	if (this->host != nullptr && IsWindowVisible(this->host)) {
		if (this->coveringHost() != nullptr) return;
		auto found = this->tiles.find(window);
		if (found == this->tiles.end()) return;
		const auto& tile = *found->second;
		const auto box = tile.at(now);
		const auto size = tile.showingRetained ? tile.retainedSize : tile.liveSize;
		if (tile.waitingForSize || (tile.pop != nullptr && now - tile.start < Duration * frequency())
		    || size.width() < box.width() - 1 || size.height() < box.height() - 1
		    || std::ranges::any_of(this->tiles, [window, box, now](const auto& other) {
			    return other.first != window && other.second->at(now).intersects(box);
		    })
		    || std::ranges::any_of(this->closingTiles, [box](const auto& other) {
			    return box.intersects(QRectF(other.bounds));
		    }))
			return;
		bounds = box.toRect();
		radius = tile.radius;
	}
	if (bounds.isEmpty() || !this->work.contains(bounds)) return;
	const auto uncovered = isUncovered(window, bounds, this->host);
	auto pixels = uncovered ? captureWorkspace(bounds) : captureWindowForClose(window, bounds.size());
	if (pixels.isNull()) {
		qCDebug(logDwindleScene) << "Dwindle close capture failed" << window << "uncovered" << uncovered;
		return;
	}
	qCDebug(logDwindleScene) << "Dwindle close captured" << window << "uncovered" << uncovered;

	this->closeFrames.insert_or_assign(window, CloseFrame {std::move(pixels), bounds, radius, now});
	this->closeExpiry.start(static_cast<int>(CloseFrameLifetime * 1000));
}

void Scene::acceptCachedClose(HWND window, CloseFrame frame) {
	if (this->closeFrames.contains(window)) return;
	this->closeFrames.emplace(window, std::move(frame));
	this->closeExpiry.start(static_cast<int>(CloseFrameLifetime * 1000));
}

bool Scene::hasClosingTile() const {
	const auto now = counter();
	return std::ranges::any_of(this->closeFrames, [now](const auto& entry) {
		return now - entry.second.captured <= CloseFrameLifetime * frequency()
		    && entry.second.ready(entry.first);
	});
}

bool Scene::addClosingTiles(qint64 now) {
	bool added = false;
	for (auto it = this->closeFrames.begin(); it != this->closeFrames.end();) {
		const auto& [hwnd, close] = *it;
		if (now - close.captured > CloseFrameLifetime * frequency()) {
			it = this->closeFrames.erase(it);
			continue;
		}
		if (!close.ready(hwnd)) {
			++it;
			continue;
		}

		auto content = this->upload(close.pixels);
		const auto start = counter();
		Tile tile;
		tile.radius = close.radius;
		if (content != nullptr && SUCCEEDED(this->device->CreateVisual(tile.container.GetAddressOf()))
		    && SUCCEEDED(this->device->CreateRectangleClip(tile.clip.GetAddressOf()))
		    && SUCCEEDED(tile.clip->SetLeft(0.0f)) && SUCCEEDED(tile.clip->SetTop(0.0f))
		    && SUCCEEDED(tile.container->SetClip(tile.clip.Get()))
		    && SUCCEEDED(tile.container->SetContent(content.Get()))
		    && this->pop(tile, close.bounds, start, true, false)
		    && SUCCEEDED(this->root->AddVisual(tile.container.Get(), FALSE, nullptr))) {
			const auto end = start + static_cast<qint64>(PopOutDuration * frequency());
			this->closingTiles.append(ClosingTile {std::move(tile.container), close.bounds, end});
			added = true;
			qCDebug(logDwindleScene) << "Animating Dwindle close" << hwnd << close.bounds;
		}
		it = this->closeFrames.erase(it);
	}
	return added;
}

bool Scene::removeFinishedClosingTiles(qint64 now) {
	const auto count = this->closingTiles.size();
	this->closingTiles.removeIf([this, now](const ClosingTile& tile) {
		return now >= tile.end && SUCCEEDED(this->root->RemoveVisual(tile.visual.Get()));
	});
	return count != this->closingTiles.size();
}

void Scene::publishDragScene() const {
	DragScene scene {this->visibleWork, {}};
	scene.tiles.reserve(static_cast<qsizetype>(this->tiles.size()));
	for (const auto& [hwnd, tile]: this->tiles) {
		scene.tiles.append(DragTile {hwnd, *tile, tile->order});
	}
	std::ranges::sort(scene.tiles, std::greater {}, &DragTile::order);
	auto& registry = dragScenes();
	auto lock = std::lock_guard(registry.mutex);
	registry.byHost.insert_or_assign(this->host, std::move(scene));
}

QRect Scene::frameForDrag(HWND window) const {
	const auto found = this->tiles.find(window);
	if (this->host == nullptr || !IsWindowVisible(this->host) || found == this->tiles.end()) return {};
	return found->second->shownAt(counter()).toRect();
}

bool Scene::matches(const QList<DwindleTile>& plan) const {
	if (plan.size() != static_cast<qsizetype>(this->tiles.size())) return false;
	return std::all_of(plan.begin(), plan.end(), [this](const auto& placement) {
		auto it = this->tiles.find(placement.window);
		return it != this->tiles.end() && it->second->target == placement.target;
	});
}

bool Scene::show(const QList<DwindleTile>& plan, const QRect& layoutWork) {
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
		if (releaser()) releaser()({it->first});
		it = this->tiles.erase(it);
	}
	const auto needFrame = this->tiles.empty() && std::ranges::any_of(plan, [](const auto& tile) {
		return !tile.previous.isEmpty() && tile.previous.size() != tile.target.size();
	});
	const auto before = needFrame ? captureWorkspace(this->work) : QImage();
	if (!this->coverFrame.isNull()) {
		for (const auto& placement: plan) {
			auto found = this->tiles.find(placement.window);
			if (found == this->tiles.end()) continue;
			auto& tile = *found->second;
			if (tile.target.isEmpty() || tile.retained != nullptr || tile.from != QRectF(tile.target)) continue;
			this->retain(tile, {placement.window, tile.target, placement.target, true}, this->coverFrame);
		}
	}
	for (const auto& placement: plan) {
		auto [it, added] = this->tiles.try_emplace(placement.window);
		if (!added) continue;
		it->second = std::make_unique<Tile>();
		it->second->order = ++this->lastTileOrder;
		if (!this->addTile(*it->second, placement, before) || !this->addDiagnosticMarker(*it->second)) {
			qCDebug(logDwindleScene) << "Could not attach tile" << placement.window;
			return false;
		}
	}
	const auto fresh = !IsWindowVisible(this->host);
	this->visibleWork = fresh ? layoutWork : this->visibleWork.united(layoutWork);
	for (const auto& placement: plan) {
		this->visibleWork = this->visibleWork.united(placement.previous.intersected(this->work));
	}
	const auto clip = this->visibleWork.translated(-this->work.topLeft());
	const D2D_RECT_F bounds {
	    static_cast<float>(clip.x()), static_cast<float>(clip.y()),
	    static_cast<float>(clip.x() + clip.width()), static_cast<float>(clip.y() + clip.height()),
	};
	if (FAILED(this->root->SetClip(bounds))) return false;
	const auto start = counter();
	QList<QRect> targets;
	targets.reserve(plan.size());
	QList<std::optional<QRectF>> origins(plan.size());
	QList<bool> movable(plan.size(), false);
	for (qsizetype i = 0; i < plan.size(); ++i) {
		const auto& placement = plan[i];
		const auto& tile = *this->tiles.at(placement.window);
		targets.append(placement.target);
		if (!tile.target.isEmpty()) {
			origins[i] = tile.at(start);
		} else if (!placement.previous.isEmpty()) {
			origins[i] = placement.previous;
			movable[i] = !placement.settled;
		}
	}
	dwindle::dropStackedOrigins(origins, movable);
	for (qsizetype i = 0; i < plan.size(); ++i) {
		if (!movable[i] || origins[i].has_value()) continue;
		if (!this->awaitSize(*this->tiles.at(plan[i].window), plan[i].target)) return false;
	}
	for (qsizetype i = 0; i < plan.size(); ++i) {
		const auto& placement = plan[i];
		auto& tile = *this->tiles.at(placement.window);
		const auto scheduled = origins[i].has_value()
		                         ? this->update(tile, *origins[i], placement.target, start)
		                         : this->pop(tile, placement.target, start, false, placement.previous.isEmpty());
		if (!scheduled) {
			qCDebug(logDwindleScene) << "Could not schedule tile" << placement.window;
			return false;
		}
	}
	if (diagnosticsEnabled()) {
		for (const auto& placement: plan) {
			auto& tile = *this->tiles.at(placement.window);
			tile.diagnosticState = -1;
		}
	}
	const auto closing = this->addClosingTiles(start);
	if (plan.isEmpty() && !closing && this->closingTiles.isEmpty()) {
		this->hide();
		return false;
	}
	if (!succeeded(this->device->Commit(), "Commit")) return false;
	if (fresh && !this->raise()) return false;
	this->publishDragScene();
	this->retries = 0;
	this->released = false;
	this->finishing = false;
	this->hurrying = false;
	this->idle.start(static_cast<int>((plan.isEmpty() ? PopOutDuration : Duration) * 1000) + SettleMs);
	this->sample.start();
	qCDebug(logDwindleScene) << "Animating" << plan.size() << "tiles in" << this->work;
	return true;
}

bool Scene::raise() {
	if (this->barMonitor == nullptr) {
		this->barMonitor = MonitorFromWindow(this->host, MONITOR_DEFAULTTONULL);
		WinPanelWindow::setDwindleBarAbove(this->barMonitor, true);
	}
	auto panels = WinPanelWindow::aboveWindowHandles();
	auto above = HWND_TOPMOST;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (panels.contains(hwnd) && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
			above = hwnd;
	}
	return SetWindowPos(this->host, above, 0, 0, 0, 0,
	                    SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW) != FALSE;
}

HWND Scene::coveringHost() const {
	if ((GetWindowLongPtrW(this->host, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) return this->host;
	for (auto* above = GetWindow(this->host, GW_HWNDPREV); above != nullptr;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		if (IsWindowVisible(above) && !cloaked(above)
		    && (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0
		    && frame(above).intersects(this->visibleWork))
			return above;
	}
	return nullptr;
}

QList<HWND> Scene::occludingTiles() const {
	QList<HWND> windows;
	for (auto* above = GetWindow(this->host, GW_HWNDPREV); above != nullptr;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		if (this->tiles.contains(above) && IsWindowVisible(above) && !IsIconic(above)
		    && !cloaked(above) && frame(above).intersects(this->visibleWork))
			windows.append(above);
	}
	return windows;
}

bool Scene::parkOccluders(qint64 now) {
	const auto windows = this->occludingTiles();
	if (windows.isEmpty() || !occlusionStager()) return false;
	for (auto* hwnd: windows) {
		auto& tile = *this->tiles.at(hwnd);
		if (tile.occlusionRequested == 0) {
			if (!occlusionStager()(hwnd)) return false;
			tile.occlusionRequested = now;
			if (this->occlusionStarted == 0) this->occlusionStarted = now;
			qCDebug(logDwindleScene) << "Dwindle parked covering tile" << hwnd;
		} else if (static_cast<double>(now - tile.occlusionRequested) / frequency() > OcclusionMoveGrace) {
			qCDebug(logDwindleScene) << "Dwindle covering tile did not park" << hwnd;
			return false;
		}
	}
	return true;
}

void Scene::inspectParticipants(qint64 now) {
	QSet<HWND> inFront;
	for (auto* above = GetWindow(this->host, GW_HWNDPREV); above != nullptr;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		if (this->tiles.contains(above)) inFront.insert(above);
	}
	for (auto& [hwnd, tile]: this->tiles) {
		if (!IsWindow(hwnd)) continue;
		const auto actual = frame(hwnd);
		const auto onScreen = IsWindowVisible(hwnd) && !cloaked(hwnd) && actual.intersects(this->work);
		const auto exposed = onScreen && inFront.contains(hwnd);
		const auto state = (onScreen ? 1 : 0) | (exposed ? 2 : 0)
		                 | (tile->waitingForSize ? 4 : 0) | (tile->showingRetained ? 8 : 0);
		const auto size = tile->source.sourceCrop.size();
		const auto offset = tile->source.contentOffset;
		if (state == tile->diagnosticState && size == tile->diagnosticSize
		    && offset == tile->diagnosticOffset)
			continue;
		tile->diagnosticState = state;
		tile->diagnosticSize = size;
		tile->diagnosticOffset = offset;
		qCDebug(logDwindleScene) << "Dwindle participant" << hwnd << "visible" << onScreen
		                         << "aboveScene" << exposed << "released" << this->released
		                         << "ageMs" << (now - tile->start) * 1000 / frequency()
		                         << "actual" << actual << "visual" << tile->at(now)
		                         << "target" << tile->target << "source" << size
		                         << "offset" << offset << "waiting" << tile->waitingForSize
		                         << "retained" << tile->showingRetained;
	}
}

void Scene::refresh() {
	if (this->host == nullptr || !IsWindow(this->backdropWindow)) {
		this->hide();
		return;
	}
	const auto now = counter();
	if (diagnosticsEnabled()) this->inspectParticipants(now);
	if (!this->released && this->occlusionStarted != 0
	    && static_cast<double>(now - this->occlusionStarted) / frequency() > OcclusionStageLimit) {
		qCDebug(logDwindleScene) << "Dwindle ending after a covering tile was parked";
		this->finish(true);
		return;
	}
	if (!this->released
	    && (cloaked(this->host) || this->coveringHost() != nullptr || !this->occludingTiles().isEmpty())) {
		auto* over = this->coveringHost();
		const auto wasCloaked = cloaked(this->host);
		for (auto pass = 0; pass < 16; ++pass) {
			auto* stray = this->coveringHost();
			if (stray == nullptr || stray == this->host) break;
			SetWindowPos(stray, HWND_NOTOPMOST, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
		}
		const auto raised = this->raise();
		auto* remaining = this->coveringHost();
		const auto covered = !this->occludingTiles().isEmpty();
		if (raised && !cloaked(this->host) && covered
		    && (remaining == nullptr || this->tiles.contains(remaining)) && this->parkOccluders(now)) {
			return;
		}
		if (!raised || cloaked(this->host) || remaining != nullptr || covered) {
			wchar_t name[128] {};
			if (over != nullptr && over != this->host) GetClassNameW(over, name, 128);
			qCDebug(logDwindleScene) << "Scene host stays hidden: cloaked" << wasCloaked << "topmost"
			                         << ((GetWindowLongPtrW(this->host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
			                         << "under" << over << QString::fromWCharArray(name) << "raised" << raised
			                         << "foreground" << GetForegroundWindow();
			this->hide();
			return;
		}
		qCDebug(logDwindleScene) << "Scene host raised again; put back in the normal band" << over;
	}
	if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0
	    || GetAsyncKeyState(VK_MBUTTON) < 0)
	{
		this->finish();
		return;
	}
	const auto foreground = GetForegroundWindow();
	const auto intersecting = this->visibleWork.intersects(frame(foreground));
	if (foreground != nullptr && intersecting && !this->tiles.contains(foreground)) {
		if (foreground != this->stranger) {
			this->stranger = foreground;
			this->strangerSince = now;
		}
		if (static_cast<double>(now - this->strangerSince) / frequency() > JoinGrace) {
			this->finish();
			return;
		}
	} else {
		this->stranger = nullptr;
	}
	bool changed = this->removeFinishedClosingTiles(now);
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
			if (SUCCEEDED(tile->container->AddVisual(tile->liveFrame.Get(), FALSE, tile->diagnosticMarker.Get()))) {
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

void Scene::release() {
	if (this->released) return;
	this->released = true;
	this->occlusionStarted = 0;
	for (auto& [hwnd, tile]: this->tiles) tile->occlusionRequested = 0;
	if (diagnosticsEnabled()) qCDebug(logDwindleScene) << "Dwindle handoff" << this->tiles.size();
	QList<HWND> windows;
	for (const auto& [hwnd, tile]: this->tiles) windows.append(hwnd);
	if (!windows.isEmpty() && releaser()) releaser()(windows);
}

void Scene::hold() {
	this->holdUntil = counter() + static_cast<qint64>(CoverHold * frequency());
	if (!this->coverFrame.isNull()) return;
	QTimer::singleShot(0, &this->sample, [this] {
		if (this->visible() && this->coverFrame.isNull()) this->coverFrame = captureWorkspace(this->work);
	});
}

void Scene::finish(bool hurry) {
	this->hurrying = this->hurrying || hurry;
	if (this->finishing) return;
	this->finishing = true;
	this->holdUntil = 0;
	this->idle.start(0);
}

void Scene::settle() {
	const auto now = counter();
	if (now < this->holdUntil) {
		this->idle.start(static_cast<int>((this->holdUntil - now) * 1000 / frequency()) + 1);
		return;
	}
	const auto placed = std::all_of(this->tiles.begin(), this->tiles.end(), [](const auto& pair) {
		const auto& [hwnd, tile] = pair;
		if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return true;
		const auto actual = frame(hwnd);
		const auto target = tile->target.adjusted(-2, -2, 2, 2);
		return !actual.isEmpty() && target.contains(actual)
		    && std::abs(actual.width() - tile->target.width()) <= 4
		    && std::abs(actual.height() - tile->target.height()) <= 4;
	});
	if (!this->released) {
		this->release();
		this->retries = 0;
		this->idle.start(16);
		return;
	}
	const auto arrived = std::all_of(this->tiles.begin(), this->tiles.end(), [this](const auto& pair) {
		const auto& [hwnd, tile] = pair;
		return !IsWindow(hwnd) || !IsWindowVisible(hwnd) || frame(hwnd).intersects(this->work);
	});
	if ((!arrived && this->retries < ArrivalRetries)
	    || (!placed && !this->hurrying && this->retries < ReleaseRetries)) {
		++this->retries;
		this->idle.start(16);
		return;
	}
	if (!arrived) qCDebug(logDwindleScene) << "Handing over before every window arrived";
	this->hide();
}

void Scene::hide() {
	{
		auto& registry = dragScenes();
		auto lock = std::lock_guard(registry.mutex);
		registry.byHost.erase(this->host);
	}
	this->idle.stop();
	this->sample.stop();
	this->release();
	this->finishing = false;
	this->hurrying = false;
	this->stranger = nullptr;
	this->holdUntil = 0;
	this->coverFrame = {};
	if (this->host != nullptr) ShowWindow(this->host, SW_HIDE);
	if (this->barMonitor != nullptr)
		WinPanelWindow::setDwindleBarAbove(std::exchange(this->barMonitor, nullptr), false);
	if (this->target != nullptr) this->target->SetRoot(nullptr);
	if (this->device != nullptr) this->device->Commit();
	this->tiles.clear();
	this->lastTileOrder = 0;
	this->closingTiles.clear();
	if (this->backdrop.handle != nullptr) {
		DwmUnregisterThumbnail(this->backdrop.handle);
		this->backdrop.handle = nullptr;
	}
	this->backdrop.visual.Reset();
	this->root.Reset();
	this->tileColor.Reset();
	this->diagnosticColor.Reset();
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

void DwindleScene::snapshotClose(HMONITOR monitor, const QRect& work, HWND window) {
	if (work.isEmpty()) return;
	auto& scene = scenes()[monitor];
	const auto viewport = sceneViewport(monitor, work);
	if (scene == nullptr || scene->work != viewport) scene = std::make_unique<Scene>(viewport);
	scene->snapshotClose(window);
}

void DwindleScene::cacheCloseFrame(HWND window, const QRect& bounds) {
	if (!IsWindowVisible(window) || IsIconic(window) || cloaked(window)
	    || IsHungAppWindow(window) || frame(window) != bounds) return;

	DWORD pid = 0;
	DWORD affinity = 0;
	RECT rect {};
	if (!GetWindowThreadProcessId(window, &pid) || !GetWindowRect(window, &rect)
	    || (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE)) return;

	const QRect raw(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
	if (!raw.contains(bounds)) return;
	const auto radius = windowRadius(window);
	const auto captured = counter();
	quint64 generation = 0;
	{
		auto& cache = closeCache();
		auto lock = std::lock_guard(cache.mutex);
		generation = cache.generation;
	}

	startCloseCapture([window, raw, bounds, radius, captured, pid, generation] {
		auto pixels = captureWindowPixels(window, raw, bounds);
		DWORD currentPid = 0;
		if (pixels.isNull() || !GetWindowThreadProcessId(window, &currentPid)
		    || pid != currentPid || frame(window) != bounds) return;

		auto& cache = closeCache();
		auto lock = std::lock_guard(cache.mutex);
		if (cache.generation != generation) return;
		if (auto it = cache.frames.find(window); it != cache.frames.end()) forgetCachedFrame(cache, it);
		while (!cache.frames.empty() && cache.bytes + pixels.sizeInBytes() > CloseCacheBudget) {
			auto oldest = std::ranges::min_element(cache.frames, {}, [](const auto& entry) {
				return entry.second.frame.captured;
			});
			forgetCachedFrame(cache, oldest);
		}
		if (pixels.sizeInBytes() > CloseCacheBudget) return;
		cache.bytes += pixels.sizeInBytes();
		cache.frames.emplace(window, CachedCloseFrame {pid, {std::move(pixels), bounds, radius, captured}});
	});
}

void DwindleScene::useCachedCloseFrame(HMONITOR monitor, const QRect& work, HWND window, quint32 pid,
                                      const QRect& bounds, bool destroyedOnly) {
	CloseFrame snapshot;
	{
		auto& cache = closeCache();
		auto lock = std::lock_guard(cache.mutex);
		auto found = cache.frames.find(window);
		if (found == cache.frames.end()) return;
		const auto& stored = found->second;
		if (stored.pid == pid && stored.frame.bounds == bounds
		    && counter() - stored.frame.captured <= CachedCloseLifetime * frequency())
			snapshot = stored.frame;
		if (destroyedOnly) forgetCachedFrame(cache, found);
	}
	if (snapshot.pixels.isNull()) return;

	auto& scene = scenes()[monitor];
	const auto viewport = sceneViewport(monitor, work);
	if (scene == nullptr || scene->work != viewport) scene = std::make_unique<Scene>(viewport);
	snapshot.captured = counter();
	snapshot.destroyedOnly = destroyedOnly;
	scene->acceptCachedClose(window, std::move(snapshot));
}

void DwindleScene::clearCloseCache() {
	auto& cache = closeCache();
	auto lock = std::lock_guard(cache.mutex);
	cache.frames.clear();
	cache.bytes = 0;
	++cache.generation;
}

bool DwindleScene::hasClosingTile(HMONITOR monitor, const QRect& work) {
	auto found = scenes().find(monitor);
	return found != scenes().end() && found->second->work == sceneViewport(monitor, work) && found->second->hasClosingTile();
}

void DwindleScene::present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed) {
	auto found = scenes().find(monitor);
	const auto closing = DwindleScene::hasClosingTile(monitor, work);
	if (work.isEmpty() || (tiles.isEmpty() && !closing)) {
		DwindleScene::dismiss(monitor);
		return;
	}
	if (!changed && !closing && (found == scenes().end() || !found->second->visible())) return;
	const auto viewport = sceneViewport(monitor, work);
	if (!closing && found != scenes().end() && found->second->visible()
	    && found->second->work == viewport && found->second->matches(tiles)) return;
	auto& scene = scenes()[monitor];
	if (scene == nullptr || scene->work != viewport) scene = std::make_unique<Scene>(viewport);
	if (!scene->show(tiles, work)) {
		qCDebug(logDwindleScene) << "Using immediate placement";
		DwindleScene::dismiss(monitor);
	}
}

void DwindleScene::cover(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles) {
	if (work.isEmpty()) return;
	auto& scene = scenes()[monitor];
	const auto viewport = sceneViewport(monitor, work);
	if (scene != nullptr && scene->visible() && scene->work == viewport) {
		scene->hold();
		return;
	}
	if (scene == nullptr || scene->work != viewport) scene = std::make_unique<Scene>(viewport);
	if (!scene->show(tiles, work)) {
		qCDebug(logDwindleScene) << "Could not cover" << tiles.size() << "tiles";
		DwindleScene::dismiss(monitor);
		return;
	}
	scene->hold();
	qCDebug(logDwindleScene) << "Covering" << tiles.size() << "tiles for a new window";
}

void DwindleScene::dismiss(HMONITOR monitor) {
	auto found = scenes().find(monitor);
	if (found == scenes().end()) return;
	if (found->second->visible()) {
		qCDebug(logDwindleScene) << "Dismissing a visible scene; handing windows over first";
		found->second->finish(true);
		return;
	}
	scenes().erase(found);
}

bool DwindleScene::presenting(HMONITOR monitor) {
	auto found = scenes().find(monitor);
	return found != scenes().end() && found->second->visible();
}

std::optional<HWND> DwindleScene::windowAt(POINT point, HWND nativeHit) {
	if (!DwindleScene::enabled()) return std::nullopt;
	const QPoint cursor(point.x, point.y);
	auto& registry = dragScenes();
	auto lock = std::lock_guard(registry.mutex);
	const auto now = counter();
	for (const auto& [host, scene]: registry.byHost) {
		if (!scene.work.contains(cursor)) continue;
		for (auto* hwnd = GetWindow(host, GW_HWNDPREV); nativeHit != nullptr && hwnd != nullptr;
		     hwnd = GetWindow(hwnd, GW_HWNDPREV))
		{
			if (hwnd == nativeHit) return std::nullopt;
		}
		for (const auto& tile: scene.tiles) {
			if (tile.frame.shownAt(now).contains(cursor)) return tile.window;
		}
		return static_cast<HWND>(nullptr);
	}
	return std::nullopt;
}

QRect DwindleScene::frameForDrag(HWND window) {
	for (const auto& [monitor, scene]: scenes()) {
		const auto frame = scene->frameForDrag(window);
		if (!frame.isEmpty()) return frame;
	}
	return {};
}

void DwindleScene::onOcclusion(std::function<bool(HWND)> stage) {
	occlusionStager() = std::move(stage);
}

void DwindleScene::onRelease(std::function<void(const QList<HWND>&)> release) {
	releaser() = std::move(release);
}

void DwindleScene::dismissAll() { scenes().clear(); }

}
