#include "desktop_slide.hpp"
#include <limits>
#include <cstddef>
#include <mutex>
#include <vector>
#include <thread>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>

#include <pdh.h>
#include <qcoreapplication.h>
#include <qset.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qdir.h>
#include <qfile.h>
#include <qimage.h>
#include <qstandardpaths.h>

#include "panel_window.hpp"
#include "scratchpad.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

constexpr auto* OVERLAY_CLASS = L"QuickshellDesktopSlide";
constexpr double SLIDE_MS = 700;
constexpr int WORKSPACE_GAP = 50;
constexpr int SETTLE_RETRY_MS = 30;
constexpr int SETTLE_RETRIES = 30;
constexpr int HOLD_MS = 150;
// end-4's curve has covered 99.5 % of the way by then (about 10 px left,
// moving about 1 px a frame). The real switch happens there, under the cover:
// any stutter it causes is too small to see, and the new windows take input.
constexpr int COMMIT_MS = 400;
constexpr auto* SNAPSHOT_CLASS = L"QuickshellDesktopSlideSnapshot";
constexpr int CORNER_ZONE = 40;
constexpr int CORNER_TOLERANCE = 6;
constexpr int CORNER_RADIUS = 8;
constexpr int WARM_DELAY_MS = 2500;

// Window stills outlive a shell restart on disk: a browser on another desktop
// is cloaked and won't paint, so after a restart there would be no way to get
// one until it is visited. Each still is a PNG named after its window, with the
// frame and process it was taken for; it only counts while both still match.
QString snapshotDir() {
	static const auto dir = []() {
		auto path = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
		          + QStringLiteral("/quickshell/slide-snapshots");
		QDir().mkpath(path);
		return path;
	}();
	return dir;
}

QString snapshotFile(HWND hwnd) {
	return snapshotDir() + QStringLiteral("/%1.png").arg(reinterpret_cast<quintptr>(hwnd), 0, 16); // NOLINT
}

DWORD processOf(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid;
}

QString frameText(const QRect& frame) {
	return QStringLiteral("%1,%2,%3,%4").arg(frame.x()).arg(frame.y()).arg(frame.width()).arg(frame.height());
}

void saveSnapshot(HWND hwnd, const QRect& frame, QImage image) {
	auto pid = processOf(hwnd);
	std::thread([hwnd, frame, pid, image = std::move(image)]() mutable {
		image.setText(QStringLiteral("frame"), frameText(frame));
		image.setText(QStringLiteral("pid"), QString::number(pid));
		auto path = snapshotFile(hwnd);
		image.save(path + QStringLiteral(".tmp"), "PNG", 30);
		QFile::remove(path);
		QFile::rename(path + QStringLiteral(".tmp"), path);
	}).detach();
}

// On a worker thread: the saved still of this window, if it is still right.
QList<quint32> loadSnapshot(HWND hwnd, const QRect& frame) {
	QImage image;
	if (!image.load(snapshotFile(hwnd), "PNG")) return {};
	if (image.text(QStringLiteral("frame")) != frameText(frame)) return {};
	if (image.text(QStringLiteral("pid")) != QString::number(processOf(hwnd))) return {};
	image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	if (image.size() != frame.size()) return {};
	QList<quint32> pixels(static_cast<qsizetype>(frame.width()) * frame.height());
	for (auto y = 0; y < frame.height(); y++) {
		std::memcpy(pixels.data() + static_cast<qsizetype>(y) * frame.width(), image.constScanLine(y), sizeof(quint32) * frame.width());
	}
	return pixels;
}

// Stills of windows that are gone.
void pruneSnapshotFiles() {
	for (const auto& name: QDir(snapshotDir()).entryList({QStringLiteral("*.png"), QStringLiteral("*.tmp")}, QDir::Files)) {
		bool ok = false;
		auto value = name.section('.', 0, 0).toULongLong(&ok, 16);
		if (!ok || !IsWindow(reinterpret_cast<HWND>(static_cast<quintptr>(value)))) { // NOLINT
			QFile::remove(snapshotDir() + '/' + name);
		}
	}
}
constexpr LONGLONG OFF_DISPLAY_MIN_BYTES = 32LL * 1024 * 1024;

// end-4's menu_decel curve for the workspaces animation.
constexpr double CURVE_X1 = 0.1;
constexpr double CURVE_Y1 = 1.0;
constexpr double CURVE_X2 = 0.0;
constexpr double CURVE_Y2 = 1.0;

double bezier(double a, double b, double t) {
	auto u = 1.0 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

double ease(double x) {
	auto lo = 0.0;
	auto hi = 1.0;
	for (auto i = 0; i < 40; i++) {
		auto mid = (lo + hi) / 2;
		if (bezier(CURVE_X1, CURVE_X2, mid) < x) lo = mid;
		else hi = mid;
	}
	return bezier(CURVE_Y1, CURVE_Y2, (lo + hi) / 2);
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	// Until the switch is committed the windows under the cover are the old
	// desktop's, so clicks stop here. Afterwards the cover lets them through.
	case WM_NCHITTEST: return HTCLIENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATEANDEAT;
	case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
	case WM_PAINT: {
		PAINTSTRUCT paint {};
		auto* dc = BeginPaint(hwnd, &paint);
		FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
		EndPaint(hwnd, &paint);
		return 0;
	}
	default: return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

HWND createOverlay(const QRect& rect) {
	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &overlayProc;
		cls.hInstance = instance;
		cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		cls.lpszClassName = OVERLAY_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	return CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
	    OVERLAY_CLASS,
	    L"",
	    WS_POPUP,
	    rect.x(),
	    rect.y(),
	    rect.width(),
	    rect.height(),
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
}

QRect rectOf(HWND hwnd) {
	RECT r {};
	if (!GetWindowRect(hwnd, &r)) return {};
	return {r.left, r.top, r.right - r.left, r.bottom - r.top};
}

bool topmost(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

QList<HWND> topLevelWindows() {
	QList<HWND> list;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (IsWindowVisible(hwnd)) reinterpret_cast<QList<HWND>*>(param)->append(hwnd);
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&list)
	);
	return list;
}

bool ownWindow(HWND hwnd) {
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	return pid == GetCurrentProcessId();
}

// A tiled window can carry a region that clips it to its tile; the thumbnail
// ignores it, so the source is cut to the region's bounds instead.
QRect sourceOf(HWND hwnd, const QRect& rect) {
	RECT box {};
	auto kind = GetWindowRgnBox(hwnd, &box);
	if (kind == SIMPLEREGION || kind == COMPLEXREGION) {
		return {box.left, box.top, box.right - box.left, box.bottom - box.top};
	}
	return {0, 0, rect.width(), rect.height()};
}

// Wait for the next compositor frame, the way wiri paces its animations
// (MIT, jolionlands/wiri: src/animation/clock.rs): Windows 11's compositor
// clock when dcomp.dll exports it, DwmFlush otherwise.
void waitForFrame() {
	using WaitFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
	static auto wait = []() -> WaitFn {
		auto* dcomp = LoadLibraryW(L"dcomp.dll");
		if (dcomp == nullptr) return nullptr;
		return reinterpret_cast<WaitFn>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
	}();
	if (wait != nullptr && wait(0, nullptr, 100) != WAIT_FAILED) return;
	DwmFlush();
}

double refreshPeriodMs() {
	DWM_TIMING_INFO timing {.cbSize = sizeof(DWM_TIMING_INFO)};
	LARGE_INTEGER frequency {};
	if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0
	    && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
	{
		return static_cast<double>(timing.qpcRefreshPeriod) * 1000.0 / static_cast<double>(frequency.QuadPart);
	}
	return 1000.0 / 60.0;
}

// The desktop window repaints its wallpaper when the desktop changes and shows
// white for a frame, so the backdrop is a still copy put in place on show.
// Browsers and video players keep a video surface that makes their live
// thumbnail expensive for the compositor, even when nothing plays. They slide
// as still copies; everything else stays live.
// Processes that render on a GPU other than the one driving the display. The
// compositor copies each of their frames across GPUs, which makes a live
// thumbnail of them as costly as a video. Refreshed off the GUI thread.
struct OffDisplay {
	std::mutex mutex;
	// Executables rather than processes: Chromium and Firefox render in a GPU
	// process of their own, not the one that owns the window.
	QSet<QString> executables;
};

OffDisplay& offDisplay() {
	static OffDisplay state; // NOLINT
	return state;
}

bool displayLuid(LUID& luid) {
	UINT32 pathCount = 0;
	UINT32 modeCount = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)
	    != ERROR_SUCCESS)
	{
		return false;
	}
	for (UINT32 i = 0; i < pathCount; i++) {
		auto index = paths[i].sourceInfo.modeInfoIdx;
		if (index >= modeCount || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
		const auto& position = modes[index].sourceMode.position;
		if (position.x != 0 || position.y != 0) continue;
		luid = paths[i].sourceInfo.adapterId;
		return true;
	}
	return false;
}

// Reads the "GPU Process Memory" counters that Task Manager shows: a process
// holding video memory on another adapter renders there. pdh.dll is loaded at
// run time, so nothing links to it.
QSet<QString> scanOffDisplayProcesses() {
	QSet<QString> found;
	LUID display {};
	if (!displayLuid(display)) return found;

	using OpenQuery = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
	using AddCounter = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
	using Collect = PDH_STATUS(WINAPI*)(PDH_HQUERY);
	using GetArray = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
	using CloseQuery = PDH_STATUS(WINAPI*)(PDH_HQUERY);
	static auto* pdh = LoadLibraryW(L"pdh.dll");
	if (pdh == nullptr) return found;
	auto resolve = [](const char* name) { return reinterpret_cast<void*>(GetProcAddress(pdh, name)); };
	auto open = reinterpret_cast<OpenQuery>(resolve("PdhOpenQueryW"));
	auto add = reinterpret_cast<AddCounter>(resolve("PdhAddEnglishCounterW"));
	auto collect = reinterpret_cast<Collect>(resolve("PdhCollectQueryData"));
	auto getArray = reinterpret_cast<GetArray>(resolve("PdhGetFormattedCounterArrayW"));
	auto close = reinterpret_cast<CloseQuery>(resolve("PdhCloseQuery"));
	if (open == nullptr || add == nullptr || collect == nullptr || getArray == nullptr || close == nullptr) {
		return found;
	}

	PDH_HQUERY query = nullptr;
	if (open(nullptr, 0, &query) != ERROR_SUCCESS) return found;
	PDH_HCOUNTER counter = nullptr;
	if (add(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &counter) == ERROR_SUCCESS
	    && collect(query) == ERROR_SUCCESS)
	{
		DWORD size = 0;
		DWORD count = 0;
		if (getArray(counter, PDH_FMT_LARGE, &size, &count, nullptr) == static_cast<PDH_STATUS>(0x800007D2L)) {
			std::vector<std::byte> buffer(size);
			auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
			if (getArray(counter, PDH_FMT_LARGE, &size, &count, items) == ERROR_SUCCESS) {
				for (DWORD i = 0; i < count; i++) {
					if (items[i].FmtValue.largeValue < OFF_DISPLAY_MIN_BYTES) continue;
					unsigned pid = 0;
					unsigned high = 0;
					unsigned low = 0;
					if (swscanf_s(items[i].szName, L"pid_%u_luid_0x%x_0x%x", &pid, &high, &low) != 3) continue;
					if (low == display.LowPart && static_cast<LONG>(high) == display.HighPart) continue;
					auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
					if (process == nullptr) continue;
					wchar_t path[MAX_PATH] {};
					DWORD length = MAX_PATH;
					if (QueryFullProcessImageNameW(process, 0, path, &length)) {
						found.insert(QString::fromWCharArray(path, static_cast<int>(length)).toLower());
					}
					CloseHandle(process);
				}
			}
		}
	}
	close(query);
	return found;
}

bool heavyWindow(const TrackedWindow* window) {
	static const QStringList names = {
	    QStringLiteral("zen.exe"),      QStringLiteral("firefox.exe"),  QStringLiteral("librewolf.exe"),
	    QStringLiteral("floorp.exe"),   QStringLiteral("waterfox.exe"), QStringLiteral("chrome.exe"),
	    QStringLiteral("msedge.exe"),   QStringLiteral("brave.exe"),    QStringLiteral("opera.exe"),
	    QStringLiteral("vivaldi.exe"),  QStringLiteral("chromium.exe"), QStringLiteral("thorium.exe"),
	    QStringLiteral("mpv.exe"),      QStringLiteral("vlc.exe"),      QStringLiteral("mpc-hc64.exe"),
	    QStringLiteral("mpc-be64.exe"), QStringLiteral("potplayermini64.exe"),
	};
	{
		auto& state = offDisplay();
		auto lock = std::lock_guard(state.mutex);
		if (state.executables.contains(window->exePath().toLower())) return true;
	}
	auto path = window->exePath();
	auto name = path.mid(path.lastIndexOf(QLatin1Char('\\')) + 1).toLower();
	if (names.contains(name)) return true;

	// Every Chromium-based app (Electron: Discord, Spotify, VS Code, Slack...)
	// stops painting while its desktop is hidden, just like the browsers, so
	// its live thumbnail is empty on the way in. They share a window class.
	wchar_t cls[64] {};
	GetClassNameW(window->hwnd(), cls, 64);
	return wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0;
}

HWND createSnapshotHost(int x, int width, int height) {
	static auto registered = false; // NOLINT
	auto* instance = GetModuleHandleW(nullptr);
	if (!registered) {
		WNDCLASSEXW cls {};
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = &DefWindowProcW;
		cls.hInstance = instance;
		cls.lpszClassName = SNAPSHOT_CLASS;
		registered = RegisterClassExW(&cls) != 0;
	}
	auto* host = CreateWindowExW(
	    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
	    SNAPSHOT_CLASS,
	    L"",
	    WS_POPUP,
	    x,
	    0,
	    width,
	    height,
	    nullptr,
	    nullptr,
	    instance,
	    nullptr
	);
	if (host == nullptr) return nullptr;
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(host, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
	return host;
}

RECT toRect(const QRect& rect) { return {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1}; }

} // namespace

DesktopSlide* DesktopSlide::instance() {
	static QPointer<DesktopSlide> slide; // NOLINT
	if (slide.isNull()) slide = new DesktopSlide(QCoreApplication::instance());
	return slide.data();
}

DesktopSlide::DesktopSlide(QObject* parent): QObject(parent) {
	this->warmTimer.setSingleShot(true);
	this->warmTimer.setInterval(WARM_DELAY_MS);
	QObject::connect(&this->warmTimer, &QTimer::timeout, this, &DesktopSlide::warmUp);
	QObject::connect(VirtualDesktops::instance(), &VirtualDesktops::currentChanged, this, [this]() {
		this->warmTimer.start();
	});
	this->warmTimer.start();
}

DesktopSlide::~DesktopSlide() { this->finish(); }

bool DesktopSlide::begin(qsizetype from, qsizetype to) {
	if (to < 0) return false;
	if (this->overlay != nullptr) this->retarget(to);
	else if (from >= 0 && from != to) this->start(from, to);
	return this->overlay != nullptr;
}

bool DesktopSlide::start(qsizetype from, qsizetype to) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr) return false;

	auto any = false;
	for (auto* window: tracker->windows()) {
		auto desktop = window->desktop();
		if (!window->minimized() && (desktop == from || desktop == to)) any = true;
	}
	if (!any) return false;

	POINT cursor {};
	GetCursorPos(&cursor);
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &info)) return false;
	const auto& m = info.rcMonitor;
	this->monitor = QRect(m.left, m.top, m.right - m.left, m.bottom - m.top);

	this->overlay = createOverlay(this->monitor);
	if (this->overlay == nullptr) return false;
	BOOL noTransitions = TRUE;
	DwmSetWindowAttribute(this->overlay, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
	auto dpr = GetDpiForWindow(this->overlay) / 96.0;
	this->travel = this->monitor.width() + static_cast<int>(std::lround(WORKSPACE_GAP * dpr));

	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (hwnd == this->overlay || topmost(hwnd)) continue;
		// The desktop hosts the wallpaper and the shell's embedded widgets: it
		// stays live over the still copy so neither freezes during the slide.
		if (!ownWindow(hwnd) && hwnd != GetShellWindow()) continue;
		auto rect = rectOf(hwnd);
		if (!rect.contains(this->monitor)) continue;

		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;
		auto local = rect.translated(-this->monitor.topLeft());
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY
		              | DWM_TNP_SOURCECLIENTAREAONLY;
		props.fVisible = TRUE;
		props.opacity = 255;
		props.rcDestination = toRect(local);
		DwmUpdateThumbnailProperties(thumbnail, &props);
		this->thumbs.append({thumbnail, local, QRect(), -1});
	}

	this->pruneSnapshots();
	this->prepareBackdrop();
	this->bases.insert(from, 0);
	this->target = from;
	this->cameraFrom = 0;
	this->cameraTo = 0;
	this->addDesktop(from, true);
	this->retarget(to);

	WinPanelWindow::setCoverActive(true);
	this->raised = WinPanelWindow::raiseDemoted();
	{
		// The first frame on screen must already match what was there.
		auto lock = std::lock_guard(this->mutex);
		this->layoutThumbs(0);
	}
	this->placeUnderPanels();
	ShowWindow(this->overlay, SW_SHOWNOACTIVATE);
	DwmFlush();
	{
		// The slide starts on the first frame the cover is on screen; the setup
		// before it must not eat into end-4's front-loaded curve.
		auto lock = std::lock_guard(this->mutex);
		this->startedAt = std::chrono::steady_clock::now();
	}

	this->stop.store(false);
	this->settleRetries = 0;
	this->worker = std::thread(&DesktopSlide::run, this);
	return true;
}

void DesktopSlide::retarget(qsizetype to) {
	{
		auto lock = std::lock_guard(this->mutex);
		if (to == this->target) return;

		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration<double, std::milli>(now - this->startedAt).count();
		auto progress = this->cameraFrom == this->cameraTo ? 1.0 : std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		// Like Hyprland, only the desktop under the camera and the requested one
		// sit side by side: the new one goes right next to what is on screen, and
		// any other desktop on the way between them leaves the strip.
		auto here = this->target;
		auto nearest = std::numeric_limits<double>::max();
		for (auto iter = this->bases.cbegin(); iter != this->bases.cend(); ++iter) {
			auto distance = std::abs(iter.value() - camera);
			if (distance < nearest) {
				nearest = distance;
				here = iter.key();
			}
		}
		auto base = to == here ? this->bases.value(here)
		                       : this->bases.value(here) + (to > here ? this->travel : -this->travel);
		auto low = std::min(camera, static_cast<double>(base));
		auto high = std::max(camera, static_cast<double>(base));
		for (auto iter = this->bases.begin(); iter != this->bases.end();) {
			auto key = iter.key();
			auto value = static_cast<double>(iter.value());
			auto inTheWay = key != here && key != to && value >= low && value <= high;
			if (!inTheWay) {
				++iter;
				continue;
			}
			for (auto i = this->thumbs.size() - 1; i >= 0; i--) {
				if (this->thumbs[i].desktop != key) continue;
				DwmUnregisterThumbnail(this->thumbs[i].thumbnail);
				this->thumbs.removeAt(i);
			}
			iter = this->bases.erase(iter);
		}
		this->bases.insert(to, base);

		this->cameraFrom = camera;
		this->cameraTo = this->bases.value(to);
		this->startedAt = now;
		this->target = to;
		this->settledSent = false;
		this->commitSent = false;
	}
	this->setClickThrough(false);
	this->settleRetries = 0;
	this->addDesktop(to, false);
}

void DesktopSlide::addDesktop(qsizetype desktop, bool capture) {
	auto* tracker = WindowTracker::instance();
	if (tracker == nullptr || this->overlay == nullptr) return;

	{
		auto lock = std::lock_guard(this->mutex);
		for (const auto& thumb: this->thumbs) {
			if (thumb.desktop == desktop) return;
		}
	}

	QList<Thumb> added;
	auto windows = topLevelWindows();
	for (auto i = windows.size() - 1; i >= 0; i--) {
		auto* hwnd = windows[i];
		if (ownWindow(hwnd)) continue;
		auto* window = tracker->windowFor(hwnd);
		// Special windows are pinned to every desktop; they leave with special.
		if (Scratchpad::holds(window)) continue;
		if (window == nullptr || window->minimized() || window->desktop() != desktop) continue;
		auto rect = rectOf(hwnd);
		if (!rect.intersects(this->monitor)) continue;

		auto source = sourceOf(hwnd, rect);
		auto local = source.translated(rect.topLeft() - this->monitor.topLeft());

		auto frame = local;
		RECT bounds {};
		if (source.size() == rect.size()
		    && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
		{
			frame = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top)
			            .translated(-this->monitor.topLeft());
		}
		auto heavy = heavyWindow(window);
		auto snapshot = capture && heavy ? this->takeSnapshot(hwnd, frame) : false;
		// A browser on another desktop is cloaked and paints nothing until the
		// real switch, so its live thumbnail is a black page. Any copy taken
		// from the screen beats that, even one from before tiling moved or
		// resized the window: it is stretched to where the window is now.
		if (!snapshot && heavy) {
			auto known = this->snapshots.constFind(hwnd);
			snapshot = known != this->snapshots.constEnd() && IsWindow(known->host) && !known->frame.isEmpty();
		}
		if (snapshot) {
			const auto& copy = this->snapshots.value(hwnd);
			auto copied = QRect(QPoint(), copy.frame.size());
			HTHUMBNAIL thumbnail = nullptr;
			if (SUCCEEDED(DwmRegisterThumbnail(this->overlay, copy.host, &thumbnail))) {
				DWM_THUMBNAIL_PROPERTIES props {};
				props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE;
				props.rcSource = {0, 0, copied.width(), copied.height()};
				props.opacity = 255;
				props.fVisible = FALSE;
				DwmUpdateThumbnailProperties(thumbnail, &props);
				added.append({thumbnail, frame, copied, desktop});
				continue;
			}
		}

		HTHUMBNAIL thumbnail = nullptr;
		if (FAILED(DwmRegisterThumbnail(this->overlay, hwnd, &thumbnail))) continue;

		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE;
		props.rcSource = toRect(source);
		props.opacity = 255;
		props.fVisible = FALSE;
		DwmUpdateThumbnailProperties(thumbnail, &props);
		added.append({thumbnail, local, source, desktop});
	}

	auto lock = std::lock_guard(this->mutex);
	this->thumbs.append(added);
}

void DesktopSlide::run() {
	auto last = std::chrono::steady_clock::now();
	auto period = refreshPeriodMs();
	while (!this->stop.load()) {
		// The clock can wake early; never step faster than the display refreshes.
		do {
			waitForFrame();
		} while (!this->stop.load()
		         && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - last).count()
		                < period * 0.75);
		last = std::chrono::steady_clock::now();
		auto lock = std::lock_guard(this->mutex);
		auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - this->startedAt).count();
		auto progress = std::clamp(elapsed / SLIDE_MS, 0.0, 1.0);
		auto camera = this->cameraFrom + (this->cameraTo - this->cameraFrom) * ease(progress);

		this->layoutThumbs(camera);

		if ((elapsed >= COMMIT_MS || progress >= 1.0) && !this->commitSent) {
			this->commitSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onCommit, Qt::QueuedConnection);
		}
		if (progress >= 1.0 && !this->settledSent) {
			this->settledSent = true;
			QMetaObject::invokeMethod(this, &DesktopSlide::onSettled, Qt::QueuedConnection);
		}
	}
}

// Called with the mutex held.
void DesktopSlide::layoutThumbs(double camera) {
	for (const auto& thumb: this->thumbs) {
		if (thumb.desktop < 0) continue;
		auto offset = static_cast<int>(std::lround(this->bases.value(thumb.desktop) - camera));
		auto rect = thumb.rect.translated(offset, 0);
		DWM_THUMBNAIL_PROPERTIES props {};
		props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE;
		props.fVisible = rect.intersects(QRect(QPoint(), this->monitor.size())) ? TRUE : FALSE;
		props.rcDestination = toRect(rect);
		DwmUpdateThumbnailProperties(thumb.thumbnail, &props);
	}
}

void DesktopSlide::onCommit() {
	{
		auto lock = std::lock_guard(this->mutex);
		if (!this->commitSent) return;
	}
	VirtualDesktops::instance()->commitSwitch();
	this->setClickThrough(true);
}

void DesktopSlide::setClickThrough(bool through) {
	if (this->overlay == nullptr) return;
	auto style = GetWindowLongPtrW(this->overlay, GWL_EXSTYLE);
	auto wanted = through ? style | WS_EX_TRANSPARENT : style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
	if (wanted != style) SetWindowLongPtrW(this->overlay, GWL_EXSTYLE, wanted);
}

void DesktopSlide::onSettled() {
	qsizetype target = -1;
	{
		auto lock = std::lock_guard(this->mutex);
		if (!this->settledSent) return;
		target = this->target;
	}
	VirtualDesktops::instance()->commitSwitch();

	auto* desktops = VirtualDesktops::instance();
	if (desktops->currentIndex() != target && this->settleRetries++ < SETTLE_RETRIES) {
		QTimer::singleShot(SETTLE_RETRY_MS, this, &DesktopSlide::onSettled);
		return;
	}

	// Keystrokes a moment apart keep the same cover instead of rebuilding it.
	QTimer::singleShot(HOLD_MS, this, [this, target]() {
		{
			auto lock = std::lock_guard(this->mutex);
			if (!this->settledSent || this->target != target) return;
		}
		this->finish();
	});
}

// Panels are left alone: repainting their full-screen transparent windows to
// restack them can flash a frame. The cover slides in just under the lowest.
void DesktopSlide::placeUnderPanels() {
	HWND lowest = nullptr;
	for (auto* hwnd: topLevelWindows()) {
		if (hwnd == this->overlay || !ownWindow(hwnd) || !topmost(hwnd)) continue;
		lowest = hwnd;
	}
	SetWindowPos(
	    this->overlay,
	    lowest != nullptr ? lowest : HWND_TOPMOST,
	    0,
	    0,
	    0,
	    0,
	    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
}

void DesktopSlide::prepareBackdrop() {
	auto* screen = GetDC(nullptr);
	auto width = this->monitor.width();
	auto height = this->monitor.height();
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* bits = nullptr;
	this->wallBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	this->wallBits = static_cast<quint32*>(bits);
	this->wallDc = CreateCompatibleDC(screen);
	SelectObject(this->wallDc, this->wallBitmap);

	RECT fill {0, 0, width, height};
	FillRect(this->wallDc, &fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
	if (auto* desktop = GetShellWindow()) {
		RECT r {};
		GetWindowRect(desktop, &r);
		auto* full = CreateCompatibleDC(screen);
		auto* fullBitmap = CreateCompatibleBitmap(screen, r.right - r.left, r.bottom - r.top);
		auto* fullPrevious = SelectObject(full, fullBitmap);
		if (PrintWindow(desktop, full, PW_RENDERFULLCONTENT)) {
			BitBlt(this->wallDc, 0, 0, width, height, full, this->monitor.x() - r.left, this->monitor.y() - r.top, SRCCOPY);
		}
		SelectObject(full, fullPrevious);
		DeleteObject(fullBitmap);
		DeleteDC(full);
	}
	GdiFlush();

	POINT origin {this->monitor.x(), this->monitor.y()};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, 0};
	UpdateLayeredWindow(this->overlay, screen, &origin, &size, this->wallDc, &source, 0, &blend, ULW_OPAQUE);
	ReleaseDC(nullptr, screen);
}

void DesktopSlide::releaseBackdrop() {
	if (this->wallDc != nullptr) DeleteDC(this->wallDc);
	if (this->wallBitmap != nullptr) DeleteObject(this->wallBitmap);
	this->wallDc = nullptr;
	this->wallBitmap = nullptr;
	this->wallBits = nullptr;
}

// A live thumbnail of a window that draws video, even paused, costs the
// compositor every frame. A window on screen when the slide starts is copied
// once instead, and the copy is kept to slide it back in next time if the
// window has not moved. Corners left showing the wallpaper are cut away by
// comparing with the backdrop, whatever radius rounds them.
bool DesktopSlide::takeSnapshot(HWND hwnd, const QRect& frame) {
	auto screenRect = QRect(QPoint(), this->monitor.size());
	if (this->wallBits == nullptr || !screenRect.contains(frame) || frame.isEmpty()) return false;

	auto width = frame.width();
	auto height = frame.height();
	auto& snapshot = this->snapshots[hwnd];
	if (snapshot.host != nullptr && snapshot.frame.size() != frame.size()) {
		DestroyWindow(snapshot.host);
		snapshot.host = nullptr;
	}
	auto parkX = GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 100;
	if (snapshot.host == nullptr) {
		snapshot.host = createSnapshotHost(parkX, width, height);
		if (snapshot.host == nullptr) {
			this->snapshots.remove(hwnd);
			return false;
		}
	}

	auto* screen = GetDC(nullptr);
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* raw = nullptr;
	auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
	auto* dc = CreateCompatibleDC(screen);
	SelectObject(dc, bitmap);
	BitBlt(dc, 0, 0, width, height, screen, this->monitor.x() + frame.x(), this->monitor.y() + frame.y(), SRCCOPY);
	GdiFlush();

	auto* bits = static_cast<quint32*>(raw);
	auto zone = std::min({CORNER_ZONE, width / 2, height / 2});
	auto wallWidth = this->monitor.width();
	auto close = [](quint32 a, quint32 b) {
		auto channel = [](quint32 v, int shift) { return static_cast<int>((v >> shift) & 0xff); };
		return std::abs(channel(a, 16) - channel(b, 16)) <= CORNER_TOLERANCE
		    && std::abs(channel(a, 8) - channel(b, 8)) <= CORNER_TOLERANCE
		    && std::abs(channel(a, 0) - channel(b, 0)) <= CORNER_TOLERANCE;
	};
	for (auto y = 0; y < height; y++) {
		auto cornerRow = y < zone || y >= height - zone;
		for (auto x = 0; x < width; x++) {
			auto& p = bits[y * width + x];
			auto cornerColumn = x < zone || x >= width - zone;
			if (cornerRow && cornerColumn && close(p, this->wallBits[(frame.y() + y) * wallWidth + frame.x() + x])) {
				p = 0;
			} else {
				p |= 0xff000000u;
			}
		}
	}

	// Copied for the disk while the bitmap is still alive.
	auto still = QImage(static_cast<const uchar*>(raw), width, height, QImage::Format_ARGB32_Premultiplied).copy();
	POINT origin {parkX, 0};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(snapshot.host, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
	DeleteDC(dc);
	DeleteObject(bitmap);
	ReleaseDC(nullptr, screen);
	ShowWindow(snapshot.host, SW_SHOWNOACTIVATE);
	snapshot.frame = frame.translated(this->monitor.topLeft());
	saveSnapshot(hwnd, snapshot.frame, std::move(still));
	return true;
}

// Right after the shell starts no copy is kept yet, so the first slide to each
// desktop would fall back to live browser thumbnails. A moment after the
// desktops settle, browsers on the other desktops are asked to paint
// themselves once on a worker thread; the screen copy taken when leaving a
// desktop later replaces this with exact corners.
void DesktopSlide::warmUp() {
	if (this->overlay != nullptr) {
		this->warmTimer.start();
		return;
	}
	QPointer<DesktopSlide> self = this;
	std::thread([self]() {
		pruneSnapshotFiles();
		auto pids = scanOffDisplayProcesses();
		{
			auto& state = offDisplay();
			auto lock = std::lock_guard(state.mutex);
			state.executables = pids;
		}
		if (self.isNull()) return;
		QMetaObject::invokeMethod(
		    self.data(),
		    [self]() {
			    if (!self.isNull()) self->warmCapture();
		    },
		    Qt::QueuedConnection
		);
	}).detach();
}

void DesktopSlide::warmCapture() {
	if (this->overlay != nullptr) return;
	auto* tracker = WindowTracker::instance();
	auto* desktops = VirtualDesktops::instance();
	if (tracker == nullptr || desktops == nullptr) return;
	this->pruneSnapshots();

	for (auto* window: tracker->windows()) {
		if (window->minimized() || window->desktop() < 0 || window->desktop() == desktops->currentIndex()) continue;
		if (!heavyWindow(window)) continue;
		auto* hwnd = window->hwnd();
		RECT bounds {};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) continue;
		auto frame = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
		auto known = this->snapshots.constFind(hwnd);
		if (known != this->snapshots.constEnd() && known->frame == frame) continue;
		if (frame.isEmpty()) continue;

		auto radius = static_cast<int>(std::lround(CORNER_RADIUS * GetDpiForWindow(hwnd) / 96.0));
		QPointer<DesktopSlide> self = this;
		std::thread([self, hwnd, frame, radius]() {
			RECT raw {};
			if (!GetWindowRect(hwnd, &raw)) return;
			auto width = raw.right - raw.left;
			auto height = raw.bottom - raw.top;
			auto left = frame.x() - raw.left;
			auto top = frame.y() - raw.top;
			if (width <= 0 || height <= 0 || left < 0 || top < 0 || left + frame.width() > width
			    || top + frame.height() > height)
			{
				return;
			}

			auto* screen = GetDC(nullptr);
			BITMAPINFO info {};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = width;
			info.bmiHeader.biHeight = -height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			void* bits = nullptr;
			auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			auto* dc = CreateCompatibleDC(screen);
			ReleaseDC(nullptr, screen);
			SelectObject(dc, bitmap);
			auto painted = PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT) != FALSE;
			GdiFlush();

			QList<quint32> pixels;
			if (painted) {
				auto w = frame.width();
				auto h = frame.height();
				pixels.resize(static_cast<qsizetype>(w) * h);
				auto* source = static_cast<const quint32*>(bits);
				auto r = static_cast<double>(radius);
				for (auto y = 0; y < h; y++) {
					for (auto x = 0; x < w; x++) {
						auto p = source[static_cast<qsizetype>(top + y) * width + left + x] | 0xff000000u;
						auto cx = x < r ? r - x - 0.5 : (x >= w - r ? x - (w - r) + 0.5 : 0.0);
						auto cy = y < r ? r - y - 0.5 : (y >= h - r ? y - (h - r) + 0.5 : 0.0);
						if (cx > 0 && cy > 0 && std::sqrt(cx * cx + cy * cy) > r) p = 0;
						pixels[static_cast<qsizetype>(y) * w + x] = p;
					}
				}
			}
			DeleteDC(dc);
			DeleteObject(bitmap);
			if (pixels.isEmpty() || self.isNull()) return;

			// A browser on another desktop is cloaked and stops painting, so
			// PrintWindow hands back its frame with a black page. Like wiri,
			// keep only copies with something in the interior (a dark page is
			// not pure black); without one the window slides live, and its copy
			// comes from the screen the next time it is left.
			{
				auto w = frame.width();
				auto h = frame.height();
				// Nor a flat page of any colour: Electron hands back an empty
				// dark grey page when cloaked, which is as blank as black.
				qsizetype samples = 0;
				qsizetype lit = 0;
				double sum = 0;
				double squares = 0;
				for (auto y = h / 4; y < h - h / 8; y += 8) {
					for (auto x = w / 8; x < w - w / 8; x += 8) {
						auto p = pixels[static_cast<qsizetype>(y) * w + x];
						auto brightest = std::max({(p >> 16) & 0xffu, (p >> 8) & 0xffu, p & 0xffu});
						samples++;
						if (brightest > 3) lit++;
						sum += brightest;
						squares += static_cast<double>(brightest) * brightest;
					}
				}
				auto mean = samples > 0 ? sum / static_cast<double>(samples) : 0.0;
				auto spread = samples > 0 ? std::sqrt(std::max(0.0, squares / static_cast<double>(samples) - mean * mean)) : 0.0;
				if (samples > 0 && (lit * 100 < samples * 2 || spread < 4.0)) pixels.clear();
			}
			auto fromDisk = pixels.isEmpty();
			if (fromDisk) pixels = loadSnapshot(hwnd, frame);
			if (pixels.isEmpty() || self.isNull()) return;

			QMetaObject::invokeMethod(
			    self.data(),
			    [self, hwnd, frame, pixels, fromDisk]() {
				    if (!self.isNull() && self->overlay == nullptr) self->storeSnapshot(hwnd, frame, pixels, !fromDisk);
			    },
			    Qt::QueuedConnection
			);
		}).detach();
	}
}

void DesktopSlide::storeSnapshot(HWND hwnd, const QRect& frame, const QList<quint32>& pixels, bool persist) {
	if (!IsWindow(hwnd)) return;
	auto width = frame.width();
	auto height = frame.height();
	auto& snapshot = this->snapshots[hwnd];
	if (snapshot.host != nullptr && snapshot.frame.size() != frame.size()) {
		DestroyWindow(snapshot.host);
		snapshot.host = nullptr;
	}
	auto parkX = GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 100;
	if (snapshot.host == nullptr) {
		snapshot.host = createSnapshotHost(parkX, width, height);
		if (snapshot.host == nullptr) {
			this->snapshots.remove(hwnd);
			return;
		}
	}

	auto* screen = GetDC(nullptr);
	BITMAPINFO info {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void* raw = nullptr;
	auto* bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
	auto* dc = CreateCompatibleDC(screen);
	SelectObject(dc, bitmap);
	std::memcpy(raw, pixels.constData(), sizeof(quint32) * pixels.size());

	POINT origin {parkX, 0};
	SIZE size {width, height};
	POINT source {0, 0};
	BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
	UpdateLayeredWindow(snapshot.host, screen, &origin, &size, dc, &source, 0, &blend, ULW_ALPHA);
	DeleteDC(dc);
	DeleteObject(bitmap);
	ReleaseDC(nullptr, screen);
	ShowWindow(snapshot.host, SW_SHOWNOACTIVATE);
	snapshot.frame = frame;
	if (persist) {
		saveSnapshot(hwnd, frame, QImage(reinterpret_cast<const uchar*>(pixels.constData()), width, height, QImage::Format_ARGB32_Premultiplied).copy()); // NOLINT
	}
}

void DesktopSlide::pruneSnapshots() {
	for (auto iter = this->snapshots.begin(); iter != this->snapshots.end();) {
		if (IsWindow(iter.key()) && IsWindow(iter->host)) {
			++iter;
			continue;
		}
		if (iter->host != nullptr) DestroyWindow(iter->host);
		if (!IsWindow(iter.key())) QFile::remove(snapshotFile(iter.key()));
		iter = this->snapshots.erase(iter);
	}
}

void DesktopSlide::finish() {
	this->stop.store(true);
	if (this->worker.joinable()) this->worker.join();
	for (const auto& thumb: this->thumbs) DwmUnregisterThumbnail(thumb.thumbnail);
	this->thumbs.clear();
	this->bases.clear();
	this->target = -1;
	this->settledSent = false;
	this->commitSent = false;
	this->releaseBackdrop();
	if (this->overlay == nullptr) return;
	DestroyWindow(this->overlay);
	this->overlay = nullptr;
	WinPanelWindow::setCoverActive(false);
	WinPanelWindow::restack(this->raised);
	this->raised.clear();
}

void waitForCompositorFrame() { waitForFrame(); }
double compositorRefreshMs() { return refreshPeriodMs(); }

} // namespace qs::windows
