#include "scratchpad.hpp"
#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>

#include <qcoreapplication.h>
#include <qscreen.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qrect.h>
#include <qtypes.h>

#include "desktop_slide.hpp"
#include "tiling.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logScratchpad, "quickshell.windows.scratchpad", QtWarningMsg);

constexpr auto* SCRATCHPAD_PROP = L"QuickshellScratchpadRect";
constexpr std::size_t CRASH_SLOTS = 32;
constexpr int DIM_ALPHA = 51;
constexpr int DIM_STEP = 18;
constexpr int DIM_FRAME_MS = 16;
constexpr ULONGLONG FOCUS_GRACE_MS = 400;
// end-4's specialWorkspaceIn and specialWorkspaceOut: slidevert from below.
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

void raise(HWND hwnd) {
	SetWindowPos(
	    hwnd,
	    HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
	);
}

// The window left special without going through remove(): the tracker lost it
// for a moment, or it was recreated. If it's still alive it must not stay
// pinned to every desktop, or it follows the user around.
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

// Windows a previous shell left in special: killed, crashed or restarted
// without releasing them. The mark only goes once the window is unpinned, or
// it would stay on every desktop with nothing left to tell it apart.
// Returns whether some are still waiting for the desktop accessor.
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

} // namespace

namespace {
QPointer<Scratchpad> SCRATCHPAD; // NOLINT
}

Scratchpad* Scratchpad::instance() {
	if (SCRATCHPAD.isNull()) SCRATCHPAD = new Scratchpad(QCoreApplication::instance());
	return SCRATCHPAD.data();
}

bool Scratchpad::holds(const TrackedWindow* window) {
	return !SCRATCHPAD.isNull() && SCRATCHPAD->contains(window);
}

Scratchpad::~Scratchpad() { this->closeCover(); }

Scratchpad::Scratchpad(QObject* parent)
    : QObject(parent) {
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
		// A switch begun before opening special commits after the desktop slide.
		// Its currentChanged only acknowledges the desktop already shown at open.
		auto* desktops = VirtualDesktops::instance();
		if (this->mShown && desktops->currentIndex() != this->openedDesktop) this->hideNow();
	});
	// The real switch is deferred under the desktop slide; special leaves as
	// soon as the switch is asked for, like Hyprland, not when it commits.
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::shownChanged, this, [this]() {
		if (this->mShown && VirtualDesktops::instance()->shownIndex() != this->openedDesktop) this->hideNow();
	});
}

// A desktop switch covers the screen with its own slide, and special's slide
// out on top of it would leave a ghost of the window behind. It just goes.
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
		// Super+S in the middle of the slide out turns it back.
		this->hiding = false;
		this->openedDesktop = VirtualDesktops::instance()->shownIndex();
		this->showDim();
		this->slide(true);
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
		    || FAILED(DwmGetWindowAttribute(entry.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
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
	for (const auto& entry: this->entries) raise(entry.hwnd);
	this->showDim();
	this->slide(true);
	if (!this->entries.isEmpty()) forceForegroundWindow(this->entries.last().hwnd);
}

int Scratchpad::slideDistance() const {
	auto rects = monitorRects(this->mMonitor);
	return rects.valid ? rects.monitor.height() : 0;
}

// Moving the real windows every frame stutters: each move goes through the
// app and lands whenever it gets to it. Like the desktop slide, the motion is
// live DWM thumbnails on a click-through cover, stepped on the compositor's
// clock off the GUI thread. The real windows wait below the screen and move
// once, under their thumbnails, when the slide in ends.
void Scratchpad::moveReal(int offset) {
	this->realOffset = offset;
	if (auto* tiling = TilingManager::active()) tiling->setSpecialOffset(offset);
}

void Scratchpad::slide(bool in) {
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) return;
	this->slideGeneration++;

	if (this->cover == nullptr) {
		// Showing windows go below the screen first; on hide the cover must be
		// up before they leave.
		if (in) this->moveReal(this->slideDistance());
		this->openCover();
	}
	if (this->realOffset != this->slideDistance()) this->moveReal(this->slideDistance());

	{
		auto lock = std::lock_guard(this->slideMutex);
		this->slideIn = in;
		this->slideFrom = this->slideOffset;
		this->slideTo = in ? 0 : this->slideDistance();
		this->slideStart = std::chrono::steady_clock::now();
		this->slideDoneSent = false;
	}
	if (!this->slideWorker.joinable()) {
		this->slideStop.store(false);
		this->slideWorker = std::thread(&Scratchpad::runSlide, this);
	}
}

void Scratchpad::openCover() {
	auto rects = monitorRects(this->mMonitor);
	if (!rects.valid) return;
	auto* tiling = TilingManager::active();
	auto frames = tiling != nullptr ? tiling->specialFrames() : QHash<HWND, QRect>();

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
	const auto& monitor = rects.monitor;
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
	DwmSetWindowAttribute(this->cover, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));

	// Thumbnails stack in the order they are registered: bottom first.
	QList<HWND> stack;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		for (const auto& entry: this->entries) {
			if (entry.hwnd == hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd)) stack.prepend(hwnd);
		}
	}
	QList<CoverThumb> thumbs;
	for (auto* hwnd: stack) {
		QRect frame;
		if (frames.contains(hwnd)) {
			frame = frames.value(hwnd);
		} else {
			RECT bounds {};
			if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) continue;
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
	ShowWindow(this->cover, SW_SHOWNOACTIVATE);
	DwmFlush();
}

// Right above the topmost special window, so the shell's panels stay on top.
void Scratchpad::placeCover() {
	if (this->cover == nullptr) return;
	HWND top = nullptr;
	for (auto* hwnd = GetTopWindow(nullptr); hwnd != nullptr && top == nullptr; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
		for (const auto& entry: this->entries) {
			if (entry.hwnd == hwnd) top = hwnd;
		}
	}
	auto* above = top != nullptr ? GetWindow(top, GW_HWNDPREV) : nullptr;
	if (above == this->cover) return;
	SetWindowPos(
	    this->cover,
	    above != nullptr ? above : HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
	);
}

// Called with slideMutex held. The source follows the window's own frame, so
// a window still settling into its tile shows as it is.
void Scratchpad::layoutCover(double offset) {
	auto shift = static_cast<int>(std::lround(offset));
	for (const auto& thumb: this->coverThumbs) {
		RECT window {};
		RECT bounds {};
		if (!GetWindowRect(thumb.hwnd, &window)
		    || FAILED(DwmGetWindowAttribute(thumb.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
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
		props.rcDestination = {dest.left(), dest.top(), dest.left() + dest.width(), dest.top() + dest.height()};
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
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();

		auto lock = std::lock_guard(this->slideMutex);
		auto elapsed = std::chrono::duration<double, std::milli>(last - this->slideStart).count();
		auto progress = std::clamp(elapsed / (this->slideIn ? SLIDE_IN_MS : SLIDE_OUT_MS), 0.0, 1.0);
		auto eased = this->slideIn ? curve(0.05, 0.7, 0.1, 1.0, progress) : curve(0.3, 0.0, 0.8, 0.15, progress);
		this->slideOffset = static_cast<int>(std::lround(this->slideFrom + (this->slideTo - this->slideFrom) * eased));
		this->layoutCover(this->slideOffset);

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
		if (this->hiding) this->finishHide();
		return;
	}
	// The windows come up under their thumbnails; the cover leaves once they
	// are there, so nothing jumps.
	this->moveReal(0);
	this->waitForLanding(this->slideGeneration, 0);
}

void Scratchpad::waitForLanding(int generation, int tries) {
	if (generation != this->slideGeneration || this->cover == nullptr) return;
	auto landed = true;
	{
		auto lock = std::lock_guard(this->slideMutex);
		for (const auto& thumb: this->coverThumbs) {
			RECT bounds {};
			if (FAILED(DwmGetWindowAttribute(thumb.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) continue;
			if (bounds.top != thumb.frame.top() || bounds.left != thumb.frame.left()) landed = false;
		}
	}
	if (!landed && tries < LANDING_TRIES) {
		QTimer::singleShot(LANDING_STEP_MS, this, [this, generation, tries]() {
			this->waitForLanding(generation, tries + 1);
		});
		return;
	}
	DwmFlush();
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
	this->mShown = false;
	this->openedDesktop = -1;
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
			if (!this->preparing) raise(entry.hwnd);
		}
		if (tiling != nullptr) tiling->markWindow(entry.window);
	}

	if (tiling != nullptr || !this->mShown) return;
	auto work = monitorRects(this->mMonitor).work;
	auto size = QSize(work.width() * 4 / 5, work.height() * 17 / 20);
	auto rect = QRect(work.center() - QPoint(size.width() / 2, size.height() / 2), size);
	for (auto& entry: this->entries) {
		if (IsWindow(entry.hwnd)) placeAt(entry.hwnd, HWND_TOPMOST, rect);
	}
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
		    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
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

	auto work = monitorRects(this->mMonitor).work;
	if (work.isEmpty()) return;
	SetWindowPos(
	    this->dim,
	    HWND_TOPMOST,
	    work.x(),
	    work.y(),
	    work.width(),
	    work.height(),
	    SWP_NOACTIVATE | SWP_SHOWWINDOW
	);
	this->dimTarget = DIM_ALPHA;
	this->dimTimer.start();
}

void Scratchpad::hideDim() {
	if (this->dim == nullptr) return;
	this->dimTarget = 0;
	this->dimTimer.start();
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
	if (this->mShown && this->entries.isEmpty()) this->setShown(false);
	else this->relayout();
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
		// The mark stays: unpinning isn't safe from a crash handler, and the
		// next shell finds the window by it and unpins it.
	}
}

} // namespace qs::windows
