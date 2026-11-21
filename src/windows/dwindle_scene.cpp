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
	SIZE sourceSize {};

	~Thumbnail() {
		if (this->handle != nullptr) DwmUnregisterThumbnail(this->handle);
	}
};

struct Tile {
	Thumbnail source;
	ComPtr<IDCompositionVisual> container;
	ComPtr<IDCompositionRectangleClip> clip;
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
	bool makeTileColor(ID3D11DeviceContext* context);
	bool addTile(Tile& tile, const DwindleTile& placement);
	bool update(Tile& tile, const QRectF& from, const QRect& target, qint64 now);
	bool addSource(Thumbnail& thumb, HWND window, const QRect& crop = {});
	bool setNativeSize(Thumbnail& thumb);
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
	    || !this->makeTileColor(graphics.context.Get()))
		return false;

	const auto crop = this->work.translated(-frame(this->backdropWindow).topLeft());
	if (!this->addSource(this->backdrop, this->backdropWindow, crop)) {
		qCDebug(logDwindleScene) << "Could not attach desktop background" << crop;
		return false;
	}
	return succeeded(this->root->AddVisual(this->backdrop.visual.Get(), TRUE, nullptr), "Add backdrop");
}

bool Scene::makeTileColor(ID3D11DeviceContext* context) {
	if (FAILED(this->device->CreateSurface(
	        1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE, this->tileColor.GetAddressOf()
	    ))) return false;
	ComPtr<IDXGISurface> surface;
	POINT offset {};
	if (!succeeded(this->tileColor->BeginDraw(nullptr, IID_PPV_ARGS(surface.GetAddressOf()), &offset),
	               "BeginDraw"))
		return false;

	ComPtr<ID3D11Texture2D> texture;
	auto copied = succeeded(surface.As(&texture), "Tile color texture");
	if (copied) {
		constexpr UINT32 color = 0xff202020;
		const D3D11_BOX region {
		    static_cast<UINT>(offset.x), static_cast<UINT>(offset.y), 0,
		    static_cast<UINT>(offset.x + 1), static_cast<UINT>(offset.y + 1), 1,
		};
		context->UpdateSubresource(texture.Get(), 0, &region, &color, sizeof(color), 0);
	}
	return succeeded(this->tileColor->EndDraw(), "EndDraw") && copied;
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
	return !crop.isEmpty() || this->setNativeSize(thumb);
}

bool Scene::setNativeSize(Thumbnail& thumb) {
	SIZE size {};
	if (!succeeded(DwmQueryThumbnailSourceSize(thumb.handle, &size), "DwmQueryThumbnailSourceSize")
	    || size.cx <= 0 || size.cy <= 0)
		return false;
	if (size.cx == thumb.sourceSize.cx && size.cy == thumb.sourceSize.cy) return true;
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_RECTDESTINATION
	             | DWM_TNP_RECTSOURCE | CompositionThumbnail;
	props.fVisible = TRUE;
	props.opacity = 255;
	props.rcDestination = {0, 0, size.cx, size.cy};
	props.rcSource = props.rcDestination;
	if (!succeeded(DwmUpdateThumbnailProperties(thumb.handle, &props), "DwmUpdateThumbnailProperties"))
		return false;
	thumb.sourceSize = size;
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

bool Scene::addTile(Tile& tile, const DwindleTile& placement) {
	tile.waitingForSize = placement.previous.isEmpty();
	ComPtr<IDCompositionVisual> fill;
	D2D_MATRIX_3X2_F stretch {};
	stretch._11 = this->work.width();
	stretch._22 = this->work.height();
	return this->addSource(tile.source, placement.window)
	    && SUCCEEDED(this->device->CreateVisual(fill.GetAddressOf()))
	    && SUCCEEDED(fill->SetContent(this->tileColor.Get()))
	    && SUCCEEDED(fill->SetTransform(stretch))
	    && SUCCEEDED(this->device->CreateVisual(tile.container.GetAddressOf()))
	    && SUCCEEDED(tile.container->AddVisual(fill.Get(), TRUE, nullptr))
	    && SUCCEEDED(this->device->CreateRectangleClip(tile.clip.GetAddressOf()))
	    && SUCCEEDED(tile.clip->SetLeft(0.0f))
	    && SUCCEEDED(tile.clip->SetTop(0.0f))
	    && SUCCEEDED(tile.container->SetClip(tile.clip.Get()))
	    && (tile.waitingForSize
	        || SUCCEEDED(tile.container->AddVisual(tile.source.visual.Get(), FALSE, nullptr)))
	    && SUCCEEDED(this->root->AddVisual(tile.container.Get(), FALSE, nullptr));
}

bool Scene::update(Tile& tile, const QRectF& origin, const QRect& target, qint64 now) {
	tile.from = origin;
	tile.target = target;
	tile.start = now;
	const auto from = tile.from.translated(-this->work.topLeft());
	const auto to = tile.target.translated(-this->work.topLeft());
	const auto radius = [](const QRectF& box) {
		return static_cast<float>(std::min({8.0, box.width() / 2, box.height() / 2}));
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
	for (const auto& placement: plan) {
		auto [it, added] = this->tiles.try_emplace(placement.window);
		if (!added) continue;
		it->second = std::make_unique<Tile>();
		if (!this->addTile(*it->second, placement)) {
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
	bool attached = false;
	for (auto& [hwnd, tile]: this->tiles) {
		if (!IsWindow(hwnd) || !this->setNativeSize(tile->source) || !tile->waitingForSize)
			continue;
		const auto actual = frame(hwnd);
		if (std::abs(actual.width() - tile->target.width()) > 4
		    || std::abs(actual.height() - tile->target.height()) > 4)
			continue;
		if (SUCCEEDED(tile->container->AddVisual(tile->source.visual.Get(), FALSE, nullptr))) {
			tile->waitingForSize = false;
			attached = true;
		}
	}
	if (attached && FAILED(this->device->Commit())) this->hide();
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
