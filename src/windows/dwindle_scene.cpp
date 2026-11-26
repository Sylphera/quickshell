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
#include <vector>

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
#include "inactive_dim.hpp"
#include "layer_order.hpp"
#include "panel_window.hpp"
#include "scratchpad.hpp"
#include "tiling.hpp"
#include "util.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logDwindleScene, "quickshell.windows.dwindle.scene", QtWarningMsg);

constexpr double Duration = 0.3;
constexpr float PopInScale = 0.8f;
constexpr float PopOutScale = 0.9f;
constexpr double PopOutDuration = 0.2;
constexpr double CloseFrameLifetime = 1.0;
constexpr double CloseSourceHold = 0.16;
constexpr double CachedCloseLifetime = 4.0;
constexpr qint64 CloseCacheBudget = 32 * 1024 * 1024;
constexpr auto SettleMs = 55;
constexpr double JoinGrace = 0.12;
constexpr double CoverHold = 0.7;
constexpr int ReleaseRetries = 12;
constexpr int ArrivalRetries = 90;
constexpr double OcclusionMoveGrace = 0.05;
constexpr double OcclusionStageLimit = 0.33;

bool& sceneDuringDrag() {
	static auto during = false;
	return during;
}

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
	auto dip = configured && radius >= 0 && radius <= 64 ? radius : (smallCorner ? 4 : 8);
	if (!configured) {
		// Windhawk's custom-corner-radius hooks DWM itself: DWMWA_WINDOW_CORNER_PREFERENCE
		// still reports the *style*, not the radius displayed by the compositor.
		// Mirror the enabled mod's settings instead of assuming Windows' 4/8 DIP defaults.
		constexpr auto* mod = L"SOFTWARE\\Windhawk\\Engine\\Mods\\custom-corner-radius";
		DWORD disabled = 1;
		DWORD size = sizeof(disabled);
		if (RegGetValueW(HKEY_LOCAL_MACHINE, mod, L"Disabled", RRF_RT_REG_DWORD,
		                 nullptr, &disabled, &size) == ERROR_SUCCESS && disabled == 0) {
			constexpr auto* settings =
			    L"SOFTWARE\\Windhawk\\Engine\\Mods\\custom-corner-radius\\Settings";
			DWORD value = 0;
			size = sizeof(value);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, settings,
			                 smallCorner ? L"smallRadius" : L"radius", RRF_RT_REG_DWORD,
			                 nullptr, &value, &size) == ERROR_SUCCESS && value <= 64)
				dip = static_cast<int>(value);
		}
	}
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

struct Shade {
	ComPtr<IDCompositionVisual> visual;
	ComPtr<IDCompositionEffectGroup> effect;
	DimLevel level;
	bool muted = false;
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
	Shade shade;
	ComPtr<IDCompositionVisual> container;
	ComPtr<IDCompositionRectangleClip> clip;
	ComPtr<IDCompositionVisual> liveFrame;
	ComPtr<IDCompositionVisual> livePixels;
	ComPtr<IDCompositionRectangleClip> liveClip;
	QSize liveSize;
	QPoint liveOffset;
	ComPtr<IDCompositionVisual> placeholder;
	ComPtr<IDCompositionVisual> closeSource;
	qint64 closeSourceUntil = 0;
	ComPtr<IDCompositionVisual> retained;
	QSize retainedSize;
	float radius = 0;
	float radiusFrom = 0;
	[[nodiscard]] float radiusAt(qint64 now) const {
		const auto progress = static_cast<double>(now - this->start) / (Duration * frequency());
		return std::lerp(this->radiusFrom, this->radius, static_cast<float>(dwindle::ease(progress)));
	}
	IDCompositionVisual* visibleContent = nullptr;
	bool fadingIn = false;
	bool closingReflow = false;
	DwindleFullscreen fullscreen = DwindleFullscreen::None;
	bool floating = false;
	int order = 0;
	bool waitingForSize = false;
	ComPtr<IDCompositionScaleTransform> pop;
	ComPtr<IDCompositionEffectGroup> fade;
	float opacityFrom = 1.0f;
	float opacityTarget = 1.0f;
	qint64 opacityStart = 0;

	[[nodiscard]] float opacityAt(qint64 now) const {
		if (this->opacityStart == 0) return this->opacityTarget;
		const auto elapsed = static_cast<double>(now - this->opacityStart) / (Duration * frequency());
		return std::lerp(this->opacityFrom, this->opacityTarget,
		                 static_cast<float>(dwindle::ease(elapsed)));
	}

	ComPtr<IDCompositionVisual> diagnosticMarker;
	qint64 occlusionRequested = 0;
	int diagnosticState = -1;
	QSize diagnosticSize;
	QPoint diagnosticOffset;
};

struct FloatingCopy {
	Thumbnail source;
	ComPtr<IDCompositionVisual> container;
	ComPtr<IDCompositionRectangleClip> clip;
	ComPtr<IDCompositionEffectGroup> fade;
	Shade shade;
	QRect shown;
	float opacityFrom = 1.0f;
	float opacityTarget = 1.0f;
	qint64 opacityStart = 0;

	[[nodiscard]] float opacityAt(qint64 now) const {
		if (this->opacityStart == 0) return this->opacityTarget;
		const auto elapsed = static_cast<double>(now - this->opacityStart) / (Duration * frequency());
		return std::lerp(this->opacityFrom, this->opacityTarget,
		                 static_cast<float>(dwindle::ease(elapsed)));
	}
};

struct Underlay {
	Thumbnail source;
	Shade shade;
	ComPtr<IDCompositionVisual> frame;
	ComPtr<IDCompositionRectangleClip> clip;
};

struct CloseFrame {
	QImage pixels;
	QRect bounds;
	float radius;
	qint64 captured;
	float opacity = 1.0f;
	[[nodiscard]] bool ready(HWND hwnd) const { return !IsWindowVisible(hwnd); }
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
	std::unique_ptr<Tile> tile;
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
	explicit Scene(const QRect& work, HWND dim = nullptr): work(work), dim(dim) {
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

	bool show(QList<DwindleTile> plan, const QRect& layoutWork, HWND focused, bool instant = false);
	void snapshotClose(HWND window);
	void holdCloseSource(HWND window);
	void acceptCachedClose(HWND window, CloseFrame frame);
	[[nodiscard]] bool hasClosingTile() const;
	void hold();
	void hide();
	[[nodiscard]] bool visible() const { return this->host != nullptr; }
	[[nodiscard]] bool ownsPlacement() const {
		return this->visible() && !this->released && !this->finishing;
	}
	[[nodiscard]] bool matches(const QList<DwindleTile>& plan) const;
	[[nodiscard]] QRect frameForDrag(HWND window) const;
	[[nodiscard]] HWND hostWindow() const { return this->host; }
	[[nodiscard]] HWND focusedWindow() const { return this->focused; }
	[[nodiscard]] bool held() const { return this->focused != nullptr && this->released; }

	QRect work;
	HWND dim = nullptr;

private:
	QRect visibleWork;
	bool prepare(const QList<DwindleTile>& plan);
	bool addShade(Shade& shade, HWND window, IDCompositionVisual* parent);
	bool syncShade(Shade& shade, HWND window);
	void muteUnderlay(bool mute);
	bool addUnderlay(const QList<DwindleTile>& plan);
	[[nodiscard]] QList<HWND> floatingWindows(HWND cover) const;
	void copyFloating(qint64 now, HWND cover, bool uncovering);
	bool followFloating(HWND window, FloatingCopy& copy);
	void dropFloating();
	[[nodiscard]] bool staysAbove(HWND hwnd) const;
	bool updateFullscreenBar(HWND window, qint64 now, bool instant);
	void publishDragScene() const;
	bool makeTileColor();
	bool addDiagnosticMarker(Tile& tile);
	void inspectParticipants(qint64 now);
	ComPtr<IDCompositionSurface> upload(const QImage& image);
	bool addTile(Tile& tile, const DwindleTile& placement, const QImage& before);
	bool awaitSize(Tile& tile);
	bool setTileContent(Tile& tile, IDCompositionVisual* content);
	bool attachPlaceholder(Tile& tile);
	bool attachCloseContent(Tile& tile, const CloseFrame& close);
	void retain(Tile& tile, const DwindleTile& placement, const QImage& before);
	bool retainedVisible(Tile& tile, bool visible);
	bool resizeLiveClip(Tile& tile);
	bool alignLiveReflow(Tile& tile);
	bool update(Tile& tile, const QRectF& from, const QRect& target, qint64 now,
	            std::optional<float> cornerRadius);
	bool pop(Tile& tile, const QRect& target, qint64 now, bool closing, float initialOpacity);
	bool startCloseVisual(HWND window, std::unique_ptr<Tile> tile, const QRect& bounds,
	                      qint64 now, float opacity);
	void closeVisibleTile(HWND window, const Tile& tile);
	bool addClosingTiles(qint64 now);
	bool removeFinishedClosingTiles(qint64 now);
	bool addSource(Thumbnail& thumb, HWND window, const QRect& crop = {});
	bool setNativeSize(Thumbnail& thumb, HWND window);
	void refresh();
	void settle();
	bool raise();
	void keepBarAbove();
	void hideFront();
	bool showFront();
	bool holdNow();
	[[nodiscard]] HWND coveringHost() const;
	[[nodiscard]] QList<HWND> occludingTiles() const;
	bool parkOccluders(qint64 now);
	bool animate(float from, float to, qint64 time, auto setter, double duration = Duration);

	HWND host = nullptr;
	HWND focused = nullptr;
	HWND hiddenFront = nullptr;
	QSet<HWND> coveredFloats;
	HWND backdropWindow = nullptr;
	HMONITOR barMonitor = nullptr;
	ComPtr<IDCompositionDesktopDevice> desktopDevice;
	ComPtr<IDCompositionDevice> device;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> root;
	ComPtr<IDCompositionSurface> tileColor;
	ComPtr<IDCompositionSurface> diagnosticColor;
	ComPtr<IDCompositionVisual> handoffFrame;
	Thumbnail backdrop;
	std::unique_ptr<Thumbnail> fullscreenBar;
	ComPtr<IDCompositionEffectGroup> fullscreenBarFade;
	std::map<HWND, std::unique_ptr<Underlay>> underlay;
	std::map<HWND, std::unique_ptr<FloatingCopy>> floats;
	ComPtr<IDCompositionVisual> dimVisual;
	ComPtr<IDCompositionEffectGroup> dimFade;
	ComPtr<IDCompositionSurface> dimColor;
	std::map<HWND, std::unique_ptr<Tile>> tiles;
	int lastTileOrder = 0;
	std::map<HWND, CloseFrame> closeFrames;
	std::vector<ClosingTile> closingTiles;
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

HWND backdropFor(const QRect& work, HWND host, HWND skip) {
	HWND found = nullptr;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (hwnd == host || hwnd == skip || !IsWindowVisible(hwnd) || cloaked(hwnd) || !frame(hwnd).contains(work))
			continue;
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == GetCurrentProcessId() && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0)
			return hwnd;
		if (hwnd == GetShellWindow()) found = hwnd;
	}
	return found;
}

bool Scene::prepare(const QList<DwindleTile>& plan) {
	if (this->host != nullptr) return true;
	this->host = createHost(this->work);
	if (this->host == nullptr) {
		qCDebug(logDwindleScene) << "Overlay creation failed" << GetLastError();
		return false;
	}
	this->backdropWindow = backdropFor(this->work, this->host, this->dim);
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
	if (!succeeded(this->root->AddVisual(this->backdrop.visual.Get(), TRUE, nullptr), "Add backdrop"))
		return false;
	return this->dim == nullptr || this->addUnderlay(plan);
}

bool Scene::addUnderlay(const QList<DwindleTile>& plan) {
	D2D_MATRIX_3X2_F stretch {};
	stretch._11 = static_cast<float>(this->work.width());
	stretch._22 = static_cast<float>(this->work.height());
	const auto level = static_cast<float>(Scratchpad::instance()->dimLevel()) / 255.0f;
	if (this->dimColor == nullptr
	    || !succeeded(this->device->CreateVisual(this->dimVisual.GetAddressOf()), "Dim visual")
	    || !succeeded(this->device->CreateEffectGroup(this->dimFade.GetAddressOf()), "Dim effect")
	    || FAILED(this->dimFade->SetOpacity(level)) || FAILED(this->dimVisual->SetContent(this->dimColor.Get()))
	    || FAILED(this->dimVisual->SetTransform(stretch)) || FAILED(this->dimVisual->SetEffect(this->dimFade.Get())))
		return false;

	QList<HWND> windows;
	auto* tiling = TilingManager::active();
	auto* tracker = WindowTracker::instance();
	auto covered = false;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr && hwnd != this->backdropWindow;
	     hwnd = GetWindow(hwnd, GW_HWNDNEXT))
	{
		if (hwnd == this->dim) {
			windows.prepend(hwnd);
			continue;
		}
		if (covered || hwnd == this->host || !IsWindowVisible(hwnd) || IsIconic(hwnd) || cloaked(hwnd)) continue;
		if (Scratchpad::instance()->containsWindow(hwnd)) continue;
		if (std::ranges::any_of(plan, [hwnd](const auto& tile) { return tile.window == hwnd; })) continue;
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == GetCurrentProcessId()) continue;
		RECT raw {};
		if (!GetWindowRect(hwnd, &raw)) continue;
		const QRect bounds(raw.left, raw.top, raw.right - raw.left, raw.bottom - raw.top);
		if (!bounds.intersects(this->work)) continue;
		windows.prepend(hwnd);
		covered = tiling != nullptr && tiling->covers(tracker->windowFor(hwnd));
	}
	for (auto* hwnd: windows) {
		if (hwnd == this->dim) {
			if (!succeeded(this->root->AddVisual(this->dimVisual.Get(), FALSE, nullptr), "Add dim")) return false;
			continue;
		}
		RECT raw {};
		if (!GetWindowRect(hwnd, &raw)) continue;
		const QRect bounds(raw.left, raw.top, raw.right - raw.left, raw.bottom - raw.top);
		const auto shown = bounds.intersected(this->work);
		auto under = std::make_unique<Underlay>();
		if (!this->addSource(under->source, hwnd, shown.translated(-bounds.topLeft()))) continue;
		const auto visible = frame(hwnd);
		const auto radius = windowRadius(hwnd);
		if (FAILED(under->source.visual->SetOffsetX(static_cast<float>(shown.x() - this->work.x())))
		    || FAILED(under->source.visual->SetOffsetY(static_cast<float>(shown.y() - this->work.y())))
		    || FAILED(this->root->AddVisual(under->source.visual.Get(), FALSE, nullptr))
		    || FAILED(this->device->CreateVisual(under->frame.GetAddressOf()))
		    || FAILED(this->device->CreateRectangleClip(under->clip.GetAddressOf()))
		    || FAILED(under->frame->SetOffsetX(static_cast<float>(visible.x() - this->work.x())))
		    || FAILED(under->frame->SetOffsetY(static_cast<float>(visible.y() - this->work.y())))
		    || FAILED(under->clip->SetLeft(0.0f)) || FAILED(under->clip->SetTop(0.0f))
		    || FAILED(under->clip->SetRight(static_cast<float>(visible.width())))
		    || FAILED(under->clip->SetBottom(static_cast<float>(visible.height())))
		    || FAILED(under->clip->SetTopLeftRadiusX(radius)) || FAILED(under->clip->SetTopLeftRadiusY(radius))
		    || FAILED(under->clip->SetTopRightRadiusX(radius)) || FAILED(under->clip->SetTopRightRadiusY(radius))
		    || FAILED(under->clip->SetBottomLeftRadiusX(radius))
		    || FAILED(under->clip->SetBottomLeftRadiusY(radius))
		    || FAILED(under->clip->SetBottomRightRadiusX(radius))
		    || FAILED(under->clip->SetBottomRightRadiusY(radius))
		    || FAILED(under->frame->SetClip(under->clip.Get()))
		    || !this->addShade(under->shade, hwnd, under->frame.Get())
		    || FAILED(this->root->AddVisual(under->frame.Get(), FALSE, nullptr)))
			return false;
		this->underlay.emplace(hwnd, std::move(under));
	}
	return true;
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

bool Scene::addShade(Shade& shade, HWND window, IDCompositionVisual* parent) {
	D2D_MATRIX_3X2_F stretch {};
	stretch._11 = static_cast<float>(this->work.width() + 2);
	stretch._22 = static_cast<float>(this->work.height() * 2);
	shade.level = InactiveDim::levelOf(window);
	return SUCCEEDED(this->device->CreateVisual(shade.visual.GetAddressOf()))
	    && SUCCEEDED(this->device->CreateEffectGroup(shade.effect.GetAddressOf()))
	    && InactiveDim::applyLevel(this->device.Get(), shade.effect.Get(), shade.level)
	    && SUCCEEDED(shade.visual->SetContent(this->dimColor.Get()))
	    && SUCCEEDED(shade.visual->SetTransform(stretch))
	    && SUCCEEDED(shade.visual->SetEffect(shade.effect.Get()))
	    && SUCCEEDED(parent->AddVisual(shade.visual.Get(), FALSE, nullptr));
}

bool Scene::syncShade(Shade& shade, HWND window) {
	if (shade.effect == nullptr || shade.muted) return false;
	const auto level = InactiveDim::levelOf(window);
	if (level == shade.level) return false;
	if (!InactiveDim::applyLevel(this->device.Get(), shade.effect.Get(), level)) return false;
	shade.level = level;
	return true;
}

void Scene::muteUnderlay(bool mute) {
	for (auto& [hwnd, under]: this->underlay) {
		if (std::exchange(under->shade.muted, mute) == mute) continue;
		if (mute) under->shade.effect->SetOpacity(0.0f);
		else InactiveDim::applyLevel(this->device.Get(), under->shade.effect.Get(), under->shade.level);
	}
}

bool Scene::makeTileColor() {
	auto black = QImage(1, 1, QImage::Format_RGB32);
	black.fill(0xff000000);
	this->dimColor = this->upload(black);
	if (this->dimColor == nullptr) return false;
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
	tile.radius = tile.radiusFrom = windowRadius(placement.window);
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
	    || FAILED(this->device->CreateEffectGroup(tile.fade.GetAddressOf()))
	    || FAILED(tile.fade->SetOpacity(1.0f))
	    || FAILED(tile.container->SetEffect(tile.fade.Get()))
	    || !this->addShade(tile.shade, placement.window, tile.container.Get())
	    || FAILED(this->root->AddVisual(tile.container.Get(), FALSE, nullptr)))
		return false;

	if (!tile.waitingForSize) {
		this->retain(tile, placement, before);
		return this->setTileContent(tile, tile.liveFrame.Get());
	}
	return this->awaitSize(tile);
}

bool Scene::awaitSize(Tile& tile) {
	if (!this->attachPlaceholder(tile)) return false;
	tile.retained.Reset();
	tile.retainedSize = {};
	tile.waitingForSize = true;
	return true;
}

bool Scene::setTileContent(Tile& tile, IDCompositionVisual* content) {
	if (tile.visibleContent == content) return true;
	if (FAILED(tile.container->AddVisual(content, TRUE, nullptr))) return false;
	if (tile.visibleContent != nullptr && FAILED(tile.container->RemoveVisual(tile.visibleContent))) {
		tile.container->RemoveVisual(content);
		return false;
	}
	tile.visibleContent = content;
	const auto muted = content != nullptr && content == tile.retained.Get();
	if (muted != tile.shade.muted && tile.shade.effect != nullptr) {
		tile.shade.muted = muted;
		if (muted) tile.shade.effect->SetOpacity(0.0f);
		else InactiveDim::applyLevel(this->device.Get(), tile.shade.effect.Get(), tile.shade.level);
	}
	return true;
}

bool Scene::attachPlaceholder(Tile& tile) {
	if (tile.placeholder == nullptr) {
		D2D_MATRIX_3X2_F stretch {};
		stretch._11 = this->work.width();
		stretch._22 = this->work.height();
		if (FAILED(this->device->CreateVisual(tile.placeholder.GetAddressOf()))
		    || FAILED(tile.placeholder->SetContent(this->tileColor.Get()))
		    || FAILED(tile.placeholder->SetTransform(stretch)))
			return false;
	}
	return this->setTileContent(tile, tile.placeholder.Get());
}

bool Scene::resizeLiveClip(Tile& tile) {
	const auto size = tile.source.sourceCrop.size();
	if (FAILED(tile.livePixels->SetOffsetX(static_cast<float>(tile.source.contentOffset.x())))
	    || FAILED(tile.livePixels->SetOffsetY(static_cast<float>(tile.source.contentOffset.y())))
	    || FAILED(tile.liveClip->SetRight(static_cast<float>(size.width())))
	    || FAILED(tile.liveClip->SetBottom(static_cast<float>(size.height()))))
		return false;
	tile.liveSize = size;
	tile.liveOffset = tile.source.contentOffset;
	return this->alignLiveReflow(tile);
}

bool Scene::alignLiveReflow(Tile& tile) {
	if (!tile.closingReflow || tile.liveSize.isEmpty()) return true;
	const auto target = QRectF(tile.target);
	const auto right = std::abs(target.left() - tile.from.left())
	                   > std::abs(target.right() - tile.from.right());
	const auto bottom = std::abs(target.top() - tile.from.top())
	                    > std::abs(target.bottom() - tile.from.bottom());
	const auto offset = [](bool anchored, double extent, int pixels) {
		return anchored ? static_cast<float>(std::max(0.0, extent - pixels)) : 0.0f;
	};
	return this->animate(offset(right, tile.from.width(), tile.liveSize.width()),
	                     offset(right, target.width(), tile.liveSize.width()), tile.start,
	                     [&tile](auto value) { return tile.liveFrame->SetOffsetX(value); })
	    && this->animate(offset(bottom, tile.from.height(), tile.liveSize.height()),
	                     offset(bottom, target.height(), tile.liveSize.height()), tile.start,
	                     [&tile](auto value) { return tile.liveFrame->SetOffsetY(value); });
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
	if (tile.retained == nullptr || tile.waitingForSize || tile.closeSourceUntil != 0) return false;
	auto* content = visible ? tile.retained.Get() : tile.liveFrame.Get();
	return tile.visibleContent != content && this->setTileContent(tile, content);
}

bool Scene::pop(Tile& tile, const QRect& target, qint64 now, bool closing, float initialOpacity) {
	tile.from = target;
	tile.target = target;
	tile.radiusFrom = tile.radius;
	tile.start = now;
	tile.popStart = closing ? 0 : now;
	tile.fadingIn = !closing && initialOpacity < 1.0f;
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
	    || FAILED(this->device->CreateScaleTransform(tile.pop.ReleaseAndGetAddressOf()))
	    || FAILED(tile.pop->SetCenterX(target.width() / 2.0f))
	    || FAILED(tile.pop->SetCenterY(target.height() / 2.0f))
	    || FAILED(this->device->CreateEffectGroup(tile.fade.ReleaseAndGetAddressOf())))
		return false;
	const auto duration = closing ? PopOutDuration : Duration;
	const auto fromScale = closing ? 1.0f : PopInScale;
	const auto toScale = closing ? PopOutScale : 1.0f;
	return this->animate(fromScale, toScale, now,
	                     [&tile](auto value) { return tile.pop->SetScaleX(value); }, duration)
	    && this->animate(fromScale, toScale, now,
	                     [&tile](auto value) { return tile.pop->SetScaleY(value); }, duration)
	    && this->animate(initialOpacity, closing ? 0.0f : 1.0f, now,
	                     [&tile](auto value) { return tile.fade->SetOpacity(value); }, duration)
	    && SUCCEEDED(tile.container->SetTransform(tile.pop.Get()))
	    && SUCCEEDED(tile.container->SetEffect(tile.fade.Get()));
}

bool Scene::update(Tile& tile, const QRectF& origin, const QRect& target, qint64 now,
                   std::optional<float> cornerRadius) {
	if (tile.pop != nullptr
	    && (!this->animate(origin.width() / 2.0f, target.width() / 2.0f, now,
	                       [&tile](auto value) { return tile.pop->SetCenterX(value); })
	        || !this->animate(origin.height() / 2.0f, target.height() / 2.0f, now,
	                          [&tile](auto value) { return tile.pop->SetCenterY(value); })))
		return false;
	tile.radiusFrom = cornerRadius == 0.0f ? 0.0f : tile.radiusAt(now);
	tile.radius = cornerRadius.value_or(tile.radius);
	if (cornerRadius && tile.radiusFrom != tile.radius)
		qCDebug(logDwindleScene) << "Dwindle fullscreen corner radius" << tile.radiusFrom << "to" << tile.radius;
	tile.from = origin;
	tile.target = target;
	tile.start = now;
	if (tile.retainedSize.width() >= origin.width() - 1
	    && tile.retainedSize.height() >= origin.height() - 1
	    && (tile.target.width() < origin.width() - 1 || tile.target.height() < origin.height() - 1))
		this->retainedVisible(tile, true);
	const auto from = tile.from.translated(-this->work.topLeft());
	const auto to = tile.target.translated(-this->work.topLeft());
	const auto radius = [](float limit, const QRectF& box) {
		return static_cast<float>(std::min({double(limit), box.width() / 2, box.height() / 2}));
	};
	const auto corner = [this, fromRadius = radius(tile.radiusFrom, from),
	                     toRadius = radius(tile.radius, to), now](auto setter) {
		return this->animate(fromRadius, toRadius, now, setter);
	};
	return this->alignLiveReflow(tile)
	    && corner([&tile](auto value) { return tile.clip->SetTopLeftRadiusX(value); })
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
		const auto retained = tile.retained != nullptr && tile.visibleContent == tile.retained.Get();
		const auto size = retained ? tile.retainedSize : tile.liveSize;
		if (tile.waitingForSize || (tile.popStart != 0 && now - tile.popStart < Duration * frequency())
		    || size.width() < box.width() - 1 || size.height() < box.height() - 1
		    || std::ranges::any_of(this->tiles, [window, box, now](const auto& other) {
			    return other.first != window && other.second->at(now).intersects(box);
		    })
		    || std::ranges::any_of(this->closingTiles, [box](const auto& other) {
			    return box.intersects(QRectF(other.bounds));
		    }))
			return;
		bounds = box.toRect();
		radius = tile.radiusAt(now);
	}
	if (bounds.isEmpty() || !this->work.contains(bounds)) return;
	const auto uncovered = isUncovered(window, bounds, this->host);
	auto pixels = uncovered ? captureWorkspace(bounds) : captureWindowForClose(window, bounds.size());
	if (pixels.isNull()) {
		qCDebug(logDwindleScene) << "Dwindle close using silhouette" << window << "uncovered" << uncovered;
	} else {
		qCDebug(logDwindleScene) << "Dwindle close captured" << window << "uncovered" << uncovered;
	}

	this->closeFrames.insert_or_assign(window, CloseFrame {std::move(pixels), bounds, radius, now});
	this->closeExpiry.start(static_cast<int>(CloseFrameLifetime * 1000));
}

void Scene::holdCloseSource(HWND window) {
	const auto found = this->tiles.find(window);
	if (found == this->tiles.end() || !this->ownsPlacement()) return;
	auto& tile = *found->second;
	if (tile.closeSourceUntil != 0) return;
	const auto now = counter();
	const auto snapshot = this->closeFrames.find(window);
	const auto bounds = tile.at(now);
	auto held = false;
	if (snapshot != this->closeFrames.end()) {
		const auto& pixels = snapshot->second.pixels;
		if (pixels.width() >= bounds.width() - 1 && pixels.height() >= bounds.height() - 1) {
			auto surface = this->upload(pixels);
			held = surface != nullptr
			    && SUCCEEDED(this->device->CreateVisual(tile.closeSource.GetAddressOf()))
			    && SUCCEEDED(tile.closeSource->SetContent(surface.Get()))
			    && this->setTileContent(tile, tile.closeSource.Get());
		}
	}
	if (!held && !this->attachPlaceholder(tile)) return;
	tile.closeSourceUntil = now + static_cast<qint64>(CloseSourceHold * frequency());
	this->device->Commit();
}

void Scene::acceptCachedClose(HWND window, CloseFrame frame) {
	auto existing = this->closeFrames.find(window);
	if (existing != this->closeFrames.end()) {
		if (!existing->second.pixels.isNull()) return;
		existing->second.pixels = std::move(frame.pixels);
	} else {
		this->closeFrames.emplace(window, std::move(frame));
	}
	this->closeExpiry.start(static_cast<int>(CloseFrameLifetime * 1000));
}

bool Scene::hasClosingTile() const {
	const auto now = counter();
	return std::ranges::any_of(this->closeFrames, [now](const auto& entry) {
		return now - entry.second.captured <= CloseFrameLifetime * frequency()
		    && entry.second.ready(entry.first);
	}) || std::ranges::any_of(this->tiles, [](const auto& entry) {
		return !IsWindowVisible(entry.first) && !IsIconic(entry.first);
	});
}

bool Scene::startCloseVisual(HWND window, std::unique_ptr<Tile> tile, const QRect& bounds,
                             qint64 now, float opacity) {
	if (!this->pop(*tile, bounds, now, true, opacity)
	    || FAILED(this->root->AddVisual(tile->container.Get(), FALSE, nullptr)))
		return false;

	const auto end = now + static_cast<qint64>(PopOutDuration * frequency());
	this->closingTiles.push_back({std::move(tile), bounds, end});
	qCDebug(logDwindleScene) << "Animating Dwindle close" << window << bounds;
	return true;
}

void Scene::closeVisibleTile(HWND window, const Tile& tile) {
	if (IsWindowVisible(window) || IsIconic(window)) return;
	const auto now = counter();
	const auto bounds = tile.shownAt(now).toRect();
	if (bounds.isEmpty() || !this->work.contains(bounds)) return;

	auto opacity = tile.opacityAt(now);
	if (tile.fadingIn && tile.opacityStart == 0) {
		const auto progress = static_cast<double>(now - tile.popStart) / (Duration * frequency());
		opacity = static_cast<float>(dwindle::ease(progress));
	}
	if (opacity <= 0.0f) {
		this->closeFrames.erase(window);
		return;
	}
	if (auto close = this->closeFrames.find(window); close != this->closeFrames.end()) {
		close->second.bounds = bounds;
		close->second.opacity = opacity;
		return;
	}
	this->closeFrames.emplace(window, CloseFrame {{}, bounds, tile.radius, now, opacity});
}

bool Scene::attachCloseContent(Tile& tile, const CloseFrame& close) {
	if (close.pixels.width() < close.bounds.width() || close.pixels.height() < close.bounds.height())
		return this->attachPlaceholder(tile);
	auto content = this->upload(close.pixels);
	return content != nullptr ? SUCCEEDED(tile.container->SetContent(content.Get()))
	                          : this->attachPlaceholder(tile);
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

		auto tile = std::make_unique<Tile>();
		tile->radius = close.radius;
		if (SUCCEEDED(this->device->CreateVisual(tile->container.GetAddressOf()))
		    && SUCCEEDED(this->device->CreateRectangleClip(tile->clip.GetAddressOf()))
		    && SUCCEEDED(tile->clip->SetLeft(0.0f)) && SUCCEEDED(tile->clip->SetTop(0.0f))
		    && SUCCEEDED(tile->container->SetClip(tile->clip.Get()))
		    && this->attachCloseContent(*tile, close))
			added |= this->startCloseVisual(hwnd, std::move(tile), close.bounds, now, close.opacity);
		it = this->closeFrames.erase(it);
	}
	return added;
}

bool Scene::removeFinishedClosingTiles(qint64 now) {
	return std::erase_if(this->closingTiles, [this, now](const ClosingTile& close) {
		return now >= close.end && SUCCEEDED(this->root->RemoveVisual(close.tile->container.Get()));
	}) != 0;
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

bool Scene::updateFullscreenBar(HWND window, qint64 now, bool instant) {
	if (this->fullscreenBar != nullptr) {
		if (FAILED(this->root->RemoveVisual(this->fullscreenBar->visual.Get()))) return false;
	}
	if (window == nullptr) {
		this->fullscreenBar.reset();
		this->fullscreenBarFade.Reset();
		return true;
	}
	if (this->fullscreenBar == nullptr) {
		const auto bounds = frame(window);
		const auto shown = bounds.intersected(this->work);
		if (shown.isEmpty()) return true;
		auto bar = std::make_unique<Thumbnail>();
		if (!this->addSource(*bar, window, shown.translated(-bounds.topLeft()))) return true;
		if (FAILED(bar->visual->SetOffsetX(static_cast<float>(shown.x() - this->work.x())))
		    || FAILED(bar->visual->SetOffsetY(static_cast<float>(shown.y() - this->work.y())))
		    || FAILED(this->device->CreateEffectGroup(this->fullscreenBarFade.ReleaseAndGetAddressOf()))
		    || !this->animate(instant ? 0.0f : 1.0f, 0.0f, now,
		                      [this](auto value) { return this->fullscreenBarFade->SetOpacity(value); })
		    || FAILED(bar->visual->SetEffect(this->fullscreenBarFade.Get())))
			return false;
		this->fullscreenBar = std::move(bar);
	}
	return SUCCEEDED(this->root->AddVisual(this->fullscreenBar->visual.Get(), FALSE, nullptr));
}

bool Scene::matches(const QList<DwindleTile>& plan) const {
	if (plan.size() != static_cast<qsizetype>(this->tiles.size())) return false;
	return std::all_of(plan.begin(), plan.end(), [this](const auto& placement) {
		auto it = this->tiles.find(placement.window);
		return it != this->tiles.end() && it->second->target == placement.target
		    && it->second->fullscreen == placement.fullscreen
		    && (!placement.cornerRadius || it->second->radius == *placement.cornerRadius);
	});
}

bool Scene::show(QList<DwindleTile> plan, const QRect& layoutWork, HWND focused, bool instant) {
	if (!this->prepare(plan)) return false;
	if (!this->showFront()) return false;
	this->muteUnderlay(false);
	for (auto& [hwnd, tile]: this->tiles) this->syncShade(tile->shade, hwnd);
	for (auto& [hwnd, under]: this->underlay) this->syncShade(under->shade, hwnd);
	if (this->dim != nullptr) {
		auto* scratchpad = Scratchpad::instance();
		for (auto it = this->underlay.begin(); it != this->underlay.end();) {
			if (IsWindowVisible(it->first) && !scratchpad->containsWindow(it->first)) {
				++it;
				continue;
			}
			if (FAILED(this->root->RemoveVisual(it->second->source.visual.Get()))
			    || FAILED(this->root->RemoveVisual(it->second->frame.Get())))
				return false;
			it = this->underlay.erase(it);
		}
	}
	const auto continuing = !this->tiles.empty();
	if (this->focused != focused) {
		if (this->focused != nullptr) {
			if (!this->raise()) return false;
		}
		this->focused = focused;
	}
	if (this->handoffFrame != nullptr) {
		if (FAILED(this->root->RemoveVisual(this->handoffFrame.Get()))) return false;
		this->handoffFrame.Reset();
	}
	plan.removeIf([](const DwindleTile& tile) { return !IsWindowVisible(tile.window); });
	for (auto it = this->tiles.begin(); it != this->tiles.end();) {
		auto present = std::any_of(plan.begin(), plan.end(), [hwnd = it->first](const auto& tile) {
			return tile.window == hwnd;
		});
		if (present) {
			++it;
			continue;
		}
		if (FAILED(this->root->RemoveVisual(it->second->container.Get()))) return false;
		this->closeVisibleTile(it->first, *it->second);
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
	for (auto placement = plan.begin(); placement != plan.end();) {
		auto [it, added] = this->tiles.try_emplace(placement->window);
		if (added) {
			it->second = std::make_unique<Tile>();
			it->second->order = ++this->lastTileOrder;
			auto& tile = *it->second;
			if (!this->addTile(tile, *placement, before) || !this->addDiagnosticMarker(tile)) {
				if (IsWindowVisible(placement->window)) {
					qCDebug(logDwindleScene) << "Could not attach tile" << placement->window;
					return false;
				}
				if (tile.container != nullptr) this->root->RemoveVisual(tile.container.Get());
				this->tiles.erase(it);
				placement = plan.erase(placement);
				continue;
			}
		}
		++placement;
	}
	const auto fresh = !IsWindowVisible(this->host);
	this->visibleWork = fresh ? layoutWork : this->visibleWork.united(layoutWork);
	for (const auto& placement: plan) {
		this->visibleWork = this->visibleWork.united(placement.previous.intersected(this->work));
		this->visibleWork = this->visibleWork.united(placement.target.intersected(this->work));
	}
	const auto clip = this->visibleWork.translated(-this->work.topLeft());
	const D2D_RECT_F bounds {
	    static_cast<float>(clip.x()), static_cast<float>(clip.y()),
	    static_cast<float>(clip.x() + clip.width()), static_cast<float>(clip.y() + clip.height()),
	};
	if (FAILED(this->root->SetClip(bounds))) return false;
	const auto start = counter();
	const auto closing = this->addClosingTiles(start);
	const auto closingReflow = !this->closingTiles.empty();
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
		if (!this->awaitSize(*this->tiles.at(plan[i].window))) return false;
	}
	HWND fullscreen = nullptr;
	HWND leavingFullscreen = nullptr;
	for (qsizetype i = 0; i < plan.size(); ++i) {
		const auto& placement = plan[i];
		auto& tile = *this->tiles.at(placement.window);
		tile.fullscreen = placement.fullscreen;
		tile.floating = placement.floating;
		if (tile.fullscreen == DwindleFullscreen::Active) fullscreen = placement.window;
		if (tile.fullscreen == DwindleFullscreen::Leaving) leavingFullscreen = placement.window;
		if (!tile.target.isEmpty() && tile.target == placement.target
		    && (!placement.cornerRadius || tile.radius == *placement.cornerRadius))
			continue;
		auto scheduled = false;
		if (origins[i].has_value()) {
			tile.closingReflow |= closingReflow;
			scheduled = this->update(tile, *origins[i], placement.target, start, placement.cornerRadius);
		} else {
			const auto opening = placement.previous.isEmpty();
			const auto from = opening && layoutWork.contains(placement.openingFrame)
			                ? placement.openingFrame : placement.target;
			scheduled = this->pop(tile, from, start, false, opening ? 0.0f : 1.0f);
			if (scheduled && from != placement.target) {
				scheduled = this->update(tile, QRectF(from), placement.target, start, placement.cornerRadius);
				qCDebug(logDwindleScene) << "Dwindle opening" << placement.window << from
				                         << "to" << placement.target;
			}
		}
		if (!scheduled) {
			qCDebug(logDwindleScene) << "Could not schedule tile" << placement.window;
			return false;
		}
	}
	auto* bar = fullscreen != nullptr
	          ? WinPanelWindow::fullscreenBarHandle(MonitorFromWindow(this->host, MONITOR_DEFAULTTONULL))
	          : nullptr;
	if (!this->updateFullscreenBar(bar, start, instant && !continuing)) return false;
	auto* front = fullscreen != nullptr ? fullscreen
	              : this->focused != nullptr ? this->focused : leavingFullscreen;
	if (front != nullptr) {
		auto found = this->tiles.find(front);
		if (found == this->tiles.end()) return false;
		if (FAILED(this->root->RemoveVisual(found->second->container.Get()))
		    || FAILED(this->root->AddVisual(found->second->container.Get(), FALSE, nullptr)))
			return false;
		found->second->order = ++this->lastTileOrder;
	}
	auto* fadeAround = fullscreen != nullptr ? fullscreen : this->focused;
	for (auto& [hwnd, tile]: this->tiles) {
		const auto target = fadeAround != nullptr && hwnd != fadeAround ? 0.0f : 1.0f;
		if (!continuing && leavingFullscreen != nullptr && hwnd != leavingFullscreen)
			tile->opacityTarget = 0.0f;
		if (!continuing && instant && target == 0.0f) tile->opacityTarget = 0.0f;
		if (!continuing && tile->opacityTarget == 0.0f && target == 0.0f
		    && FAILED(tile->fade->SetOpacity(0.0f))) return false;
		if (tile->opacityTarget == target) continue;
		const auto opacity = tile->opacityAt(start);
		if (!this->animate(opacity, target, start, [&tile](auto value) {
		        return tile->fade->SetOpacity(value);
		    })) return false;
		tile->opacityFrom = opacity;
		tile->opacityTarget = target;
		tile->opacityStart = start;
	}
	if (diagnosticsEnabled()) {
		for (const auto& placement: plan) {
			auto& tile = *this->tiles.at(placement.window);
			tile.diagnosticState = -1;
		}
	}
	if (plan.isEmpty() && !closing && this->closingTiles.empty()) {
		this->hide();
		return false;
	}
	const auto uncovering = !continuing && (leavingFullscreen != nullptr || (instant && fadeAround != nullptr));
	this->copyFloating(start, fadeAround, uncovering);
	if (!succeeded(this->device->Commit(), "Commit")) return false;
	if (fresh && instant && !continuing && this->focused != nullptr && this->focused == fadeAround)
		return this->holdNow();
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

bool Scene::holdNow() {
	this->keepBarAbove();
	this->dropFloating();
	this->hideFront();
	if (this->hiddenFront != this->focused) return false;
	this->publishDragScene();
	this->retries = 0;
	this->released = true;
	this->finishing = false;
	this->hurrying = false;
	if (SetWindowPos(this->host, this->focused, 0, 0, 0, 0,
	                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) == FALSE)
		return false;
	this->idle.start(250);
	qCDebug(logDwindleScene) << "Covering the others under" << this->focused;
	return true;
}

void Scene::hideFront() {
	if (this->hiddenFront == this->focused) return;
	auto found = this->tiles.find(this->focused);
	if (found == this->tiles.end() || FAILED(found->second->fade->SetOpacity(0.0f))) return;
	this->muteUnderlay(true);
	if (SUCCEEDED(this->device->Commit())) this->hiddenFront = this->focused;
}

bool Scene::showFront() {
	auto* window = std::exchange(this->hiddenFront, nullptr);
	auto found = this->tiles.find(window);
	if (window == nullptr || found == this->tiles.end()) return true;
	if (FAILED(found->second->fade->SetOpacity(1.0f)) || FAILED(this->device->Commit())) return false;
	found->second->opacityFrom = 1.0f;
	found->second->opacityTarget = 1.0f;
	found->second->opacityStart = 0;
	DwmFlush();
	return true;
}

QList<HWND> Scene::floatingWindows(HWND cover) const {
	QList<HWND> windows;
	const auto special = this->dim != nullptr;
	auto* tracker = WindowTracker::instance();
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr && hwnd != this->backdropWindow;
	     hwnd = GetWindow(hwnd, GW_HWNDNEXT))
	{
		if (hwnd == this->host || isOwnProcessWindow(hwnd)) continue;
		const auto tile = this->tiles.find(hwnd);
		if (tile != this->tiles.end() && !tile->second->floating && hwnd != cover) continue;
		if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) || cloaked(hwnd)) continue;
		if (!special && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) continue;
		auto* window = tracker->windowFor(hwnd);
		if (special != Scratchpad::holds(window)) continue;
		auto* owner = GetWindow(hwnd, GW_OWNER);
		const auto dialog = owner != nullptr && tracker->windowFor(GetAncestor(owner, GA_ROOTOWNER)) != nullptr
		                 && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
		if (window == nullptr && !dialog) continue;
		if (frame(hwnd).intersects(this->visibleWork)) windows.prepend(hwnd);
	}
	return windows;
}

bool Scene::followFloating(HWND window, FloatingCopy& copy) {
	const auto bounds = frame(window);
	if (bounds.isEmpty()) return false;
	if (bounds == copy.shown && copy.source.sourceCrop.size() == bounds.size()) return true;
	const auto radius = windowRadius(window);
	if (!this->setNativeSize(copy.source, window)
	    || FAILED(copy.source.visual->SetOffsetX(static_cast<float>(copy.source.contentOffset.x())))
	    || FAILED(copy.source.visual->SetOffsetY(static_cast<float>(copy.source.contentOffset.y())))
	    || FAILED(copy.container->SetOffsetX(static_cast<float>(bounds.x() - this->work.x())))
	    || FAILED(copy.container->SetOffsetY(static_cast<float>(bounds.y() - this->work.y())))
	    || FAILED(copy.clip->SetLeft(0.0f)) || FAILED(copy.clip->SetTop(0.0f))
	    || FAILED(copy.clip->SetRight(static_cast<float>(bounds.width())))
	    || FAILED(copy.clip->SetBottom(static_cast<float>(bounds.height())))
	    || FAILED(copy.clip->SetTopLeftRadiusX(radius)) || FAILED(copy.clip->SetTopLeftRadiusY(radius))
	    || FAILED(copy.clip->SetTopRightRadiusX(radius)) || FAILED(copy.clip->SetTopRightRadiusY(radius))
	    || FAILED(copy.clip->SetBottomLeftRadiusX(radius)) || FAILED(copy.clip->SetBottomLeftRadiusY(radius))
	    || FAILED(copy.clip->SetBottomRightRadiusX(radius))
	    || FAILED(copy.clip->SetBottomRightRadiusY(radius)))
		return false;
	copy.shown = bounds;
	return true;
}

void Scene::copyFloating(qint64 now, HWND cover, bool uncovering) {
	const auto windows = this->floatingWindows(cover);
	for (auto it = this->floats.begin(); it != this->floats.end();) {
		if (windows.contains(it->first) && !this->tiles.contains(it->first)) {
			++it;
			continue;
		}
		this->root->RemoveVisual(it->second->container.Get());
		it = this->floats.erase(it);
	}
	const auto coverAt = windows.indexOf(cover);
	const auto covered = QSet<HWND>(windows.begin(), windows.begin() + qMax(qsizetype(0), coverAt));
	for (auto* hwnd: windows) {
		if (auto tile = this->tiles.find(hwnd); tile != this->tiles.end()) {
			this->root->RemoveVisual(tile->second->container.Get());
			this->root->AddVisual(tile->second->container.Get(), FALSE, nullptr);
			tile->second->order = ++this->lastTileOrder;
			continue;
		}
		auto found = this->floats.find(hwnd);
		if (found == this->floats.end()) {
			auto copy = std::make_unique<FloatingCopy>();
			copy->opacityTarget = uncovering || this->coveredFloats.contains(hwnd) ? 0.0f : 1.0f;
			if (!this->addSource(copy->source, hwnd)
			    || FAILED(this->device->CreateVisual(copy->container.GetAddressOf()))
			    || FAILED(this->device->CreateRectangleClip(copy->clip.GetAddressOf()))
			    || FAILED(this->device->CreateEffectGroup(copy->fade.GetAddressOf()))
			    || FAILED(copy->fade->SetOpacity(copy->opacityTarget))
			    || FAILED(copy->container->SetEffect(copy->fade.Get()))
			    || FAILED(copy->container->SetClip(copy->clip.Get()))
			    || FAILED(copy->container->AddVisual(copy->source.visual.Get(), FALSE, nullptr))
			    || !this->addShade(copy->shade, hwnd, copy->container.Get())
			    || !this->followFloating(hwnd, *copy))
				continue;
			found = this->floats.emplace(hwnd, std::move(copy)).first;
		} else {
			this->followFloating(hwnd, *found->second);
			this->root->RemoveVisual(found->second->container.Get());
		}
		auto& copy = *found->second;
		this->root->AddVisual(copy.container.Get(), FALSE, nullptr);
		const auto target = covered.contains(hwnd) ? 0.0f : 1.0f;
		if (copy.opacityTarget == target) continue;
		const auto opacity = copy.opacityAt(now);
		if (!this->animate(opacity, target, now, [&copy](auto value) { return copy.fade->SetOpacity(value); }))
			continue;
		copy.opacityFrom = opacity;
		copy.opacityTarget = target;
		copy.opacityStart = now;
	}
	this->coveredFloats = covered;
}

void Scene::dropFloating() {
	if (this->floats.empty()) return;
	for (auto& [hwnd, copy]: this->floats) this->root->RemoveVisual(copy->container.Get());
	this->floats.clear();
	this->device->Commit();
}

bool Scene::staysAbove(HWND hwnd) const {
	if (hwnd == this->host || this->tiles.contains(hwnd)) return false;
	if (!layers::staysAbove(hwnd) && !floatsAboveTiles(hwnd)) return false;
	if (Scratchpad::holds(WindowTracker::instance()->windowFor(hwnd))) return false;
	return frame(hwnd).intersects(this->visibleWork);
}

void Scene::keepBarAbove() {
	if (this->barMonitor != nullptr) return;
	this->barMonitor = MonitorFromWindow(this->host, MONITOR_DEFAULTTONULL);
	WinPanelWindow::setDwindleBarAbove(this->barMonitor, true);
}

bool Scene::raise() {
	this->keepBarAbove();
	auto panels = WinPanelWindow::aboveWindowHandles();
	auto above = HWND_TOPMOST;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) continue;
		if (panels.contains(hwnd) || this->staysAbove(hwnd)) above = hwnd;
	}
	return SetWindowPos(this->host, above, 0, 0, 0, 0,
	                    SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW) != FALSE;
}

HWND Scene::coveringHost() const {
	if ((GetWindowLongPtrW(this->host, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) return this->host;
	for (auto* above = GetWindow(this->host, GW_HWNDPREV); above != nullptr;
	     above = GetWindow(above, GW_HWNDPREV))
	{
		if (above == this->focused) continue;
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
		if (above == this->focused) continue;
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
		const auto retained = tile->retained != nullptr && tile->visibleContent == tile->retained.Get();
		const auto state = (onScreen ? 1 : 0) | (exposed ? 2 : 0)
		                 | (tile->waitingForSize ? 4 : 0) | (retained ? 8 : 0);
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
		                         << "retained" << retained;
	}
}

void Scene::refresh() {
	if (this->host == nullptr || !IsWindow(this->backdropWindow)
	    || (this->dim != nullptr && !IsWindowVisible(this->dim))) {
		this->hide();
		return;
	}
	const auto now = counter();
	if (diagnosticsEnabled()) this->inspectParticipants(now);
	if (this->released && !this->occludingTiles().isEmpty()) {
		this->hide();
		return;
	}
	if (!this->released && this->occlusionStarted != 0) {
		if (this->occludingTiles().isEmpty()) {
			// The covering windows have finished parking. Don't abort the scene
			// after the grace period: let the normal reflow/close animation settle.
			// This matters for special, whose windows start in the topmost band.
			this->occlusionStarted = 0;
			for (auto& [hwnd, tile]: this->tiles) tile->occlusionRequested = 0;
		} else if (static_cast<double>(now - this->occlusionStarted) / frequency() > OcclusionStageLimit) {
			qCDebug(logDwindleScene) << "Dwindle ending with covering tiles still in front";
			this->finish(true);
			return;
		}
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
	if (this->focused == nullptr && !sceneDuringDrag() && (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0
	    || GetAsyncKeyState(VK_MBUTTON) < 0))
	{
		this->finish();
		return;
	}
	const auto foreground = GetForegroundWindow();
	const auto intersecting = this->visibleWork.intersects(frame(foreground));
	if (foreground != nullptr && intersecting && !this->tiles.contains(foreground)
	    && !this->floats.contains(foreground))
	{
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
	for (auto& [hwnd, under]: this->underlay) changed |= this->syncShade(under->shade, hwnd);
	for (auto& [hwnd, copy]: this->floats) changed |= this->syncShade(copy->shade, hwnd);
	for (auto& [hwnd, tile]: this->tiles) changed |= this->syncShade(tile->shade, hwnd);
	for (auto& [hwnd, copy]: this->floats) {
		if (IsWindow(hwnd) && frame(hwnd) != copy->shown) changed |= this->followFloating(hwnd, *copy);
	}
	for (auto& [hwnd, tile]: this->tiles) {
		if (tile->closeSourceUntil != 0) {
			if (now < tile->closeSourceUntil || !IsWindowVisible(hwnd)) continue;
			auto* content = tile->waitingForSize ? tile->placeholder.Get() : tile->liveFrame.Get();
			if (!this->setTileContent(*tile, content)) continue;
			tile->closeSourceUntil = 0;
			tile->closeSource.Reset();
			changed = true;
		}
		if (!IsWindow(hwnd) || !this->setNativeSize(tile->source, hwnd)) continue;
		if ((tile->liveSize != tile->source.sourceCrop.size()
		     || tile->liveOffset != tile->source.contentOffset)
		    && this->resizeLiveClip(*tile))
			changed = true;
		const auto moving = tile->at(now);
		const auto fills = [&moving](const QSize& size) {
			return size.width() >= moving.width() - 1 && size.height() >= moving.height() - 1;
		};
		if (tile->waitingForSize) {
			const auto animationStart = std::max(tile->popStart, tile->start);
			if (tile->fadingIn && now - animationStart < Duration * frequency()) continue;
			const auto size = tile->liveSize;
			if (!fills(size) || std::abs(size.width() - tile->target.width()) > 4
			    || std::abs(size.height() - tile->target.height()) > 4)
				continue;
			if (this->setTileContent(*tile, tile->liveFrame.Get())) {
				tile->waitingForSize = false;
				changed = true;
			}
		}
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
	const auto placed = std::all_of(this->tiles.begin(), this->tiles.end(), [this](const auto& pair) {
		const auto& [hwnd, tile] = pair;
		if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || !tile->target.intersects(this->work)) return true;
		const auto actual = frame(hwnd);
		const auto target = tile->target.adjusted(-2, -2, 2, 2);
		return !actual.isEmpty() && target.contains(actual)
		    && std::abs(actual.width() - tile->target.width()) <= 4
		    && std::abs(actual.height() - tile->target.height()) <= 4;
	});
	if (!this->released) {
		const auto resizing = std::ranges::any_of(this->tiles, [](const auto& pair) {
			return pair.second->closingReflow;
		});
		if (resizing && !placed && !this->hurrying && !this->finishing
		    && this->occludingTiles().isEmpty())
		{
			auto surface = this->upload(captureWorkspace(this->work));
			ComPtr<IDCompositionVisual> frame;
			if (surface != nullptr && SUCCEEDED(this->device->CreateVisual(frame.GetAddressOf()))
			    && SUCCEEDED(frame->SetContent(surface.Get()))
			    && SUCCEEDED(this->root->AddVisual(frame.Get(), FALSE, nullptr)))
			{
				this->handoffFrame = std::move(frame);
				this->device->Commit();
			}
		}
		this->release();
		this->retries = 0;
		this->idle.start(this->handoffFrame != nullptr ? 32 : 16);
		return;
	}
	const auto arrived = std::all_of(this->tiles.begin(), this->tiles.end(), [this](const auto& pair) {
		const auto& [hwnd, tile] = pair;
		return !IsWindow(hwnd) || !IsWindowVisible(hwnd) || !tile->target.intersects(this->work)
		    || frame(hwnd).intersects(this->work);
	});
	if ((!arrived && this->retries < ArrivalRetries)
	    || (!placed && !this->hurrying && this->retries < ReleaseRetries)) {
		++this->retries;
		this->idle.start(16);
		return;
	}
	if (!arrived) qCDebug(logDwindleScene) << "Handing over before every window arrived";
	if (this->focused != nullptr && !this->finishing && IsWindow(this->focused)) {
		constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
		if ((GetWindowLongPtrW(this->host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
			SetWindowPos(this->host, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
		if (GetWindow(this->focused, GW_HWNDNEXT) != this->host)
			SetWindowPos(this->host, this->focused, 0, 0, 0, 0, flags);
		this->dropFloating();
		this->hideFront();
		this->sample.stop();
		this->idle.start(250);
		return;
	}
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
	this->focused = nullptr;
	this->hiddenFront = nullptr;
	this->coveredFloats.clear();
	if (this->host != nullptr && IsWindowVisible(this->host)) {
		ShowWindow(this->host, SW_HIDE);
		QTimer::singleShot(0, InactiveDim::instance(), &InactiveDim::comeBack);
	}
	this->release();
	this->finishing = false;
	this->hurrying = false;
	this->stranger = nullptr;
	this->holdUntil = 0;
	this->coverFrame = {};
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
	this->fullscreenBar.reset();
	this->fullscreenBarFade.Reset();
	this->underlay.clear();
	this->floats.clear();
	this->dimVisual.Reset();
	this->dimFade.Reset();
	this->dimColor.Reset();
	this->root.Reset();
	this->tileColor.Reset();
	this->diagnosticColor.Reset();
	this->handoffFrame.Reset();
	this->target.Reset();
	this->device.Reset();
	this->desktopDevice.Reset();
	if (this->host != nullptr) DestroyWindow(this->host);
	this->host = nullptr;
	this->backdropWindow = nullptr;
}

HWND underlayDim(HMONITOR monitor) {
	auto* pad = Scratchpad::instance();
	if (!pad->shown() || pad->monitor() != monitor) return nullptr;
	auto* dim = pad->dimWindow();
	return dim != nullptr && IsWindowVisible(dim) ? dim : nullptr;
}

auto& scenes() {
	static auto* byMonitor = new std::map<HMONITOR, std::unique_ptr<Scene>>();
	return *byMonitor;
}

auto& keptScenes() {
	static auto* byMonitor = new std::map<HMONITOR, std::unique_ptr<Scene>>();
	return *byMonitor;
}

void takeBackKept(HMONITOR monitor) {
	auto kept = keptScenes().find(monitor);
	if (kept == keptScenes().end() || underlayDim(monitor) != nullptr) return;
	auto scene = std::move(kept->second);
	keptScenes().erase(kept);
	if (scene->held()) scenes()[monitor] = std::move(scene);
}

}

float DwindleScene::cornerRadius(HWND window) { return windowRadius(window); }

bool DwindleScene::enabled() {
	static const auto enabled = !qEnvironmentVariableIsSet("II_DWINDLE_SCENE") || qEnvironmentVariableIntValue("II_DWINDLE_SCENE") != 0;
	return enabled;
}

void DwindleScene::warmUp() { (void)graphics(); }

void DwindleScene::snapshotClose(HMONITOR monitor, const QRect& work, HWND window) {
	if (work.isEmpty()) return;
	auto& scene = scenes()[monitor];
	const auto viewport = sceneViewport(monitor, work);
	if (scene == nullptr || scene->work != viewport || scene->dim != underlayDim(monitor))
		scene = std::make_unique<Scene>(viewport, underlayDim(monitor));
	scene->snapshotClose(window);
}

bool DwindleScene::cacheCloseFrame(HWND window, const QRect& bounds) {
	if (!IsWindowVisible(window) || IsIconic(window) || cloaked(window)
	    || IsHungAppWindow(window) || frame(window) != bounds) return false;

	DWORD pid = 0;
	DWORD affinity = 0;
	RECT rect {};
	if (!GetWindowThreadProcessId(window, &pid) || !GetWindowRect(window, &rect)
	    || (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE)) return false;

	const QRect raw(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
	if (!raw.contains(bounds)) return false;
	const auto radius = windowRadius(window);
	const auto captured = counter();
	quint64 generation = 0;
	{
		auto& cache = closeCache();
		auto lock = std::lock_guard(cache.mutex);
		generation = cache.generation;
	}

	return startCloseCapture([window, raw, bounds, radius, captured, pid, generation] {
		auto pixels = QImage();
		if (frame(window) == bounds && isUncovered(window, bounds, nullptr)) {
			pixels = captureWorkspace(bounds);
			if (!isUncovered(window, bounds, nullptr)) pixels = {};
		}
		if (pixels.isNull()) pixels = captureWindowPixels(window, raw, bounds);
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

void DwindleScene::prepareClose(HMONITOR monitor, const QRect& work, HWND window, quint32 pid,
                                const QRect& bounds, bool removed) {
	const auto now = counter();
	CloseFrame snapshot {{}, bounds, windowRadius(window), now};
	{
		auto& cache = closeCache();
		auto lock = std::lock_guard(cache.mutex);
		if (auto found = cache.frames.find(window); found != cache.frames.end()) {
			const auto& stored = found->second;
			if (stored.pid == pid && now - stored.frame.captured <= CachedCloseLifetime * frequency())
				snapshot = stored.frame;
			if (removed) forgetCachedFrame(cache, found);
		}
	}
	if (snapshot.pixels.isNull() && !removed) return;

	const auto viewport = sceneViewport(monitor, work);
	const auto known = scenes().find(monitor);
	if (known != scenes().end() && known->second->work == viewport) {
		const auto shown = known->second->frameForDrag(window);
		if (!shown.isEmpty()) snapshot.bounds = shown;
	}
	if (snapshot.bounds.isEmpty() || !viewport.contains(snapshot.bounds)) return;
	auto& scene = scenes()[monitor];
	if (scene == nullptr || scene->work != viewport || scene->dim != underlayDim(monitor))
		scene = std::make_unique<Scene>(viewport, underlayDim(monitor));
	snapshot.captured = now;
	scene->acceptCachedClose(window, std::move(snapshot));
}

bool DwindleScene::hasCloseFrame(HWND window) {
	auto& cache = closeCache();
	auto lock = std::lock_guard(cache.mutex);
	return cache.frames.contains(window);
}

void DwindleScene::holdCloseSource(HMONITOR monitor, HWND window) {
	const auto found = scenes().find(monitor);
	if (found != scenes().end()) found->second->holdCloseSource(window);
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
	return found != scenes().end() && found->second != nullptr && found->second->work == sceneViewport(monitor, work)
	    && found->second->hasClosingTile();
}

void DwindleScene::present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed,
                          HWND focused) {
	takeBackKept(monitor);
	auto found = scenes().find(monitor);
	const auto closing = DwindleScene::hasClosingTile(monitor, work);
	if (work.isEmpty() || (tiles.isEmpty() && !closing)) {
		DwindleScene::dismiss(monitor);
		return;
	}
	if (!changed && !closing && focused == nullptr
	    && (found == scenes().end() || !found->second->visible())) return;
	const auto viewport = sceneViewport(monitor, work);
	if (!closing && found != scenes().end() && found->second->visible()
	    && found->second->work == viewport && found->second->dim == underlayDim(monitor)
	    && found->second->matches(tiles) && found->second->focusedWindow() == focused) return;
	auto& scene = scenes()[monitor];
	if (scene == nullptr || scene->work != viewport || scene->dim != underlayDim(monitor))
		scene = std::make_unique<Scene>(viewport, underlayDim(monitor));
	if (!scene->show(tiles, work, focused, !changed && !closing)) {
		qCDebug(logDwindleScene) << "Using immediate placement";
		DwindleScene::dismiss(monitor);
	}
}

void DwindleScene::cover(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles) {
	if (work.isEmpty()) return;
	takeBackKept(monitor);
	auto& scene = scenes()[monitor];
	const auto viewport = sceneViewport(monitor, work);
	if (scene != nullptr && scene->visible() && scene->work == viewport) {
		scene->hold();
		return;
	}
	if (scene == nullptr || scene->work != viewport || scene->dim != underlayDim(monitor))
		scene = std::make_unique<Scene>(viewport, underlayDim(monitor));
	if (!scene->show(tiles, work, scene->focusedWindow())) {
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
	return found != scenes().end() && found->second->ownsPlacement();
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

HWND DwindleScene::coverUnder(HWND window) {
	for (const auto& [monitor, scene]: scenes()) {
		if (scene != nullptr && scene->visible() && scene->held() && scene->focusedWindow() == window)
			return scene->hostWindow();
	}
	return nullptr;
}

HWND DwindleScene::specialHost() {
	for (const auto& [monitor, scene]: scenes()) {
		if (scene != nullptr && scene->visible() && scene->dim != nullptr) return scene->hostWindow();
	}
	return nullptr;
}

bool DwindleScene::holdsSpecial() {
	for (const auto& [monitor, scene]: scenes()) {
		if (scene != nullptr && scene->visible() && scene->dim != nullptr) return scene->held();
	}
	return false;
}

bool DwindleScene::animating() {
	return std::ranges::any_of(scenes(), [](const auto& entry) {
		return entry.second != nullptr && entry.second->visible() && !entry.second->held();
	});
}

void DwindleScene::setDuringDrag(bool during) { sceneDuringDrag() = during; }

void DwindleScene::dismissAll() {
	scenes().clear();
	keptScenes().clear();
}

void DwindleScene::dismissFor(HWND window) {
	std::erase_if(scenes(), [window](const auto& entry) {
		return entry.second == nullptr || !entry.second->held() || entry.second->focusedWindow() == window;
	});
}

void DwindleScene::drop(HMONITOR monitor) {
	auto found = scenes().find(monitor);
	if (found == scenes().end()) return;
	auto scene = std::move(found->second);
	scenes().erase(found);
	if (scene != nullptr && scene->held() && scene->dim == nullptr) {
		keptScenes()[monitor] = std::move(scene);
	}
}

}
