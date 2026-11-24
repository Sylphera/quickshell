#include "taskbar.hpp"
#include <algorithm>
#include <atomic>
#include <cwchar>

#include <qt_windows.h>
#include <servprov.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qmetaobject.h>
#include <qpointer.h>
#include <qscreen.h>
#include <qstring.h>

#include "../core/logcat.hpp"
#include "input_mask.hpp"
#include "startup.hpp"
#include "util.hpp"

namespace qs::windows {

namespace {

QS_LOGGING_CATEGORY(logTaskbar, "quickshell.windows.taskbar", QtWarningMsg);

std::atomic<bool> gConcealing = false;      // NOLINT
std::atomic<bool> gTurnedOnAutoHide = false; // NOLINT

constexpr int TRIGGER_PX = 2;
constexpr qint64 HIDE_DELAY_MS = 700;
constexpr int CHECK_REVEALED_MS = 250;
constexpr int CHECK_CONCEALED_MS = 2000;
constexpr qint64 BURST_MS = 1000;
constexpr int BURST_MAX_HIDES = 10;
constexpr int STALE_AUTOHIDE_CHECK_MS = 15000;
constexpr int DISPLAY_SETTLE_MS = 1000;

constexpr auto STATE_KEY = L"Software\\Quickshell";
constexpr auto AUTOHIDE_OWNED_VALUE = L"TaskbarAutoHideOwned";
constexpr auto EXPLORER_ADVANCED = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr auto SNAP_BAR_VALUE = L"EnableSnapBar";
constexpr auto SNAP_BAR_BACKUP = L"SnapBarBackup";
constexpr auto SNAP_BAR_MUTEX = L"Local\\QuickshellSnapBar";
std::atomic<bool> gSnapBarOwned = false;

struct SnapBarSetting {
	DWORD present = 0;
	DWORD value = 0;
};

bool readSnapBar(SnapBarSetting& setting) {
	DWORD size = sizeof(setting.value);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER, EXPLORER_ADVANCED, SNAP_BAR_VALUE, RRF_RT_REG_DWORD,
	    nullptr, &setting.value, &size
	);
	setting.present = status == ERROR_SUCCESS;
	return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool writeSnapBar(const SnapBarSetting& setting) {
	if (!setting.present) {
		const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, SNAP_BAR_VALUE);
		return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
	}
	return RegSetKeyValueW(HKEY_CURRENT_USER, EXPLORER_ADVANCED, SNAP_BAR_VALUE, REG_DWORD,
	                       &setting.value, sizeof(setting.value)) == ERROR_SUCCESS;
}

bool supportsSnapBarCache() {
	const auto build = windowsBuild();
	return build == 26100 || build == 26200;
}

struct __declspec(uuid("4214f6fa-eb36-4e2f-9ca2-23fdc1832df7")) ImmersiveSettingsCache:
    IUnknown {
	virtual HRESULT STDMETHODCALLTYPE OnSettingChanged(int setting) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetBOOL(int setting, BOOL* enabled) = 0;
};

bool syncSnapBarSetting(bool enabled) {
	constexpr CLSID immersiveShell =
	    {0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
	constexpr GUID cacheService =
	    {0x53660488, 0x8855, 0x460b, {0xa9, 0xab, 0x5c, 0xfc, 0x6b, 0x50, 0x12, 0xca}};
	constexpr int snapBarSetting = 12;
	const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;

	bool synced = false;
	{
		Microsoft::WRL::ComPtr<IServiceProvider> shell;
		Microsoft::WRL::ComPtr<ImmersiveSettingsCache> cache;
		BOOL cached = FALSE;
		synced = SUCCEEDED(CoCreateInstance(immersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell)))
		    && SUCCEEDED(shell->QueryService(cacheService, IID_PPV_ARGS(&cache)))
		    && SUCCEEDED(cache->OnSettingChanged(snapBarSetting))
		    && SUCCEEDED(cache->GetBOOL(snapBarSetting, &cached)) && (cached != FALSE) == enabled;
	}
	if (SUCCEEDED(apartment)) CoUninitialize();
	return synced;
}

bool restoreSnapBar(bool sync = true) {
	SnapBarSetting previous;
	DWORD size = sizeof(previous);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER, STATE_KEY, SNAP_BAR_BACKUP, RRF_RT_REG_BINARY, nullptr, &previous, &size
	);
	if (status == ERROR_FILE_NOT_FOUND) return true;
	if (status != ERROR_SUCCESS || size != sizeof(previous) || previous.present > 1) return false;

	SnapBarSetting current;
	if (!readSnapBar(current)) return false;
	if (current.present && current.value == 0) {
		if (!writeSnapBar(previous)) return false;
		current = previous;
	}
	if (!sync) return true;
	if (supportsSnapBarCache() && !syncSnapBarSetting(!current.present || current.value != 0)) return false;

	const auto removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, STATE_KEY, SNAP_BAR_BACKUP);
	return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
}

bool autoHideOwned() {
	DWORD value = 0;
	DWORD size = sizeof(value);
	auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    AUTOHIDE_OWNED_VALUE,
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &value,
	    &size
	);

	return status == ERROR_SUCCESS && value != 0;
}

void setAutoHideOwned(bool owned) {
	if (!owned) {
		RegDeleteKeyValueW(HKEY_CURRENT_USER, STATE_KEY, AUTOHIDE_OWNED_VALUE);
		return;
	}

	DWORD value = 1;
	RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    AUTOHIDE_OWNED_VALUE,
	    REG_DWORD,
	    &value,
	    sizeof(value)
	);
}

bool isTaskbarWindow(HWND hwnd) {
	if (isTrayHookWindow(hwnd)) return false;

	wchar_t cls[32] {};
	GetClassNameW(hwnd, cls, 32);
	return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

void showAllTaskbars() {
	EnumWindows(
	    [](HWND hwnd, LPARAM /*param*/) -> BOOL {
		    if (isTaskbarWindow(hwnd)) ShowWindowAsync(hwnd, SW_SHOWNA);
		    return TRUE;
	    },
	    0
	);
}

UINT appBarState() {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
}

void setAutoHide(bool autoHide) {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	data.hWnd = explorerTaskbarWindow();
	data.lParam = (appBarState() & ABS_ALWAYSONTOP) | (autoHide ? ABS_AUTOHIDE : 0);
	SHAppBarMessage(ABM_SETSTATE, &data);
}

QRect physicalRect(const RECT& rect) {
	return {QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1)};
}

bool edgeIsHorizontal(TaskbarEdge edge) {
	return edge == TaskbarEdge::Top || edge == TaskbarEdge::Bottom;
}

bool primaryBarEdge(HWND hwnd, TaskbarEdge& edge, int& thickness) {
	APPBARDATA data {};
	data.cbSize = sizeof(data);
	data.hWnd = hwnd;
	if (SHAppBarMessage(ABM_GETTASKBARPOS, &data) == 0) return false;

	switch (data.uEdge) {
	case ABE_LEFT: edge = TaskbarEdge::Left; break;
	case ABE_TOP: edge = TaskbarEdge::Top; break;
	case ABE_RIGHT: edge = TaskbarEdge::Right; break;
	case ABE_BOTTOM: edge = TaskbarEdge::Bottom; break;
	default: return false;
	}

	thickness = edgeIsHorizontal(edge) ? static_cast<int>(data.rc.bottom - data.rc.top)
	                                    : static_cast<int>(data.rc.right - data.rc.left);

	return true;
}

TaskbarEdge inferBarEdge(const QRect& window, const QRect& monitor) {
	if (window.width() >= window.height()) {
		auto toTop = window.top() - monitor.top();
		auto toBottom = monitor.bottom() - window.bottom();
		return toTop <= toBottom ? TaskbarEdge::Top : TaskbarEdge::Bottom;
	}

	auto toLeft = window.left() - monitor.left();
	auto toRight = monitor.right() - window.right();
	return toLeft <= toRight ? TaskbarEdge::Left : TaskbarEdge::Right;
}

} // namespace

TaskbarManager* TaskbarManager::instance() {
	static QPointer<TaskbarManager> manager; // NOLINT

	if (manager.isNull()) {
		manager = new TaskbarManager(QCoreApplication::instance());
	}

	return manager.data();
}

TaskbarManager::TaskbarManager(QObject* parent): QObject(parent) {
	QObject::connect(&this->checkTimer, &QTimer::timeout, this, &TaskbarManager::onCheck);

	this->snapBarRetry.setSingleShot(true);
	this->snapBarRetry.setInterval(1000);
	QObject::connect(&this->snapBarRetry, &QTimer::timeout, this, &TaskbarManager::acquireSnapBar);
	this->displayTimer.setSingleShot(true);
	this->displayTimer.setInterval(DISPLAY_SETTLE_MS);
	QObject::connect(&this->displayTimer, &QTimer::timeout, this, &TaskbarManager::onDisplayChanged);

	if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
		auto schedule = [this]() { this->displayTimer.start(); };
		auto watchScreen = [this, schedule](QScreen* screen) {
			QObject::connect(screen, &QScreen::geometryChanged, this, schedule);
			QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, schedule);
		};

		for (auto* screen: QGuiApplication::screens()) watchScreen(screen);

		QObject::connect(
		    app,
		    &QGuiApplication::screenAdded,
		    this,
		    [watchScreen, schedule](QScreen* screen) {
			    watchScreen(screen);
			    schedule();
		    }
		);
		QObject::connect(app, &QGuiApplication::screenRemoved, this, schedule);
		QObject::connect(app, &QGuiApplication::primaryScreenChanged, this, schedule);
	}

	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
		this->setSuppressSnapBar(false);
		this->disable(false);
	});

	startup::afterFirstFrame(this, [this]() {
		QTimer::singleShot(STALE_AUTOHIDE_CHECK_MS, this, &TaskbarManager::releaseStaleAutoHide);
	});
}

TaskbarManager::~TaskbarManager() {
	this->setSuppressSnapBar(false);
	this->disable(false);
}

void TaskbarManager::setSuppressSnapBar(bool enabled) {
	if (this->snapBarRequested == enabled) return;
	this->snapBarRequested = enabled;
	if (enabled) {
		this->acquireSnapBar();
	} else {
		this->snapBarRetry.stop();
		if (gSnapBarOwned.exchange(false) && !restoreSnapBar())
			qCWarning(logTaskbar) << "Could not restore the Windows Snap bar preference.";
		if (this->snapBarMutex != nullptr) {
			ReleaseMutex(this->snapBarMutex);
			CloseHandle(this->snapBarMutex);
			this->snapBarMutex = nullptr;
		}
	}
	emit this->suppressSnapBarChanged();
}

void TaskbarManager::acquireSnapBar() {
	if (!this->snapBarRequested || this->snapBarMutex != nullptr) return;

	auto* mutex = CreateMutexW(nullptr, FALSE, SNAP_BAR_MUTEX);
	if (mutex == nullptr) return;
	const auto ownership = WaitForSingleObject(mutex, 0);
	if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) {
		CloseHandle(mutex);
		if (ownership == WAIT_TIMEOUT) this->snapBarRetry.start();
		return;
	}
	this->snapBarMutex = mutex;

	SnapBarSetting previous;
	if (!restoreSnapBar() || !readSnapBar(previous)) {
		qCWarning(logTaskbar) << "Could not read the Windows Snap bar preference.";
		return;
	}
	if (!supportsSnapBarCache()) return;
	if (previous.present && previous.value == 0) {
		if (!syncSnapBarSetting(false))
			qCWarning(logTaskbar) << "Could not refresh the Windows Snap bar preference.";
		return;
	}

	if (RegSetKeyValueW(HKEY_CURRENT_USER, STATE_KEY, SNAP_BAR_BACKUP, REG_BINARY,
	                    &previous, sizeof(previous)) != ERROR_SUCCESS) {
		qCWarning(logTaskbar) << "Could not save the Windows Snap bar preference.";
		return;
	}
	gSnapBarOwned.store(true);
	if (!writeSnapBar({1, 0})) {
		gSnapBarOwned.store(false);
		RegDeleteKeyValueW(HKEY_CURRENT_USER, STATE_KEY, SNAP_BAR_BACKUP);
		qCWarning(logTaskbar) << "Could not suppress the Windows Snap bar.";
		return;
	}
	if (!syncSnapBarSetting(false)) {
		qCWarning(logTaskbar) << "Explorer did not apply the Windows Snap bar preference.";
		if (restoreSnapBar()) gSnapBarOwned.store(false);
	}
}

void TaskbarManager::setHoverOnly(bool hoverOnly) {
	if (hoverOnly == this->mHoverOnly) return;
	this->mHoverOnly = hoverOnly;
	this->refreshMode();
	emit this->hoverOnlyChanged();
}

void TaskbarManager::setAlwaysHidden(bool alwaysHidden) {
	if (alwaysHidden == this->mAlwaysHidden) return;
	this->mAlwaysHidden = alwaysHidden;
	this->refreshMode();
	emit this->alwaysHiddenChanged();
}

void TaskbarManager::refreshMode() {
	if (!this->mHoverOnly && !this->mAlwaysHidden) {
		this->disable(true);
		return;
	}
	if (!this->enabled) {
		this->enable();
		return;
	}
	POINT cursor {};
	GetCursorPos(&cursor);
	if (!this->mAlwaysHidden && (this->atTrigger({cursor.x, cursor.y}) || this->overBar({cursor.x, cursor.y}))) {
		this->reveal();
	} else {
		this->conceal();
	}
}

void TaskbarManager::turnOffAutoHide() {
	if (this->enabled) return;

	gTurnedOnAutoHide.store(false);
	setAutoHideOwned(false);

	if ((appBarState() & ABS_AUTOHIDE) != 0) {
		setAutoHide(false);
		qCInfo(logTaskbar) << "Turned taskbar auto-hide off for the Windows taskbar mode.";
	}

	showAllTaskbars();
}

void TaskbarManager::showAfterShell() { showAllTaskbars(); }

void TaskbarManager::restoreForCrash() {
	if (gSnapBarOwned.exchange(false)) restoreSnapBar(false);
	if (gConcealing.load()) showAllTaskbars();
}

void TaskbarManager::releaseStaleAutoHide() {
	if (this->enabled || gTurnedOnAutoHide.load() || !autoHideOwned()) return;

	if ((appBarState() & ABS_AUTOHIDE) != 0) {
		setAutoHide(false);
		qCInfo(logTaskbar) << "Hover only mode is off; turned taskbar auto-hide back off.";
	}

	setAutoHideOwned(false);
}

void TaskbarManager::enable() {
	if (this->enabled) return;
	this->enabled = true;

	auto owned = autoHideOwned();

	if ((appBarState() & ABS_AUTOHIDE) == 0) {
		setAutoHide(true);
		gTurnedOnAutoHide.store(true);
		if (!owned) setAutoHideOwned(true);
		qCInfo(logTaskbar) << "Turned on taskbar auto-hide for hover only mode"
		                   << "(undone when hover only mode is turned off).";
	} else if (owned) {
		gTurnedOnAutoHide.store(true);
	}

	this->findBars();

	auto* tracker = InputMaskTracker::instance();
	QObject::connect(tracker, &InputMaskTracker::cursorMoved, this, &TaskbarManager::onCursorMoved);
	tracker->acquireCursorEvents();

	gConcealing.store(true);

	POINT cursor {};
	GetCursorPos(&cursor);
	auto position = QPoint(cursor.x, cursor.y);

	if (!this->mAlwaysHidden && (this->atTrigger(position) || this->overBar(position))) this->reveal();
	else this->conceal();
}

void TaskbarManager::disable(bool restoreAutoHide) {
	if (!this->enabled) return;
	this->enabled = false;
	this->revealed = false;
	this->checkTimer.stop();

	auto* tracker = InputMaskTracker::instance();
	QObject::disconnect(tracker, &InputMaskTracker::cursorMoved, this, nullptr);
	tracker->releaseCursorEvents();
	this->unwatchExplorer();

	showAllTaskbars();
	gConcealing.store(false);

	if (!restoreAutoHide) return;

	if (gTurnedOnAutoHide.exchange(false)) {
		setAutoHide(false);
		qCInfo(logTaskbar) << "Hover only mode turned off; turned taskbar auto-hide back off.";
	}

	setAutoHideOwned(false);
}

void TaskbarManager::findBars() {
	this->bars.clear();

	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (!isTaskbarWindow(hwnd)) return TRUE;

		    auto* self = reinterpret_cast<TaskbarManager*>(param); // NOLINT(performance-no-int-to-ptr)

		    MONITORINFO info {};
		    info.cbSize = sizeof(info);
		    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) return TRUE;
		    auto monitor = physicalRect(info.rcMonitor);

		    RECT rect {};
		    GetWindowRect(hwnd, &rect);
		    auto window = physicalRect(rect);

		    TaskbarEdge edge = TaskbarEdge::Bottom;
		    int thickness = 0;
		    auto* primary = explorerTaskbarWindow();

		    if (hwnd != primary || !primaryBarEdge(hwnd, edge, thickness)) {
			    edge = inferBarEdge(window, monitor);
			    thickness = edgeIsHorizontal(edge) ? window.height() : window.width();
		    }

		    self->bars.append({
		        .hwnd = hwnd,
		        .monitor = monitor,
		        .edge = edge,
		        .thickness = thickness,
		    });

		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(this)
	);

	qCDebug(logTaskbar) << "Found" << this->bars.length() << "taskbar windows";

	if (this->enabled) this->watchExplorer();
}

bool TaskbarManager::atTrigger(QPoint position) const {
	for (const auto& bar: this->bars) {
		if (!bar.monitor.contains(position)) continue;

		switch (bar.edge) {
		case TaskbarEdge::Top:
			if (position.y() < bar.monitor.top() + TRIGGER_PX) return true;
			break;
		case TaskbarEdge::Bottom:
			if (position.y() > bar.monitor.bottom() - TRIGGER_PX) return true;
			break;
		case TaskbarEdge::Left:
			if (position.x() < bar.monitor.left() + TRIGGER_PX) return true;
			break;
		case TaskbarEdge::Right:
			if (position.x() > bar.monitor.right() - TRIGGER_PX) return true;
			break;
		}
	}

	return false;
}

bool TaskbarManager::overBar(QPoint position) const {
	for (const auto& bar: this->bars) {
		QRect shown;

		switch (bar.edge) {
		case TaskbarEdge::Top:
			shown = QRect(bar.monitor.left(), bar.monitor.top(), bar.monitor.width(), bar.thickness);
			break;
		case TaskbarEdge::Bottom:
			shown = QRect(
			    bar.monitor.left(),
			    bar.monitor.bottom() - bar.thickness + 1,
			    bar.monitor.width(),
			    bar.thickness
			);
			break;
		case TaskbarEdge::Left:
			shown = QRect(bar.monitor.left(), bar.monitor.top(), bar.thickness, bar.monitor.height());
			break;
		case TaskbarEdge::Right:
			shown = QRect(
			    bar.monitor.right() - bar.thickness + 1,
			    bar.monitor.top(),
			    bar.thickness,
			    bar.monitor.height()
			);
			break;
		}

		if (shown.contains(position)) return true;
	}

	return false;
}

bool TaskbarManager::taskbarPopupActive() {
	auto* foreground = GetForegroundWindow();
	if (foreground == nullptr) return false;
	if (isTaskbarWindow(foreground)) return true;

	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);

	auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) return false;

	wchar_t path[MAX_PATH] {};
	DWORD size = MAX_PATH;
	auto ok = QueryFullProcessImageNameW(process, 0, path, &size);
	CloseHandle(process);
	if (!ok) return false;

	auto name = QFileInfo(QString::fromWCharArray(path, static_cast<qsizetype>(size))).fileName().toLower();

	if (name == "shellexperiencehost.exe" || name == "startmenuexperiencehost.exe"
	    || name == "searchhost.exe" || name == "shellhost.exe"
	    || name == "searchapp.exe" || name == "searchui.exe")
	{
		return true;
	}

	if (name == "explorer.exe") {
		wchar_t cls[64] {};
		GetClassNameW(foreground, cls, 64);
		return wcscmp(cls, L"#32768") == 0 || wcscmp(cls, L"NotifyIconOverflowWindow") == 0
		    || wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0
		    || wcscmp(cls, L"Xaml_WindowedPopupClass") == 0
		    || wcscmp(cls, L"DV2ControlHost") == 0;
	}

	return false;
}

void TaskbarManager::watchExplorer() {
	DWORD pid = 0;
	if (!this->bars.isEmpty()) GetWindowThreadProcessId(this->bars.first().hwnd, &pid);
	if (pid == this->watchedPid && (this->showHook != nullptr || pid == 0)) return;

	this->unwatchExplorer();
	if (pid == 0) return;

	this->showHook = SetWinEventHook(
	    EVENT_OBJECT_SHOW,
	    EVENT_OBJECT_SHOW,
	    nullptr,
	    &TaskbarManager::onWinEvent,
	    pid,
	    0,
	    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
	);
	this->watchedPid = pid;
}

void TaskbarManager::unwatchExplorer() {
	if (this->showHook != nullptr) UnhookWinEvent(this->showHook);
	this->showHook = nullptr;
	this->watchedPid = 0;
}

void CALLBACK TaskbarManager::onWinEvent(
    HWINEVENTHOOK /*hook*/,
    DWORD /*event*/,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*thread*/,
    DWORD /*time*/
) {
	if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) return;

	auto* self = TaskbarManager::instance();
	QMetaObject::invokeMethod(self, [self, hwnd]() { self->onBarShown(hwnd); }, Qt::QueuedConnection);
}

void TaskbarManager::onBarShown(HWND hwnd) {
	if (!this->enabled || this->revealed) return;
	if (!std::ranges::any_of(this->bars, [hwnd](const Bar& bar) { return bar.hwnd == hwnd; })) {
		if (!isTaskbarWindow(hwnd)) return;
		this->findBars();
	}

	POINT cursor {};
	GetCursorPos(&cursor);
	auto position = QPoint(cursor.x, cursor.y);
	if (!this->mAlwaysHidden && this->atTrigger(position)) {
		this->reveal();
		return;
	}

	auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - this->burstStart > BURST_MS) {
		this->burstStart = now;
		this->burstHides = 0;
	}
	if (++this->burstHides > BURST_MAX_HIDES) return;

	if (IsWindowVisible(hwnd)) ShowWindowAsync(hwnd, SW_HIDE);
}

void TaskbarManager::reveal() {
	if (this->mAlwaysHidden) {
		this->conceal();
		return;
	}
	if (this->bars.isEmpty()) this->findBars();

	for (const auto& bar: this->bars) ShowWindowAsync(bar.hwnd, SW_SHOWNA);

	this->revealed = true;
	this->leftAt = 0;
	this->checkTimer.start(CHECK_REVEALED_MS);
}

void TaskbarManager::conceal() {
	for (const auto& bar: this->bars) {
		if (IsWindowVisible(bar.hwnd)) ShowWindowAsync(bar.hwnd, SW_HIDE);
	}

	this->revealed = false;
	this->leftAt = 0;
	this->checkTimer.start(CHECK_CONCEALED_MS);
}

void TaskbarManager::onCursorMoved(QPoint position) {
	if (!this->enabled) return;
	if (this->mAlwaysHidden) return;

	if (!this->revealed) {
		if (this->atTrigger(position)) this->reveal();
		return;
	}

	if (this->overBar(position) || this->atTrigger(position)) this->leftAt = 0;
	else if (this->leftAt == 0) this->leftAt = QDateTime::currentMSecsSinceEpoch();
}

void TaskbarManager::onCheck() {
	if (!this->enabled) return;

	auto stale = this->bars.isEmpty();
	for (const auto& bar: this->bars) stale = stale || !IsWindow(bar.hwnd);
	if (stale) this->findBars();
	if (this->mAlwaysHidden) {
		this->conceal();
		return;
	}

	if (!this->revealed) {
		for (const auto& bar: this->bars) {
			if (IsWindowVisible(bar.hwnd)) ShowWindowAsync(bar.hwnd, SW_HIDE);
		}

		return;
	}

	POINT cursor {};
	GetCursorPos(&cursor);
	auto position = QPoint(cursor.x, cursor.y);

	if (this->overBar(position) || this->atTrigger(position) || taskbarPopupActive()) {
		this->leftAt = 0;
		return;
	}

	auto now = QDateTime::currentMSecsSinceEpoch();
	if (this->leftAt == 0) this->leftAt = now;
	else if (now - this->leftAt >= HIDE_DELAY_MS) this->conceal();
}

void TaskbarManager::onDisplayChanged() {
	if (!this->enabled) return;

	this->findBars();
	if (!this->revealed) this->conceal();
}

Taskbar::Taskbar(QObject* parent): QObject(parent) {
	QObject::connect(
	    TaskbarManager::instance(),
	    &TaskbarManager::hoverOnlyChanged,
	    this,
	    &Taskbar::hoverOnlyChanged
	);
	QObject::connect(
	    TaskbarManager::instance(),
	    &TaskbarManager::alwaysHiddenChanged,
	    this,
	    &Taskbar::alwaysHiddenChanged
	);
	QObject::connect(
	    TaskbarManager::instance(),
	    &TaskbarManager::suppressSnapBarChanged,
	    this,
	    &Taskbar::suppressSnapBarChanged
	);
}

bool Taskbar::hoverOnly() const { return TaskbarManager::instance()->hoverOnly(); }
void Taskbar::setHoverOnly(bool hoverOnly) { TaskbarManager::instance()->setHoverOnly(hoverOnly); }
bool Taskbar::alwaysHidden() const { return TaskbarManager::instance()->alwaysHidden(); }
void Taskbar::setAlwaysHidden(bool alwaysHidden) { TaskbarManager::instance()->setAlwaysHidden(alwaysHidden); }
bool Taskbar::suppressSnapBar() const { return TaskbarManager::instance()->suppressSnapBar(); }
void Taskbar::setSuppressSnapBar(bool enabled) { TaskbarManager::instance()->setSuppressSnapBar(enabled); }
void Taskbar::turnOffAutoHide() { TaskbarManager::instance()->turnOffAutoHide(); }

} // namespace qs::windows
