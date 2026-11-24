#include "scratchpad.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include <dwmapi.h>
#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qrect.h>
#include <qscreen.h>
#include <qtypes.h>

#include "desktop_slide.hpp"
#include "dwindle_scene.hpp"
#include "layer_order.hpp"
#include "panel_window.hpp"
#include "tiling.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logScratchpad, "quickshell.windows.scratchpad", QtWarningMsg);

constexpr auto* SCRATCHPAD_PROP = L"QuickshellScratchpadRect";
constexpr std::size_t CRASH_SLOTS = 32;
constexpr int DIM_ALPHA = 51;
constexpr int DIM_STEP = 18;
constexpr int DIM_FRAME_MS = 16;
constexpr ULONGLONG FOCUS_GRACE_MS = 400;
constexpr double SLIDE_IN_MS = 280;
constexpr double SLIDE_OUT_MS = 120;
constexpr int LANDING_TRIES = 20;
constexpr int ORPHAN_RETRY_MS = 250;
constexpr int ORPHAN_RETRIES = 40;
constexpr int LANDING_STEP_MS = 8;
constexpr auto* COVER_CLASS = L"QuickshellScratchpadCover";
constexpr COLORREF COVER_KEY = RGB(255, 0, 255);

double bezierAt(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double curve(double x1, double y1, double x2, double y2, double x) {
	auto lo = 0.0;
	auto hi = 1.0;
	for (auto i = 0; i < 40; i++) {
		auto mid = (lo + hi) / 2;
		if (bezierAt(x1, x2, mid) < x) lo = mid;
		else hi = mid;
	}
	return bezierAt(y1, y2, (lo + hi) / 2);
}
constexpr auto* DIM_CLASS = L"QuickshellScratchpadDim";
constexpr int ORDER_VERIFY_MS = 60;
constexpr int ORDER_RETRIES = 3;

struct CrashSlot {
	std::atomic<HWND> hwnd {nullptr};
	std::atomic<quint64> rect {0};
};
std::array<CrashSlot, CRASH_SLOTS> gCrashSlots {}; // NOLINT

quint64 packRect(const QRect& rect) {
	auto part = [](int value) { return static_cast<quint64>(static_cast<quint16>(value)); };
	auto packed = (part(rect.x()) << 48) | (part(rect.y()) << 32) | (part(rect.width()) << 16)
	            | part(rect.height());
	return packed == 0 ? 1 : packed;
}

QRect unpackRect(quint64 packed) {
	auto part = [packed](int shift) { return static_cast<qint16>((packed >> shift) & 0xffff); };
	return QRect(part(48), part(32), static_cast<quint16>(part(16)), static_cast<quint16>(part(0)));
}

void remember(HWND hwnd, const QRect& rect) {
	auto packed = packRect(rect);
	SetPropW(hwnd, SCRATCHPAD_PROP, reinterpret_cast<HANDLE>(static_cast<quintptr>(packed)));

	for (auto& slot: gCrashSlots) {
		if (slot.hwnd.load() == hwnd) {
			slot.rect.store(packed);
			return;
		}
	}
	for (auto& slot: gCrashSlots) {
		HWND empty = nullptr;
		if (slot.hwnd.compare_exchange_strong(empty, hwnd)) {
			slot.rect.store(packed);
			return;
		}
	}
}

void forget(HWND hwnd) {
	RemovePropW(hwnd, SCRATCHPAD_PROP);
	for (auto& slot: gCrashSlots) {
		auto* current = hwnd;
		slot.hwnd.compare_exchange_strong(current, nullptr);
	}
}

void placeAt(HWND hwnd, HWND insertAfter, const QRect& rect) {
	SetWindowPos(
	    hwnd,
	    insertAfter,
	    rect.x(),
	    rect.y(),
	    rect.width(),
	    rect.height(),
	    SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

void release(HWND hwnd, const QRect& rect) {
	if (IsWindow(hwnd)) {
		if (!rect.isEmpty()) placeAt(hwnd, HWND_NOTOPMOST, rect);
		VirtualDesktops::instance()->pinWindow(hwnd, false);
	}
	forget(hwnd);
}

QPoint parkPosition() {
	return QPoint(
	    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 100,
	    GetSystemMetrics(SM_YVIRTUALSCREEN)
	);
}

bool isParked(const RECT& rect) { return rect.left >= parkPosition().x() - 10; }

QRect visibleRect(HWND hwnd) {
	if (IsZoomed(hwnd)) {
		WINDOWPLACEMENT placement {};
		placement.length = sizeof(placement);
		if (GetWindowPlacement(hwnd, &placement)) {
			const auto& r = placement.rcNormalPosition;
			return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
		}
	}

	RECT r {};
	if (!GetWindowRect(hwnd, &r) || isParked(r)) return QRect();
	return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
}

struct OrphanScan {
	QList<HWND> held;
	bool pending = false;
};

bool recoverOrphans(const QList<HWND>& held = {}) {
	OrphanScan scan {held, false};
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto packed = reinterpret_cast<quintptr>(GetPropW(hwnd, SCRATCHPAD_PROP));
		    if (packed == 0) return TRUE;
		    auto* scan = reinterpret_cast<OrphanScan*>(param);
		    if (scan->held.contains(hwnd)) return TRUE;
		    auto* desktops = VirtualDesktops::instance();
		    if (!desktops->accessorLoaded() || !desktops->pinWindow(hwnd, false)) {
			    scan->pending = true;
			    return TRUE;
		    }
		    auto rect = unpackRect(packed);
		    if (!rect.isEmpty()) placeAt(hwnd, HWND_NOTOPMOST, rect);
		    RemovePropW(hwnd, SCRATCHPAD_PROP);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&scan)
	);
	return scan.pending;
}

}

namespace {
QPointer<Scratchpad> SCRATCHPAD; // NOLINT
}

Scratchpad* Scratchpad::instance() {
	if (SCRATCHPAD.isNull()) SCRATCHPAD = new Scratchpad(QCoreApplication::instance());
	return SCRATCHPAD.data();
}

void Scratchpad::raiseWindow(HWND /*hwnd*/) { Scratchpad::instance()->order(); }

bool Scratchpad::holds(const TrackedWindow* window) {
	return !SCRATCHPAD.isNull() && SCRATCHPAD->contains(window);
}

Scratchpad::~Scratchpad() { this->closeCover(); }

Scratchpad::Scratchpad(QObject* parent): QObject(parent) {
	if (recoverOrphans()) {
		auto* retry = new QTimer(this);
		retry->setInterval(ORPHAN_RETRY_MS);
		QObject::connect(retry, &QTimer::timeout, this, [this, retry, tries = 0]() mutable {
			QList<HWND> held;
			for (const auto& entry: this->entries) held.append(entry.hwnd);
			if (!recoverOrphans(held) || ++tries >= ORPHAN_RETRIES) retry->deleteLater();
		});
		retry->start();
	}
	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, &Scratchpad::releaseAll);
	}

	this->dimTimer.setInterval(DIM_FRAME_MS);
	QObject::connect(&this->dimTimer, &QTimer::timeout, this, &Scratchpad::stepDim);
	this->orderTimer.setSingleShot(true);
	this->orderTimer.setInterval(ORDER_VERIFY_MS);
	QObject::connect(&this->orderTimer, &QTimer::timeout, this, &Scratchpad::order);
	QObject::connect(
	    WindowTracker::instance(),
	    &WindowTracker::foregroundChanged,
	    this,
	    &Scratchpad::onForeground
	);

	auto* tracker = WindowTracker::instance();
	QObject::connect(tracker, &WindowTracker::windowAdded, this, &Scratchpad::onWindowAdded);
	QObject::connect(tracker, &WindowTracker::windowRemoved, this, &Scratchpad::onWindowRemoved);
	QObject::connect(
	    tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &Scratchpad::onActiveWindowChanged
	);
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::currentChanged, this, [this]() {
		auto* desktops = VirtualDesktops::instance();
		if (this->mShown && desktops->currentIndex() != this->openedDesktop) this->hideNow();
	});
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::shownChanged, this, [this]() {
		if (this->mShown && VirtualDesktops::instance()->shownIndex() != this->openedDesktop)
			this->hideNow();
	});
}

void Scratchpad::hideNow() {
	if (!this->mShown) return;
	this->preparing = false;
	this->slideGeneration++;
	this->dimTimer.stop();
	this->dimAlpha = 0;
	this->dimTarget = 0;
	if (this->dim != nullptr) ShowWindow(this->dim, SW_HIDE);
	this->finishHide();
}

bool Scratchpad::contains(const TrackedWindow* window) const {
	if (window == nullptr) return false;
	for (const auto& entry: this->entries) {
		if (entry.window == window) return true;
	}
	return false;
}

bool Scratchpad::containsWindow(HWND hwnd) const {
	return std::ranges::any_of(this->entries, [hwnd](const Entry& entry) { return entry.hwnd == hwnd; });
}

void Scratchpad::toggle() {
	this->prune();
	this->setShown(!this->mShown || this->hiding);
}

void Scratchpad::add(TrackedWindow* window, bool follow) {
	this->prune();
	if (window == nullptr) return;

	if (!this->contains(window)) {
		auto* hwnd = window->hwnd();
		if (!VirtualDesktops::instance()->pinWindow(hwnd, true)) {
			qCWarning(logScratchpad) << "Couldn't pin" << window << "to all desktops";
		}
		this->entries.append(Entry {.window = window, .hwnd = hwnd, .rect = visibleRect(hwnd)});
		auto& entry = this->entries.last();
		remember(hwnd, entry.rect);
		emit this->membershipChanged();

		if (this->mShown) {
			this->relayout();
			forceForegroundWindow(hwnd);
		} else if (!follow) {
			this->park(entry);
			if (auto* tiling = TilingManager::active()) tiling->markWindow(window);
		}
	}

	if (follow) this->setShown(true);
}

void Scratchpad::remove(TrackedWindow* window) {
	for (auto i = 0; i < this->entries.size(); i++) {
		auto& entry = this->entries[i];
		if (entry.window != window) continue;

		if (!entry.rect.isEmpty()) placeAt(entry.hwnd, HWND_NOTOPMOST, entry.rect);
		VirtualDesktops::instance()->pinWindow(entry.hwnd, false);
		forget(entry.hwnd);
		this->entries.removeAt(i);
		emit this->membershipChanged();
		if (auto* tiling = TilingManager::active()) tiling->markWindow(window);
		return;
	}
}

void Scratchpad::setShown(bool shown) {
	if (shown && this->hiding) {
		this->hiding = false;
		this->openedDesktop = VirtualDesktops::instance()->shownIndex();
		this->shownAt = GetTickCount64();
		auto* focus = this->focusCandidate();
		this->showDim();
		this->slide(true);
		if (focus != nullptr) forceForegroundWindow(focus);
		return;
	}
	if (shown == this->mShown || (!shown && this->hiding)) return;

	if (shown) {
		POINT cursor {};
		GetCursorPos(&cursor);
		this->mMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
		this->openedDesktop = VirtualDesktops::instance()->shownIndex();
		this->shownAt = GetTickCount64();
		this->mShown = true;
		this->preparing = true;
		auto* desktops = VirtualDesktops::instance();
		for (const auto& entry: this->entries) {
			if (IsWindow(entry.hwnd) && !desktops->isWindowPinned(entry.hwnd))
				desktops->pinWindow(entry.hwnd, true);
		}
		this->slideOffset = this->slideDistance();
		this->moveReal(this->slideOffset);
		this->relayout();
		this->waitForTakeoff(++this->slideGeneration, 0);
		emit this->shownChanged();
	} else {
		if (this->preparing) {
			this->preparing = false;
			this->slideGeneration++;
			this->finishHide();
			return;
		}
		this->hideDim();
		if (TilingManager::active() == nullptr || this->entries.isEmpty()) {
			this->finishHide();
			return;
		}
		this->hiding = true;
		this->slide(false);
	}
}

void Scratchpad::waitForTakeoff(int generation, int tries) {
	if (generation != this->slideGeneration || !this->preparing || !this->mShown) return;
	auto* tiling = TilingManager::active();
	auto frames = tiling != nullptr ? tiling->specialFrames() : QHash<HWND, QRect>();
	auto ready = tiling != nullptr;
	for (const auto& entry: this->entries) {
		RECT bounds {};
		if (!frames.contains(entry.hwnd)
		    || FAILED(
		        DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))
		    )
		    || bounds.left != frames.value(entry.hwnd).left()
		    || bounds.top != frames.value(entry.hwnd).top() + this->slideDistance())
		{
			ready = false;
			break;
		}
	}
	if (!ready && tries < LANDING_TRIES) {
		QTimer::singleShot(LANDING_STEP_MS, this, [this, generation, tries]() {
			this->waitForTakeoff(generation, tries + 1);
		});
		return;
	}
	this->preparing = false;
	auto* focus = this->focusCandidate();
	this->showDim();
	this->slide(true);
	if (focus != nullptr) forceForegroundWindow(focus);
}

QList<HWND> Scratchpad::layerChain() const {
	QList<HWND> windows;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (this->containsWindow(hwnd) && IsWindowVisible(hwnd) && !IsIconic(hwnd)) windows.append(hwnd);
	}

	auto* first = GetForegroundWindow();
	if (auto* tiling = TilingManager::active()) {
		if (auto* fullscreen = tiling->specialFullscreenWindow()) first = fullscreen->hwnd();
		else if (auto* maximized = tiling->specialMaximizedWindow()) first = maximized->hwnd();
	}
	if (windows.removeOne(first)) windows.prepend(first);

	auto* host = DwindleScene::specialHost();
	if (host != nullptr) {
		auto held = DwindleScene::holdsSpecial() && !windows.isEmpty();
		windows.insert(held ? 1 : 0, host);
	}
	if (this->cover != nullptr) windows.prepend(this->cover);
	if (this->dim != nullptr && IsWindowVisible(this->dim)) windows.append(this->dim);
	return windows;
}

void Scratchpad::order() {
	if (!this->mShown || this->preparing) {
		this->orderRetries = 0;
		return;
	}
	if (!layers::apply(this->layerChain())) {
		this->orderRetries = 0;
		return;
	}
	if (this->orderRetries++ < ORDER_RETRIES) this->orderTimer.start();
	else this->orderRetries = 0;
}

void Scratchpad::focusBelow() {
	if (!this->containsWindow(GetForegroundWindow())) return;
	auto* tracker = WindowTracker::instance();
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (this->containsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
		if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) continue;
		if (tracker->windowFor(hwnd) == nullptr) continue;
		DWORD cloaked = 0;
		DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
		if (cloaked != 0) continue;
		forceForegroundWindow(hwnd);
		return;
	}
	forceForegroundWindow(GetShellWindow());
}

void Scratchpad::onForeground(HWND hwnd) {
	if (!this->mShown || this->hiding) return;
	if (this->containsWindow(hwnd)) {
		this->order();
		return;
	}
	if (isOwnProcessWindow(hwnd) || WindowTracker::instance()->windowFor(hwnd) != nullptr) return;
	auto captioned = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
	auto topmost = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	if (!captioned || topmost || IsZoomed(hwnd) || !IsWindowVisible(hwnd)) return;

	this->lifted.insert(hwnd);
	constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
	this->orderTimer.start();
}

void Scratchpad::dropLifted() {
	constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
	for (auto* hwnd: std::as_const(this->lifted)) {
		if (IsWindow(hwnd)) SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
	}
	this->lifted.clear();
}

int Scratchpad::slideDistance() const {
	auto rects = monitorRects(this->mMonitor);
	return rects.valid ? rects.monitor.height() : 0;
}

void Scratchpad::moveReal(int offset) {
	this->realOffset = offset;
	if (auto* tiling = TilingManager::active()) tiling->setSpecialOffset(offset);
}

void Scratchpad::jumpReal(int offset) {
	auto delta = offset - this->realOffset;
	if (delta != 0) {
		for (const auto& entry: this->entries) {
			RECT rect {};
			if (!IsWindow(entry.hwnd) || IsIconic(entry.hwnd) || !GetWindowRect(entry.hwnd, &rect))
				continue;
			SetWindowPos(
			    entry.hwnd,
			    nullptr,
			    rect.left,
			    rect.top + delta,
			    0,
			    0,
			    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER
			);
		}
	}
	this->moveReal(offset);
}

void Scratchpad::slide(bool in) {
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) return;
	this->slideGeneration++;
	if (in) DwindleScene::drop(this->mMonitor);

	if (this->cover == nullptr) {
		if (in) {
			this->moveReal(this->slideDistance());
			this->openCover(true);
		} else {
			this->openCover(false);
			if (this->cover != nullptr) ShowWindow(this->cover, SW_SHOWNOACTIVATE);
			this->jumpReal(this->slideDistance());
			DwmFlush();
		}
	}
	if (this->realOffset != this->slideDistance()) this->moveReal(this->slideDistance());

	{
		auto lock = std::lock_guard(this->slideMutex);
		this->slideIn = in;
		this->slideFrom = this->slideOffset;
		this->slideTo = in ? 0 : this->slideDistance();
		this->slideSpan = this->slideDistance();
		this->slideStart = std::chrono::steady_clock::now();
		this->slideDoneSent = false;
	}
	if (!this->slideWorker.joinable()) {
		this->slideStop.store(false);
		this->slideWorker = std::thread(&Scratchpad::runSlide, this);
	}
}

void Scratchpad::openCover(bool show) {
	auto rects = monitorRects(this->mMonitor);
	if (!rects.valid) return;
	auto* tiling = TilingManager::active();
	auto frames = tiling != nullptr ? tiling->specialFrames() : QHash<HWND, QRect>();
	auto* maximized = tiling != nullptr ? tiling->specialMaximizedWindow() : nullptr;

	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &DefWindowProcW;
		cls.hInstance = instance;
		cls.hbrBackground = CreateSolidBrush(COVER_KEY);
		cls.lpszClassName = COVER_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	auto monitor = rects.monitor.adjusted(0, 1, 0, 0);
	this->cover = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
	    COVER_CLASS,
	    L"",
	    WS_POPUP,
	    monitor.x(),
	    monitor.y(),
	    monitor.width(),
	    monitor.height(),
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (this->cover == nullptr) return;
	this->coverOrigin = monitor.topLeft();
	SetLayeredWindowAttributes(this->cover, COVER_KEY, 255, LWA_COLORKEY);
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(
	    this->cover,
	    DWMWA_TRANSITIONS_FORCEDISABLED,
	    &noTransitions,
	    sizeof(noTransitions)
	);

	QList<HWND> stack;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		if (this->containsWindow(hwnd) && IsWindowVisible(hwnd) && !IsIconic(hwnd)) stack.prepend(hwnd);
	}
	QList<CoverThumb> thumbs;
	for (auto* hwnd: stack) {
		if (maximized != nullptr && hwnd != maximized->hwnd() && frames.contains(hwnd)) continue;
		QRect frame;
		if (!show && maximized != nullptr && hwnd == maximized->hwnd())
			frame = DwindleScene::frameForDrag(hwnd);
		if (frame.isEmpty()) frame = frames.value(hwnd);
		if (frame.isEmpty()) {
			RECT bounds {};
			if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
				continue;
			frame = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
			            .translated(0, -this->realOffset);
		}
		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->cover, hwnd, &thumbnail))) continue;
		thumbs.append({hwnd, thumbnail, frame});
	}
	{
		auto lock = std::lock_guard(this->slideMutex);
		this->coverThumbs = thumbs;
		this->layoutCover(this->slideOffset);
	}
	this->placeCover();
	if (!show) return;
	ShowWindow(this->cover, SW_SHOWNOACTIVATE);
	DwmFlush();
}

void Scratchpad::placeCover() {
	if (this->cover != nullptr) layers::apply(this->layerChain());
}

void Scratchpad::layoutCover(double offset) {
	auto shift = static_cast<int>(std::lround(offset));
	for (const auto& thumb: this->coverThumbs) {
		RECT window {};
		RECT bounds {};
		if (!GetWindowRect(thumb.hwnd, &window)
		    || FAILED(
		        DwmGetWindowAttribute(thumb.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))
		    ))
		{
			continue;
		}
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.rcSource = {
		    bounds.left - window.left,
		    bounds.top - window.top,
		    bounds.right - window.left,
		    bounds.bottom - window.top
		};
		auto dest = thumb.frame.translated(-this->coverOrigin).translated(0, shift);
		props.rcDestination =
		    {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
		props.opacity = 255;
		props.fVisible = TRUE;
		props.fSourceClientAreaOnly = FALSE;
		DwmUpdateThumbnailProperties(thumb.thumbnail, &props);
	}
}

void Scratchpad::runSlide() {
	auto last = std::chrono::steady_clock::now();
	auto period = compositorRefreshMs();
	while (!this->slideStop.load()) {
		do {
			waitForCompositorFrame();
		} while (!this->slideStop.load()
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last)
		                    .count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();

		auto lock = std::lock_guard(this->slideMutex);
		auto elapsed = std::chrono::duration<double, std::milli>(last - this->slideStart).count();
		auto progress = std::clamp(elapsed / (this->slideIn ? SLIDE_IN_MS : SLIDE_OUT_MS), 0.0, 1.0);
		auto eased =
		    this->slideIn ? curve(0.05, 0.7, 0.1, 1.0, progress) : curve(0.3, 0.0, 0.8, 0.15, progress);
		this->slideOffset =
		    static_cast<int>(std::lround(this->slideFrom + (this->slideTo - this->slideFrom) * eased));
		this->layoutCover(this->slideOffset);
		if (this->dim != nullptr && this->slideSpan > 0) {
			auto shown =
			    std::clamp(1.0 - static_cast<double>(this->slideOffset) / this->slideSpan, 0.0, 1.0);
			this->dimAlpha = static_cast<int>(std::lround(DIM_ALPHA * shown));
			SetLayeredWindowAttributes(this->dim, 0, static_cast<BYTE>(this->dimAlpha), LWA_ALPHA);
		}

		if (progress >= 1.0 && !this->slideDoneSent) {
			this->slideDoneSent = true;
			QMetaObject::invokeMethod(this, &Scratchpad::onSlideDone, Qt::QueuedConnection);
		}
	}
}

void Scratchpad::onSlideDone() {
	bool in = false;
	{
		auto lock = std::lock_guard(this->slideMutex);
		if (!this->slideDoneSent) return;
		in = this->slideIn;
	}
	if (!in) {
		this->closeCover();
		if (this->dim != nullptr && this->dimTarget == 0) {
			this->dimAlpha = 0;
			ShowWindow(this->dim, SW_HIDE);
		}
		if (this->hiding) this->finishHide();
		return;
	}
	this->slideStop.store(true);
	if (this->slideWorker.joinable()) this->slideWorker.join();
	{
		auto lock = std::lock_guard(this->slideMutex);
		for (const auto& thumb: this->coverThumbs) {
			DWM_THUMBNAIL_PROPERTIES props {};
			props.dwFlags = DWM_TNP_VISIBLE;
			props.fVisible = FALSE;
			DwmUpdateThumbnailProperties(thumb.thumbnail, &props);
		}
	}
	this->jumpReal(0);
	this->closeCover();
}

void Scratchpad::closeCover() {
	this->slideStop.store(true);
	if (this->slideWorker.joinable()) this->slideWorker.join();
	auto lock = std::lock_guard(this->slideMutex);
	for (const auto& thumb: this->coverThumbs) DwmUnregisterThumbnail(thumb.thumbnail);
	this->coverThumbs.clear();
	this->slideDoneSent = false;
	if (this->cover != nullptr) {
		DestroyWindow(this->cover);
		this->cover = nullptr;
	}
}

void Scratchpad::finishHide() {
	this->hiding = false;
	this->closeCover();
	this->dimTimer.stop();
	this->dimTarget = 0;
	this->dimAlpha = 0;
	if (this->dim != nullptr) {
		SetLayeredWindowAttributes(this->dim, 0, 0, LWA_ALPHA);
		ShowWindow(this->dim, SW_HIDE);
	}
	this->mShown = false;
	this->openedDesktop = -1;
	this->orderTimer.stop();
	this->dropLifted();
	this->focusBelow();
	for (auto& entry: this->entries) this->park(entry);
	this->relayout();
	this->slideOffset = 0;
	this->moveReal(0);
	emit this->shownChanged();
}

void Scratchpad::park(Entry& entry) {
	if (!IsWindow(entry.hwnd)) return;

	if (IsZoomed(entry.hwnd)) ShowWindowAsync(entry.hwnd, SW_SHOWNOACTIVATE);
	auto position = parkPosition();
	SetWindowPos(
	    entry.hwnd,
	    HWND_NOTOPMOST,
	    position.x(),
	    position.y(),
	    0,
	    0,
	    SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

void Scratchpad::relayout() {
	auto* tiling = TilingManager::active();
	for (auto& entry: this->entries) {
		if (!IsWindow(entry.hwnd)) continue;
		if (this->mShown) {
			if (IsZoomed(entry.hwnd)) ShowWindowAsync(entry.hwnd, SW_SHOWNOACTIVATE);
		}
		if (tiling != nullptr) tiling->markWindow(entry.window);
	}

	if (tiling != nullptr) {
		this->order();
		return;
	}
	if (!this->mShown) return;
	auto work = monitorRects(this->mMonitor).work;
	auto size = QSize(work.width() * 4 / 5, work.height() * 17 / 20);
	auto rect = QRect(work.center() - QPoint(size.width() / 2, size.height() / 2), size);
	for (auto& entry: this->entries) {
		if (!IsWindow(entry.hwnd)) continue;
		placeAt(entry.hwnd, HWND_TOPMOST, rect);
	}
	this->order();
}

HWND Scratchpad::focusCandidate() const {
	if (auto* tiling = TilingManager::active()) {
		if (auto* fullscreen = tiling->specialFullscreenWindow()) return fullscreen->hwnd();
		if (auto* maximized = tiling->specialMaximizedWindow()) return maximized->hwnd();
	}
	return this->entries.isEmpty() ? nullptr : this->entries.last().hwnd;
}

void Scratchpad::showDim() {
	if (this->dim == nullptr) {
		static auto registered = false; // NOLINT
		auto* instance = GetModuleHandleW(nullptr);
		if (!registered) {
			WNDCLASSEXW cls {};
			cls.cbSize = sizeof(cls);
			cls.lpfnWndProc = &Scratchpad::dimProc;
			cls.hInstance = instance;
			cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
			cls.lpszClassName = DIM_CLASS;
			registered = RegisterClassExW(&cls) != 0;
		}
		this->dim = CreateWindowExW(
		    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		    DIM_CLASS,
		    L"",
		    WS_POPUP,
		    0,
		    0,
		    0,
		    0,
		    nullptr,
		    nullptr,
		    instance,
		    nullptr
		);
		if (this->dim == nullptr) {
			qCWarning(logScratchpad) << "Couldn't create the dim window:" << GetLastError();
			return;
		}
		SetLayeredWindowAttributes(this->dim, 0, 0, LWA_ALPHA);
	}

	auto* desktops = VirtualDesktops::instance();
	desktops->moveWindow(this->dim, desktops->currentIndex());
	this->placeDim();
	this->dimTarget = DIM_ALPHA;
	if (this->slideDrivesDim()) this->dimTimer.stop();
	else this->dimTimer.start();
}

void Scratchpad::placeDim() {
	auto work = monitorRects(this->mMonitor).work;
	if (this->dim == nullptr || work.isEmpty()) return;

	SetWindowPos(
	    this->dim,
	    nullptr,
	    work.x(),
	    work.y(),
	    work.width(),
	    work.height(),
	    SWP_NOZORDER | SWP_NOACTIVATE
	);
	auto chain = this->layerChain();
	if (!chain.contains(this->dim)) chain.append(this->dim);
	layers::apply(chain);
	ShowWindow(this->dim, SW_SHOWNOACTIVATE);
	this->order();
}

void Scratchpad::hideDim() {
	if (this->dim == nullptr) return;
	this->dimTarget = 0;
	if (this->slideDrivesDim()) this->dimTimer.stop();
	else this->dimTimer.start();
}

bool Scratchpad::slideDrivesDim() const {
	return TilingManager::active() != nullptr && !this->entries.isEmpty();
}

void Scratchpad::stepDim() {
	if (this->dim == nullptr) {
		this->dimTimer.stop();
		return;
	}

	if (this->dimAlpha < this->dimTarget) {
		this->dimAlpha = qMin(this->dimTarget, this->dimAlpha + DIM_STEP);
	} else {
		this->dimAlpha = qMax(this->dimTarget, this->dimAlpha - DIM_STEP);
	}
	SetLayeredWindowAttributes(this->dim, 0, static_cast<BYTE>(this->dimAlpha), LWA_ALPHA);

	if (this->dimAlpha == this->dimTarget) {
		this->dimTimer.stop();
		if (this->dimAlpha == 0) ShowWindow(this->dim, SW_HIDE);
	}
}

LRESULT CALLBACK Scratchpad::dimProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
		QMetaObject::invokeMethod(
		    Scratchpad::instance(),
		    [] { Scratchpad::instance()->setShown(false); },
		    Qt::QueuedConnection
		);
		return 0;
	default: return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

void Scratchpad::onWindowAdded(TrackedWindow* window) {
	if (this->mShown) this->add(window, false);
}

void Scratchpad::onWindowRemoved(TrackedWindow* window) {
	for (auto i = this->entries.size() - 1; i >= 0; i--) {
		const auto& entry = this->entries[i];
		if (entry.window != window) continue;
		release(entry.hwnd, entry.rect);
		this->entries.removeAt(i);
	}
	this->prune();
	if (this->mShown && this->entries.isEmpty()) {
		this->setShown(false);
		return;
	}
	this->relayout();
	if (!this->mShown) return;

	this->shownAt = GetTickCount64();
	QTimer::singleShot(0, this, [this]() {
		if (!this->mShown || this->entries.isEmpty()) return;
		auto* active = WindowTracker::instance()->activeWindow();
		if (active != nullptr && this->contains(active)) return;
		if (auto* focus = this->focusCandidate()) forceForegroundWindow(focus);
	});
}

void Scratchpad::onActiveWindowChanged() {
	if (!this->mShown || GetTickCount64() - this->shownAt < FOCUS_GRACE_MS) return;
	auto* active = WindowTracker::instance()->activeWindow();
	if (active == nullptr || this->contains(active)) return;
	this->setShown(false);
}

void Scratchpad::releaseAll() {
	this->dimTimer.stop();
	if (this->dim != nullptr) DestroyWindow(this->dim);
	this->dim = nullptr;

	for (auto& entry: this->entries) {
		if (!IsWindow(entry.hwnd)) continue;
		if (!entry.rect.isEmpty()) placeAt(entry.hwnd, HWND_NOTOPMOST, entry.rect);
		VirtualDesktops::instance()->pinWindow(entry.hwnd, false);
		forget(entry.hwnd);
	}
	this->entries.clear();
}

void Scratchpad::prune() {
	for (auto i = this->entries.size() - 1; i >= 0; i--) {
		const auto& entry = this->entries[i];
		if (entry.window != nullptr && IsWindow(entry.hwnd)) continue;
		release(entry.hwnd, entry.rect);
		this->entries.removeAt(i);
	}
}

void Scratchpad::restoreForCrash() {
	for (auto& slot: gCrashSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd)) continue;
		auto rect = unpackRect(slot.rect.load());
		if (!rect.isEmpty()) placeAt(hwnd, HWND_NOTOPMOST, rect);
	}
}

}
