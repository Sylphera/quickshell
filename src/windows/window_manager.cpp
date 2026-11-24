#include "window_manager.hpp"
#include <cstdlib>
#include <cwchar>

#include <qcolor.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qtimer.h>

#include "desktop_trimmer.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_decorations.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logWorkspace, "quickshell.windows.workspace", QtWarningMsg);

constexpr qint64 WARP_TIMEOUT_MS = 2000;
constexpr int WARP_FALLBACK_MS = 500;
constexpr int FOCUS_REPAIR_MS = 150;
constexpr int SWITCH_POPUP_RETRY_MS = 1500;

bool isNullGuid(const GUID& id) { return IsEqualGUID(id, GUID {}) != FALSE; }

bool isCloaked(HWND hwnd) {
	DWORD cloaked = 0;
	return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
	    && cloaked != 0;
}

bool isDesktopWindow(HWND hwnd) {
	if (hwnd == GetShellWindow()) return true;
	wchar_t name[16] {};
	GetClassNameW(hwnd, name, 16);
	return wcscmp(name, L"WorkerW") == 0 || wcscmp(name, L"Progman") == 0;
}

constexpr auto* SWITCH_POPUP_CLASS = L"XamlExplorerHostIslandWindow";
constexpr auto* SWITCH_POPUP_PROP = L"QuickshellSwitchPopupMasked";

bool looksLikeSwitchPopup(HWND hwnd) {
	auto* taskbar = explorerTaskbarWindow();
	if (taskbar == nullptr) return false;
	DWORD popupPid = 0;
	DWORD explorerPid = 0;
	GetWindowThreadProcessId(hwnd, &popupPid);
	GetWindowThreadProcessId(taskbar, &explorerPid);
	if (popupPid == 0 || popupPid != explorerPid) return false;

	RECT rect {};
	if (!GetWindowRect(hwnd, &rect)) return false;
	auto width = rect.right - rect.left;
	auto height = rect.bottom - rect.top;
	if (width < 65 || width > 650 || height < 25 || height > 150) return false;

	MONITORINFO monitor {.cbSize = sizeof(MONITORINFO)};
	auto* screenHandle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	if (!GetMonitorInfoW(screenHandle, &monitor)) return false;
	const auto& screen = monitor.rcMonitor;
	if (rect.bottom < screen.bottom - 180 || rect.bottom > screen.bottom + 25) return false;
	auto center = (rect.left + rect.right) / 2;
	auto screenCenter = (screen.left + screen.right) / 2;
	return std::abs(center - screenCenter) <= (screen.right - screen.left) / 5;
}

bool hasWindowRegion(HWND hwnd) {
	auto* probe = CreateRectRgn(0, 0, 0, 0);
	if (probe == nullptr) return true;
	auto result = GetWindowRgn(hwnd, probe) != ERROR;
	DeleteObject(probe);
	return result;
}

HWND maskSwitchPopup() {
	HWND hwnd = nullptr;
	while ((hwnd = FindWindowExW(nullptr, hwnd, SWITCH_POPUP_CLASS, nullptr)) != nullptr) {
		if (GetPropW(hwnd, SWITCH_POPUP_PROP) != nullptr) return hwnd;
	}

	while ((hwnd = FindWindowExW(nullptr, hwnd, SWITCH_POPUP_CLASS, nullptr)) != nullptr) {
		if (!looksLikeSwitchPopup(hwnd) || hasWindowRegion(hwnd)) continue;
		if (SetPropW(hwnd, SWITCH_POPUP_PROP, reinterpret_cast<HANDLE>(1)) == FALSE) continue;

		auto* empty = CreateRectRgn(0, 0, 0, 0);
		if (empty != nullptr && SetWindowRgn(hwnd, empty, TRUE) != 0) return hwnd;

		if (empty != nullptr) DeleteObject(empty);
		RemovePropW(hwnd, SWITCH_POPUP_PROP);
	}

	return nullptr;
}

void unmaskSwitchPopup(HWND hwnd) {
	if (hwnd == nullptr || !IsWindow(hwnd) || GetPropW(hwnd, SWITCH_POPUP_PROP) == nullptr) return;
	SetWindowRgn(hwnd, nullptr, TRUE);
	RemovePropW(hwnd, SWITCH_POPUP_PROP);
}

} // namespace

WorkspaceBehavior* WorkspaceBehavior::instance() {
	static QPointer<WorkspaceBehavior> behavior; // NOLINT

	if (behavior.isNull()) {
		behavior = new WorkspaceBehavior(QCoreApplication::instance());
	}

	return behavior.data();
}

WorkspaceBehavior::WorkspaceBehavior(QObject* parent)
    : QObject(parent)
    , desktops(VirtualDesktops::instance()) {
	this->currentIndex = this->desktops->currentIndex();
	const auto& list = this->desktops->desktops();
	if (this->currentIndex >= 0 && this->currentIndex < list.length()) {
		this->currentId = list[this->currentIndex].id;
	}

	QObject::connect(
	    this->desktops,
	    &VirtualDesktops::currentChanged,
	    this,
	    &WorkspaceBehavior::onCurrentChanged
	);

	this->fallbackTimer.setSingleShot(true);
	this->fallbackTimer.setInterval(WARP_FALLBACK_MS);
	QObject::connect(&this->fallbackTimer, &QTimer::timeout, this, &WorkspaceBehavior::warpToTopmost);

	this->focusTimer.setSingleShot(true);
	this->focusTimer.setInterval(FOCUS_REPAIR_MS);
	QObject::connect(&this->focusTimer, &QTimer::timeout, this, &WorkspaceBehavior::repairFocus);

	QTimer::singleShot(0, this, &WorkspaceBehavior::watchTracker);
}

void WorkspaceBehavior::setBackAndForth(bool enabled) {
	if (enabled == this->mBackAndForth) return;
	this->mBackAndForth = enabled;
	emit this->backAndForthChanged();
}

void WorkspaceBehavior::setWarpOnChange(qint32 mode) {
	mode = qBound(0, mode, 2);
	if (mode == this->mWarpOnChange) return;
	this->mWarpOnChange = mode;
	if (mode == 0) this->cancelWarp();
	emit this->warpOnChangeChanged();
}

WorkspaceBehavior::~WorkspaceBehavior() { unmaskSwitchPopup(this->switchPopup); }

void WorkspaceBehavior::setHideSwitchPopup(bool enabled) {
	if (enabled == this->mHideSwitchPopup) return;
	this->mHideSwitchPopup = enabled;

	if (enabled) {
		this->updateSwitchPopup();
	} else {
		unmaskSwitchPopup(this->switchPopup);
		this->switchPopup = nullptr;
	}

	emit this->hideSwitchPopupChanged();
}

void WorkspaceBehavior::updateSwitchPopup() {
	if (!this->mHideSwitchPopup) return;
	if (this->switchPopup != nullptr && IsWindow(this->switchPopup)
	    && GetPropW(this->switchPopup, SWITCH_POPUP_PROP) != nullptr)
	{
		return;
	}

	this->switchPopup = maskSwitchPopup();
}

void WorkspaceBehavior::onCurrentChanged() {
	this->updateSwitchPopup();
	if (this->mHideSwitchPopup && this->switchPopup == nullptr) {
		QTimer::singleShot(SWITCH_POPUP_RETRY_MS, this, &WorkspaceBehavior::updateSwitchPopup);
	}

	auto index = this->desktops->currentIndex();
	const auto& list = this->desktops->desktops();
	auto id = index >= 0 && index < list.length() ? list[index].id : GUID {};

	auto same = isNullGuid(id) || isNullGuid(this->currentId) ? index == this->currentIndex
	                                                           : IsEqualGUID(id, this->currentId);

	if (!same && this->currentIndex >= 0) {
		this->previousId = this->currentId;
		this->previousIndex = this->currentIndex;
	}

	this->currentId = id;
	this->currentIndex = index;
	this->tryWarp();
}

qsizetype WorkspaceBehavior::previousDesktop() const {
	if (this->previousIndex < 0) return -1;
	if (!isNullGuid(this->previousId)) return this->desktops->indexOf(this->previousId);
	return this->previousIndex < this->desktops->count() ? this->previousIndex : -1;
}

void WorkspaceBehavior::watchTracker() {
	if (this->tracker != nullptr) return;
	this->tracker = WindowTracker::instance();

	auto retry = &WorkspaceBehavior::tryWarp;
	QObject::connect(this->tracker, &WindowTracker::activeWindowChanged, this, retry);
	QObject::connect(this->tracker, &WindowTracker::flushed, this, retry);

	QObject::connect(this->tracker, &WindowTracker::activeWindowChanged, this, [this]() {
		if (auto* active = this->tracker->activeWindow()) this->lastActive = active;
	});
	QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, [this](TrackedWindow* window) {
		if (window != this->lastActive) return;
		this->lastActive = nullptr;
		this->expectFocus(this->desktops->currentIndex());
	});
}

void WorkspaceBehavior::prepareWarp(qsizetype desktop, bool fromActive) {
	this->cancelWarp();
	if (this->mWarpOnChange == 0 || WorkspaceBehavior::cursorOverShell()) return;

	this->watchTracker();
	this->warpFrom = fromActive ? nullptr : this->tracker->activeWindow();
	this->warpDesktop = desktop;
	this->warpDeadline = QDateTime::currentMSecsSinceEpoch() + WARP_TIMEOUT_MS;
}

void WorkspaceBehavior::cancelWarp() {
	this->warpDesktop = -1;
	this->warpFrom = nullptr;
	this->fallbackTimer.stop();
}

void WorkspaceBehavior::tryWarp() {
	if (this->warpDesktop < 0 || this->tracker == nullptr) return;

	if (QDateTime::currentMSecsSinceEpoch() > this->warpDeadline) {
		this->cancelWarp();
		return;
	}

	if (this->desktops->currentIndex() != this->warpDesktop) return;

	auto* window = this->tracker->activeWindow();
	auto qualifies = window != nullptr && window != this->warpFrom.data() && !window->minimized()
	              && (window->desktop() == this->warpDesktop || window->desktop() == -1);

	if (!qualifies) {
		if (!this->fallbackTimer.isActive()) this->fallbackTimer.start();
		return;
	}

	this->cancelWarp();
	WorkspaceBehavior::warpCursor(window->hwnd());
}

void WorkspaceBehavior::warpToTopmost() {
	if (this->warpDesktop < 0 || this->tracker == nullptr) return;

	auto desktop = this->warpDesktop;
	this->cancelWarp();
	if (this->desktops->currentIndex() != desktop) return;

	struct Search {
		WindowTracker* tracker = nullptr;
		qsizetype desktop = -1;
		HWND found = nullptr;
	} search {.tracker = this->tracker, .desktop = desktop};

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param); // NOLINT(performance-no-int-to-ptr)
		    auto* window = search->tracker->windowFor(hwnd);
		    if (window == nullptr || window->minimized() || window->desktop() != search->desktop) {
			    return TRUE;
		    }

		    search->found = hwnd;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	if (search.found != nullptr) WorkspaceBehavior::warpCursor(search.found);
}

void WorkspaceBehavior::warpToActive() {
	if (this->mWarpOnChange == 0 || WorkspaceBehavior::cursorOverShell()) return;

	this->watchTracker();
	auto* window = this->tracker->activeWindow();
	if (window == nullptr || window->minimized()) return;

	auto desktop = window->desktop();
	if (desktop != -1 && desktop != this->desktops->currentIndex()) return;

	WorkspaceBehavior::warpCursor(window->hwnd());
}

void WorkspaceBehavior::expectFocus(qsizetype desktop) {
	this->focusDesktop = desktop;
	this->focusTimer.start();
}

void WorkspaceBehavior::repairFocus() {
	auto desktop = this->focusDesktop;
	this->focusDesktop = -1;
	if (desktop < 0 || this->desktops->currentIndex() != desktop) return;

	auto* foreground = GetForegroundWindow();
	if (foreground != nullptr && isOwnProcessWindow(foreground)) return;
	if (foreground != nullptr && !isCloaked(foreground) && !isDesktopWindow(foreground)) return;

	this->watchTracker();

	struct Search {
		WindowTracker* tracker = nullptr;
		HWND found = nullptr;
	} search {.tracker = this->tracker};

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param); // NOLINT(performance-no-int-to-ptr)
		    if (search->tracker->windowFor(hwnd) == nullptr || IsIconic(hwnd) || isCloaked(hwnd)) {
			    return TRUE;
		    }

		    search->found = hwnd;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	if (search.found == nullptr) {
		this->tracker->refreshForeground();
		qCDebug(logWorkspace) << "Nothing to focus on this desktop after a desktop change";
		return;
	}
	if (search.found == foreground) return;

	auto ok = forceForegroundWindow(search.found);
	this->tracker->refreshForeground();
	qCDebug(logWorkspace) << "Nothing on this desktop had focus after a desktop change, moved it to"
	                      << search.found << (ok ? "" : "(failed)");
	if (!ok) qCWarning(logWorkspace) << "Couldn't move focus off a window on another desktop";
}

bool WorkspaceBehavior::cursorOverShell() {
	POINT cursor {};
	if (!GetCursorPos(&cursor)) return false;

	auto* hwnd = WindowFromPoint(cursor);
	if (hwnd == nullptr) return false;

	auto* root = GetAncestor(hwnd, GA_ROOT);
	return isOwnProcessWindow(root != nullptr ? root : hwnd);
}

void WorkspaceBehavior::warpCursor(HWND hwnd) {
	RECT frame {};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		if (!GetWindowRect(hwnd, &frame)) return;
	}

	POINT cursor {};
	if (GetCursorPos(&cursor) && PtInRect(&frame, cursor)) return;

	SetCursorPos((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
}

WindowManager::WindowManager(QObject* parent): QObject(parent) {
	auto* decorations = DecorationManager::instance();
	auto* behavior = WorkspaceBehavior::instance();

	// clang-format off
	QObject::connect(decorations, &DecorationManager::bordersChanged, this, &WindowManager::bordersChanged);
	QObject::connect(decorations, &DecorationManager::activeColorChanged, this, &WindowManager::activeBorderColorChanged);
	QObject::connect(decorations, &DecorationManager::inactiveColorChanged, this, &WindowManager::inactiveBorderColorChanged);
	QObject::connect(decorations, &DecorationManager::cornersChanged, this, &WindowManager::cornersChanged);
	QObject::connect(decorations, &DecorationManager::activeOpacityChanged, this, &WindowManager::activeOpacityChanged);
	QObject::connect(decorations, &DecorationManager::inactiveOpacityChanged, this, &WindowManager::inactiveOpacityChanged);
	QObject::connect(behavior, &WorkspaceBehavior::backAndForthChanged, this, &WindowManager::workspaceBackAndForthChanged);
	QObject::connect(behavior, &WorkspaceBehavior::warpOnChangeChanged, this, &WindowManager::warpOnChangeWorkspaceChanged);
	QObject::connect(behavior, &WorkspaceBehavior::hideSwitchPopupChanged, this, &WindowManager::hideDesktopPopupChanged);
	QObject::connect(DesktopTrimmer::instance(), &DesktopTrimmer::enabledChanged, this, &WindowManager::removeEmptyDesktopsChanged);
	// clang-format on
}

bool WindowManager::decorationsSupported() { return DecorationManager::supported(); }

QString WindowManager::borders() const { return DecorationManager::instance()->borders(); }
void WindowManager::setBorders(const QString& borders) {
	DecorationManager::instance()->setBorders(borders);
}

QColor WindowManager::activeBorderColor() const { return DecorationManager::instance()->activeColor(); }
void WindowManager::setActiveBorderColor(const QColor& color) {
	DecorationManager::instance()->setActiveColor(color);
}

QColor WindowManager::inactiveBorderColor() const {
	return DecorationManager::instance()->inactiveColor();
}
void WindowManager::setInactiveBorderColor(const QColor& color) {
	DecorationManager::instance()->setInactiveColor(color);
}

QString WindowManager::corners() const { return DecorationManager::instance()->corners(); }
void WindowManager::setCorners(const QString& corners) {
	DecorationManager::instance()->setCorners(corners);
}

qreal WindowManager::activeOpacity() const { return DecorationManager::instance()->activeOpacity(); }
void WindowManager::setActiveOpacity(qreal opacity) {
	DecorationManager::instance()->setActiveOpacity(opacity);
}

qreal WindowManager::inactiveOpacity() const {
	return DecorationManager::instance()->inactiveOpacity();
}
void WindowManager::setInactiveOpacity(qreal opacity) {
	DecorationManager::instance()->setInactiveOpacity(opacity);
}

bool WindowManager::workspaceBackAndForth() const {
	return WorkspaceBehavior::instance()->backAndForth();
}
void WindowManager::setWorkspaceBackAndForth(bool enabled) {
	WorkspaceBehavior::instance()->setBackAndForth(enabled);
}

qint32 WindowManager::warpOnChangeWorkspace() const {
	return WorkspaceBehavior::instance()->warpOnChange();
}
void WindowManager::setWarpOnChangeWorkspace(qint32 mode) {
	WorkspaceBehavior::instance()->setWarpOnChange(mode);
}

bool WindowManager::hideDesktopPopup() const {
	return WorkspaceBehavior::instance()->hideSwitchPopup();
}
void WindowManager::setHideDesktopPopup(bool enabled) {
	WorkspaceBehavior::instance()->setHideSwitchPopup(enabled);
}

bool WindowManager::removeEmptyDesktops() const { return DesktopTrimmer::instance()->enabled(); }

void WindowManager::setRemoveEmptyDesktops(bool enabled) {
	DesktopTrimmer::instance()->setEnabled(enabled);
}

} // namespace qs::windows
