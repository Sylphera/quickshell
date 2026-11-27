#include "inactive_dim.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <qt_windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dcompanimation.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <qcoreapplication.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qrect.h>
#include <qregion.h>
#include <qtimer.h>
#include <qtypes.h>

#include "desktop_slide.hpp"
#include "dim_plan.hpp"
#include "dwindle_scene.hpp"
#include "panel_window.hpp"
#include "scratchpad.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

using Microsoft::WRL::ComPtr;

Q_LOGGING_CATEGORY(logDim, "quickshell.windows.dim", QtWarningMsg);

constexpr auto* DIM_CLASS = L"QuickshellInactiveDim";
constexpr float STRENGTH = 0.05f;
constexpr double FADE_SECONDS = 0.8;
constexpr int FADE_STEPS = 24;
constexpr int REFRESH_MS = 8;
constexpr int BUSY_MS = 30;

qint64 nowMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

HWND shadeSource() {
	static auto* source = []() -> HWND {
		constexpr auto* name = L"QuickshellDimShade";
		constexpr int size = 16;
		WNDCLASSW cls {};
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpfnWndProc = &DefWindowProcW;
		cls.lpszClassName = name;
		RegisterClassW(&cls);
		auto x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (size * 4);
		auto y = GetSystemMetrics(SM_YVIRTUALSCREEN) - (size * 4);
		auto* window = CreateWindowExW(
		    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
		    name,
		    L"",
		    WS_POPUP,
		    x,
		    y,
		    size,
		    size,
		    nullptr,
		    nullptr,
		    cls.hInstance,
		    nullptr
		);
		if (window == nullptr) return nullptr;
		auto* screen = GetDC(nullptr);
		BITMAPINFO info {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = size;
		info.bmiHeader.biHeight = -size;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		void* raw = nullptr;
		auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
		auto* memory = CreateCompatibleDC(screen);
		auto* previous = SelectObject(memory, bitmap);
		std::fill_n(static_cast<uint32_t*>(raw), size * size, 0xff000000);
		GdiFlush();
		POINT origin {x, y};
		SIZE extent {size, size};
		POINT from {0, 0};
		BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		UpdateLayeredWindow(window, screen, &origin, &extent, memory, &from, 0, &blend, ULW_ALPHA);
		SelectObject(memory, previous);
		DeleteDC(memory);
		DeleteObject(bitmap);
		ReleaseDC(nullptr, screen);
		ShowWindow(window, SW_SHOWNOACTIVATE);
		return window;
	}();
	return source;
}

QRect virtualScreen() {
	return {
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN),
	    GetSystemMetrics(SM_CXVIRTUALSCREEN),
	    GetSystemMetrics(SM_CYVIRTUALSCREEN),
	};
}

QRect visibleFrame(HWND hwnd) {
	RECT r {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
		GetWindowRect(hwnd, &r);
	}
	return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool isDialogOfTracked(HWND hwnd) {
	if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return false;
	auto* owner = GetWindow(hwnd, GW_OWNER);
	return owner != nullptr
	    && WindowTracker::instance()->windowFor(GetAncestor(owner, GA_ROOTOWNER)) != nullptr;
}

bool isShown(HWND hwnd) {
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || isOwnProcessWindow(hwnd)) return false;
	if (WindowTracker::instance()->windowFor(hwnd) == nullptr && !isDialogOfTracked(hwnd)) return false;
	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	return cloaked == 0;
}

LRESULT CALLBACK dimProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (msg == WM_NCHITTEST) return HTTRANSPARENT;
	if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}

struct InactiveDim::Scene {
	struct Shade {
		ComPtr<IDCompositionVisual> visual;
		ComPtr<IDCompositionEffectGroup> effect;
		ComPtr<IDCompositionRectangleClip> clip;
		QRect rect;
		QRegion region;
		float radius = -1;
		DimLevel level;

		[[nodiscard]] bool gone() const { return this->level.to == 0 && this->level.now() == 0; }
	};

	~Scene() {
		if (this->host != nullptr) DestroyWindow(this->host);
	}

	bool create();
	bool makeColor();
	bool shape(HWND hwnd, Shade& shade, const DimArea& area);
	bool update(const QList<DimArea>& areas, const std::map<HWND, DimLevel>& levels);
	void clear();
	void place(const QList<HWND>& windows);

	HWND host = nullptr;
	QRect bounds;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDCompositionDesktopDevice> desktopDevice;
	ComPtr<IDCompositionDevice> device;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> root;
	ComPtr<IDCompositionSurface> color;
	std::map<HWND, Shade> shades;
};

bool InactiveDim::Scene::create() {
	auto* module = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.hInstance = module;
	cls.lpfnWndProc = &dimProc;
	cls.lpszClassName = DIM_CLASS;
	RegisterClassW(&cls);

	this->bounds = virtualScreen();
	this->host = CreateWindowExW(
	    WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
	        | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
	    DIM_CLASS,
	    L"",
	    WS_POPUP,
	    this->bounds.x(),
	    this->bounds.y(),
	    this->bounds.width(),
	    this->bounds.height(),
	    nullptr,
	    nullptr,
	    module,
	    nullptr
	);
	if (this->host == nullptr) return false;
	SetLayeredWindowAttributes(this->host, 0, 255, LWA_ALPHA);
	BOOL disable = TRUE;
	DwmSetWindowAttribute(
	    this->host,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &disable,
	    sizeof(disable)
	);

	ComPtr<ID3D11Device> d3d;
	ComPtr<IDXGIDevice> dxgi;
	return SUCCEEDED(D3D11CreateDevice(
	           nullptr,
	           D3D_DRIVER_TYPE_HARDWARE,
	           nullptr,
	           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	           nullptr,
	           0,
	           D3D11_SDK_VERSION,
	           d3d.GetAddressOf(),
	           nullptr,
	           this->context.GetAddressOf()
	       ))
	    && SUCCEEDED(d3d.As(&dxgi))
	    && SUCCEEDED(
	        DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(this->desktopDevice.GetAddressOf()))
	    )
	    && SUCCEEDED(this->desktopDevice.As(&this->device))
	    && SUCCEEDED(
	        this->device->CreateTargetForHwnd(this->host, TRUE, this->target.GetAddressOf())
	    )
	    && SUCCEEDED(this->device->CreateVisual(this->root.GetAddressOf()))
	    && SUCCEEDED(this->target->SetRoot(this->root.Get())) && this->makeColor();
}

bool InactiveDim::Scene::makeColor() {
	if (FAILED(this->device->CreateSurface(
	        1,
	        1,
	        DXGI_FORMAT_B8G8R8A8_UNORM,
	        DXGI_ALPHA_MODE_IGNORE,
	        this->color.GetAddressOf()
	    )))
	{
		return false;
	}
	ComPtr<IDXGISurface> surface;
	POINT offset {};
	if (FAILED(this->color->BeginDraw(nullptr, IID_PPV_ARGS(surface.GetAddressOf()), &offset))) {
		return false;
	}
	ComPtr<ID3D11Texture2D> texture;
	auto copied = SUCCEEDED(surface.As(&texture));
	if (copied) {
		const uint32_t black = 0xff000000;
		const D3D11_BOX box {
		    static_cast<UINT>(offset.x),
		    static_cast<UINT>(offset.y),
		    0,
		    static_cast<UINT>(offset.x + 1),
		    static_cast<UINT>(offset.y + 1),
		    1,
		};
		this->context->UpdateSubresource(texture.Get(), 0, &box, &black, sizeof(black), 0);
	}
	return SUCCEEDED(this->color->EndDraw()) && copied;
}

bool InactiveDim::Scene::shape(HWND hwnd, Shade& shade, const DimArea& area) {
	if (shade.visual == nullptr) {
		if (FAILED(this->device->CreateVisual(shade.visual.GetAddressOf()))
		    || FAILED(this->device->CreateEffectGroup(shade.effect.GetAddressOf()))
		    || FAILED(this->device->CreateRectangleClip(shade.clip.GetAddressOf()))
		    || FAILED(shade.effect->SetOpacity(0.0f))
		    || FAILED(shade.visual->SetEffect(shade.effect.Get()))
		    || FAILED(shade.visual->SetClip(shade.clip.Get()))
		    || FAILED(this->root->AddVisual(shade.visual.Get(), FALSE, nullptr)))
		{
			return false;
		}
	}

	auto radius = DwindleScene::cornerRadius(hwnd);
	if (shade.rect == area.rect && shade.region == area.region && shade.radius == radius) return true;
	shade.rect = area.rect;
	shade.region = area.region;
	shade.radius = radius;

	if (FAILED(shade.visual->RemoveAllVisuals())
	    || FAILED(shade.visual->SetOffsetX(static_cast<float>(area.rect.x() - this->bounds.x())))
	    || FAILED(shade.visual->SetOffsetY(static_cast<float>(area.rect.y() - this->bounds.y())))
	    || FAILED(shade.clip->SetLeft(0.0f)) || FAILED(shade.clip->SetTop(0.0f))
	    || FAILED(shade.clip->SetRight(static_cast<float>(area.rect.width())))
	    || FAILED(shade.clip->SetBottom(static_cast<float>(area.rect.height())))
	    || FAILED(shade.clip->SetTopLeftRadiusX(radius))
	    || FAILED(shade.clip->SetTopLeftRadiusY(radius))
	    || FAILED(shade.clip->SetTopRightRadiusX(radius))
	    || FAILED(shade.clip->SetTopRightRadiusY(radius))
	    || FAILED(shade.clip->SetBottomLeftRadiusX(radius))
	    || FAILED(shade.clip->SetBottomLeftRadiusY(radius))
	    || FAILED(shade.clip->SetBottomRightRadiusX(radius))
	    || FAILED(shade.clip->SetBottomRightRadiusY(radius)))
	{
		return false;
	}

	for (const auto& part: area.region) {
		ComPtr<IDCompositionVisual> piece;
		D2D_MATRIX_3X2_F stretch {};
		stretch._11 = static_cast<float>(part.width());
		stretch._22 = static_cast<float>(part.height());
		if (FAILED(this->device->CreateVisual(piece.GetAddressOf()))
		    || FAILED(piece->SetContent(this->color.Get())) || FAILED(piece->SetTransform(stretch))
		    || FAILED(piece->SetOffsetX(static_cast<float>(part.x() - area.rect.x())))
		    || FAILED(piece->SetOffsetY(static_cast<float>(part.y() - area.rect.y())))
		    || FAILED(shade.visual->AddVisual(piece.Get(), FALSE, nullptr)))
		{
			return false;
		}
	}
	return true;
}

bool InactiveDim::Scene::update(
    const QList<DimArea>& areas,
    const std::map<HWND, DimLevel>& levels
) {
	auto levelOf = [&levels](HWND hwnd) {
		auto found = levels.find(hwnd);
		return found == levels.end() ? DimLevel() : found->second;
	};
	for (auto it = this->shades.begin(); it != this->shades.end();) {
		auto listed = std::ranges::any_of(areas, [hwnd = it->first](const auto& area) {
			return area.id == reinterpret_cast<quintptr>(hwnd);
		});
		if (listed && !(it->second.level == levelOf(it->first) && it->second.gone())) {
			++it;
			continue;
		}
		this->root->RemoveVisual(it->second.visual.Get());
		it = this->shades.erase(it);
	}

	for (const auto& area: areas) {
		auto* hwnd = reinterpret_cast<HWND>(area.id); // NOLINT(performance-no-int-to-ptr)
		auto level = levelOf(hwnd);
		auto found = this->shades.find(hwnd);
		if (found == this->shades.end()) {
			if (level.to == 0 && level.now() == 0) continue;
			found = this->shades.try_emplace(hwnd).first;
		}
		auto& shade = found->second;
		if (!this->shape(hwnd, shade, area)) return false;
		if (shade.level == level) continue;
		if (!InactiveDim::applyLevel(this->device.Get(), shade.effect.Get(), level)) return false;
		shade.level = level;
	}
	return SUCCEEDED(this->device->Commit());
}

void InactiveDim::Scene::clear() {
	if (this->shades.empty()) return;
	this->root->RemoveAllVisuals();
	this->shades.clear();
	this->device->Commit();
}

void InactiveDim::Scene::place(const QList<HWND>& windows) {
	const auto panels = WinPanelWindow::aboveWindowHandles();
	HWND lowest = nullptr;
	auto overPanel = false;
	auto underWindow = false;
	auto passed = false;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (hwnd == this->host) {
			passed = true;
		} else if (panels.contains(hwnd) && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
			lowest = hwnd;
			overPanel = passed;
			underWindow = false;
		} else if (!passed && windows.contains(hwnd)) {
			underWindow = true;
		}
	}
	if (IsWindowVisible(this->host) && !overPanel && !underWindow) return;
	SetWindowPos(
	    this->host,
	    lowest == nullptr ? HWND_TOPMOST : lowest,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
	);
}

InactiveDim* InactiveDim::instance() {
	static QPointer<InactiveDim> dim; // NOLINT
	if (dim.isNull()) dim = new InactiveDim(QCoreApplication::instance());
	return dim.data();
}

InactiveDim::InactiveDim(QObject* parent): QObject(parent) {
	this->timer.setSingleShot(true);
	this->timer.setTimerType(Qt::PreciseTimer);
	QObject::connect(&this->timer, &QTimer::timeout, this, &InactiveDim::refresh);
}

InactiveDim::~InactiveDim() = default;

void InactiveDim::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	if (enabled) {
		this->watch();
		this->schedule(REFRESH_MS);
	} else {
		this->timer.stop();
		this->scene.reset();
		this->levels.clear();
	}
	emit this->enabledChanged();
}

void InactiveDim::watch() {
	if (this->watching) return;
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr) return;
	this->watching = true;

	auto soon = [this] { this->schedule(REFRESH_MS); };
	QObject::connect(tracker, &WindowTracker::flushed, this, soon);
	QObject::connect(tracker, &WindowTracker::foregroundChanged, this, soon);
	QObject::connect(tracker, &WindowTracker::activeWindowChanged, this, soon);
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::currentChanged, this, soon);
	QObject::connect(Scratchpad::instance(), &Scratchpad::shownChanged, this, soon);
}

void InactiveDim::schedule(int delay) {
	if (!this->mEnabled) return;
	if (this->timer.isActive() && this->timer.remainingTime() <= delay) return;
	this->timer.start(delay);
}

float DimLevel::now() const {
	auto progress = static_cast<double>(nowMs() - this->start) / (FADE_SECONDS * 1000);
	return this->from + (this->to - this->from) * static_cast<float>(dimCurve(progress));
}

DimLevel DimLevel::toward(float goal) const {
	if (this->to == goal) return *this;
	return {.from = this->now(), .to = goal, .start = nowMs()};
}

DimLevel InactiveDim::levelOf(HWND hwnd) {
	auto* dim = InactiveDim::instance();
	if (!dim->mEnabled) return {};
	auto found = dim->levels.find(hwnd);
	return found == dim->levels.end() ? DimLevel() : found->second;
}

bool InactiveDim::applyLevel(
    IDCompositionDevice* device,
    IDCompositionEffectGroup* effect,
    const DimLevel& level
) {
	auto begin = static_cast<double>(nowMs() - level.start) / (FADE_SECONDS * 1000);
	if (begin >= 1 || level.from == level.to) return SUCCEEDED(effect->SetOpacity(level.to));

	ComPtr<IDCompositionAnimation> animation;
	if (FAILED(device->CreateAnimation(animation.GetAddressOf()))) return false;
	auto span = level.to - level.from;
	auto length = 1.0 - begin;
	for (auto i = 0; i < FADE_STEPS; i++) {
		auto at = begin + length * i / FADE_STEPS;
		auto next = begin + length * (i + 1) / FADE_STEPS;
		auto value = level.from + span * static_cast<float>(dimCurve(at));
		auto after = level.from + span * static_cast<float>(dimCurve(next));
		auto slope = (after - value) / static_cast<float>((next - at) * FADE_SECONDS);
		if (FAILED(animation->AddCubic((at - begin) * FADE_SECONDS, value, slope, 0, 0))) return false;
	}
	return SUCCEEDED(animation->End(length * FADE_SECONDS, level.to))
	    && SUCCEEDED(effect->SetOpacity(animation.Get()));
}

HTHUMBNAIL InactiveDim::shadeThumbnail(HWND destination, float level) {
	auto opacity = std::clamp(qRound(level * 255), 0, 255);
	auto* source = shadeSource();
	HTHUMBNAIL thumbnail = nullptr;
	if (source == nullptr || FAILED(DwmRegisterThumbnail(destination, source, &thumbnail))) return nullptr;
	DWM_THUMBNAIL_PROPERTIES props {};
	props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
	props.rcSource = {4, 4, 12, 12};
	props.opacity = static_cast<BYTE>(opacity);
	props.fVisible = FALSE;
	DwmUpdateThumbnailProperties(thumbnail, &props);
	return thumbnail;
}

void InactiveDim::stepAside() {
	if (this->scene != nullptr && IsWindowVisible(this->scene->host)) ShowWindow(this->scene->host, SW_HIDE);
}

void InactiveDim::comeBack() {
	if (this->mEnabled && this->watching) this->refresh();
}

void InactiveDim::updateLevels(const QList<HWND>& shown) {
	std::erase_if(this->levels, [](const auto& entry) { return !IsWindow(entry.first); });
	if (!shown.contains(this->focused)) return;
	for (auto* hwnd: shown) {
		auto goal = hwnd == this->focused ? 0.0f : STRENGTH;
		auto [entry, added] = this->levels.try_emplace(hwnd, DimLevel {.from = goal, .to = goal});
		if (!added) entry->second = entry->second.toward(goal);
	}
}

void InactiveDim::hide() {
	if (this->scene == nullptr) return;
	if (IsWindowVisible(this->scene->host)) ShowWindow(this->scene->host, SW_HIDE);
	this->scene->clear();
}

void InactiveDim::refresh() {
	if (!this->mEnabled) return;
	this->watch();
	if (!this->watching) {
		this->schedule(FADE_STEPS * BUSY_MS);
		return;
	}
	const auto screen = virtualScreen();
	QList<DimWindow> windows;
	QList<HWND> handles;
	QList<HWND> shown;
	auto* foreground = GetForegroundWindow();
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (!isShown(hwnd)) continue;
		shown.append(hwnd);
		if (hwnd == foreground) this->focused = hwnd;
		auto rect = visibleFrame(hwnd).intersected(screen);
		if (rect.isEmpty()) continue;
		windows.append({.id = reinterpret_cast<quintptr>(hwnd), .rect = rect});
		handles.append(hwnd);
	}
	this->updateLevels(shown);

	if (DesktopSlide::instance()->busy()) {
		this->stepAside();
		this->schedule(BUSY_MS);
		return;
	}

	auto areas = planDim(windows, reinterpret_cast<quintptr>(this->focused));
	auto dims = std::ranges::any_of(handles, [](HWND hwnd) {
		auto level = InactiveDim::levelOf(hwnd);
		return level.to != 0 || level.now() != 0;
	});
	if (!dims) {
		this->hide();
		return;
	}

	if (this->scene != nullptr && this->scene->bounds != screen) this->scene.reset();
	if (this->scene == nullptr) {
		this->scene = std::make_unique<Scene>();
		if (!this->scene->create()) {
			qCWarning(logDim) << "Could not create the dim overlay" << GetLastError();
			this->scene.reset();
			return;
		}
	}
	if (!this->scene->update(areas, this->levels)) {
		qCWarning(logDim) << "Could not update the dim overlay";
		this->scene.reset();
		return;
	}
	const auto animating = DwindleScene::animating() || Scratchpad::instance()->moving();
	if (animating) this->schedule(REFRESH_MS);
	else this->scene->place(handles);
	if (!animating) qCDebug(logDim) << "Dimming" << this->scene->shades.size() << "of" << handles.length()
	                << "windows, focused" << this->focused;
	const auto fading = std::ranges::any_of(handles, [](HWND hwnd) {
		auto level = InactiveDim::levelOf(hwnd);
		return level.to == 0 && level.now() != 0;
	});
	if (fading) this->schedule(static_cast<int>(FADE_SECONDS * 1000) + REFRESH_MS);
}

}
