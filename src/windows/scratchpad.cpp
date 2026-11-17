#include "scratchpad.hpp"
#include <array>
#include <atomic>

#include <qcoreapplication.h>
#include <qscreen.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qrect.h>
#include <qtypes.h>

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

void recoverOrphans() {
	EnumWindows(
	    [](HWND hwnd, LPARAM /*param*/) -> BOOL {
		    auto packed = reinterpret_cast<quintptr>(GetPropW(hwnd, SCRATCHPAD_PROP));
		    if (packed == 0) return TRUE;
		    auto rect = unpackRect(packed);
		    if (!rect.isEmpty()) placeAt(hwnd, HWND_NOTOPMOST, rect);
		    VirtualDesktops::instance()->pinWindow(hwnd, false);
		    RemovePropW(hwnd, SCRATCHPAD_PROP);
		    return TRUE;
	    },
	    0
	);
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

Scratchpad::~Scratchpad() = default;

Scratchpad::Scratchpad(QObject* parent)
    : QObject(parent) {
	recoverOrphans();
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
		this->setShown(false);
	});
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
	this->setShown(!this->mShown);
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
	if (shown == this->mShown) return;

	if (shown) {
		POINT cursor {};
		GetCursorPos(&cursor);
		this->mMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
		this->shownAt = GetTickCount64();
		this->mShown = true;
		this->showDim();
		this->relayout();
		if (!this->entries.isEmpty()) forceForegroundWindow(this->entries.last().hwnd);
	} else {
		this->mShown = false;
		for (auto& entry: this->entries) this->park(entry);
		this->relayout();
		this->hideDim();
	}

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
			raise(entry.hwnd);
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
		forget(entry.hwnd);
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
		forget(entry.hwnd);
		this->entries.removeAt(i);
	}
}

void Scratchpad::restoreForCrash() {
	for (auto& slot: gCrashSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd)) continue;
		auto rect = unpackRect(slot.rect.load());
		if (!rect.isEmpty()) placeAt(hwnd, HWND_NOTOPMOST, rect);
		RemovePropW(hwnd, SCRATCHPAD_PROP);
	}
}

} // namespace qs::windows
