#include "tiling.hpp"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include <qpointer.h>

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qsavefile.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>

#include "dwindle_scene.hpp"
#include "focus_on_activate.hpp"
#include "panel_window.hpp"
#include "scratchpad.hpp"
#include "super_drag.hpp"
#include "tiling_target.hpp"
#include "startup.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_decorations.hpp"
#include "window_motion.hpp"
#include "window_tracker.hpp"

#include <dwmapi.h>

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logTiling, "quickshell.windows.tiling", QtWarningMsg);

std::atomic<bool> gRestoreWindowShadows {false};

void setWindowShadowsSuppressed(bool suppress) {
    if (suppress) {
        if (gRestoreWindowShadows.load()) return;
        BOOL enabled = FALSE;
        if (!SystemParametersInfoW(SPI_GETDROPSHADOW, 0, &enabled, 0)) {
            qCWarning(logTiling) << "Failed to read Windows window shadows" << GetLastError();
            return;
        }
        if (!enabled) return;
        if (!SystemParametersInfoW(SPI_SETDROPSHADOW, 0, nullptr, SPIF_SENDCHANGE)) {
            qCWarning(logTiling) << "Failed to disable Windows window shadows" << GetLastError();
            return;
        }
        gRestoreWindowShadows.store(true);
    } else if (gRestoreWindowShadows.exchange(false)) {
        if (!SystemParametersInfoW(SPI_SETDROPSHADOW, 0, reinterpret_cast<PVOID>(UINT_PTR {1}), SPIF_SENDCHANGE)) {
            gRestoreWindowShadows.store(true);
            qCWarning(logTiling) << "Failed to restore Windows window shadows" << GetLastError();
        }
    }
}

using tiling::Edge;

void setFullscreenLook(HWND hwnd, bool fullscreen);
enum class LeftMark : quint8 { None, Fullscreen, FullscreenOverMaximized, Maximized };

void markFullscreen(HWND hwnd, const QRect& raw, LeftMark mark);
void unmarkFullscreen(HWND hwnd);
HANDLE propOf(int value);

bool framelessWindow(HWND hwnd) {
	RECT window {};
	RECT client {};
	return GetWindowRect(hwnd, &window) && GetClientRect(hwnd, &client) && client.right > 0
	    && client.right == window.right - window.left && client.bottom == window.bottom - window.top;
}

bool framelessCover(HWND hwnd) {
	if (IsZoomed(hwnd) || !framelessWindow(hwnd)) return false;
	RECT window {};
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetWindowRect(hwnd, &window)
	    || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
		return false;
	const auto& screen = info.rcMonitor;
	return window.left <= screen.left && window.top <= screen.top && window.right >= screen.right
	    && window.bottom >= screen.bottom;
}

constexpr int SYNC_DELAY_MS = 30;
constexpr int FLOATING_TOLERANCE = 5;
constexpr int NOTICE_MINIMUM = 40;
constexpr int NOTICE_SETTLE_MS = 150;
constexpr double FLOATING_WIDTH = 0.6;
constexpr double FLOATING_HEIGHT = 0.65;
constexpr double FLOATING_MAXIMUM = 0.9;
constexpr int DWINDLE_FIRST_OPENING_INTERVAL_MS = 72;
constexpr int DWINDLE_OPENING_INTERVAL_MS = 28;
constexpr int VERIFY_DELAY_MS = 150;
constexpr int DEADLINE_MS = 2000;
constexpr int HUNG_RETRY_MS = 500;
constexpr int HEAL_MS = 500;
constexpr double DETACHED_SIZE = 0.8489;
constexpr int UNMAXIMIZE_LIMIT = 6;
constexpr qint64 UNMAXIMIZE_RESET_MS = 500;
constexpr int UNMAXIMIZE_SETTLE_MS = 400;
constexpr qint32 MAX_CORRECTIONS = 2;
constexpr qint64 CORRECTION_RESET_MS = 1500;
constexpr int EDGE_TOLERANCE = 2;
constexpr int TINY_CORNER_LIMIT = 80;
constexpr int CHROMIUM_CORNER_LIMIT = 256;
constexpr int SYSTEM_CAPTION_MIN = 8;
constexpr int TINY_CORNER_RADIUS = 12;
constexpr qint32 SPECIAL_GAPS_OUT = 30;
constexpr GUID SPECIAL_DESKTOP = {0x5ec1a100, 0x0000, 0x0000, {0x73, 0x70, 0x65, 0x63, 0x69, 0x61, 0x6c, 0x00}};
constexpr std::size_t TINY_CORNER_SLOTS = 256;
constexpr auto* TINY_CORNER_PROP = L"QuickshellTilingTinyRounded";
constexpr auto* CHROMIUM_CORNER_PROP = L"QuickshellTilingChromiumRounded";

struct TinyCornerSlot {
	std::atomic<HWND> hwnd {nullptr};
	std::atomic<int> originalPreference {0};
	std::atomic<bool> borderSuppressed {false};
};
std::array<TinyCornerSlot, TINY_CORNER_SLOTS> gTinyCornerSlots {}; // NOLINT

void rememberTinyCorners(HWND hwnd, int preference) {
	for (auto& slot: gTinyCornerSlots) {
		if (slot.hwnd.load() == hwnd) return;
	}
	for (auto& slot: gTinyCornerSlots) {
		if (slot.hwnd.load() == nullptr) {
			slot.originalPreference.store(preference);
			slot.borderSuppressed.store(false);
			slot.hwnd.store(hwnd);
			return;
		}
	}
}
void forgetTinyCorners(HWND hwnd) {
	for (auto& slot: gTinyCornerSlots) {
		if (slot.hwnd.load() == hwnd) slot.hwnd.store(nullptr);
	}
}
void rememberTinyBorder(HWND hwnd) {
	for (auto& slot: gTinyCornerSlots) {
		if (slot.hwnd.load() == hwnd) slot.borderSuppressed.store(true);
	}
}

bool markTinyCornerOwner(HWND hwnd, int preference) {
	if (preference < 0 || preference > 3) return false;
	auto value = static_cast<UINT_PTR>(preference + 1);
	return SetPropW(hwnd, TINY_CORNER_PROP, reinterpret_cast<HANDLE>(value)) != FALSE;
}

constexpr UINT_PTR TINY_CORNER_NC_DISABLED = 0x100;

bool restoreChromiumCorners(HWND hwnd, bool keepRound = false) {
	if (RemovePropW(hwnd, CHROMIUM_CORNER_PROP) == nullptr) return false;
	if (keepRound) return true;

	auto current = DWM_WINDOW_CORNER_PREFERENCE {};
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &current, sizeof(current)))
	    && current == DWMWCP_ROUND)
	{
		auto preference = DWMWCP_DEFAULT;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
	}
	return true;
}

void recoverOrphanedTinyCorners() {
	int restored = 0;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (!IsHungAppWindow(hwnd)) restoreChromiumCorners(hwnd);
		    auto saved = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, TINY_CORNER_PROP));
		    auto original = saved & 0xff;
		    if (original < 1 || original > 4 || IsHungAppWindow(hwnd)) return TRUE;
		    if (SetWindowRgn(hwnd, nullptr, TRUE) == 0) return TRUE;
		    if ((saved & TINY_CORNER_NC_DISABLED) != 0) {
			    auto nc = DWMNCRP_USEWINDOWSTYLE;
			    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &nc, sizeof(nc));
		    }
		    auto preference = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(original - 1);
		    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
		    auto border = COLORREF(DWMWA_COLOR_DEFAULT);
		    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
		    RemovePropW(hwnd, TINY_CORNER_PROP);
		    ++*reinterpret_cast<int*>(param); // NOLINT(performance-no-int-to-ptr)
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&restored)
	);
	if (restored > 0) qCInfo(logTiling) << "Recovered" << restored << "stale adaptive window regions.";
}

TilingManager* gManager = nullptr; // NOLINT

int rightOf(const QRect& rect) { return rect.left() + rect.width(); }
int bottomOf(const QRect& rect) { return rect.top() + rect.height(); }

QString windowClass(HWND hwnd) {
	wchar_t buffer[128] {};
	auto length = GetClassNameW(hwnd, buffer, 128);
	return QString::fromWCharArray(buffer, length);
}

bool readFrame(HWND hwnd, QRect& raw, QRect& frame) {
	RECT rawRect {};
	if (!GetWindowRect(hwnd, &rawRect)) return false;

	RECT frameRect {};
	auto hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frameRect, sizeof(frameRect));
	if (FAILED(hr)) frameRect = rawRect;

	raw = toQRect(rawRect);
	frame = toQRect(frameRect);
	return true;
}

QRect currentFrame(HWND hwnd) {
	QRect raw;
	QRect frame;
	readFrame(hwnd, raw, frame);
	return frame;
}

QRect frameWithInsets(const QRect& raw, const QMargins& insets) {
	return QRect(
	    raw.left() + insets.left(),
	    raw.top() + insets.top(),
	    qMax(1, raw.width() - insets.left() - insets.right()),
	    qMax(1, raw.height() - insets.top() - insets.bottom())
	);
}

void setFrame(HWND hwnd, const QRect& target, bool tiled = false, const QMargins* insets = nullptr,
              bool wait = false, bool refreshNativeFrame = false) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	auto left = insets ? insets->left() : frame.left() - raw.left();
	auto top = insets ? insets->top() : frame.top() - raw.top();
	auto right = insets ? insets->right() : rightOf(raw) - rightOf(frame);
	auto bottom = insets ? insets->bottom() : bottomOf(raw) - bottomOf(frame);

	auto flags = UINT(SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
	if (!wait) flags |= SWP_ASYNCWINDOWPOS;
	if (refreshNativeFrame) flags |= SWP_FRAMECHANGED;
	else if (tiled) flags |= SWP_NOSENDCHANGING;
	SetWindowPos(
	    hwnd,
	    nullptr,
	    target.left() - left,
	    target.top() - top,
	    target.width() + left + right,
	    target.height() + top + bottom,
	    flags
	);
}

void setRawRect(HWND hwnd, const QRect& rect, bool wait = false) {
	SetWindowPos(
	    hwnd,
	    nullptr,
	    rect.left(),
	    rect.top(),
	    rect.width(),
	    rect.height(),
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | (wait ? 0 : SWP_ASYNCWINDOWPOS)
	);
}

bool hasSystemCaption(HWND hwnd) {
	RECT window {};
	RECT client {};
	if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client)) return false;
	POINT origin {client.left, client.top};
	if (!ClientToScreen(hwnd, &origin)) return false;
	return origin.y - window.top >= SYSTEM_CAPTION_MIN;
}

QRect fullscreenFrame(HWND hwnd, const QRect& monitor) {
	auto frame = monitor.adjusted(-1, -1, 1, 1);
	if (!hasSystemCaption(hwnd)) return frame;
	RECT bounds {};
	POINT origin {0, 0};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
	    || !ClientToScreen(hwnd, &origin))
	{
		return frame;
	}
	auto caption = origin.y - bounds.top;
	if (caption > 0) frame.setTop(frame.top() - caption);
	return frame;
}

QPoint cursorPos() {
	POINT point {};
	GetCursorPos(&point);
	return {point.x, point.y};
}

QRect physicalMonitor(QScreen* screen) { return monitorRects(monitorForScreen(screen)).monitor; }

qint32 toPhysical(qint32 logical, qreal dpr) {
	return static_cast<qint32>(std::lround(logical * dpr));
}

constexpr std::size_t PARK_SLOTS = 256;

struct ParkSlot {
	std::atomic<HWND> hwnd {nullptr};
	std::atomic<quint64> rect {0};
};

std::array<ParkSlot, PARK_SLOTS> gParkSlots {}; // NOLINT
std::atomic<ATOM> gParkAtom {0};               // NOLINT

LPCWSTR atomName(ATOM atom) {
	auto value = static_cast<ULONG_PTR>(atom);
	return reinterpret_cast<LPCWSTR>(value); // NOLINT(performance-no-int-to-ptr)
}

ATOM parkAtom() {
	auto atom = gParkAtom.load();
	if (atom != 0) return atom;

	atom = GlobalAddAtomW(L"QuickshellTilingParked");
	gParkAtom.store(atom);
	return atom;
}

quint64 packRect(const QRect& rect) {
	auto x = static_cast<quint64>(static_cast<quint16>(qBound(-32768, rect.left(), 32767)));
	auto y = static_cast<quint64>(static_cast<quint16>(qBound(-32768, rect.top(), 32767)));
	auto width = static_cast<quint64>(qBound(1, rect.width(), 65535));
	auto height = static_cast<quint64>(qBound(1, rect.height(), 65535));
	return x | (y << 16) | (width << 32) | (height << 48);
}

QRect unpackRect(quint64 packed) {
	auto x = static_cast<qint16>(packed & 0xFFFF);
	auto y = static_cast<qint16>((packed >> 16) & 0xFFFF);
	auto width = static_cast<int>((packed >> 32) & 0xFFFF);
	auto height = static_cast<int>((packed >> 48) & 0xFFFF);
	return {x, y, width, height};
}

void rememberParked(HWND hwnd, const QRect& restore) {
	auto packed = packRect(restore);
	ParkSlot* target = nullptr;

	for (auto& slot: gParkSlots) {
		auto* value = slot.hwnd.load();
		if (value == hwnd) {
			target = &slot;
			break;
		}
		if (value == nullptr && target == nullptr) target = &slot;
	}

	if (target != nullptr) {
		target->rect.store(packed);
		target->hwnd.store(hwnd);
	}

	if (auto atom = parkAtom(); atom != 0) {
		auto handle = static_cast<quintptr>(packed);
		SetPropW(hwnd, atomName(atom), reinterpret_cast<HANDLE>(handle)); // NOLINT
	}
}

void forgetParked(HWND hwnd) {
	for (auto& slot: gParkSlots) {
		if (slot.hwnd.load() == hwnd) slot.hwnd.store(nullptr);
	}

	if (auto atom = gParkAtom.load(); atom != 0) RemovePropW(hwnd, atomName(atom));
}

QRect clampInto(QRect rect, const QRect& work) {
	if (!work.isValid()) return rect;
	rect.setSize(rect.size().boundedTo(work.size()));
	rect.moveLeft(qBound(work.left(), rect.left(), rightOf(work) - rect.width()));
	rect.moveTop(qBound(work.top(), rect.top(), bottomOf(work) - rect.height()));
	return rect;
}

QRect onScreen(const QRect& rect) {
	auto native = toRECT(rect);
	if (MonitorFromRect(&native, MONITOR_DEFAULTTONULL) != nullptr) return rect;
	return clampInto(rect, monitorRects(MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST)).work);
}

QRect rawFor(HWND hwnd, const QRect& wanted) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return wanted;
	return wanted.adjusted(
	    raw.left() - frame.left(),
	    raw.top() - frame.top(),
	    rightOf(raw) - rightOf(frame),
	    bottomOf(raw) - bottomOf(frame)
	);
}

QRect frameFor(HWND hwnd, const QRect& wanted) {
	QRect raw;
	QRect frame;
	if (wanted.isNull() || !readFrame(hwnd, raw, frame)) return wanted;
	return wanted.adjusted(
	    frame.left() - raw.left(),
	    frame.top() - raw.top(),
	    rightOf(frame) - rightOf(raw),
	    bottomOf(frame) - bottomOf(raw)
	);
}

constexpr const wchar_t* FLOAT_SIZE_PROP = L"qs.floatsize2";
constexpr const wchar_t* TOPMOST_PROP = L"qs.topmost";

void rememberFloatSize(HWND hwnd, const QSize& size) {
	if (size.width() < FLOATING_TOLERANCE || size.height() < FLOATING_TOLERANCE) return;
	const auto packed = (static_cast<quintptr>(qMin(size.width(), 0xffff)) << 16)
	                  | static_cast<quintptr>(qMin(size.height(), 0xffff));
	SetPropW(hwnd, FLOAT_SIZE_PROP, reinterpret_cast<HANDLE>(packed)); // NOLINT
}

QSize floatSizeOf(HWND hwnd) {
	const auto packed = reinterpret_cast<quintptr>(GetPropW(hwnd, FLOAT_SIZE_PROP));
	return {static_cast<int>(packed >> 16), static_cast<int>(packed & 0xffff)};
}

bool onAnyMonitor(const QRect& rect) {
	auto native = toRECT(rect);
	return MonitorFromRect(&native, MONITOR_DEFAULTTONULL) != nullptr;
}

void restorePlacement(HWND hwnd, const QRect& rect) {
	WINDOWPLACEMENT placement {};
	placement.length = sizeof(placement);
	if (!GetWindowPlacement(hwnd, &placement)) return;
	placement.rcNormalPosition = toRECT(rect);
	SetWindowPlacement(hwnd, &placement);
}

tiling::CenterMode centerModeOf(const QString& value) {
	if (value == QStringLiteral("always")) return tiling::CenterMode::Always;
	if (value == QStringLiteral("never")) return tiling::CenterMode::Never;
	return tiling::CenterMode::Overflow;
}

} // namespace

TilingManager* TilingManager::instance() {
	static auto* instance = new TilingManager(); // NOLINT
	return instance;
}

TilingManager* TilingManager::active() {
	return gManager != nullptr && gManager->mEnabled ? gManager : nullptr;
}

TilingManager::TilingManager() {
	gManager = this;
	QObject::connect(WindowMotion::instance(), &WindowMotion::landed, this, &TilingManager::onMotionLanded);

	this->syncTimer.setSingleShot(true);
	this->syncTimer.setInterval(SYNC_DELAY_MS);
	QObject::connect(&this->syncTimer, &QTimer::timeout, this, &TilingManager::sync);
	this->restackTimer.setSingleShot(true);
	this->restackTimer.setInterval(0);
	QObject::connect(&this->restackTimer, &QTimer::timeout, this, &TilingManager::restack);
	QObject::connect(this, &TilingManager::windowTilingChanged, this, [this](TrackedWindow* window) {
		if (Scratchpad::holds(window)) Scratchpad::raiseWindow(window->hwnd());
		this->restackTimer.start();
	});

	this->verifyTimer.setSingleShot(true);
	this->verifyTimer.setInterval(VERIFY_DELAY_MS);
	QObject::connect(&this->verifyTimer, &QTimer::timeout, this, &TilingManager::verify);

	this->deadlineTimer.setSingleShot(true);
	this->deadlineTimer.setInterval(DEADLINE_MS);
	QObject::connect(&this->deadlineTimer, &QTimer::timeout, this, &TilingManager::onDeadline);
	this->closeCacheTimer.setInterval(100);
	QObject::connect(&this->closeCacheTimer, &QTimer::timeout, this, &TilingManager::cacheNextCloseFrame);
	this->healTimer.setSingleShot(true);
	this->healTimer.setInterval(HEAL_MS);
	QObject::connect(&this->healTimer, &QTimer::timeout, this, &TilingManager::heal);

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
			setParkNewWindows(false);
			this->unparkAll();
			releaseEarlyParks();
			setWindowShadowsSuppressed(false);
		});
	}

	TilingManager::rescueParked();
}

void TilingManager::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;

	if (enabled) {
		if (!this->start()) {
			this->mEnabled = false;
			return;
		}
		if (this->mSuppressWindowShadows) setWindowShadowsSuppressed(true);
	} else {
		this->stop();
		setWindowShadowsSuppressed(false);
	}
	focus_on_activate::setEnabled(enabled);

	this->syncWheel();
	emit this->enabledChanged();
}

void TilingManager::setAnimateMoves(bool animate) {
	if (animate == this->mAnimateMoves) return;
	this->mAnimateMoves = animate;
	this->updateEarlyParking();
	if (this->mEnabled && animate && DwindleScene::enabled()) this->closeCacheTimer.start();
	if (!animate) {
		this->closeCacheTimer.stop();
		DwindleScene::clearCloseCache();
		DwindleScene::dismissAll();
		QSet<Layout*> affected;
		for (auto& [window, m]: this->managed) {
			if (!m.focusMaximized()) continue;
			m.mode = nextMode(m.mode, TileModeEvent::Restore);
			m.fullscreenToggled = false;
			if (m.layout != nullptr) affected.insert(m.layout);
		}
		for (auto* layout: affected) this->markLayout(layout);
	}
	emit this->animateMovesChanged();
}

namespace {
QRect maximizedFrame(HWND hwnd, const QRect& work) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return work;
	const auto dpi = GetDpiForWindow(hwnd);
	const auto border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
	return work.adjusted(-border + frame.left() - raw.left(), 0, border + frame.right() - raw.right(),
	                     border + frame.bottom() - raw.bottom());
}

QRect parkedFrame(const QRect& tile, bool peek = false) {
	auto x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + (peek ? -1 : 100);
	return QRect(QPoint(x, tile.top()), tile.size());
}
}

void TilingManager::placeMoving(Managed& m, const Layout* layout, const QRect& tile) {
	auto* motion = WindowMotion::instance();
	auto found = this->moving.find(m.window);
	if (found != this->moving.end()) {
		auto chromium = WindowMotion::travelsAsStill(m.window->hwnd());
		auto parked = chromium && this->barMoving
		                ? (m.target.isNull() ? parkedFrame(tile) : m.target)
		                : QRect(parkedFrame(tile, chromium).topLeft(), tile.size());
		if (*found != tile) {
			*found = tile;
			motion->retarget(m.window->hwnd(), tile, parked);
		}
		if (chromium && this->barMoving) this->place(m, parked);
		else this->hold(m, tile);
		return;
	}
	m.fresh = false;
	auto animate = this->canAnimate(m, layout, tile);
	m.dropped = false;
	m.fullscreenToggled = false;
	if (animate) {
		auto from = this->managedFrame(m);
		auto chromium = WindowMotion::travelsAsStill(m.window->hwnd());
		auto still = chromium && this->barMoving;
		auto parked = still ? QRect(parkedFrame(tile).topLeft(), from.size())
		                    : QRect(parkedFrame(tile, chromium).topLeft(), tile.size());
		if (motion->begin(m.window->hwnd(), from, tile, parked, false, still)) {
			this->moving.insert(m.window, tile);
			if (still) this->place(m, parked);
			else this->hold(m, tile);
			return;
		}
	}
	this->place(m, tile);
}

bool TilingManager::canAnimate(const Managed& m, const Layout* layout, const QRect& tile) const {
	if (!m.fullscreenToggled) return false;
	if (m.dropped) return this->mAnimateMoves && !m.offscreen && IsWindowVisible(m.window->hwnd()) && !IsIconic(m.window->hwnd());
	if (!this->mAnimateMoves || m.target.isNull() || tile == m.target) return false;
	if ((m.verifying && !this->barMoving && !m.fullscreenToggled) || m.offscreen || m.tinyCornersApplied || m.window == this->dragging) return false;

	auto* hwnd = m.window->hwnd();
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsHungAppWindow(hwnd)) return false;
	auto frame = this->managedFrame(m);
	if (frame != m.target) {
		auto off = std::max({
		    std::abs(frame.left() - m.target.left()),
		    std::abs(frame.top() - m.target.top()),
		    std::abs(frame.right() - m.target.right()),
		    std::abs(frame.bottom() - m.target.bottom()),
		});
		if (off > 48 && !m.fullscreenToggled) return false;
	}

	if (this->desktops->shownIndex() != this->desktops->currentIndex()) return false;
	if (m.window->desktop() != this->desktops->currentIndex()) return false;

	auto rects = monitorRects(monitorForScreen(layout->screen));
	auto bounds = rects.monitor.adjusted(-4, -4, 4, 4);
	return rects.valid && (m.fullscreenToggled || (bounds.contains(m.target) && bounds.contains(tile)));
}

void TilingManager::haltMotion(HWND hwnd, bool resize) {
	for (auto& [window, m]: this->managed) {
		if (window->hwnd() != hwnd || m.layout != nullptr || !m.focusMaximized() || m.fullscreenRestore.isNull())
			continue;
		DwindleScene::dismissAll();
		if (resize) {
			this->forgetFloat(m);
			break;
		}
		const auto frame = this->managedFrame(m);
		const auto point = cursorPos();
		const auto size = m.fullscreenRestore.size();
		const auto across = static_cast<double>(point.x() - frame.left()) / qMax(1, frame.width());
		const auto down = static_cast<double>(point.y() - frame.top()) / qMax(1, frame.height());
		const QRect grabbed(
		    QPoint(point.x() - qRound(across * size.width()), point.y() - qRound(down * size.height())), size
		);
		if (IsWindow(hwnd) && !IsHungAppWindow(hwnd)) setRawRect(hwnd, grabbed, true);
		this->forgetFloat(m);
		return;
	}

	const auto frame = DwindleScene::frameForDrag(hwnd);
	if (!frame.isEmpty()) {
		DwindleScene::dismissAll();
		if (!resize && IsWindow(hwnd) && !IsHungAppWindow(hwnd))
			setFrame(hwnd, frame, false, nullptr, true);
	}

	for (auto iter = this->moving.begin(); iter != this->moving.end(); ++iter) {
		if (iter.key()->hwnd() != hwnd) continue;
		auto tile = iter.value();
		this->moving.erase(iter);
		auto shown = WindowMotion::instance()->stopAt(hwnd);
		setFrame(hwnd, shown.isNull() ? tile : shown, false, nullptr, true);
		return;
	}
}

void TilingManager::onMotionLanded(HWND hwnd, const QRect& /*frame*/) {
	for (auto iter = this->moving.begin(); iter != this->moving.end(); ++iter) {
		if (iter.key()->hwnd() != hwnd) continue;
		auto* window = iter.key();
		auto tile = iter.value();
		this->moving.erase(iter);
		auto found = this->managed.find(window);
		if (found != this->managed.end()) {
			this->place(found->second, tile);
			if (found->second.maximizePending) this->finishMaximize(found->second);
		}
		return;
	}
}

void TilingManager::setSuppressWindowShadows(bool suppress) {
	if (suppress == this->mSuppressWindowShadows) return;
	this->mSuppressWindowShadows = suppress;
	if (this->mEnabled) setWindowShadowsSuppressed(suppress);
	emit this->suppressWindowShadowsChanged();
}

void TilingManager::setGapsIn(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsIn) return;
	this->mGapsIn = gaps;
	this->markAll();
	emit this->gapsInChanged();
}

void TilingManager::setGapsOut(qint32 gaps) {
	gaps = qMax(0, gaps);
	if (gaps == this->mGapsOut) return;
	this->mGapsOut = gaps;
	this->markAll();
	emit this->gapsOutChanged();
}

void TilingManager::setPreserveSplit(bool preserve) {
	if (preserve == this->mPreserveSplit) return;
	this->mPreserveSplit = preserve;
	this->markAll();
	emit this->preserveSplitChanged();
}

void TilingManager::setExcluded(const QStringList& excluded) {
	if (excluded == this->mExcluded) return;
	this->mExcluded = excluded;

	QSet<TrackedWindow*> before;
	for (const auto& [window, m]: this->managed) {
		if (this->isExcluded(window)) before.insert(window);
	}

	this->excludedPatterns.clear();
	this->excludedTitles.clear();

	for (const auto& entry: excluded) {
		auto pattern = entry.trimmed();
		auto byTitle = pattern.startsWith("title:", Qt::CaseInsensitive);
		if (byTitle) pattern = pattern.mid(6).trimmed();
		if (pattern.isEmpty()) continue;

		auto expression = QRegularExpression(
		    QRegularExpression::wildcardToRegularExpression(
		        pattern,
		        QRegularExpression::NonPathWildcardConversion
		    ),
		    QRegularExpression::CaseInsensitiveOption
		);

		if (byTitle) this->excludedTitles.append(expression);
		else this->excludedPatterns.append(expression);
	}

	for (auto& [window, m]: this->managed) {
		if (this->isExcluded(window) != before.contains(window)) m.override = Override::None;
	}

	this->markAll();
	emit this->excludedChanged();
}

void TilingManager::setLayout(const QString& layout) {
	auto normalized = layout.trimmed().toLower();
	if (normalized != QStringLiteral("master") && normalized != QStringLiteral("scrolling")) {
		normalized = QStringLiteral("dwindle");
	}

	if (normalized == this->mLayout) return;
	this->mLayout = normalized;
	if (this->mEnabled) this->rebuildLayouts();
	this->syncWheel();
	emit this->layoutChanged();
}

void TilingManager::syncWheel() const {
	SuperDragManager::instance()->setWheelColumns(
	    this->mEnabled && this->mLayout == QStringLiteral("scrolling")
	);
}

void TilingManager::setSmartSplit(bool smart) {
	if (smart == this->mSmartSplit) return;
	this->mSmartSplit = smart;
	this->markAll();
	emit this->smartSplitChanged();
}

void TilingManager::setForceSplit(qint32 force) {
	force = qBound(0, force, 2);
	if (force == this->mForceSplit) return;
	this->mForceSplit = force;
	emit this->forceSplitChanged();
}

void TilingManager::setDefaultSplitRatio(double ratio) {
	ratio = qBound(0.1, ratio, 1.9);
	if (qFuzzyCompare(ratio, this->mDefaultSplitRatio)) return;
	this->mDefaultSplitRatio = ratio;
	emit this->defaultSplitRatioChanged();
}

void TilingManager::setMasterNewStatus(const QString& status) {
	auto value = status.trimmed().toLower();
	if (value != QStringLiteral("master") && value != QStringLiteral("inherit")) {
		value = QStringLiteral("slave");
	}

	if (value == this->mMasterNewStatus) return;
	this->mMasterNewStatus = value;
	this->markAll();
	emit this->masterNewStatusChanged();
}

void TilingManager::setMasterNewOnTop(bool onTop) {
	if (onTop == this->mMasterNewOnTop) return;
	this->mMasterNewOnTop = onTop;
	this->markAll();
	emit this->masterNewOnTopChanged();
}

void TilingManager::setMasterOrientation(const QString& orientation) {
	static const auto known = QStringList {
	    QStringLiteral("left"),
	    QStringLiteral("right"),
	    QStringLiteral("top"),
	    QStringLiteral("bottom"),
	    QStringLiteral("center"),
	};

	auto value = orientation.trimmed().toLower();
	if (!known.contains(value)) value = QStringLiteral("left");
	if (value == this->mMasterOrientation) return;
	this->mMasterOrientation = value;
	this->markAll();
	emit this->masterOrientationChanged();
}

void TilingManager::setMasterFactor(double factor) {
	factor = qBound(0.05, factor, 0.95);
	if (qFuzzyCompare(factor, this->mMasterFactor)) return;
	this->mMasterFactor = factor;
	this->markAll();
	emit this->masterFactorChanged();
}

void TilingManager::setScrollingColumnWidth(double width) {
	width = qBound(tiling::ScrollingLayout::MIN_WIDTH, width, tiling::ScrollingLayout::MAX_WIDTH);
	if (qFuzzyCompare(width, this->mScrollingColumnWidth)) return;
	this->mScrollingColumnWidth = width;
	this->markAll();
	emit this->scrollingColumnWidthChanged();
}

void TilingManager::setScrollingPresets(const QList<double>& presets) {
	if (presets == this->mScrollingPresets) return;
	this->mScrollingPresets = presets;
	this->markAll();
	emit this->scrollingPresetsChanged();
}

void TilingManager::setScrollingCenter(const QString& center) {
	auto value = center.trimmed().toLower();
	if (value != QStringLiteral("always") && value != QStringLiteral("never")) {
		value = QStringLiteral("overflow");
	}

	if (value == this->mScrollingCenter) return;
	this->mScrollingCenter = value;
	this->markAll();
	emit this->scrollingCenterChanged();
}

tiling::Kind TilingManager::layoutKind() const {
	if (this->mLayout == QStringLiteral("master")) return tiling::Kind::Master;
	if (this->mLayout == QStringLiteral("scrolling")) return tiling::Kind::Scrolling;
	return tiling::Kind::Dwindle;
}

tiling::Options TilingManager::options() const {
	auto options = tiling::Options();
	options.preserveSplit = this->mPreserveSplit;
	options.smartSplit = this->mSmartSplit;
	options.forceSplit = this->mForceSplit;
	options.defaultSplitRatio = this->mDefaultSplitRatio;
	options.newOnTop = this->mMasterNewOnTop;
	options.mfact = this->mMasterFactor;
	options.columnWidth = this->mScrollingColumnWidth;
	options.widthPresets = this->mScrollingPresets;
	options.centerMode = centerModeOf(this->mScrollingCenter);

	if (this->fitToggled) {
		options.centerMode = options.centerMode == tiling::CenterMode::Always
		                       ? tiling::CenterMode::Overflow
		                       : tiling::CenterMode::Always;
	}

	if (this->mMasterNewStatus == QStringLiteral("master")) {
		options.newStatus = tiling::NewStatus::Master;
	} else if (this->mMasterNewStatus == QStringLiteral("inherit")) {
		options.newStatus = tiling::NewStatus::Inherit;
	}

	const auto& o = this->mMasterOrientation;
	if (o == QStringLiteral("right")) options.orientation = tiling::Orientation::Right;
	else if (o == QStringLiteral("top")) options.orientation = tiling::Orientation::Top;
	else if (o == QStringLiteral("bottom")) options.orientation = tiling::Orientation::Bottom;
	else if (o == QStringLiteral("center")) options.orientation = tiling::Orientation::Center;

	return options;
}

void TilingManager::rebuildLayouts() {
	auto kind = this->layoutKind();
	auto options = this->options();

	auto seed = options;
	seed.smartSplit = false;
	seed.forceSplit = 0;
	seed.newStatus = tiling::NewStatus::Slave;
	seed.newOnTop = false;

	for (auto& [key, layout]: this->layouts) {
		if (layout->tree->kind() == kind) continue;
		for (auto& [window, m]: this->managed) {
			if (m.layout != layout.get() || !m.focusMaximized()) continue;
			m.mode = nextMode(m.mode, TileModeEvent::Restore);
			m.fullscreenToggled = false;
		}

		auto fresh = tiling::LayoutEngine::create(kind);
		fresh->configure(seed);
		auto area = layout->tree->area();
		if (area.isValid()) fresh->compute(area);

		auto previous = tiling::LayoutEngine::Id(0);
		for (auto id: layout->tree->ids()) {
			fresh->insert(id, previous, nullptr);
			previous = id;
		}

		fresh->configure(options);
		layout->tree = std::move(fresh);

		if (auto* scroll = TilingManager::scrollingOf(layout.get())) {
			this->prepareScrolling(layout.get());
			if (layout->lastFocused != nullptr) scroll->activate(idOf(layout->lastFocused));
		}
	}

	this->edgeDragState = EdgeDrag();
	this->relayout();
}

void TilingManager::relayout() {
	if (!this->mEnabled) return;

	for (auto& [window, m]: this->managed) {
		m.accepted = QRect();
		m.corrections = 0;
	}

	this->markAll();
}

void TilingManager::expectBarSpace(const QString& screenName, bool top, int reserved) {
	if (!this->mEnabled) return;
	QScreen* screen = nullptr;
	for (auto* candidate: QGuiApplication::screens()) {
		if (candidate->name() == screenName) screen = candidate;
	}
	if (screen == nullptr) return;
	auto* monitor = monitorForScreen(screen);
	auto rects = monitorRects(monitor);
	if (!rects.valid) return;

	auto physical = std::max(0, static_cast<int>(std::lround(reserved * screen->devicePixelRatio())));
	auto work = rects.work;
	if (top) work.setTop(std::min(rects.monitor.top() + physical, work.bottom()));
	else work.setBottom(std::max(rects.monitor.bottom() - physical, work.top()));
	expectWorkArea(monitor, work, 1500);

	if (DwindleScene::enabled() && this->mLayout == QStringLiteral("dwindle")) {
		if (this->barRelayoutPending) return;
		this->barRelayoutPending = true;
		QTimer::singleShot(0, this, [this] {
			this->barRelayoutPending = false;
			if (!this->mEnabled) return;
			WinPanelWindow::raiseDemoted();
			this->barMoving = true;
			this->relayout();
			this->sync();
			this->barMoving = false;
		});
		return;
	}

	this->barMoving = true;
	this->relayout();
	this->sync();
	this->barMoving = false;
}

void TilingManager::markAll() {
	if (!this->mEnabled) return;
	for (auto& [window, m]: this->managed) this->dirtyWindows.insert(window);
	for (auto& [key, layout]: this->layouts) this->dirtyLayouts.insert(layout.get());
	this->schedule();
}

bool TilingManager::isTiled(TrackedWindow* window) const {
	auto iter = this->managed.find(window);
	return iter != this->managed.end() && iter->second.layout != nullptr;
}

bool TilingManager::allowsHoverFocus(TrackedWindow* window) const {
	if (window != nullptr && IsWindowEnabled(window->hwnd()) == FALSE) return false;
	auto target = this->managed.find(window);
	if (target == this->managed.end() || target->second.layout == nullptr) return true;

	const auto special = Scratchpad::holds(window);
	for (const auto& [other, m]: this->managed) {
		if (other == window || other->minimized()) continue;
		if (m.focusMaximized() && m.layout == nullptr && this->floats(m) && Scratchpad::holds(other) == special
		    && (special || (other->desktop() == window->desktop() && other->screen() == window->screen())))
			return false;
		if (m.focusMaximized() && m.layout == target->second.layout) return false;
		if (m.fullscreen() && Scratchpad::holds(other) == special
		    && (special || (other->desktop() == window->desktop() && other->screen() == window->screen())))
		{
			return false;
		}
	}

	return true;
}

bool TilingManager::occupiesDwindleTile(HWND hwnd) const {
	if (this->tracker == nullptr) return false;
	auto found = this->managed.find(this->tracker->windowFor(hwnd));
	if (found == this->managed.end()) return false;

	const auto& m = found->second;
	if (this->floats(m)) {
		return !m.fullscreen() && !m.appFullscreen && !m.window->borderlessFullscreen()
		    && !(m.window->fullscreen() && framelessCover(hwnd));
	}
	if (m.layout == nullptr || (m.layout->special && !this->sceneShows(m.layout, 0))
	    || m.layout->tree->kind() == tiling::Kind::Master
	    || m.fullscreen() || m.offscreen || m.staged() || m.target.isEmpty()
	    || this->stateOf(m) != State::Tiled || m.window->borderlessFullscreen())
		return false;

	const auto actual = this->managedFrame(m);
	return actual == m.target || (m.verifying && actual == m.before);
}

bool TilingManager::start() {
	this->ownerMutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickshellTiling");
	if (this->ownerMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
		qCWarning(logTiling) << "Another Quickshell process already tiles windows;"
		                     << "tiling stays off in this one.";
		if (this->ownerMutex != nullptr) CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return false;
	}

	recoverOrphanedTinyCorners();
	if (DwindleScene::enabled()) {
		QTimer::singleShot(0, this, &DwindleScene::warmUp);
		DwindleScene::onOcclusion([this](HWND hwnd) { return this->stageOccluding(hwnd); });
		DwindleScene::onRelease([this](const QList<HWND>& windows) { this->releaseStaged(windows); });
	}

	this->tracker = WindowTracker::instance();
	this->desktops = this->tracker->desktops();
	startup::afterFirstFrame(this, [this] {
		this->updateEarlyParking();
		this->openStartupLayouts();
	});

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &TilingManager::manage)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &TilingManager::unmanage)
	);
	c.append(QObject::connect(
	    this->tracker, &WindowTracker::windowCreated, this, &TilingManager::coverNewWindow
	));
	c.append(QObject::connect(this->tracker, &WindowTracker::foregroundChanged, this, [this] {
		this->restackTimer.start();
	}));
	c.append(QObject::connect(this->tracker, &WindowTracker::windowCreated, this, [this] {
		QTimer::singleShot(NOTICE_SETTLE_MS, this, [this] { this->restackTimer.start(); });
	}));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::activeWindowChanged,
	    this,
	    &TilingManager::onActiveWindowChanged
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeStarted,
	    this,
	    &TilingManager::onMoveSizeStarted
	));
	c.append(QObject::connect(
	    this->tracker,
	    &WindowTracker::moveSizeEnded,
	    this,
	    &TilingManager::onMoveSizeEnded
	));
	c.append(QObject::connect(this->desktops, &VirtualDesktops::shownChanged, this, [this]() {
		DwindleScene::dismissAll();
		this->updateEarlyParking();
		this->updateShellFullscreen();
		this->markCovering();
	}));
	c.append(QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, [this]() {
		this->markAll();
	}));
	c.append(QObject::connect(Scratchpad::instance(), &Scratchpad::shownChanged, this, [this]() {
		if (Scratchpad::instance()->shown()) {
			this->warnFullscreenConflict(nullptr, this->desktops->shownIndex());
			return;
		}
		this->markCovering();
	}));

	c.append(QObject::connect(this->desktops, &VirtualDesktops::currentChanged, this, [this] {
		DwindleScene::dismissAll();
		this->updateEarlyParking();
		this->markCovering();
	}));

	if (auto* app = qobject_cast<QGuiApplication*>(QGuiApplication::instance())) {
		auto changed = &TilingManager::onScreensChanged;
		c.append(QObject::connect(app, &QGuiApplication::screenAdded, this, changed));
		c.append(QObject::connect(app, &QGuiApplication::screenRemoved, this, changed));
	}

	this->onScreensChanged();

	for (auto* window: this->tracker->windows()) this->manage(window);

	auto windows = this->tracker->windows();
	std::stable_sort(windows.begin(), windows.end(), [](TrackedWindow* a, TrackedWindow* b) {
		auto ca = a->rect().center();
		auto cb = b->rect().center();
		return ca.x() != cb.x() ? ca.x() < cb.x() : ca.y() < cb.y();
	});

	QHash<Layout*, TrackedWindow*> previous;
	for (auto* window: windows) {
		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto* layout = this->layoutFor(window, true);
		if (layout == nullptr) continue;

		this->insertInto(m, layout, previous.value(layout), nullptr);
		previous.insert(layout, window);
	}

	this->onActiveWindowChanged();
	this->schedule();
	if (this->mAnimateMoves && DwindleScene::enabled()) this->closeCacheTimer.start();
	return true;
}

void TilingManager::stop() {
	setParkNewWindows(false);
	this->closeCacheTimer.stop();
	DwindleScene::clearCloseCache();
	DwindleScene::dismissAll();
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();

	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
	}

	this->stopMotions();
	QList<TrackedWindow*> tiled;
	for (auto& [window, m]: this->managed) {
		if (m.fullscreen()) {
			this->leaveFullscreen(m);
			setFullscreenLook(window->hwnd(), false);
		}
		if (this->floats(m)) {
			if (m.focusMaximized()) this->restoreFloat(m);
			if (m.floatMoving && !m.floatFrame.isNull()) setFrame(window->hwnd(), m.floatFrame);
			restoreTopmost(m);
		}
		this->bringBack(m);
		this->restoreTinyCorners(m);
		QObject::disconnect(window, nullptr, this, nullptr);
		if (m.layout == nullptr) continue;

		tiled.append(window);
		this->restoreFloating(m);
	}

	this->managed.clear();
	WinPanelWindow::setShellFullscreen({});
	this->layouts.clear();
	this->dirtyWindows.clear();
	this->dirtyLayouts.clear();
	this->unverified.clear();
	this->dragging = nullptr;
	this->edgeDragState = EdgeDrag();
	this->syncTimer.stop();
	this->restackTimer.stop();
	this->verifyTimer.stop();
	this->deadlineTimer.stop();
	this->healTimer.stop();

	if (this->ownerMutex != nullptr) {
		ReleaseMutex(this->ownerMutex);
		CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
	}

	for (auto* window: tiled) emit this->windowTilingChanged(window);
}

void TilingManager::manage(TrackedWindow* window) {
	if (this->managed.contains(window)) return;

	auto& m = this->managed[window];
	m.window = window;
	m.fresh = startup::settled();
	m.managedAt = QDateTime::currentMSecsSinceEpoch();
	m.elevated = isMoreElevated(window->hwnd());
	m.appFullscreen = framelessCover(window->hwnd());

	if (m.elevated) {
		qCDebug(logTiling) << "Window" << window->addressHex() << window->appId()
		                   << "belongs to a more privileged process; it floats.";
	}

	if (!m.elevated && startup::settled() && window->maximized() && !window->minimized()) {
		qCDebug(logTiling) << "Window" << window->addressHex() << window->appId()
		                   << "opened maximized; restoring it to tile it.";
		ShowWindowAsync(window->hwnd(), SW_RESTORE);
	}

	this->restoreLeftFullscreen(window);
	m.fresh = startup::settled();
	if (m.fresh && !window->maximized() && !window->minimized() && floatSizeOf(window->hwnd()).isEmpty()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) rememberFloatSize(window->hwnd(), raw.size());
	}

	auto mark = [this, window]() { this->markWindow(window); };
	if (m.fresh && !m.appFullscreen) {
		QTimer::singleShot(0, this, [this, tracked = QPointer(window)] {
			if (tracked != nullptr && this->managed.contains(tracked.data())) this->warnFullscreenConflict(tracked.data());
		});
	}
	QObject::connect(window, &TrackedWindow::minimizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::maximizedChanged, this, mark);
	QObject::connect(window, &TrackedWindow::fullscreenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::desktopChanged, this, mark);
	QObject::connect(window, &TrackedWindow::screenChanged, this, mark);
	QObject::connect(window, &TrackedWindow::titleChanged, this, [this, window]() {
		if (!this->excludedTitles.isEmpty()) this->markWindow(window);
	});
	QObject::connect(window, &TrackedWindow::rectChanged, this, [this, window]() {
		this->onRectChanged(window);
	});

	this->markWindow(window);
}

void TilingManager::unmanage(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	this->restoreTinyCorners(iter->second);

	auto wasFullscreen = iter->second.fullscreen() || window->fullscreen();
	auto* from = iter->second.layout;
	const auto hwnd = window->hwnd();
	auto closing = false;
	if (DwindleScene::enabled() && this->mAnimateMoves && from != nullptr
	    && this->layoutAlive(from) && from->tree->kind() != tiling::Kind::Master
	    && !iter->second.fullscreen() && !iter->second.offscreen && !window->minimized()
	    && this->sceneShows(from, this->desktops->shownIndex())
	    && this->desktops->shownIndex() == this->desktops->currentIndex()
	    && (!IsWindow(hwnd) || !IsWindowVisible(hwnd)))
	{
		auto* monitor = monitorForScreen(from->screen);
		const auto rects = monitorRects(monitor);
		if (rects.valid) {
			DwindleScene::prepareClose(monitor, rects.work, hwnd, window->pid(), iter->second.target, true);
			closing = DwindleScene::hasClosingTile(monitor, rects.work);
		}
	}
	if (from == nullptr) closing = this->closeFloat(iter->second);
	auto refocus = from != nullptr && from->lastFocused == window
	            && TilingManager::scrollingOf(from) != nullptr;

	QObject::disconnect(window, nullptr, this, nullptr);
	if (auto moved = this->moving.find(window); moved != this->moving.end()) {
		WindowMotion::instance()->cancel(window->hwnd());
		if (IsWindow(window->hwnd())) setFrame(window->hwnd(), moved.value());
		this->moving.erase(moved);
	}
	if (iter->second.layout == nullptr && (iter->second.mode != TileMode::Normal || iter->second.floatMoving)) {
		for (auto& [key, layout]: this->layouts) {
			if (this->sceneShows(layout.get(), this->desktops->currentIndex())) this->markLayout(layout.get());
		}
	}
	this->takeOut(iter->second);
	if (closing) this->syncTimer.start(0);
	if (iter->second.offscreen) forgetParked(window->hwnd());
	this->managed.erase(iter);

	this->dirtyWindows.remove(window);
	this->unverified.remove(window);
	if (this->dragging == window) this->dragging = nullptr;
	if (this->detached == window) {
		this->detached = nullptr;
		DwindleScene::setDuringDrag(false);
	}

	for (auto& [key, layout]: this->layouts) {
		if (layout->lastFocused == window) layout->lastFocused = nullptr;
	}

	if (wasFullscreen) this->updateShellFullscreen();
	if (refocus) this->refocus(from);
}

TilingManager::State TilingManager::stateOf(const Managed& m) const {
	auto* window = m.window;

	if (Scratchpad::holds(window)) {
		if (m.elevated || m.override == Override::Float) return State::Out;
		if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;
		if (window->minimized() || !Scratchpad::instance()->shown()) return State::Hidden;
		return State::Tiled;
	}
	if (window->desktop() < 0 || window->screen() == nullptr) return State::Out;
	if (!QGuiApplication::screens().contains(window->screen())) return State::Out;
	if (m.elevated) return State::Out;
	if (m.override == Override::Float) return State::Out;
	if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;

	if (window->minimized()) return State::Hidden;
	if (window->maximized() && !m.maximizedTile) return State::Suspended;

	auto framed = (GetWindowLongPtrW(window->hwnd(), GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME))
	           == (WS_CAPTION | WS_THICKFRAME);
	if (window->fullscreen() && !framed) {
		if (m.layout == nullptr || m.target.isNull() || this->managedFrame(m) != m.target) {
			return State::Suspended;
		}
	}

	return State::Tiled;
}

bool TilingManager::floatsByRule(const Managed& m) const {
	auto* hwnd = m.window->hwnd();

	auto* owner = GetWindow(hwnd, GW_OWNER);
	if (owner != nullptr && IsWindowVisible(owner)) return true;

	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	auto floatingEx = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_DLGMODALFRAME;
	if (m.wasTopmost) return true;
	if (!Scratchpad::holds(m.window)) floatingEx |= WS_EX_TOPMOST;
	if ((exStyle & floatingEx) != 0) return true;

	if ((style & WS_THICKFRAME) == 0 || (style & WS_MAXIMIZEBOX) == 0) return true;

	if (windowClass(hwnd) == QStringLiteral("#32770")) return true;

	return this->isExcluded(m.window);
}

bool TilingManager::isExcluded(TrackedWindow* window) const {
	for (const auto& pattern: this->excludedTitles) {
		if (pattern.match(window->title()).hasMatch()) return true;
	}

	if (this->excludedPatterns.isEmpty()) return false;

	auto exe = QFileInfo(window->exePath()).fileName();
	auto base = exe;
	if (base.endsWith(".exe", Qt::CaseInsensitive)) base.chop(4);

	auto candidates = QStringList {window->appId(), exe, base, windowClass(window->hwnd())};

	for (const auto& pattern: this->excludedPatterns) {
		for (const auto& candidate: candidates) {
			if (!candidate.isEmpty() && pattern.match(candidate).hasMatch()) return true;
		}
	}

	return false;
}

tiling::LayoutEngine::Id TilingManager::idOf(TrackedWindow* window) {
	return reinterpret_cast<tiling::LayoutEngine::Id>(window);
}

TrackedWindow* TilingManager::windowOf(tiling::LayoutEngine::Id id) const {
	auto* window = reinterpret_cast<TrackedWindow*>(id); // NOLINT(performance-no-int-to-ptr)
	return this->managed.contains(window) ? window : nullptr;
}

TilingManager::Layout* TilingManager::layoutFor(TrackedWindow* window, bool create) {
	if (Scratchpad::holds(window)) {
		auto* screen = screenForMonitor(Scratchpad::instance()->monitor());
		if (screen == nullptr) screen = QGuiApplication::primaryScreen();
		return this->layoutAt(SPECIAL_DESKTOP, screen, create);
	}
	auto index = window->desktop();
	if (index < 0 || index >= this->desktops->count()) return nullptr;
	return this->layoutAt(this->desktops->desktops().at(index).id, window->screen(), create);
}

TilingManager::Layout* TilingManager::layoutAt(const GUID& desktop, QScreen* screen, bool create) {
	if (screen == nullptr || !QGuiApplication::screens().contains(screen)) return nullptr;

	auto key = VirtualDesktops::guidToString(desktop) + '|' + screen->name() + '|'
	         + QString::number(reinterpret_cast<quintptr>(screen), 16);
	auto iter = this->layouts.find(key);
	if (iter != this->layouts.end()) return iter->second.get();
	if (!create) return nullptr;

	auto layout = std::make_unique<Layout>();
	layout->desktop = desktop;
	layout->screen = screen;
	layout->special = IsEqualGUID(desktop, SPECIAL_DESKTOP) != 0;
	layout->tree = tiling::LayoutEngine::create(this->layoutKind());
	layout->tree->configure(this->options());
	this->prepareScrolling(layout.get());
	auto rects = monitorRects(monitorForScreen(screen));
	if (rects.valid) layout->tree->compute(rects.work);

	auto* raw = layout.get();
	this->layouts.emplace(key, std::move(layout));
	return raw;
}

bool TilingManager::sceneShows(const Layout* layout, int index) const {
	auto* pad = Scratchpad::instance();
	const auto underPad = pad->shown() && monitorForScreen(layout->screen) == pad->monitor();
	if (layout->special) return underPad && this->mSpecialOffset == 0 && !this->mSpecialShifting;
	return !underPad && this->desktops->indexOf(layout->desktop) == index;
}

bool TilingManager::layoutAlive(const Layout* layout) const {
	if (layout->screen == nullptr) return false;
	if (!QGuiApplication::screens().contains(layout->screen.data())) return false;
	return layout->special || this->desktops->indexOf(layout->desktop) != -1;
}

void TilingManager::markWindow(TrackedWindow* window) {
	if (!this->mEnabled) return;
	this->dirtyWindows.insert(window);
	this->schedule();
}

void TilingManager::markLayout(Layout* layout) {
	if (!this->mEnabled || layout == nullptr) return;
	this->dirtyLayouts.insert(layout);
	this->schedule();
}

void TilingManager::schedule() {
	if (!this->syncTimer.isActive()) this->syncTimer.start(SYNC_DELAY_MS);
}

void TilingManager::updateEarlyParking() {
	setParkNewWindows(
	    DwindleScene::enabled() && this->mEnabled && this->mAnimateMoves && startup::settled()
	    && this->desktops != nullptr && this->desktops->shownIndex() == this->desktops->currentIndex()
	);
}

void TilingManager::sync() {
	this->updateEarlyParking();
	if (!this->mEnabled) return;

	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		auto* layout = iter->second.get();
		if (this->layoutAlive(layout)) {
			++iter;
			continue;
		}

		for (auto id: layout->tree->ids()) {
			if (auto* window = this->windowOf(id)) {
				auto& m = this->managed.at(window);
				this->restoreTinyCorners(m);
				this->bringBack(m);
				this->unpark(m);
				m.layout = nullptr;
				m.target = QRect();
				this->dirtyWindows.insert(window);
			}
		}

		this->dirtyLayouts.remove(layout);
		iter = this->layouts.erase(iter);
	}

	this->auditTrees();

	auto windows = std::move(this->dirtyWindows);
	this->dirtyWindows.clear();

	for (auto* window: this->tracker->windows()) {
		if (windows.contains(window)) this->syncWindow(window);
	}

	this->dropEmptyLayouts();

	auto layouts = std::move(this->dirtyLayouts);
	this->dirtyLayouts.clear();

	for (auto& [key, layout]: this->layouts) {
		if (layouts.contains(layout.get())) this->apply(layout.get());
	}
	this->updateShellFullscreen();
	if (!this->healTimer.isActive()) this->healTimer.start();
}

namespace {

QString fullscreenAppsPath() {
	return qEnvironmentVariable("LocalAppData") + QStringLiteral("/quickshell/State/user/fullscreen_apps.json");
}

QSet<QString>& fullscreenApps() {
	static auto* apps = [] {
		auto* set = new QSet<QString>();
		QFile file(fullscreenAppsPath());
		if (file.open(QIODevice::ReadOnly)) {
			for (const auto& value: QJsonDocument::fromJson(file.readAll()).array()) {
				if (value.isString() && !value.toString().isEmpty()) set->insert(value.toString().toLower());
			}
		}
		return set;
	}();
	return *apps;
}

QString fullscreenKey(const TrackedWindow* window) {
	const auto exe = window->exePath().toLower();
	const auto appId = window->appId().toLower();
	if (appId.isEmpty() || appId == QFileInfo(exe).completeBaseName()) return exe;
	return appId;
}

bool remembersFullscreen(const TrackedWindow* window) {
	const auto key = fullscreenKey(window);
	return !key.isEmpty() && fullscreenApps().contains(key);
}

void rememberFullscreen(const TrackedWindow* window, bool fullscreen) {
	const auto key = fullscreenKey(window);
	if (key.isEmpty()) return;
	auto& apps = fullscreenApps();
	if (fullscreen == apps.contains(key)) return;
	if (fullscreen) apps.insert(key);
	else apps.remove(key);

	auto sorted = QStringList(apps.begin(), apps.end());
	sorted.sort();
	const auto path = fullscreenAppsPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)
	    || file.write(QJsonDocument(QJsonArray::fromStringList(sorted)).toJson()) < 0 || !file.commit())
		qCWarning(logTiling) << "Could not save remembered fullscreen apps to" << path;
}

}

bool TilingManager::ignoreMaximize(Managed& m) {
	auto* window = m.window;
	auto* hwnd = window->hwnd();
	if (!window->maximized() || window->minimized() || m.elevated || m.maximizedTile || m.maximizePending
	    || !startup::settled() || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
		return false;
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - m.unmaximizedAt > UNMAXIMIZE_RESET_MS) m.unmaximized = 0;
	if (m.unmaximized >= UNMAXIMIZE_LIMIT) return false;
	m.unmaximized++;
	m.unmaximizedAt = now;
	qCDebug(logTiling) << "Window" << window->addressHex() << window->appId() << "maximized itself; restoring it";
	BOOL still = TRUE;
	DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &still, sizeof(still));
	ShowWindowAsync(hwnd, SW_RESTORE);
	QTimer::singleShot(UNMAXIMIZE_SETTLE_MS, window, [hwnd] {
		BOOL animate = FALSE;
		DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &animate, sizeof(animate));
	});
	return true;
}

void TilingManager::syncWindow(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	if (window == this->dragging) return;
	if (ignoreMaximize(m)) return;
	if (m.layout == nullptr && m.focusMaximized() && (window->minimized() || window->maximized())) {
		if (!m.fullscreenRestore.isNull()) restorePlacement(window->hwnd(), m.fullscreenRestore);
		this->forgetFloat(m);
	}
	if (m.focusMaximized() && window->minimized()) {
		m.mode = nextMode(m.mode, TileModeEvent::Restore);
		m.fullscreenToggled = true;
		this->markLayout(m.layout);
	}

	if (m.layout != nullptr && !m.maximizePending && !Scratchpad::holds(window)) {
		const auto maximized = window->maximized() && !window->minimized();
		if (maximized != m.maximizedTile) {
			m.maximizedTile = maximized;
			if (maximized) m.mode = nextMode(m.mode, TileModeEvent::Restore);
			m.fullscreenToggled = false;
		}
	}

	auto state = this->stateOf(m);
	if (state != State::Tiled) {
		if (state != State::Out || !this->openFloat(m)) releaseEarlyPark(window->hwnd());
		m.verifying = false;
		this->restoreTinyCorners(m);
	}

	if (state == State::Out) {
		if (m.layout == nullptr) {
			m.fresh = false;
			m.floated = this->floats(m);
			if (m.mode != TileMode::Normal || m.floatMoving) this->markLayout(this->floatHome(m, true));
			else if (onAnyMonitor(this->managedFrame(m))) m.floatSeen = this->managedFrame(m);
			return;
		}

		auto floated = !window->minimized() && window->desktop() >= 0 && !m.elevated
		            && (!Scratchpad::holds(window) || Scratchpad::instance()->shown());
		this->takeOut(m);
		if (floated) this->restoreFloating(m, true);
		else this->unpark(m);
		return;
	}

	auto special = Scratchpad::holds(window);
	auto index = window->desktop();
	if (!special && index >= this->desktops->count()) return;
	auto desktop = special ? SPECIAL_DESKTOP : this->desktops->desktops().at(index).id;

	const auto fromFloat = state == State::Tiled && std::exchange(m.floated, false) && m.layout == nullptr && IsWindowVisible(window->hwnd())
	                    && onAnyMonitor(this->managedFrame(m));
	const auto floatFrame = fromFloat ? this->managedFrame(m) : QRect();
	if (fromFloat && m.layout == nullptr && !m.fullscreen()) {
		const auto restored = m.focusMaximized() && !m.fullscreenRestore.isNull();
		rememberFloatSize(window->hwnd(), (restored ? m.fullscreenRestore : rawFor(window->hwnd(), floatFrame)).size());
	}
	Layout* want = nullptr;
	POINT pointer {};
	bool hasPointer = false;
	auto* current = m.layout;
	if (current != nullptr && this->layoutAlive(current) && IsEqualGUID(current->desktop, desktop)) {
		want = current;

		if (special && state == State::Tiled && current->screen != screenForMonitor(Scratchpad::instance()->monitor())) {
			if (auto* destination = this->layoutFor(window, true); destination != nullptr && destination != current) {
				this->moveToLayout(m, destination, nullptr);
				return;
			}
		}

		if (!special && state == State::Tiled && !m.verifying && !m.offscreen && !m.staged()
		    && window->screen() != current->screen)
		{
			this->moveToLayout(m, this->layoutFor(window, true), nullptr);
			return;
		}
	} else if (state == State::Tiled) {
		hasPointer = GetCursorPos(&pointer);
		if (hasPointer && current == nullptr && m.fresh && m.preTiling.isNull() && !special
		    && index == this->desktops->currentIndex() && index == this->desktops->shownIndex()
		    && this->layoutKind() == tiling::Kind::Dwindle)
		{
			auto* screen = this->tracker->screenFor(MonitorFromPoint(pointer, MONITOR_DEFAULTTONULL));
			want = this->layoutAt(desktop, screen, true);
		}
		if (want == nullptr) want = this->layoutFor(window, true);
	}

	if (want == m.layout) {
		if (m.parked && state == State::Tiled) m.target = QRect();
		m.parked = state != State::Tiled;
		this->markLayout(m.layout);
		return;
	}

	const auto transferred = m.layout != nullptr;
	const auto pacedOpening = want != nullptr && !transferred && m.fresh
	    && m.preTiling.isNull() && this->mAnimateMoves && DwindleScene::enabled() && startup::settled()
	    && this->dragging == nullptr && this->edgeDragState.window == nullptr
	    && this->sceneShows(want, this->desktops->currentIndex())
	    && (special || (index == this->desktops->currentIndex() && index == this->desktops->shownIndex()))
	    && want->tree->kind() != tiling::Kind::Master && isEarlyParked(window->hwnd());
	if (pacedOpening && !want->nextOpening.hasExpired()) {
		const auto rects = monitorRects(monitorForScreen(want->screen));
		QRect raw;
		QRect actual;
		if (rects.valid && readFrame(window->hwnd(), raw, actual) && !actual.intersects(rects.work)) {
			this->markWindow(window);
			return;
		}
	}
	if (transferred) this->takeOut(m);

	if (want != nullptr) {
		want->animateRearrangement |= transferred;
		const QPoint cursor(pointer.x, pointer.y);
		const auto firstOpening = want->tree->isEmpty();
		this->insertInto(m, want, nullptr, hasPointer ? &cursor : nullptr);
		if (fromFloat) {
			m.target = floatFrame;
			m.fullscreenToggled = true;
		}
		if (!transferred && m.fresh && remembersFullscreen(window)) {
			QTimer::singleShot(0, this, [this, tracked = QPointer(window)] {
				if (tracked == nullptr) return;
				auto found = this->managed.find(tracked.data());
				if (found == this->managed.end() || found->second.fullscreen()
				    || this->stateOf(found->second) != State::Tiled)
					return;
				qCDebug(logTiling) << "Restoring remembered fullscreen for" << fullscreenKey(tracked.data());
				this->toggleFullscreen(tracked.data(), false);
			});
		}
		if (pacedOpening) {
			const auto interval = firstOpening ? DWINDLE_FIRST_OPENING_INTERVAL_MS : DWINDLE_OPENING_INTERVAL_MS;
			want->nextOpening.setRemainingTime(interval, Qt::PreciseTimer);
		}
	}
}

void TilingManager::insertInto(
    Managed& m,
    Layout* layout,
    TrackedWindow* target,
    const QPoint* cursor
) {
	auto* window = m.window;

	if (m.layout == nullptr && (m.mode != TileMode::Normal || m.floatMoving)) {
		if (!m.fullscreenRestore.isNull()) m.preTiling = m.fullscreenRestore;
		if (m.fullscreen()) m.fullscreenRestore = QRect();
		else this->forgetFloat(m);
		m.floatFrame = QRect();
		m.floatPending = false;
		m.floatMoving = false;
		m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
		m.wasTopmost = false;
	}

	const auto newOpening = m.fresh && m.preTiling.isNull()
	    && (!layout->special || this->sceneShows(layout, 0))
	    && this->mAnimateMoves && DwindleScene::enabled()
	    && layout->tree->kind() != tiling::Kind::Master;
	if (m.preTiling.isNull()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) m.preTiling = raw;
	}

	if (target == nullptr && cursor != nullptr && layout->tree->kind() == tiling::Kind::Dwindle) {
		layout->tree->configure(this->options());
		for (auto id: layout->tree->ids()) {
			auto* existing = this->windowOf(id);
			if (existing == nullptr) continue;
			layout->tree->setHidden(id, this->stateOf(this->managed.at(existing)) == State::Hidden);
		}
		auto rects = monitorRects(monitorForScreen(layout->screen));
		if (rects.valid) layout->tree->compute(rects.work);
		target = this->windowOf(tiling::cursorInsertionTarget(*layout->tree, *cursor));
	}
	if ((cursor == nullptr || layout->tree->kind() != tiling::Kind::Dwindle)
	    && (target == nullptr || !layout->tree->contains(idOf(target))))
	{
		target = layout->lastFocused;
	}
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	auto targetId = target == nullptr ? tiling::LayoutEngine::Id(0) : idOf(target);

	layout->tree->configure(this->options());
	layout->tree->insert(idOf(window), targetId, cursor);
	this->clearMaximized(layout, window, true);

	if (auto* scroll = TilingManager::scrollingOf(layout)) {
		this->prepareScrolling(layout);
		auto focused = this->tracker->activeWindow() == window
		            || GetForegroundWindow() == window->hwnd();
		if (focused) {
			scroll->activate(idOf(window));
			layout->lastFocused = window;
		}
	}

	if (newOpening && layout->tree->area().isValid()) {
		const auto area = layout->tree->area();
		layout->tree->compute(area);
		m.openingFrame = this->stripTile(layout, window);
	}

	m.layout = layout;
	m.parked = false;
	m.target = QRect();
	m.accepted = QRect();
	m.corrections = 0;

	qCDebug(logTiling) << "Tiling" << window->addressHex() << window->appId() << "on"
	                   << VirtualDesktops::guidToString(layout->desktop) << layout->screen->name();

	this->markLayout(layout);
	emit this->windowTilingChanged(window);
}

void TilingManager::clearMaximized(Layout* layout, const TrackedWindow* except, bool animate) {
	if (layout == nullptr) return;
	for (auto& [other, existing]: this->managed) {
		if (other == except || !existing.focusMaximized()) continue;
		if (existing.layout == nullptr && this->floats(existing) && this->floatHome(existing, false) == layout) {
			this->restoreFloat(existing);
			continue;
		}
		if (existing.layout != layout) continue;
		existing.mode = nextMode(existing.mode, TileModeEvent::Restore);
		if (!animate) continue;
		existing.fullscreenToggled = true;
		this->restackTimer.start();
		this->markLayout(layout);
	}
}

void TilingManager::takeOut(Managed& m) {
	this->restoreTinyCorners(m);
	auto* layout = m.layout;
	if (layout == nullptr) return;
	m.mode = nextMode(m.mode, TileModeEvent::Restore);

	layout->tree->remove(idOf(m.window));
	if (layout->lastFocused == m.window) layout->lastFocused = nullptr;

	this->bringBack(m);
	m.layout = nullptr;
	m.target = QRect();
	m.openingFrame = {};
	m.accepted = QRect();
	m.verifying = false;
	this->unverified.remove(m.window);

	this->markLayout(layout);
	emit this->windowTilingChanged(m.window);
}

void TilingManager::prepareClose(TrackedWindow* window) {
	if (!DwindleScene::enabled() || !this->mAnimateMoves || this->barMoving
	    || this->dragging != nullptr || !startup::settled()
	    || this->desktops->shownIndex() != this->desktops->currentIndex())
		return;

	auto found = this->managed.find(window);
	if (found == this->managed.end()) {
		qCDebug(logTiling) << "Dwindle close unmanaged" << window->hwnd();
		return;
	}

	auto& m = found->second;
	if (this->floats(m)) {
		this->prepareFloatClose(m);
		return;
	}
	auto* layout = m.layout;
	if (layout == nullptr || !this->layoutAlive(layout)
	    || layout->tree->kind() == tiling::Kind::Master
	    || !this->sceneShows(layout, this->desktops->currentIndex())
	    || this->stateOf(m) != State::Tiled || m.fullscreen() || m.offscreen
	    || m.target.isEmpty() || this->moving.contains(window))
	{
		qCDebug(logTiling) << "Dwindle close ineligible" << window->hwnd() << m.target
		                  << m.fullscreen() << m.offscreen
		                  << this->moving.contains(window);
		return;
	}

	auto* monitor = monitorForScreen(layout->screen);
	const auto rects = monitorRects(monitor);
	if (rects.valid) {
		DwindleScene::snapshotClose(monitor, rects.work, window->hwnd());
		DwindleScene::prepareClose(monitor, rects.work, window->hwnd(), window->pid(), m.target, false);
		QList<DwindleTile> plan;
		bool changed = false;
		if (this->dwindlePlan(layout, rects.work, plan, changed, true))
			DwindleScene::present(monitor, rects.work, plan, true);
		DwindleScene::holdCloseSource(monitor, window->hwnd());
	}
}

void TilingManager::cacheNextCloseFrame() {
	if (!startup::settled() || this->dragging != nullptr || this->edgeDragState.window != nullptr
	    || this->barMoving || this->desktops->shownIndex() != this->desktops->currentIndex()) return;

	const auto windows = this->tracker->windows();
	for (qsizetype inspected = 0; inspected < windows.size(); ++inspected) {
		this->nextCloseWindow %= windows.size();
		auto* window = windows.at(this->nextCloseWindow++);
		auto found = this->managed.find(window);
		if (found == this->managed.end()) continue;
		const auto& m = found->second;
		if (this->floats(m)) {
			const auto special = Scratchpad::holds(window);
			if (m.mode != TileMode::Normal || m.floatMoving || m.staged()
			    || (special ? this->mSpecialOffset != 0 || this->mSpecialShifting
			                : window->desktop() != this->desktops->shownIndex()
			                      || (GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
			    || DwindleScene::presenting(monitorForScreen(window->screen())))
				continue;
			if (DwindleScene::cacheCloseFrame(window->hwnd(), this->managedFrame(m))) return;
			continue;
		}
		auto* layout = m.layout;
		if (layout == nullptr || !this->layoutAlive(layout)
		    || layout->tree->kind() == tiling::Kind::Master
		    || !this->sceneShows(layout, this->desktops->shownIndex())
		    || this->stateOf(m) != State::Tiled || m.fullscreen() || m.offscreen || m.staged()
		    || m.target.isEmpty() || this->moving.contains(window)) continue;

		auto* monitor = monitorForScreen(layout->screen);
		if (DwindleScene::presenting(monitor)) continue;
		if (DwindleScene::cacheCloseFrame(window->hwnd(), m.target)) return;
	}
}

void TilingManager::dropEmptyLayouts() {
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		if (iter->second->tree->isEmpty() && this->sceneFloats(iter->second.get()).isEmpty()) {
			auto* layout = iter->second.get();
			if (DwindleScene::enabled() && this->layoutAlive(layout)
			    && this->sceneShows(layout, this->desktops->currentIndex()))
			{
				auto* monitor = monitorForScreen(layout->screen);
				auto rects = monitorRects(monitor);
				if (rects.valid && DwindleScene::hasClosingTile(monitor, rects.work)) this->apply(layout);
			}
			this->dirtyLayouts.remove(layout);
			iter = this->layouts.erase(iter);
		} else {
			++iter;
		}
	}
}

void TilingManager::apply(Layout* layout) {
	if (!this->layoutAlive(layout)) return;
	if (!startup::settled() && this->mAnimateMoves && DwindleScene::enabled() && !layout->special
	    && this->sceneShows(layout, this->desktops->currentIndex()))
	{
		this->startupLayouts.insert(layout);
		return;
	}
	const auto animateRearrangement = std::exchange(layout->animateRearrangement, false);

	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (!rects.valid) return;

	layout->tree->configure(this->options());
	this->prepareScrolling(layout);

	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		auto hidden = window != nullptr && this->stateOf(this->managed.at(window)) == State::Hidden;
		layout->tree->setHidden(id, hidden);
	}

	layout->tree->compute(rects.work);

	auto presenting = false;
	if (DwindleScene::enabled() && this->sceneShows(layout, this->desktops->currentIndex())) {
		auto monitor = monitorForScreen(layout->screen);
		QList<DwindleTile> plan;
		bool changed = false;
		if (!this->dwindlePlan(layout, rects.work, plan, changed, false)) DwindleScene::dismiss(monitor);
		else {
			HWND focused = nullptr;
			for (auto id: layout->tree->ids()) {
				auto* member = this->windowOf(id);
				if (member == nullptr) continue;
				const auto& covering = this->managed.at(member);
				if (covering.fullscreen() || (covering.focusMaximized() && focused == nullptr))
					focused = member->hwnd();
			}
			for (auto* m: this->sceneFloats(layout)) {
				if (m->fullscreen() || (m->focusMaximized() && focused == nullptr)) focused = m->window->hwnd();
			}
			DwindleScene::present(monitor, rects.work, plan, changed, focused);
		}
		presenting = DwindleScene::presenting(monitor);
	}
	WinPanelWindow::finishFullscreenBarHandoff(monitorForScreen(layout->screen), presenting);

	auto* scroll = TilingManager::scrollingOf(layout);
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());

	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr || window == this->dragging) continue;

		auto& m = this->managed.at(window);
		m.openingFrame = {};
		m.startupMove = false;
		if (this->stateOf(m) != State::Tiled) continue;
		if (!presenting) {
			if (std::exchange(m.fullscreenLookPending, false)) setFullscreenLook(window->hwnd(), m.fullscreen());
			if (!m.fullscreen()) m.fullscreenTileRadius.reset();
		}

		auto parked = scroll != nullptr && scroll->isParked(id);
		m.stripTile = scroll != nullptr ? this->stripTile(layout, window) : QRect();
		auto restore = QRect();
		if (parked) restore = clampInto(scroll->visibleBox(id).adjusted(in, in, -in, -in), rects.work);
		this->markOffscreen(m, parked, restore);

		auto tile = this->tileRect(layout, window);
		if (m.maximizedTile && !layout->special) tile = maximizedFrame(window->hwnd(), rects.work);
		if (m.focusMaximized()) tile = this->tileRect(layout, window, true);
		if (m.fullscreen()) {
			tile = fullscreenFrame(window->hwnd(), rects.monitor);
			if (layout->special) tile.translate(0, this->mSpecialOffset);
		}
		const auto scenePlacement = presenting && !m.maximizedTile && !parked;
		const auto toggled = scenePlacement && m.fullscreenToggled;
		if (scenePlacement) m.fullscreenToggled = false;
		if (scenePlacement && (animateRearrangement || m.contractionHeld() || toggled) && this->holdContraction(m, tile))
			continue;
		if (m.contractionHeld()) {
			m.placement = nextPlacement(m.placement, TilePlacementEvent::Unhold);
			m.accepted = QRect();
		}
		const auto opening = m.target.isNull() && !m.dropped
		    && (m.fresh || isEarlyParked(window->hwnd()));
		if (scenePlacement && (m.staged() || opening)) {
			this->stage(m, tile);
			continue;
		}
		this->placeMoving(m, layout, tile);
		if (m.maximizePending && !this->moving.contains(window)) this->finishMaximize(m);
	}
	this->applyFloats(layout, presenting);
}

void TilingManager::openStartupLayouts() {
	const auto layouts = std::exchange(this->startupLayouts, {});
	for (auto* layout: layouts) {
		if (!this->layoutAlive(layout)) continue;
		for (auto id: layout->tree->ids()) {
			auto* window = this->windowOf(id);
			if (window == nullptr) continue;
			auto& m = this->managed.at(window);
			if (m.target.isNull() && !m.fullscreen() && this->stateOf(m) == State::Tiled) m.startupMove = true;
		}
		this->markLayout(layout);
	}
}

void TilingManager::applyFloats(Layout* layout, bool presenting) {
	for (auto* m: this->sceneFloats(layout)) {
		auto* hwnd = m->window->hwnd();
		if (!presenting) {
			if (std::exchange(m->fullscreenLookPending, false)) setFullscreenLook(hwnd, m->fullscreen());
			if (!m->fullscreen()) m->fullscreenTileRadius.reset();
		}
		const auto goal = this->floatGoal(*m, layout);
		if (m->floatOpening && presenting) {
			m->floatPending = false;
			continue;
		}
		if (std::exchange(m->floatOpening, false)) {
			m->placement = nextPlacement(m->placement, TilePlacementEvent::Release);
			this->forgetStage(*m);
			m->floatPending = true;
		}
		const auto actual = this->managedFrame(*m);
		const auto astray = layout->special && !presenting && goal != actual;
		const auto pending = std::exchange(m->floatPending, false) || goal != m->floatFrame || astray;
		m->floatFrame = goal;
		if (pending) {
			m->placement = nextPlacement(m->placement, TilePlacementEvent::Unhold);
			if (presenting && goal.width() <= actual.width() && goal.height() <= actual.height()
			    && goal.size() != actual.size())
			{
				m->placement = nextPlacement(m->placement, TilePlacementEvent::Hold);
				if (!this->healTimer.isActive()) this->healTimer.start();
				continue;
			}
			if (goal != actual) setFrame(hwnd, goal);
		}
		if (!presenting) {
			m->floatMoving = false;
			if (m->mode == TileMode::Normal) restoreTopmost(*m);
		}
	}
}

bool TilingManager::floats(const Managed& m) const {
	auto* window = m.window;
	return m.layout == nullptr && !m.elevated && !window->minimized() && !window->maximized()
	    && window->screen() != nullptr && this->stateOf(m) == State::Out
	    && (Scratchpad::holds(window) ? Scratchpad::instance()->shown() : window->desktop() >= 0);
}

TilingManager::Layout* TilingManager::floatHome(const Managed& m, bool create) {
	return this->layoutFor(m.window, create);
}

QList<TilingManager::Managed*> TilingManager::sceneFloats(Layout* layout) {
	QList<Managed*> found;
	for (auto& [window, m]: this->managed) {
		if (m.mode == TileMode::Normal && !m.floatMoving) continue;
		if (!this->floats(m) || this->floatHome(m, false) != layout) continue;
		found.append(&m);
	}
	return found;
}

QRect TilingManager::floatGoal(const Managed& m, const Layout* layout) const {
	const auto rects = monitorRects(monitorForScreen(layout->screen));
	if (!rects.valid) return m.floatFrame;
	const auto shift = layout->special ? this->mSpecialOffset : 0;
	if (m.fullscreen()) return fullscreenFrame(m.window->hwnd(), rects.monitor).translated(0, shift);
	if (!m.focusMaximized()) return m.floatFrame;
	const auto gap =
	    toPhysical(layout->special ? SPECIAL_GAPS_OUT : this->mGapsOut, layout->screen->devicePixelRatio());
	return rects.work.adjusted(gap, gap, -gap, -gap).translated(0, shift);
}

void TilingManager::coverOthers(Managed& m) {
	m.coveredAt = QDateTime::currentMSecsSinceEpoch();
	if (Scratchpad::holds(m.window)) {
		Scratchpad::raiseWindow(m.window->hwnd());
		return;
	}
	SetWindowPos(m.window->hwnd(), HWND_TOP, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void TilingManager::dropTopmost(Managed& m) {
	auto* hwnd = m.window->hwnd();
	if (Scratchpad::holds(m.window) || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) return;
	m.wasTopmost = true;
	SetPropW(hwnd, TOPMOST_PROP, propOf(1));
	SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void TilingManager::restoreTopmost(Managed& m) {
	if (!std::exchange(m.wasTopmost, false)) return;
	auto* hwnd = m.window->hwnd();
	RemovePropW(hwnd, TOPMOST_PROP);
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

bool TilingManager::canPin(TrackedWindow* window) const {
	auto found = this->managed.find(window);
	return found != this->managed.end() && found->second.mode == TileMode::Normal && this->floats(found->second);
}

void TilingManager::floatTo(Managed& m, const QRect& raw) {
	auto* hwnd = m.window->hwnd();
	auto* home = this->floats(m) ? this->floatHome(m, true) : nullptr;
	if (!this->mAnimateMoves || !DwindleScene::enabled() || home == nullptr
	    || !this->sceneShows(home, this->desktops->currentIndex()))
	{
		setRawRect(hwnd, raw);
		return;
	}
	m.floatFrame = frameFor(hwnd, raw);
	this->sendFloat(m);
}

bool TilingManager::openFloat(Managed& m) {
	auto* hwnd = m.window->hwnd();
	if (m.layout != nullptr || !m.fresh || !isEarlyParked(hwnd) || !this->mAnimateMoves || !DwindleScene::enabled()
	    || !startup::settled() || this->dragging != nullptr || this->edgeDragState.window != nullptr
	    || this->desktops->shownIndex() != this->desktops->currentIndex() || !IsWindowVisible(hwnd)
	    || !this->floats(m))
		return false;
	auto* home = this->floatHome(m, true);
	if (home == nullptr || home->tree->kind() == tiling::Kind::Master
	    || !this->sceneShows(home, this->desktops->currentIndex()))
		return false;
	auto raw = takeEarlyPark(hwnd);
	if (raw.isEmpty()) return false;
	const auto rects = monitorRects(monitorForScreen(home->screen));
	if (rects.valid) {
		raw.moveCenter(rects.work.center());
		raw = clampInto(raw, rects.work);
	}
	rememberParked(hwnd, raw);
	m.stageNoted = true;
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Stage);
	m.floatFrame = frameFor(hwnd, raw);
	m.floatSeen = m.floatFrame;
	m.floatOpening = true;
	if (!this->healTimer.isActive()) this->healTimer.start();
	this->sendFloat(m);
	return true;
}

void TilingManager::prepareFloatClose(Managed& m) {
	auto* window = m.window;
	auto* hwnd = window->hwnd();
	auto* home = this->floatHome(m, true);
	if (home == nullptr || home->tree->kind() == tiling::Kind::Master || m.fullscreen() || m.staged()
	    || !this->sceneShows(home, this->desktops->currentIndex())
	    || (!home->special && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0))
		return;
	auto* monitor = monitorForScreen(home->screen);
	const auto rects = monitorRects(monitor);
	const auto frame = this->managedFrame(m);
	if (!rects.valid || frame.isEmpty()) return;
	DwindleScene::snapshotClose(monitor, rects.work, hwnd);
	DwindleScene::prepareClose(monitor, rects.work, hwnd, window->pid(), frame, false);
	if (m.mode != TileMode::Normal) return;
	if (!m.floatMoving) {
		m.floatFrame = frame;
		m.floatPending = false;
		m.floatMoving = true;
	}
	QList<DwindleTile> plan;
	bool changed = false;
	if (this->dwindlePlan(home, rects.work, plan, changed, true))
		DwindleScene::present(monitor, rects.work, plan, true);
	DwindleScene::holdCloseSource(monitor, hwnd);
	this->markLayout(home);
}

bool TilingManager::closeFloat(Managed& m) {
	auto* window = m.window;
	auto* hwnd = window->hwnd();
	if (m.layout != nullptr || m.fullscreen() || m.floatSeen.isEmpty() || !DwindleScene::enabled()
	    || !this->mAnimateMoves || window->minimized() || (IsWindow(hwnd) && IsWindowVisible(hwnd))
	    || this->desktops->shownIndex() != this->desktops->currentIndex())
		return false;
	auto* pad = Scratchpad::instance();
	const auto special = Scratchpad::holds(window);
	if (!special && window->desktop() != this->desktops->currentIndex()) return false;
	if (special && !pad->shown()) return false;
	const auto native = toRECT(m.floatSeen);
	auto* screen = screenForMonitor(MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST));
	auto* home = special
	               ? this->layoutAt(SPECIAL_DESKTOP, screen, true)
	               : this->layoutAt(this->desktops->desktops().at(this->desktops->currentIndex()).id, screen, true);
	if (home == nullptr || home->tree->kind() == tiling::Kind::Master
	    || !this->sceneShows(home, this->desktops->currentIndex()))
		return false;
	auto* monitor = monitorForScreen(screen);
	const auto rects = monitorRects(monitor);
	if (!rects.valid) return false;
	if (DwindleScene::hasCloseFrame(hwnd))
		DwindleScene::prepareClose(monitor, rects.work, hwnd, window->pid(), m.floatSeen, true);
	if (!DwindleScene::hasClosingTile(monitor, rects.work)) return false;
	this->markLayout(home);
	return true;
}

void TilingManager::sendFloat(Managed& m) {
	m.floatPending = true;
	m.floatMoving = true;
	if (auto* home = this->floatHome(m, true)) this->markLayout(home);
}

void TilingManager::forgetFloat(Managed& m) {
	m.mode = nextMode(m.mode, TileModeEvent::Restore);
	m.fullscreenRestore = QRect();
	m.floatFrame = QRect();
	m.floatPending = false;
	m.floatMoving = false;
	m.floatOpening = false;
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
	restoreTopmost(m);
	unmarkFullscreen(m.window->hwnd());
	for (auto& [key, layout]: this->layouts) {
		if (this->sceneShows(layout.get(), this->desktops->currentIndex())) this->markLayout(layout.get());
	}
}

void TilingManager::restoreFloat(Managed& m) {
	auto* hwnd = m.window->hwnd();
	m.mode = nextMode(m.mode, TileModeEvent::Restore);
	m.floatFrame = frameFor(hwnd, std::exchange(m.fullscreenRestore, QRect()));
	unmarkFullscreen(hwnd);
	if (m.floatFrame.isNull()) return;
	this->sendFloat(m);
}

bool TilingManager::toggleFloatMaximize(Managed& m) {
	auto* window = m.window;
	auto* hwnd = window->hwnd();
	if (m.focusMaximized()) {
		this->restoreFloat(m);
		window->activate();
		return true;
	}

	if (m.fullscreen()) {
		m.mode = nextMode(m.mode, TileModeEvent::LeaveFullscreen);
		rememberFullscreen(window, false);
		markFullscreen(hwnd, m.fullscreenRestore, LeftMark::Maximized);
		this->updateFullscreenLook(m);
	} else {
		QRect raw;
		QRect frame;
		if (!readFrame(hwnd, raw, frame)) return false;
		if (m.floatMoving && !m.floatFrame.isNull()) raw = rawFor(hwnd, m.floatFrame);
		m.fullscreenRestore = raw;
		markFullscreen(hwnd, raw, LeftMark::Maximized);
		dropTopmost(m);
		const auto special = Scratchpad::holds(window);
		for (auto& [other, existing]: this->managed) {
			if (!existing.fullscreen() || Scratchpad::holds(other) != special) continue;
			if (special || (other->desktop() == window->desktop() && other->screen() == window->screen()))
				this->leaveFullscreen(existing);
		}
		this->clearMaximized(this->floatHome(m, true), window, true);
	}

	m.mode = nextMode(m.mode, TileModeEvent::ToggleMaximize);
	this->coverOthers(m);
	this->sendFloat(m);
	this->updateShellFullscreen();
	window->activate();
	return true;
}

void TilingManager::uncoverFor(const Managed& m) {
	auto* window = m.window;
	const auto special = Scratchpad::holds(window);
	if (!this->floats(m)) return;
	if (!special && (GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) return;
	auto changed = false;
	for (auto& [other, existing]: this->managed) {
		if (other == window || existing.mode == TileMode::Normal || other->minimized()) continue;
		if (Scratchpad::holds(other) != special) continue;
		if (!special && (other->desktop() != window->desktop() || other->screen() != window->screen())) continue;
		if (m.managedAt > existing.coveredAt || GetWindow(window->hwnd(), GW_OWNER) == other->hwnd()) continue;
		if (existing.fullscreen()) this->leaveFullscreen(existing);
		else if (existing.layout == nullptr) this->restoreFloat(existing);
		else this->clearMaximized(existing.layout, nullptr, true);
		changed = true;
	}
	if (changed) this->updateShellFullscreen();
}

void TilingManager::finishMaximize(Managed& m) {
	m.maximizePending = false;
	if (m.maximizedTile && !m.window->maximized()) m.window->setMaximized(true);
}

bool TilingManager::holdContraction(Managed& m, const QRect& tile) {
	if (m.target.isEmpty() || (m.staged() && !m.contractionHeld())) return false;
	const auto actual = this->managedFrame(m);
	if (tile.width() > actual.width() || tile.height() > actual.height() || tile.size() == actual.size())
		return false;

	if (!m.contractionHeld() || m.target != tile)
		qCDebug(logTiling) << "Holding Dwindle contraction" << m.window->hwnd() << actual << "to" << tile;
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Hold);
	if (!this->healTimer.isActive()) this->healTimer.start();
	m.target = tile;
	m.accepted = actual;
	m.verifying = false;
	m.corrections = 0;
	m.fresh = false;
	m.dropped = false;
	return true;
}

void TilingManager::stage(Managed& m, const QRect& tile) {
	auto* hwnd = m.window->hwnd();
	claimEarlyPark(hwnd);
	const auto actual = this->managedFrame(m);
	auto staged = parkedFrame(tile);
	if (m.contractionHeld()) staged.setSize(actual.size());
	if (!m.staged()) m.nativeFrameRefreshRequested = false;
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Stage);
	if (!m.offscreen && (!m.stageNoted || m.target != tile)) rememberParked(hwnd, rawFor(hwnd, tile));
	m.stageNoted = true;
	if (!this->healTimer.isActive()) this->healTimer.start();
	m.target = tile;
	m.accepted = staged;
	m.verifying = false;
	m.corrections = 0;
	if (actual != staged)
		setFrame(hwnd, staged, true, m.tinyCornersApplied ? &m.tinyFrameInsets : nullptr);
}

bool TilingManager::stageOccluding(HWND hwnd) {
	for (auto& [window, m]: this->managed) {
		if (window->hwnd() != hwnd) continue;
		if (m.layout == nullptr || (m.layout->special && !this->sceneShows(m.layout, 0)) || m.fullscreen()
		    || m.offscreen || m.target.isEmpty() || this->stateOf(m) != State::Tiled || IsHungAppWindow(hwnd))
			return false;
		const auto target = m.target;
		this->stage(m, target);
		return true;
	}
	return false;
}

void TilingManager::releaseStaged(const QList<HWND>& windows) {
	QTimer::singleShot(VERIFY_DELAY_MS, this, &WinPanelWindow::recheckFullscreenApp);
	for (auto& [window, m]: this->managed) {
		if (!windows.contains(window->hwnd())) continue;
		if (std::exchange(m.fullscreenLookPending, false)) setFullscreenLook(window->hwnd(), m.fullscreen());
		if (!m.fullscreen()) m.fullscreenTileRadius.reset();
		if (m.layout == nullptr) {
			const auto held = m.contractionHeld();
			m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
			m.unplacedSince = 0;
			m.floatMoving = false;
			if (m.mode == TileMode::Normal) restoreTopmost(m);
			const auto opening = std::exchange(m.floatOpening, false);
			if (opening) {
				this->forgetStage(m);
				if (Scratchpad::holds(window)) Scratchpad::raiseWindow(window->hwnd());
				else
					SetWindowPos(window->hwnd(), HWND_TOP, 0, 0, 0, 0,
					             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
			}
			if ((held || opening) && !m.floatFrame.isNull() && IsWindow(window->hwnd()))
				setFrame(window->hwnd(), m.floatFrame);
			continue;
		}
		if (!m.staged() && !m.contractionHeld()) continue;
		const auto wasStaged = m.staged();
		const auto wasHeld = m.contractionHeld();
		m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
		m.unplacedSince = 0;
		m.accepted = QRect();
		if (m.target.isNull() || this->stateOf(m) != State::Tiled) continue;
		if (wasStaged && m.layout != nullptr && m.layout->special) Scratchpad::raiseWindow(window->hwnd());
		else if (wasStaged)
			SetWindowPos(window->hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
		if (wasHeld)
			qCDebug(logTiling) << "Finishing Dwindle contraction" << window->hwnd() << m.target;
		this->updateTinyCorners(m, m.target);
		this->forgetStage(m);
		this->send(m, this->managedFrame(m), this->needsNativeFrameRefresh(m));
	}
}

bool TilingManager::dwindlePlan(
    Layout* layout,
    const QRect& work,
    QList<DwindleTile>& plan,
    bool& changed,
    bool still
) {
	if (!this->mAnimateMoves || this->dragging != nullptr || this->edgeDragState.window != nullptr
	    || !startup::settled() || this->desktops->shownIndex() != this->desktops->currentIndex()
	    || layout->tree->kind() == tiling::Kind::Master)
		return false;
	QSet<HWND> participants;
	auto* scroll = TilingManager::scrollingOf(layout);
	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr) continue;
		const auto& m = this->managed.at(window);
		if (this->stateOf(m) == State::Hidden) continue;
		if (this->stateOf(m) != State::Tiled || m.maximizedTile || m.elevated
		    || this->moving.contains(window))
			return false;
		auto tile = this->stripTile(layout, window);
		if (m.focusMaximized()) tile = this->tileRect(layout, window, true);
		if (m.fullscreen()) {
			const auto rects = monitorRects(monitorForScreen(layout->screen));
			if (!rects.valid) return false;
			tile = fullscreenFrame(window->hwnd(), rects.monitor);
			if (layout->special) tile.translate(0, this->mSpecialOffset);
		} else if (scroll == nullptr && !work.contains(tile)) {
			return false;
		}
		const auto placed = scroll != nullptr && m.offscreen ? m.stripTile : m.target;
		if (still) {
			if (placed != tile) return false;
			plan.append({window->hwnd(), tile, tile, true, {},
			             m.fullscreen() ? DwindleFullscreen::Active : DwindleFullscreen::None,
			             m.fullscreen() ? std::optional(0.0f) : std::nullopt});
		} else {
			auto previous = m.target.isEmpty() && !m.dropped && !m.startupMove ? QRect()
			              : scroll != nullptr && m.offscreen ? m.stripTile
			                                                 : this->managedFrame(m);
			auto settled = m.dropped || (!previous.isEmpty() && std::abs(previous.left() - placed.left()) <= 4
			                             && std::abs(previous.top() - placed.top()) <= 4
			                             && std::abs(previous.right() - placed.right()) <= 4
			                             && std::abs(previous.bottom() - placed.bottom()) <= 4);
			changed |= tile != placed;
			const auto leaving = !m.fullscreen() && m.fullscreenLookPending;
			const auto radius = m.fullscreen() ? std::optional(0.0f)
			                   : leaving ? m.fullscreenTileRadius : std::nullopt;
			const auto fullscreen = m.fullscreen() ? DwindleFullscreen::Active
			                        : leaving ? DwindleFullscreen::Leaving : DwindleFullscreen::None;
			plan.append({window->hwnd(), previous, tile, settled, m.openingFrame, fullscreen, radius});
		}
		participants.insert(window->hwnd());
	}
	for (auto* m: this->sceneFloats(layout)) {
		auto* hwnd = m->window->hwnd();
		if (!IsWindowVisible(hwnd)) continue;
		if (!layout->special && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) continue;
		const auto goal = this->floatGoal(*m, layout);
		const auto leaving = !m->fullscreen() && m->fullscreenLookPending;
		const auto radius = m->fullscreen() ? std::optional(0.0f) : leaving ? m->fullscreenTileRadius : std::nullopt;
		const auto fullscreen = m->fullscreen() ? DwindleFullscreen::Active
		                        : leaving ? DwindleFullscreen::Leaving : DwindleFullscreen::None;
		if (still && (m->floatPending || goal != m->floatFrame)) return false;
		const auto previous = still ? goal : m->floatOpening ? QRect() : this->managedFrame(*m);
		if (goal.isEmpty() || (previous.isEmpty() && !m->floatOpening)) return false;
		changed |= m->floatPending || goal != m->floatFrame;
		plan.append({hwnd, previous, goal, true, {}, fullscreen, radius, true});
		participants.insert(hwnd);
	}
	for (auto* window: this->tracker->windows()) {
		if (window->minimized() || window->desktop() != this->desktops->currentIndex()
		    || participants.contains(window->hwnd()) || !IsWindowVisible(window->hwnd())
		    || (GetWindowLongPtrW(window->hwnd(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
			continue;
		if (this->floatsFreely(window)) continue;
		QRect raw;
		QRect actual;
		if (!layout->special && readFrame(window->hwnd(), raw, actual) && actual.intersects(work)) return false;
	}
	return true;
}

bool TilingManager::floatsFreely(TrackedWindow* window) const {
	auto found = this->managed.find(window);
	if (found == this->managed.end()) return false;
	const auto& m = found->second;
	return m.layout == nullptr && !m.fullscreen() && !m.appFullscreen && !window->maximized()
	    && !window->fullscreen() && !window->borderlessFullscreen() && this->stateOf(m) == State::Out;
}

void TilingManager::coverNewWindow(HWND hwnd) {
	if (!this->mEnabled || !DwindleScene::enabled() || !IsWindow(hwnd) || IsWindowVisible(hwnd)) return;
	auto current = this->desktops->currentIndex();
	if (current < 0 || current >= this->desktops->count()) return;
	auto* pad = Scratchpad::instance();
	auto* monitor = pad->shown() ? pad->monitor() : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	auto* screen = screenForMonitor(monitor);
	if (screen == nullptr) return;
	auto* layout = pad->shown() ? this->layoutAt(SPECIAL_DESKTOP, screen, false)
	                            : this->layoutAt(this->desktops->desktops().at(current).id, screen, false);
	if (layout == nullptr || !this->sceneShows(layout, current)) return;
	auto rects = monitorRects(monitor);
	if (!rects.valid) return;
	QList<DwindleTile> plan;
	bool changed = false;
	if (this->dwindlePlan(layout, rects.work, plan, changed, true))
		DwindleScene::cover(monitor, rects.work, plan);
}

QRect TilingManager::goalFrame(HWND hwnd) const {
	if (!this->mEnabled || this->tracker == nullptr) return {};
	auto* window = this->tracker->windowFor(hwnd);
	auto found = this->managed.find(window);
	if (found == this->managed.end()) return {};
	const auto& m = found->second;
	if (m.layout == nullptr || m.layout->screen == nullptr || this->stateOf(m) != State::Tiled || m.fullscreen()
	    || m.focusMaximized())
		return {};
	auto tile = TilingManager::scrollingOf(m.layout) != nullptr ? this->stripTile(m.layout, window)
	          : m.target.isNull()                               ? this->tileRect(m.layout, window)
	                                                            : m.target;
	const auto rects = monitorRects(monitorForScreen(m.layout->screen));
	return rects.valid ? tile.intersected(rects.work) : tile;
}

QRect TilingManager::stripTile(const Layout* layout, TrackedWindow* window) const {
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return this->tileRect(layout, window);
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());
	auto shift = layout->special ? this->mSpecialOffset : 0;
	auto box = scroll->visibleBox(idOf(window));
	if (box.isEmpty()) return this->tileRect(layout, window);
	return QRect(box.left() + in, box.top() + in + shift, qMax(1, box.width() - 2 * in), qMax(1, box.height() - 2 * in));
}

QRect TilingManager::tileRect(const Layout* layout, TrackedWindow* window, bool fillLayout) const {
	auto area = layout->tree->area();
	auto box = fillLayout ? area : layout->tree->box(idOf(window));
	auto dpr = layout->screen->devicePixelRatio();
	auto in = toPhysical(this->mGapsIn, dpr);
	auto out = toPhysical(layout->special ? SPECIAL_GAPS_OUT : this->mGapsOut, dpr);
	auto shift = layout->special ? this->mSpecialOffset : 0;

	if (layout->tree->kind() == tiling::Kind::Scrolling) {
		return QRect(
		    box.left() + in,
		    box.top() + in + shift,
		    qMax(1, box.width() - 2 * in),
		    qMax(1, box.height() - 2 * in)
		);
	}

	auto left = box.left() == area.left() ? out : in;
	auto top = box.top() == area.top() ? out : in;
	auto right = rightOf(box) == rightOf(area) ? out : in;
	auto bottom = bottomOf(box) == bottomOf(area) ? out : in;

	return QRect(
	    box.left() + left,
	    box.top() + top + shift,
	    qMax(1, box.width() - left - right),
	    qMax(1, box.height() - top - bottom)
	);
}

QRect TilingManager::managedFrame(const Managed& m) const {
	QRect raw;
	QRect frame;
	if (!readFrame(m.window->hwnd(), raw, frame)) return QRect();
	return m.tinyCornersApplied ? frameWithInsets(raw, m.tinyFrameInsets) : frame;
}

void TilingManager::restoreTinyCorners(Managed& m) {
	if (m.tinyChromiumCorners) {
		auto* hwnd = m.window->hwnd();
		if (IsWindow(hwnd)) {
			auto keepRound = DecorationManager::instance()->corners() == QStringLiteral("round");
			restoreChromiumCorners(hwnd, keepRound);
		}
		forgetTinyCorners(hwnd);
		m.tinyChromiumCorners = false;
	}
	if (m.tinySmallCorners) {
		auto* hwnd = m.window->hwnd();
		if (IsWindow(hwnd)) {
			auto previous = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(m.previousCornerPreference);
			DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous));
			RemovePropW(hwnd, TINY_CORNER_PROP);
		}
		forgetTinyCorners(hwnd);
		m.tinySmallCorners = false;
	}
	if (!m.tinyCornersApplied) return;
	auto* hwnd = m.window->hwnd();
	if (IsWindow(hwnd)) {
		if (m.tinyBorderSuppressed) {
			auto border = COLORREF(DWMWA_COLOR_DEFAULT);
			auto* decorations = DecorationManager::instance();
			if (decorations->borders() == QStringLiteral("none")) {
				border = DWMWA_COLOR_NONE;
			} else if (decorations->borders() == QStringLiteral("colors")) {
				auto active = this->tracker != nullptr && this->tracker->activeWindow() == m.window;
				auto color = active ? decorations->activeColor() : decorations->inactiveColor();
				if (color.isValid()) border = RGB(color.red(), color.green(), color.blue());
			}
			DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
		}
		if (SetWindowRgn(hwnd, nullptr, TRUE) != 0) RemovePropW(hwnd, TINY_CORNER_PROP);
		auto current = DWM_WINDOW_CORNER_PREFERENCE {};
		if (SUCCEEDED(DwmGetWindowAttribute(
		        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &current, sizeof(current)
		    )) && (current == DWMWCP_DONOTROUND || current == DWMWCP_ROUNDSMALL))
		{
			auto previous = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(m.previousCornerPreference);
			DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous));
		}
	}
	forgetTinyCorners(hwnd);
	m.tinyCornersApplied = false;
	m.tinyBorderSuppressed = false;
	m.tinyCornersRect = QRect();
	m.tinyFrameInsets = QMargins();
}

void TilingManager::updateTinyCorners(Managed& m, const QRect& frame) {
	if (m.layout == nullptr || m.offscreen || m.fullscreen() || frame.isEmpty()) {
		this->restoreTinyCorners(m);
		return;
	}

	auto* hwnd = m.window->hwnd();
	if (!IsWindow(hwnd) || IsHungAppWindow(hwnd)) return;
	if (DwindleScene::enabled() && windowClass(hwnd) == QStringLiteral("Chrome_WidgetWin_1")) {
		const auto dpi = GetDpiForWindow(hwnd);
		const auto limit = dpi == 0 ? CHROMIUM_CORNER_LIMIT : MulDiv(CHROMIUM_CORNER_LIMIT, dpi, 96);
		if ((m.layout->special && !this->sceneShows(m.layout, 0))
		    || m.layout->tree->kind() != tiling::Kind::Dwindle
		    || (frame.width() >= limit && frame.height() >= limit)
		    || DecorationManager::instance()->corners() != QStringLiteral("default"))
		{
			this->restoreTinyCorners(m);
			return;
		}

		if (m.tinyChromiumCorners) return;
		auto previous = DWM_WINDOW_CORNER_PREFERENCE {};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous)))
		    || previous != DWMWCP_DEFAULT)
		{
			return;
		}
		if (!SetPropW(hwnd, CHROMIUM_CORNER_PROP, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)))) return;
		auto preference = DWMWCP_ROUND;
		if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference)))) {
			RemovePropW(hwnd, CHROMIUM_CORNER_PROP);
			return;
		}
		m.tinyChromiumCorners = true;
		rememberTinyCorners(hwnd, static_cast<int>(previous));
		return;
	}

	if (frame.width() >= TINY_CORNER_LIMIT && frame.height() >= TINY_CORNER_LIMIT) {
		this->restoreTinyCorners(m);
		return;
	}
	if (!m.tinyCornersApplied && hasSystemCaption(hwnd)) {
		if (m.tinySmallCorners) return;
		auto previous = DWM_WINDOW_CORNER_PREFERENCE {};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous)))) {
			previous = DWMWCP_DEFAULT;
		}
		if (!markTinyCornerOwner(hwnd, static_cast<int>(previous))) return;
		auto smallCorners = DWMWCP_ROUNDSMALL;
		if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &smallCorners, sizeof(smallCorners)))) {
			RemovePropW(hwnd, TINY_CORNER_PROP);
			return;
		}
		m.previousCornerPreference = static_cast<int>(previous);
		m.tinySmallCorners = true;
		rememberTinyCorners(hwnd, m.previousCornerPreference);
		return;
	}
	QRect raw;
	QRect visible;
	if (!readFrame(hwnd, raw, visible)) return;

	if (!m.tinyCornersApplied) {
		m.tinyFrameInsets = QMargins(
		    visible.left() - raw.left(),
		    visible.top() - raw.top(),
		    rightOf(raw) - rightOf(visible),
		    bottomOf(raw) - bottomOf(visible)
		);
	}
	auto regionBox = QRect(
	    m.tinyFrameInsets.left(),
	    m.tinyFrameInsets.top(),
	    qMax(1, raw.width() - m.tinyFrameInsets.left() - m.tinyFrameInsets.right()),
	    qMax(1, raw.height() - m.tinyFrameInsets.top() - m.tinyFrameInsets.bottom())
	);
	if (regionBox == m.tinyCornersRect && m.tinyCornersApplied) {
		auto nativeCorners = DWMWCP_ROUNDSMALL;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &nativeCorners, sizeof(nativeCorners));
		auto border = COLORREF(DWMWA_COLOR_NONE);
		if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border)))) {
			m.tinyBorderSuppressed = true;
			rememberTinyBorder(hwnd);
		}
		return;
	}

	if (!m.tinyCornersApplied) {
		auto* probe = CreateRectRgn(0, 0, 0, 0);
		if (probe == nullptr) return;
		auto customRegion = GetWindowRgn(hwnd, probe) != ERROR;
		DeleteObject(probe);
		if (customRegion) return;

		auto previous = DWM_WINDOW_CORNER_PREFERENCE {};
		if (FAILED(DwmGetWindowAttribute(
		        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous)
		    ))) return;
		m.previousCornerPreference = static_cast<int>(previous);
	}

	auto radius = qMin(TINY_CORNER_RADIUS, qMin(regionBox.width(), regionBox.height()) / 2);
	auto diameter = qMax(1, 2 * radius);
	auto* region = CreateRoundRectRgn(
	    regionBox.left(),
	    regionBox.top(),
	    rightOf(regionBox),
	    bottomOf(regionBox),
	    diameter,
	    diameter
	);
	if (region == nullptr) return;

	if (!m.tinyCornersApplied) {
		if (!markTinyCornerOwner(hwnd, m.previousCornerPreference)) {
			DeleteObject(region);
			return;
		}
		auto nativeCorners = DWMWCP_ROUNDSMALL;
		if (FAILED(DwmSetWindowAttribute(
		        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &nativeCorners, sizeof(nativeCorners)
		    )))
		{
			RemovePropW(hwnd, TINY_CORNER_PROP);
			DeleteObject(region);
			return;
		}
	}

	if (SetWindowRgn(hwnd, region, TRUE) == 0) {
		if (!m.tinyCornersApplied) RemovePropW(hwnd, TINY_CORNER_PROP);
		DeleteObject(region);
		if (!m.tinyCornersApplied) {
			auto previous = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(m.previousCornerPreference);
			DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous));
		}
		return;
	}
	if (!m.tinyCornersApplied) rememberTinyCorners(hwnd, m.previousCornerPreference);
	m.tinyCornersApplied = true;
	m.tinyCornersRect = regionBox;
	if (!m.tinyBorderSuppressed) {
		auto border = COLORREF(DWMWA_COLOR_NONE);
		if (SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border)))) {
			m.tinyBorderSuppressed = true;
			rememberTinyBorder(hwnd);
		}
	}
}

void TilingManager::reassertTinyDecorations() {
	for (auto& [window, m]: this->managed) {
		if (!m.tinyCornersApplied || m.offscreen || m.layout == nullptr) continue;
		if (this->stateOf(m) != State::Tiled || !IsWindow(window->hwnd())) continue;
		if (GetPropW(window->hwnd(), TINY_CORNER_PROP) == nullptr) continue;

		auto corner = DWMWCP_ROUNDSMALL;
		DwmSetWindowAttribute(window->hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		auto border = COLORREF(DWMWA_COLOR_NONE);
		DwmSetWindowAttribute(window->hwnd(), DWMWA_BORDER_COLOR, &border, sizeof(border));
	}
}

bool TilingManager::needsNativeFrameRefresh(Managed& m) {
	if (!DwindleScene::enabled() || m.layout == nullptr
	    || (m.layout->special && !this->sceneShows(m.layout, 0)) || m.staged()
	    || m.offscreen || m.fullscreen() || m.layout->tree->kind() != tiling::Kind::Dwindle)
		return false;

	auto* hwnd = m.window->hwnd();
	if (windowClass(hwnd) != QStringLiteral("Chrome_WidgetWin_1")) return false;

	QRect raw;
	QRect visible;
	if (!readFrame(hwnd, raw, visible) || raw.isEmpty()) return false;
	if (raw != visible) {
		m.nativeFrameRefreshRequested = false;
		return false;
	}

	const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	BOOL enabled = FALSE;
	return !m.nativeFrameRefreshRequested && IsWindowVisible(hwnd) && !IsHungAppWindow(hwnd)
	    && (style & (WS_CAPTION | WS_THICKFRAME)) == (WS_CAPTION | WS_THICKFRAME)
	    && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_NCRENDERING_ENABLED, &enabled, sizeof(enabled)))
	    && enabled;
}

void TilingManager::forgetStage(Managed& m) {
	if (std::exchange(m.stageNoted, false) && !m.offscreen) forgetParked(m.window->hwnd());
}

void TilingManager::place(Managed& m, const QRect& tile) {
	if (m.contractionHeld()) return;
	claimEarlyPark(m.window->hwnd());
	this->forgetStage(m);
	if (m.maximizedTile && m.window->maximized()) {
		m.target = tile;
		m.verifying = false;
		return;
	}
	auto frame = this->managedFrame(m);
	this->updateTinyCorners(m, frame);
	const auto refreshNativeFrame = this->needsNativeFrameRefresh(m);

	const auto moved = tile != m.target;
	if (moved) {
		m.target = tile;
		m.accepted = QRect();
		m.corrections = 0;
	}

	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - m.lastCorrection > CORRECTION_RESET_MS) m.corrections = 0;

	switch (placeTile({
	    .moved = moved,
	    .refresh = refreshNativeFrame,
	    .inPlace = frame == tile,
	    .accepted = !m.accepted.isNull() && frame == m.accepted,
	    .waiting = m.verifying && frame == m.before,
	    .exhausted = m.corrections >= MAX_CORRECTIONS,
	}))
	{
	case TileStep::Settled: m.verifying = false; break;
	case TileStep::Leave: break;
	case TileStep::Send: this->send(m, frame, refreshNativeFrame); break;
	case TileStep::Correct:
		m.corrections++;
		m.lastCorrection = now;
		this->send(m, frame);
		break;
	case TileStep::GiveUp: this->giveUp(m, frame); break;
	}
}

void TilingManager::send(Managed& m, const QRect& frame, bool refreshNativeFrame) {
	m.before = frame;
	m.verifying = true;
	m.wasHung = false;
	m.deadline = QDateTime::currentMSecsSinceEpoch() + DEADLINE_MS;
	setFrame(m.window->hwnd(), m.target, true,
	         m.tinyCornersApplied ? &m.tinyFrameInsets : nullptr, false, refreshNativeFrame);
	if (refreshNativeFrame) {
		m.nativeFrameRefreshRequested = true;
		auto window = QPointer<TrackedWindow>(m.window);
		QTimer::singleShot(25, this, [this, window] {
			if (window != nullptr) this->onRectChanged(window);
		});
	}
	this->armDeadline();
}

void TilingManager::armDeadline() {
	auto next = std::numeric_limits<qint64>::max();
	for (auto& [window, m]: this->managed) {
		if (!m.verifying) continue;
		if (m.layout == nullptr || m.target.isNull() || window == this->dragging
		    || this->stateOf(m) != State::Tiled)
		{
			m.verifying = false;
			continue;
		}
		next = qMin(next, m.deadline);
	}

	if (next == std::numeric_limits<qint64>::max()) {
		this->deadlineTimer.stop();
		return;
	}

	auto remaining = next - QDateTime::currentMSecsSinceEpoch();
	remaining = qBound(qint64(0), remaining, qint64(DEADLINE_MS));
	this->deadlineTimer.start(static_cast<int>(remaining));
}

namespace {

enum class Layer : quint8 { Ignored, Floating, Blocking };

Layer untiledLayer(HWND hwnd, bool tracked) {
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || isOwnProcessWindow(hwnd)) return Layer::Ignored;

	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked != 0) return Layer::Ignored;

	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	constexpr auto PASSIVE = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
	if ((exStyle & PASSIVE) != 0) return Layer::Ignored;

	BYTE alpha = 255;
	DWORD flags = 0;
	if ((exStyle & WS_EX_LAYERED) != 0 && GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags)
	    && (flags & LWA_ALPHA) != 0 && alpha == 0)
	{
		return Layer::Ignored;
	}

	static const auto shellClasses = QSet<QString> {
	    QStringLiteral("Progman"),
	    QStringLiteral("WorkerW"),
	    QStringLiteral("Shell_TrayWnd"),
	    QStringLiteral("Shell_SecondaryTrayWnd"),
	};
	if (!tracked && shellClasses.contains(windowClass(hwnd))) return Layer::Ignored;

	RECT rect {};
	if (!GetWindowRect(hwnd, &rect)) return Layer::Ignored;
	auto width = rect.right - rect.left;
	auto height = rect.bottom - rect.top;
	if (width < NOTICE_MINIMUM || height < NOTICE_MINIMUM) return Layer::Ignored;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	auto covers = width > work.width() * FLOATING_MAXIMUM && height > work.height() * FLOATING_MAXIMUM;
	if (IsZoomed(hwnd) || (covers && !tracked)) return Layer::Blocking;
	return Layer::Floating;
}

}

QRect TilingManager::floatingRect(const QRect& tile, QSize size, const QRect& work) {
	if (size.isEmpty())
		size = QSize(qRound(work.width() * FLOATING_WIDTH), qRound(work.height() * FLOATING_HEIGHT));
	if (std::abs(size.width() - tile.width()) < FLOATING_TOLERANCE
	    && std::abs(size.height() - tile.height()) < FLOATING_TOLERANCE)
		size += QSize(10, 10);
	auto rect = QRect(QPoint(), size);
	rect.moveCenter(tile.center());
	return rect;
}

void TilingManager::collectLayers(
    QSet<HWND>& blocking,
    QSet<HWND>& tiled,
    QSet<HWND>& skipped
) const {
	for (const auto& [window, m]: this->managed) {
		auto* hwnd = window->hwnd();
		if (window->desktop() != this->desktops->currentIndex() || Scratchpad::holds(window)) continue;
		if (window->minimized() || !IsWindowVisible(hwnd)) continue;
		if (window->fullscreen() || window->borderlessFullscreen() || m.fullscreen()
		    || (m.focusMaximized() && m.layout != nullptr))
		{
			blocking.insert(hwnd);
			continue;
		}
		if (m.layout == nullptr) continue;
		auto settled = !m.staged() && !m.offscreen && !IsHungAppWindow(hwnd);
		if (settled && this->stateOf(m) == State::Tiled) tiled.insert(hwnd);
		else skipped.insert(hwnd);
	}
}

void TilingManager::restack() {
	if (!this->mEnabled || this->tracker == nullptr || this->dragging != nullptr) return;
	if (this->desktops->shownIndex() != this->desktops->currentIndex()) return;

	QSet<HWND> blocking;
	QSet<HWND> tiled;
	QSet<HWND> skipped;
	this->collectLayers(blocking, tiled, skipped);
	if (tiled.isEmpty()) return;

	struct Search {
		const QSet<HWND>& blocking;
		const QSet<HWND>& tiled;
		const QSet<HWND>& skipped;
		const WindowTracker* tracker;
		QList<HWND> pending;
		QList<HWND> above;
		HWND lowest = nullptr;
	};
	auto search = Search {.blocking = blocking, .tiled = tiled, .skipped = skipped, .tracker = this->tracker};
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param);
		    if (search->blocking.contains(hwnd)) return FALSE;
		    if (search->skipped.contains(hwnd)) return TRUE;
		    if (search->tiled.contains(hwnd)) {
			    search->pending.append(hwnd);
			    return TRUE;
		    }
		    auto* window = search->tracker->windowFor(hwnd);
		    if (Scratchpad::holds(window)) return TRUE;
		    auto layer = untiledLayer(hwnd, window != nullptr);
		    if (layer == Layer::Blocking) return FALSE;
		    if (layer == Layer::Floating) {
			    search->lowest = hwnd;
			    search->above.append(search->pending);
			    search->pending.clear();
		    }
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);

	for (auto* hwnd: search.above) {
		qCDebug(logTiling) << "Keeping" << hwnd << "under" << search.lowest << windowClass(search.lowest);
		constexpr UINT flags =
		    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
		SetWindowPos(hwnd, search.lowest, 0, 0, 0, 0, flags);
	}
}

void TilingManager::restoreFloating(Managed& m, bool besideTile) {
	this->restoreTinyCorners(m);
	auto* hwnd = m.window->hwnd();
	auto previous = m.preTiling;
	m.preTiling = QRect();

	auto parkedAt = m.offscreen ? m.restore : QRect();
	if (m.offscreen) {
		forgetParked(hwnd);
		m.offscreen = false;
		m.restore = QRect();
	}

	if (previous.isNull()) previous = parkedAt;
	if (previous.isNull() && !besideTile) return;

	if (!previous.isNull() && (IsIconic(hwnd) || IsZoomed(hwnd))) {
		restorePlacement(hwnd, onScreen(previous));
		return;
	}

	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;
	if (!parkedAt.isNull()) raw = parkedAt;

	if (besideTile) {
		const auto area = this->floatArea(m.window, raw);
		const auto frame = frameFor(hwnd, floatingRect(raw, floatSizeOf(hwnd), area));
		this->floatTo(m, rawFor(hwnd, clampInto(frame, area)));
		return;
	}

	auto rect = previous;
	auto previousRect = toRECT(previous);
	auto rawRect = toRECT(raw);
	auto* then = MonitorFromRect(&previousRect, MONITOR_DEFAULTTONEAREST);
	auto* now = MonitorFromRect(&rawRect, MONITOR_DEFAULTTONEAREST);

	if (then != now) {
		rect.moveCenter(raw.center());
		rect = clampInto(rect, monitorRects(now).work);
	}

	this->floatTo(m, onScreen(rect));
}

tiling::ScrollingLayout* TilingManager::scrollingOf(const Layout* layout) {
	if (layout == nullptr || layout->tree == nullptr) return nullptr;
	if (layout->tree->kind() != tiling::Kind::Scrolling) return nullptr;
	return static_cast<tiling::ScrollingLayout*>(layout->tree.get());
}

void TilingManager::prepareScrolling(Layout* layout) const {
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr || layout->screen == nullptr) return;

	auto dpr = layout->screen->devicePixelRatio();
	scroll->setGaps(toPhysical(this->mGapsIn, dpr), toPhysical(this->mGapsOut, dpr));

	QList<QRect> others;
	for (auto* screen: QGuiApplication::screens()) {
		if (screen == layout->screen) continue;
		auto rect = physicalMonitor(screen);
		if (rect.isValid()) others.append(rect);
	}

	scroll->setMonitors(physicalMonitor(layout->screen), others);
}

tiling::ScrollingLayout* TilingManager::scrollingFor(TrackedWindow* window, Layout** layout) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return nullptr;

	auto& m = iter->second;
	auto* scroll = TilingManager::scrollingOf(m.layout);
	if (scroll == nullptr || this->stateOf(m) != State::Tiled) return nullptr;

	*layout = m.layout;
	this->prepareScrolling(m.layout);
	if (scroll->active() != idOf(window)) scroll->activate(idOf(window));
	return scroll;
}

void TilingManager::settle(Layout* layout) {
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return;

	this->prepareScrolling(layout);
	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (rects.valid) scroll->compute(rects.work);
	this->markLayout(layout);

	auto* window = this->windowOf(scroll->active());
	if (window == nullptr) return;

	auto* foreground = this->tracker->windowFor(GetForegroundWindow());
	if (window == foreground) return;
	if (foreground == nullptr && window == this->tracker->activeWindow()) return;
	if (this->stateOf(this->managed.at(window)) == State::Out) return;

	layout->lastFocused = window;
	window->activate();
}

void TilingManager::setSpecialOffset(int offset) {
	this->mSpecialOffset = offset;
	if (!this->mEnabled) return;
	if (offset != 0 && DwindleScene::enabled()) DwindleScene::drop(Scratchpad::instance()->monitor());
	this->mSpecialShifting = true;
	for (auto& [window, m]: this->managed) {
		if (m.mode != TileMode::Normal && Scratchpad::holds(window) && this->floats(m))
			this->markLayout(this->floatHome(m, true));
	}
	this->sync();
	for (auto& [key, layout]: this->layouts) {
		if (layout->special) this->apply(layout.get());
	}
	this->mSpecialShifting = false;
	if (offset != 0 || !Scratchpad::instance()->shown()) return;
	auto* covering = this->specialCoveringWindow();
	auto found = this->managed.find(covering);
	if (found == this->managed.end()) return;
	auto* layout = found->second.layout != nullptr ? found->second.layout : this->floatHome(found->second, false);
	if (layout != nullptr) this->apply(layout);
}

QHash<HWND, QRect> TilingManager::specialFrames() {
	QHash<HWND, QRect> frames;
	for (auto& [window, m]: this->managed) {
		if (m.mode == TileMode::Normal || !Scratchpad::holds(window) || !this->floats(m)) continue;
		if (auto* layout = this->floatHome(m, true))
			frames.insert(window->hwnd(), this->floatGoal(m, layout).translated(0, -this->mSpecialOffset));
	}
	for (const auto& [key, layout]: this->layouts) {
		if (!layout->special) continue;
		auto monitor = physicalMonitor(layout->screen);
		for (auto id: layout->tree->ids()) {
			if (layout->tree->isHidden(id)) continue;
			auto* window = this->windowOf(id);
			if (window == nullptr) continue;
			auto found = this->managed.find(window);
			if (found == this->managed.end() || this->stateOf(found->second) != State::Tiled) continue;
			auto frame = found->second.fullscreen()
			               ? fullscreenFrame(window->hwnd(), monitor)
			               : this->tileRect(layout.get(), window, found->second.focusMaximized())
			                     .translated(0, -this->mSpecialOffset);
			frames.insert(window->hwnd(), frame);
		}
	}
	return frames;
}

TrackedWindow* TilingManager::tileAt(const QPoint& point) {
	if (this->tracker == nullptr || this->desktops == nullptr) return nullptr;
	auto* screen = this->tracker->screenFor(MonitorFromPoint({point.x(), point.y()}, MONITOR_DEFAULTTONEAREST));
	if (screen == nullptr) return nullptr;

	Layout* layout = nullptr;
	auto* scratchpad = Scratchpad::instance();
	if (scratchpad->shown() && screenForMonitor(scratchpad->monitor()) == screen) {
		layout = this->layoutAt(SPECIAL_DESKTOP, screen, false);
	} else {
		auto current = this->desktops->currentIndex();
		if (current < 0 || current >= this->desktops->count()) return nullptr;
		layout = this->layoutAt(this->desktops->desktops().at(current).id, screen, false);
	}
	if (layout == nullptr) return nullptr;

	for (auto id: layout->tree->ids()) {
		if (layout->tree->isHidden(id) || layout->tree->isParked(id)) continue;
		if (!layout->tree->box(id).contains(point)) continue;
		auto* window = this->windowOf(id);
		if (window != nullptr && this->stateOf(this->managed.at(window)) == State::Tiled) return window;
	}
	return nullptr;
}

bool TilingManager::focusIdleStrip() {
	if (this->tracker == nullptr || this->desktops == nullptr) return false;
	if (this->tracker->windowFor(GetForegroundWindow()) != nullptr) return false;

	auto* active = this->tracker->activeWindow();
	if (active != nullptr && this->managed.contains(active)) return false;

	auto current = this->desktops->currentIndex();
	if (current < 0 || current >= this->desktops->count()) return false;

	auto point = POINT {};
	GetCursorPos(&point);
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
	if (screen == nullptr) screen = focusedScreen();

	auto* layout = this->layoutAt(this->desktops->desktops().at(current).id, screen, false);
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return false;

	auto* window = this->windowOf(scroll->active());
	if (window == nullptr || this->stateOf(this->managed.at(window)) != State::Tiled) return false;

	qCDebug(logTiling) << "No window has the focus; focusing the strip's column" << window->addressHex();
	this->settle(layout);
	return true;
}

void TilingManager::refocus(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

	auto current = this->desktops->currentIndex();
	if (current < 0 || current >= this->desktops->count()) return;
	if (!IsEqualGUID(this->desktops->desktops().at(current).id, layout->desktop)) return;

	if (auto* active = this->tracker->activeWindow()) {
		auto iter = this->managed.find(active);
		if (iter == this->managed.end() || iter->second.layout != layout) return;
	}

	this->settle(layout);
}

void TilingManager::markOffscreen(Managed& m, bool offscreen, const QRect& restore) {
	auto* hwnd = m.window->hwnd();

	if (!offscreen) {
		if (!m.offscreen) return;
		m.offscreen = false;
		m.restore = QRect();
		forgetParked(hwnd);
		return;
	}

	auto raw = rawFor(hwnd, restore);
	if (m.offscreen && raw == m.restore) return;
	m.offscreen = true;
	m.restore = raw;
	rememberParked(hwnd, raw);
}

void TilingManager::unpark(Managed& m) {
	if (!m.offscreen) return;

	auto* hwnd = m.window->hwnd();
	auto restore = m.restore;
	m.offscreen = false;
	m.restore = QRect();
	m.target = QRect();
	forgetParked(hwnd);

	if (restore.isNull() || !IsWindow(hwnd)) return;

	if (IsIconic(hwnd) || IsZoomed(hwnd)) restorePlacement(hwnd, onScreen(restore));
	else setRawRect(hwnd, onScreen(restore));
}

void TilingManager::unparkAll() {
	for (auto& [window, m]: this->managed) {
		this->bringBack(m);
		this->unpark(m);
	}
	this->stopMotions();
}

void TilingManager::stopMotions() {
	const auto stopped = std::exchange(this->moving, {});
	for (auto iter = stopped.cbegin(); iter != stopped.cend(); ++iter) {
		if (!this->managed.contains(iter.key())) continue;
		auto* hwnd = iter.key()->hwnd();
		WindowMotion::instance()->cancel(hwnd);
		if (IsWindow(hwnd)) setFrame(hwnd, iter.value());
	}
}

void TilingManager::bringBack(Managed& m) {
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
	m.unplacedSince = 0;
	if (!std::exchange(m.stageNoted, false)) return;

	auto* hwnd = m.window->hwnd();
	if (!m.offscreen) forgetParked(hwnd);
	m.floatOpening = false;
	const auto target = m.layout == nullptr ? m.floatFrame : m.target;
	if (target.isNull() || !IsWindow(hwnd)) return;
	if (IsIconic(hwnd) || IsZoomed(hwnd)) restorePlacement(hwnd, onScreen(target));
	else setFrame(hwnd, onScreen(target));
}

void TilingManager::auditTrees() {
	for (auto& [key, layout]: this->layouts) {
		TrackedWindow* maximized = nullptr;
		for (auto id: layout->tree->ids()) {
			auto* window = this->windowOf(id);
			auto* m = window == nullptr ? nullptr : &this->managed.at(window);
			if (m == nullptr || m->layout != layout.get()) {
				qCWarning(logTiling) << "Tile" << id << "has no window here; closing its gap";
				layout->tree->remove(id);
				this->dirtyLayouts.insert(layout.get());
				continue;
			}
			if (!m->focusMaximized()) continue;
			if (maximized == nullptr) {
				maximized = window;
				continue;
			}
			qCWarning(logTiling) << "Two maximized tiles in one layout; keeping the last focused";
			auto& other = this->managed.at(maximized);
			auto& older = other.focusStamp < m->focusStamp ? other : *m;
			older.mode = nextMode(older.mode, TileModeEvent::Restore);
			if (&older == &other) maximized = window;
			this->dirtyLayouts.insert(layout.get());
		}
	}

	for (auto& [window, m]: this->managed) {
		if (m.layout == nullptr) continue;
		auto known = std::ranges::any_of(this->layouts, [&m](const auto& entry) {
			return entry.second.get() == m.layout;
		});
		if (known && m.layout->tree->contains(idOf(window))) continue;
		qCWarning(logTiling) << "Window" << window->addressHex() << window->appId()
		                     << "lost its tile; tiling it again";
		m.layout = nullptr;
		m.target = QRect();
		this->dirtyWindows.insert(window);
	}
}

void TilingManager::heal() {
	if (!this->mEnabled) return;

	const auto now = QDateTime::currentMSecsSinceEpoch();
	const auto busy = this->dragging != nullptr || this->edgeDragState.window != nullptr
	               || this->desktops->shownIndex() != this->desktops->currentIndex();
	auto again = false;
	QList<HWND> stuck;
	QSet<HMONITOR> endless;

	for (auto& [window, m]: this->managed) {
		if (m.placement == TilePlacement::Placed) m.unplacedSince = 0;
		else if (m.unplacedSince == 0) m.unplacedSince = now;

		const auto alive = m.layout != nullptr && this->layoutAlive(m.layout);
		auto* monitor = alive ? monitorForScreen(m.layout->screen) : nullptr;
		const auto animated = monitor != nullptr && DwindleScene::presenting(monitor);
		const auto shown = alive && this->sceneShows(m.layout, this->desktops->currentIndex());
		const auto settled = shown && !m.offscreen && !m.verifying && !m.target.isNull()
		                  && !this->moving.contains(window) && IsWindowVisible(window->hwnd())
		                  && !IsHungAppWindow(window->hwnd()) && this->stateOf(m) == State::Tiled;
		const auto lost = settled && onAnyMonitor(m.target) && !onAnyMonitor(this->managedFrame(m));

		switch (healTile({
		    .placement = m.placement,
		    .busy = busy,
		    .animated = animated,
		    .lost = lost,
		    .unplacedMs = m.unplacedSince == 0 ? 0 : now - m.unplacedSince,
		}))
		{
		case TileHeal::None: break;
		case TileHeal::Wait: again = true; break;
		case TileHeal::Release:
			qCWarning(logTiling) << "Window" << window->addressHex() << window->appId()
			                     << "was left waiting without an animation; placing it" << m.target;
			stuck.append(window->hwnd());
			break;
		case TileHeal::EndAnimation:
			qCWarning(logTiling) << "The animation of" << window->addressHex() << window->appId()
			                     << "doesn't end; ending it";
			endless.insert(monitor);
			again = true;
			break;
		case TileHeal::Replace:
			qCWarning(logTiling) << "Window" << window->addressHex() << window->appId()
			                     << "was off every monitor; placing it" << m.target;
			m.accepted = QRect();
			m.corrections = 0;
			this->send(m, this->managedFrame(m));
			break;
		}
	}

	for (auto* monitor: endless) DwindleScene::dismiss(monitor);
	if (!stuck.isEmpty()) this->releaseStaged(stuck);
	if (again) this->healTimer.start();
}

void TilingManager::restoreForCrash() {
	setWindowShadowsSuppressed(false);
	for (auto& slot: gTinyCornerSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd)) continue;
		if (restoreChromiumCorners(hwnd) || GetPropW(hwnd, TINY_CORNER_PROP) == nullptr) continue;
		if (slot.borderSuppressed.load()) {
			auto border = COLORREF(DWMWA_COLOR_DEFAULT);
			DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
		}
		if (SetWindowRgn(hwnd, nullptr, FALSE) != 0) RemovePropW(hwnd, TINY_CORNER_PROP);
		auto previous = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(slot.originalPreference.load());
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &previous, sizeof(previous));
	}
	auto atom = gParkAtom.load();

	for (auto& slot: gParkSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd) || IsIconic(hwnd)) continue;

		auto rect = unpackRect(slot.rect.load());
		SetWindowPos(
		    hwnd,
		    nullptr,
		    rect.left(),
		    rect.top(),
		    rect.width(),
		    rect.height(),
		    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
		);

		if (atom != 0) RemovePropW(hwnd, atomName(atom));
	}
}

void TilingManager::rescueParked() {
	auto* owner = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\QuickshellTiling");
	if (owner != nullptr) {
		CloseHandle(owner);
		return;
	}

	auto atom = GlobalFindAtomW(L"QuickshellTilingParked");
	if (atom == 0) return;
	gParkAtom.store(atom);

	QList<HWND> windows;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    reinterpret_cast<QList<HWND>*>(param)->append(hwnd); // NOLINT(performance-no-int-to-ptr)
		    return TRUE;
	    },
	    reinterpret_cast<LPARAM>(&windows)
	);

	auto rescued = 0;
	for (auto* hwnd: windows) {
		auto* value = GetPropW(hwnd, atomName(atom));
		if (value == nullptr) continue;

		RemovePropW(hwnd, atomName(atom));
		auto rect = onScreen(unpackRect(static_cast<quint64>(reinterpret_cast<quintptr>(value))));

		if (IsIconic(hwnd) || IsZoomed(hwnd)) {
			WINDOWPLACEMENT placement {};
			placement.length = sizeof(placement);
			if (!GetWindowPlacement(hwnd, &placement)) continue;
			if (MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONULL) != nullptr) continue;
			placement.rcNormalPosition = toRECT(rect);
			SetWindowPlacement(hwnd, &placement);
			rescued++;
			continue;
		}

		if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr) continue;
		setRawRect(hwnd, rect);
		rescued++;
	}

	if (rescued > 0) {
		qCInfo(logTiling) << "Brought back" << rescued
		                  << "windows that a previous run left off screen.";
	}
}

void TilingManager::onScreensChanged() {
	DwindleScene::dismissAll();
	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
		auto relayoutAll = [this]() {
			for (auto& [key, layout]: this->layouts) this->markLayout(layout.get());
		};
		QObject::connect(screen, &QScreen::availableGeometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::geometryChanged, this, relayoutAll);
		QObject::connect(screen, &QScreen::logicalDotsPerInchChanged, this, relayoutAll);
	}

	this->markAll();
}

void TilingManager::yieldFullscreenTo(Managed& m) {
	auto* window = m.window;
	if (m.fullscreen() || window->borderlessFullscreen() || window->minimized()) return;
	if (this->stateOf(m) != State::Tiled) return;
	auto special = Scratchpad::holds(window);
	auto fresh = QDateTime::currentMSecsSinceEpoch() - m.managedAt < 3000;
	auto changed = false;
	for (auto& [other, o]: this->managed) {
		if (other == window || other->minimized() || Scratchpad::holds(other) != special) continue;
		if (!special && (other->desktop() != window->desktop() || other->screen() != window->screen())) continue;
		if (o.fullscreen()) {
			this->leaveFullscreen(o);
			changed = true;
			continue;
		}
		if (fresh && other->borderlessFullscreen()) {
			constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
			SetWindowPos(window->hwnd(), other->hwnd(), 0, 0, 0, 0, flags);
			other->activate();
			return;
		}
	}
	if (changed) this->updateShellFullscreen();
}

void TilingManager::onActiveWindowChanged() {
	this->reassertTinyDecorations();
	QTimer::singleShot(40, this, [this]() { this->reassertTinyDecorations(); });

	auto* window = this->tracker->activeWindow();
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	this->restackTimer.start();

	auto& m = iter->second;
	WinPanelWindow::recheckFullscreenApp();
	m.focusStamp = ++this->focusCounter;
	this->yieldFullscreenTo(m);
	if (m.layout == nullptr) {
		this->uncoverFor(m);
		return;
	}
	this->clearMaximized(m.layout, window, true);

	m.layout->lastFocused = window;

	if (auto* scroll = TilingManager::scrollingOf(m.layout)) {
		this->prepareScrolling(m.layout);
		if (scroll->activate(idOf(window))) this->markLayout(m.layout);
	}
}

void TilingManager::hold(Managed& m, const QRect& tile) {
	m.target = tile;
	m.accepted = QRect();
	m.corrections = 0;
	m.verifying = false;
}

void TilingManager::onRectChanged(TrackedWindow* window) {
	if (window == this->dragging || this->moving.contains(window)) return;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto& m = iter->second;
	if (m.layout == nullptr && !m.floatMoving && this->floats(m)) {
		const auto frame = this->managedFrame(m);
		if (onAnyMonitor(frame)) m.floatSeen = frame;
	}
	if (m.layout == nullptr || m.target.isNull()) return;

	this->unverified.insert(window);
	if (!this->verifyTimer.isActive()) this->verifyTimer.start();
}

void TilingManager::verify() {
	auto pending = std::move(this->unverified);
	this->unverified.clear();

	for (auto* window: pending) {
		auto iter = this->managed.find(window);
		if (iter == this->managed.end() || window == this->dragging || this->moving.contains(window)) continue;

		auto& m = iter->second;
		if (m.layout == nullptr || m.target.isNull()) continue;
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, m.target);
	}
}

void TilingManager::giveUp(Managed& m, const QRect& frame) {
	m.verifying = false;

	if (m.offscreen || Scratchpad::holds(m.window)) {
		m.accepted = frame;
		return;
	}

	auto bigger = frame.width() > m.target.width() + EDGE_TOLERANCE
	           || frame.height() > m.target.height() + EDGE_TOLERANCE;
	auto away = !m.target.contains(frame.center());

	if (bigger || away) {
		qCInfo(logTiling) << "Window" << m.window->addressHex() << m.window->appId()
		                  << "doesn't take its tile" << m.target << "(is" << frame << "); it floats.";
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(m.window);
	} else {
		m.accepted = frame;
	}
}

void TilingManager::onDeadline() {
	auto now = QDateTime::currentMSecsSinceEpoch();

	for (auto& [window, m]: this->managed) {
		if (!m.verifying) continue;
		if (m.layout == nullptr || m.target.isNull() || window == this->dragging
		    || this->stateOf(m) != State::Tiled)
		{
			m.verifying = false;
			continue;
		}
		if (now < m.deadline) continue;

		auto* hwnd = window->hwnd();
			auto frame = this->managedFrame(m);

		if (frame == m.target) {
			m.verifying = false;
			this->updateTinyCorners(m, frame);
			continue;
		}

		if (IsHungAppWindow(hwnd)) {
			m.wasHung = true;
			m.deadline = now + HUNG_RETRY_MS;
			continue;
		}

		if (frame != m.before || m.wasHung) {
			m.verifying = false;
			m.wasHung = false;
			this->unverified.insert(window);
			if (!this->verifyTimer.isActive()) this->verifyTimer.start();
			continue;
		}

		if (m.offscreen || Scratchpad::holds(window)) {
			m.verifying = false;
			if (!m.offscreen) m.accepted = frame;
			continue;
		}

		qCInfo(logTiling) << "Window" << window->addressHex() << window->appId()
		                  << "ignores being moved; it floats.";
		m.verifying = false;
		m.autoFloat = true;
		m.override = Override::None;
		this->markWindow(window);
	}

	this->armDeadline();
}

void TilingManager::onMoveSizeStarted(TrackedWindow* window) {
	DwindleScene::dismissFor(window->hwnd());
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	if (iter->second.layout == nullptr) {
		auto& m = iter->second;
		if (m.focusMaximized() && !m.fullscreenRestore.isNull() && GetAsyncKeyState(VK_LBUTTON) < 0) {
			const auto frame = this->managedFrame(m);
			const auto point = cursorPos();
			const auto dpi = GetDpiForWindow(window->hwnd());
			const auto border = MulDiv(8, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
			if (frame.adjusted(border, border, -border, -border).contains(point)) {
				this->regrab = window;
				this->regrabSize = m.fullscreenRestore.size();
				this->regrabAt = QPointF(
				    static_cast<double>(point.x() - frame.left()) / qMax(1, frame.width()),
				    static_cast<double>(point.y() - frame.top()) / qMax(1, frame.height())
				);
				PostMessageW(window->hwnd(), WM_CANCELMODE, 0, 0);
			}
		}
		if (m.focusMaximized() || m.floatMoving) this->forgetFloat(m);
		return;
	}
	this->restoreTinyCorners(iter->second);

	auto state = this->stateOf(iter->second);
	if (state != State::Tiled && !(state == State::Suspended && window->maximized())) return;
	this->clearMaximized(iter->second.layout, nullptr, true);

	this->dragging = window;
	this->dragOrigin = this->managedFrame(iter->second);
	this->unverified.remove(window);
}

void TilingManager::onMoveSizeEnded(TrackedWindow* window) {
	if (this->regrab == window && window != nullptr) {
		this->regrab = nullptr;
		if (GetAsyncKeyState(VK_LBUTTON) < 0) {
			const auto point = cursorPos();
			setRawRect(
			    window->hwnd(),
			    QRect(
			        point - QPoint(
			                    qRound(this->regrabAt.x() * this->regrabSize.width()),
			                    qRound(this->regrabAt.y() * this->regrabSize.height())
			                ),
			        this->regrabSize
			    ),
			    true
			);
			PostMessageW(window->hwnd(), WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(point.x(), point.y()));
			return;
		}
	}
	if (window != nullptr && this->detached == window) {
		this->detached = nullptr;
		DwindleScene::setDuringDrag(false);
		auto found = this->managed.find(window);
		if (found == this->managed.end()) return;
		auto& m = found->second;
		m.override = this->detachedOverride;
		const auto index = window->desktop();
		const auto special = Scratchpad::holds(window);
		if (!special && (index < 0 || index >= this->desktops->count())) {
			this->markWindow(window);
			return;
		}
		const auto desktop = special ? SPECIAL_DESKTOP : this->desktops->desktops().at(index).id;
		if (!this->insertAtCursor(m, desktop, cursorPos())) this->markWindow(window);
		return;
	}
	if (window != this->dragging) return;
	this->dragging = nullptr;
	const auto origin = std::exchange(this->dragOrigin, QRect());

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	auto* hwnd = window->hwnd();
	if (m.layout == nullptr || m.target.isNull() || IsIconic(hwnd) || IsZoomed(hwnd)) {
		this->markWindow(window);
		return;
	}

		auto frame = this->managedFrame(m);
	const auto target = m.accepted.isNull() ? m.target : m.accepted;
	if (frame == origin) {
		this->markLayout(m.layout);
		return;
	}

	auto movedLeft = qAbs(frame.left() - target.left()) > EDGE_TOLERANCE;
	auto movedTop = qAbs(frame.top() - target.top()) > EDGE_TOLERANCE;
	auto movedRight = qAbs(rightOf(frame) - rightOf(target)) > EDGE_TOLERANCE;
	auto movedBottom = qAbs(bottomOf(frame) - bottomOf(target)) > EDGE_TOLERANCE;
	auto moved = int(movedLeft) + int(movedTop) + int(movedRight) + int(movedBottom);
	auto resized = qAbs(frame.width() - target.width()) > EDGE_TOLERANCE
	            || qAbs(frame.height() - target.height()) > EDGE_TOLERANCE;

	if (moved == 0) {
		this->markLayout(m.layout);
		return;
	}

	m.dropped = true;
	m.target = QRect();
	if (!resized || moved == 4) {
		this->drop(m, cursorPos());
		return;
	}

	auto* layout = m.layout;
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());
	auto id = idOf(window);

	if (movedLeft) layout->tree->moveEdge(id, Edge::Left, frame.left() - in);
	if (movedRight) layout->tree->moveEdge(id, Edge::Right, rightOf(frame) + in);
	if (movedTop) layout->tree->moveEdge(id, Edge::Top, frame.top() - in);
	if (movedBottom) layout->tree->moveEdge(id, Edge::Bottom, bottomOf(frame) + in);

	this->markLayout(layout);
}

bool TilingManager::detachTile(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || this->detached != nullptr || this->dragging != nullptr) return false;
	auto& m = iter->second;
	auto* layout = m.layout;
	auto* hwnd = window->hwnd();
	if (layout == nullptr || layout->tree->kind() != tiling::Kind::Dwindle || m.fullscreen() || m.offscreen
	    || this->stateOf(m) != State::Tiled || IsZoomed(hwnd) || IsHungAppWindow(hwnd))
		return false;

	const auto shown = this->managedFrame(m);
	const auto point = cursorPos();
	const auto tile = this->tileRect(layout, window);
	if (shown.isEmpty() || tile.isEmpty()) return false;
	const auto size = QSizeF(tile.size()) * DETACHED_SIZE;
	const auto across = qBound(0.0, static_cast<double>(point.x() - shown.left()) / shown.width(), 1.0);
	const auto down = qBound(0.0, static_cast<double>(point.y() - shown.top()) / shown.height(), 1.0);

	this->restoreTinyCorners(m);
	this->detached = window;
	DwindleScene::setDuringDrag(true);
	this->detachedOverride = m.override;
	m.override = Override::Float;
	m.fresh = false;
	layout->animateRearrangement = true;
	this->takeOut(m);
	setFrame(
	    hwnd,
	    QRect(
	        point - QPoint(qRound(across * size.width()), qRound(down * size.height())),
	        size.toSize().expandedTo(QSize(1, 1))
	    ),
	    false,
	    nullptr,
	    true
	);
	return true;
}

QRect TilingManager::overviewTileRect(TrackedWindow* window) const {
	const auto iter = this->managed.find(window);
	if (!this->mEnabled || iter == this->managed.end()) return {};
	const auto& m = iter->second;
	auto* layout = m.layout;
	if (layout == nullptr || layout->special || !this->layoutAlive(layout)
	    || m.mode != TileMode::Normal || this->stateOf(m) != State::Tiled
	    || layout->tree->box(idOf(window)).isEmpty())
		return {};

	auto* screen = layout->screen.data();
	const auto rects = monitorRects(monitorForScreen(screen));
	if (!rects.valid) return {};
	return ScreenMapper(screen->geometry(), rects.monitor, screen->devicePixelRatio())
	    .toLogical(this->tileRect(layout, window));
}

bool TilingManager::dropOverviewTile(TrackedWindow* window, const QPointF& normalized) {
	if (!(normalized.x() >= 0 && normalized.x() < 1 && normalized.y() >= 0 && normalized.y() < 1)
	    || this->detached != nullptr || this->dragging != nullptr || this->overviewTileRect(window).isEmpty())
		return false;

	auto& managed = this->managed.at(window);
	auto* layout = managed.layout;
	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (!rects.valid || rects.work.isEmpty() || IsZoomed(window->hwnd()) || IsHungAppWindow(window->hwnd()))
		return false;

	layout->tree->compute(rects.work);
	const QPoint point {
	    rects.work.left() + static_cast<int>(normalized.x() * rects.work.width()),
	    rects.work.top() + static_cast<int>(normalized.y() * rects.work.height()),
	};
	if (layout->tree->box(idOf(window)).contains(point)) return false;

	this->drop(managed, point);
	layout->tree->compute(rects.work);
	for (auto id: layout->tree->ids()) {
		if (auto* affected = this->windowOf(id)) emit this->windowTilingChanged(affected);
	}
	return true;
}

bool TilingManager::insertAtCursor(Managed& m, const GUID& desktop, const QPoint& cursor) {
	const auto point = POINT {cursor.x(), cursor.y()};
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
	if (IsEqualGUID(desktop, SPECIAL_DESKTOP)) screen = screenForMonitor(Scratchpad::instance()->monitor());
	auto* layout = this->layoutAt(desktop, screen, true);
	if (layout == nullptr || layout->tree->kind() != tiling::Kind::Dwindle) return false;

	const auto frame = this->managedFrame(m);
	if (m.layout != nullptr) {
		m.layout->animateRearrangement = true;
		this->takeOut(m);
	}
	layout->animateRearrangement = true;
	this->insertInto(m, layout, nullptr, &cursor);
	m.dropped = false;
	m.floated = false;
	m.target = frame;
	m.fullscreenToggled = true;
	return true;
}

bool TilingManager::coversScreen(TrackedWindow* window) const {
	const auto iter = this->managed.find(window);
	return iter != this->managed.end() && (iter->second.fullscreen() || iter->second.focusMaximized());
}

bool TilingManager::beginGestureMove(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.fullscreen() || this->dragging != nullptr) return false;
	this->haltMotion(window->hwnd(), false);
	this->onMoveSizeStarted(window);
	return this->dragging == window;
}

void TilingManager::endGestureMove(TrackedWindow* window, std::optional<Edge> direction) {
	if (window != this->dragging) return;
	this->dragging = nullptr;
	this->dragOrigin = QRect();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;
	if (m.layout == nullptr) {
		this->markWindow(window);
		return;
	}

	m.dropped = true;
	m.target = QRect();
	if (direction && this->tiledActive() == &m) this->moveDirection(*direction);
	this->markLayout(m.layout);
}

void TilingManager::drop(Managed& m, const QPoint& cursor) {
	auto* from = m.layout;
	auto self = idOf(m.window);
	if (from->tree->kind() == tiling::Kind::Dwindle && this->insertAtCursor(m, from->desktop, cursor)) return;

	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree->ids()) {
			if (id == self) continue;
			auto* other = this->windowOf(id);
			if (other == nullptr) continue;

			auto& o = this->managed.at(other);
			if (this->stateOf(o) != State::Tiled) continue;

			if (layout->tree->box(id).contains(cursor)) {
				this->swapWindows(m, o);
				return;
			}
		}
	}

	auto point = POINT {cursor.x(), cursor.y()};
	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONULL));

	if (screen != nullptr && screen != from->screen) {
		this->moveToLayout(m, this->layoutAt(from->desktop, screen, true), nullptr);
		return;
	}

	this->markLayout(from);
}

TrackedWindow* TilingManager::focusedWindow() const {
	if (this->tracker == nullptr) return nullptr;

	auto* active = this->tracker->activeWindow();
	auto* hwnd = GetForegroundWindow();
	auto* foreground = this->tracker->windowFor(hwnd);
	if (foreground == nullptr || foreground == active) return active;

	DWORD cloaked = 0;
	DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
	if (cloaked != 0) return active;

	qCInfo(logTiling) << "The foreground window" << foreground->addressHex() << foreground->appId()
	                  << "is not the tracked active window"
	                  << (active == nullptr ? QStringLiteral("(none)") : active->addressHex())
	                  << "; tiling commands use the foreground one.";
	this->tracker->refreshForeground();
	return foreground;
}

TrackedWindow* TilingManager::fullscreenShortcutTarget(TrackedWindow* focused) const {
	POINT point {};
	if (!SuperDragManager::instance()->focusFollowsMouse() || !GetPhysicalCursorPos(&point))
		return focused;

	auto* hit = WindowFromPhysicalPoint(point);
	auto* native = hit == nullptr ? nullptr : GetAncestor(hit, GA_ROOT);
	auto visual = DwindleScene::windowAt(point, native);
	auto* target = this->tracker->windowFor(visual.value_or(native));
	auto it = this->managed.find(target);
	if (it == this->managed.end() || it->second.layout == nullptr
	    || this->stateOf(it->second) != State::Tiled
	    || !this->sceneShows(it->second.layout, this->desktops->currentIndex()))
		return focused;
	if (visual.has_value() || !this->allowsHoverFocus(target)) return target;
	return focused;
}

TilingManager::Managed* TilingManager::tiledActive() {
	if (this->tracker == nullptr) return nullptr;

	auto iter = this->managed.find(this->focusedWindow());
	if (iter == this->managed.end()) return nullptr;

	auto& m = iter->second;
	if (m.layout == nullptr || this->stateOf(m) != State::Tiled) return nullptr;
	return &m;
}

TilingManager::Managed* TilingManager::neighbour(const Managed& from, Edge direction) {
	if (from.layout == nullptr) return nullptr;

	auto source = from.layout->tree->box(idOf(from.window));
	Managed* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto& [key, layout]: this->layouts) {
		if (!IsEqualGUID(layout->desktop, from.layout->desktop) || !this->layoutAlive(layout.get())) {
			continue;
		}

		for (auto id: layout->tree->ids()) {
			auto* window = this->windowOf(id);
			if (window == nullptr || window == from.window) continue;

			auto& m = this->managed.at(window);
			if (this->stateOf(m) != State::Tiled || layout->tree->isParked(id)) continue;

			auto box = layout->tree->box(id);
			auto distance = 0;
			auto overlap = 0;

			switch (direction) {
			case Edge::Left:
				distance = source.left() - rightOf(box);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Right:
				distance = box.left() - rightOf(source);
				overlap = qMin(bottomOf(source), bottomOf(box)) - qMax(source.top(), box.top());
				break;
			case Edge::Top:
				distance = source.top() - bottomOf(box);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			case Edge::Bottom:
				distance = box.top() - bottomOf(source);
				overlap = qMin(rightOf(source), rightOf(box)) - qMax(source.left(), box.left());
				break;
			}

			if (distance < -EDGE_TOLERANCE || overlap <= 0) continue;

			if (best == nullptr || distance < bestDistance
			    || (distance == bestDistance && m.focusStamp > best->focusStamp))
			{
				best = &m;
				bestDistance = distance;
			}
		}
	}

	return best;
}

QScreen* TilingManager::screenInDirection(QScreen* from, Edge direction) const {
	auto source = physicalMonitor(from);
	QScreen* best = nullptr;
	auto bestDistance = std::numeric_limits<int>::max();

	for (auto* screen: QGuiApplication::screens()) {
		if (screen == from) continue;
		auto rect = physicalMonitor(screen);
		auto distance = 0;
		auto overlap = 0;

		switch (direction) {
		case Edge::Left:
			distance = source.left() - rightOf(rect);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Right:
			distance = rect.left() - rightOf(source);
			overlap = qMin(bottomOf(source), bottomOf(rect)) - qMax(source.top(), rect.top());
			break;
		case Edge::Top:
			distance = source.top() - bottomOf(rect);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		case Edge::Bottom:
			distance = rect.top() - bottomOf(source);
			overlap = qMin(rightOf(source), rightOf(rect)) - qMax(source.left(), rect.left());
			break;
		}

		if (distance < 0 || overlap <= 0) continue;
		if (distance < bestDistance) {
			best = screen;
			bestDistance = distance;
		}
	}

	return best;
}

void TilingManager::swapWindows(Managed& a, Managed& b) {
	auto* layoutA = a.layout;
	auto* layoutB = b.layout;
	if (layoutA == nullptr || layoutB == nullptr) return;

	if (layoutA == layoutB) {
		layoutA->tree->swap(idOf(a.window), idOf(b.window));
	} else {
		layoutA->tree->replace(idOf(a.window), idOf(b.window));
		layoutB->tree->replace(idOf(b.window), idOf(a.window));
		if (a.focusMaximized()) {
			a.mode = nextMode(a.mode, TileModeEvent::Restore);
			a.fullscreenToggled = true;
		}
		if (b.focusMaximized()) {
			b.mode = nextMode(b.mode, TileModeEvent::Restore);
			b.fullscreenToggled = true;
		}
		a.layout = layoutB;
		b.layout = layoutA;
		if (layoutA->lastFocused == a.window) layoutA->lastFocused = b.window;
		if (layoutB->lastFocused == b.window) layoutB->lastFocused = a.window;
	}

	layoutA->animateRearrangement = true;
	layoutB->animateRearrangement = true;
	this->markLayout(layoutA);
	this->markLayout(layoutB);
}

void TilingManager::moveToLayout(Managed& m, Layout* layout, TrackedWindow* target) {
	auto* from = m.layout;
	if (from == nullptr || layout == nullptr || from == layout) return;

	if (m.focusMaximized()) {
		m.mode = nextMode(m.mode, TileModeEvent::Restore);
		m.fullscreenToggled = true;
	}
	this->clearMaximized(layout, nullptr, true);

	from->tree->remove(idOf(m.window));
	if (from->lastFocused == m.window) from->lastFocused = nullptr;
	this->markLayout(from);

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	layout->tree->configure(this->options());
	layout->tree->insert(idOf(m.window), target == nullptr ? 0 : idOf(target), nullptr);
	layout->lastFocused = m.window;
	layout->animateRearrangement = true;
	m.layout = layout;
	m.target = QRect();

	if (auto* scroll = TilingManager::scrollingOf(layout)) {
		this->prepareScrolling(layout);
		scroll->activate(idOf(m.window));
	}

	this->markLayout(layout);
}

bool TilingManager::focusDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return this->focusIdleStrip();

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		auto id = scroll->neighbor(idOf(m->window), direction, false);
		if (id != 0 && this->windowOf(id) != nullptr) {
			scroll->activate(id);
			this->settle(m->layout);
			return true;
		}
	}

	auto* next = this->neighbour(*m, direction);
	if (next == nullptr) return false;

	next->window->activate();
	return true;
}

bool TilingManager::moveDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (m->layout->special && m->layout->tree->kind() != tiling::Kind::Dwindle) {
		auto* next = this->neighbour(*m, direction);
		if (next != nullptr && next->layout == m->layout) this->swapWindows(*m, *next);
		return true;
	}

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		if (scroll->moveWindow(idOf(m->window), direction)) {
			this->settle(m->layout);
		} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
			this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
		}
		return true;
	}

	auto* next = this->neighbour(*m, direction);

	if (next != nullptr && next->layout == m->layout && m->layout->tree->kind() == tiling::Kind::Dwindle) {
		this->moveInDwindle(*m, direction);
	} else if (next != nullptr && next->layout == m->layout) {
		this->swapWindows(*m, *next);
	} else if (next != nullptr) {
		this->moveToLayout(*m, next->layout, next->window);
	} else if (!m->layout->special) {
		if (auto* screen = this->screenInDirection(m->layout->screen, direction))
			this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
	}

	return true;
}

void TilingManager::moveInDwindle(Managed& m, Edge direction) {
	auto* layout = m.layout;
	auto id = idOf(m.window);
	auto box = layout->tree->box(id);
	auto center = box.center();

	QPoint focal;
	switch (direction) {
	case Edge::Left: focal = QPoint(box.left() - 1, center.y()); break;
	case Edge::Right: focal = QPoint(rightOf(box) + 1, center.y()); break;
	case Edge::Top: focal = QPoint(center.x(), box.top() - 1); break;
	case Edge::Bottom: focal = QPoint(center.x(), bottomOf(box) + 1); break;
	}

	layout->tree->remove(id);
	layout->tree->configure(this->options());
	auto rects = monitorRects(monitorForScreen(layout->screen));
	if (rects.valid) layout->tree->compute(rects.work);
	auto target = tiling::cursorInsertionTarget(*layout->tree, focal);
	layout->tree->insert(id, target, &focal);
	layout->lastFocused = m.window;
	layout->animateRearrangement = true;
	this->markLayout(layout);
}

bool TilingManager::swapDirection(Edge direction) {
	auto* m = this->tiledActive();
	if (m == nullptr) return false;

	if (auto* scroll = TilingManager::scrollingOf(m->layout)) {
		this->prepareScrolling(m->layout);
		auto* other = this->windowOf(scroll->neighbor(idOf(m->window), direction, false));
		if (other != nullptr) {
			this->swapWindows(*m, this->managed.at(other));
			this->settle(m->layout);
			return true;
		}
	}

	if (auto* next = this->neighbour(*m, direction)) this->swapWindows(*m, *next);
	return true;
}

bool TilingManager::scrollColumns(TrackedWindow* window, qint32 delta) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->scrollColumns(delta);
	this->settle(layout);
	return true;
}

bool TilingManager::scrollView(TrackedWindow* window, qint32 pixels) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->scrollBy(toPhysical(pixels, layout->screen->devicePixelRatio()));
	this->settle(layout);
	return true;
}

bool TilingManager::columnWidth(TrackedWindow* window, double value, bool relative) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->setColumnWidth(idOf(window), value, relative);
	this->settle(layout);
	return true;
}

bool TilingManager::cycleColumnWidth(TrackedWindow* window, qint32 direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->cycleWidth(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::allColumnsWidth(TrackedWindow* window, double value) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->setAllWidths(value);
	this->settle(layout);
	return true;
}

bool TilingManager::fitColumns(TrackedWindow* window, tiling::FitMode mode) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->fit(idOf(window), mode);
	this->settle(layout);
	return true;
}

bool TilingManager::focusColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	auto id = scroll->neighbor(idOf(window), direction, true);
	if (id != 0) scroll->activate(id);
	this->settle(layout);
	return true;
}

bool TilingManager::focusColumnAt(POINT point, qint32 steps) {
	auto current = this->desktops->currentIndex();
	if (steps == 0 || current < 0 || current >= this->desktops->count()) return false;

	auto* screen = this->tracker->screenFor(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST));
	if (screen == nullptr) return false;

	auto* layout = this->layoutAt(this->desktops->desktops().at(current).id, screen, false);
	auto* scroll = TilingManager::scrollingOf(layout);
	if (scroll == nullptr) return false;

	this->prepareScrolling(layout);
	auto id = scroll->active();
	auto direction = steps > 0 ? Edge::Right : Edge::Left;

	for (auto step = 0; id != 0 && step < std::abs(steps); step++) {
		auto next = scroll->neighbor(id, direction, false);
		if (next == 0) break;
		id = next;
	}

	if (id == 0 || id == scroll->active()) return false;

	scroll->activate(id);
	this->settle(layout);
	return true;
}

bool TilingManager::moveIntoColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->moveInto(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::swapColumn(TrackedWindow* window, Edge direction) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->swapColumn(idOf(window), direction);
	this->settle(layout);
	return true;
}

bool TilingManager::promote(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->promote(idOf(window));
	this->settle(layout);
	return true;
}

bool TilingManager::centerColumn(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->center(idOf(window));
	this->settle(layout);
	return true;
}

bool TilingManager::toggleFit(TrackedWindow* window) {
	Layout* layout = nullptr;
	if (this->scrollingFor(window, &layout) == nullptr) return false;

	this->fitToggled = !this->fitToggled;
	this->markAll();
	return true;
}

bool TilingManager::toggleMaximize(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;
	this->warnFullscreenConflict(window);
	auto& m = iter->second;
	if (this->floats(m)) return this->toggleFloatMaximize(m);
	if (m.fullscreen() && m.layout != nullptr && !window->minimized()
	    && this->stateOf(m) == State::Tiled && !m.elevated
	    && m.layout->tree->kind() != tiling::Kind::Master && DwindleScene::enabled()
	    && this->mAnimateMoves && this->sceneShows(m.layout, this->desktops->currentIndex())) {
		m.fullscreenWasMaximized = false;
		this->leaveFullscreen(m);
		m.maximizedTile = false;
		this->clearMaximized(m.layout, nullptr, false);
		m.mode = nextMode(m.mode, TileModeEvent::ToggleMaximize);
		this->coverOthers(m);
		m.fullscreenToggled = true;
		rememberFullscreen(window, false);
		this->updateShellFullscreen();
		window->activate();
		return true;
	}
	if (m.layout == nullptr || m.fullscreen() || m.elevated || window->minimized()
	    || this->stateOf(m) != State::Tiled)
		return false;
	if (DwindleScene::enabled() && m.layout->tree->kind() != tiling::Kind::Master
	    && this->mAnimateMoves && this->sceneShows(m.layout, this->desktops->currentIndex())) {
		if (!m.focusMaximized()) this->yieldFullscreenTo(m);
		this->clearMaximized(m.layout, window, false);
		m.mode = nextMode(m.mode, TileModeEvent::ToggleMaximize);
		if (m.focusMaximized()) this->coverOthers(m);
		else this->restackTimer.start();
		m.fullscreenToggled = true;
		this->markLayout(m.layout);
		window->activate();
		return true;
	}

	if (m.layout->special) return false;
	auto* hwnd = window->hwnd();
	m.maximizedTile = !m.maximizedTile;
	m.maximizePending = false;
	ShowWindow(hwnd, m.maximizedTile ? SW_MAXIMIZE : SW_RESTORE);
	this->markLayout(m.layout);
	window->activate();
	return true;
}

bool TilingManager::toggleFullWidth(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->toggleFullWidth(idOf(window));
	this->settle(layout);
	return true;
}

namespace {

constexpr const wchar_t* FULLSCREEN_PROP = L"qs.fullscreen";
constexpr const wchar_t* FULLSCREEN_X = L"qs.fsx";
constexpr const wchar_t* FULLSCREEN_Y = L"qs.fsy";
constexpr const wchar_t* FULLSCREEN_W = L"qs.fsw";
constexpr const wchar_t* FULLSCREEN_H = L"qs.fsh";

HANDLE propOf(int value) { return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(value)); } // NOLINT
int valueOf(HWND hwnd, const wchar_t* name) {
	return static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(hwnd, name))); // NOLINT
}

void markFullscreen(HWND hwnd, const QRect& raw, LeftMark mark) {
	SetPropW(hwnd, FULLSCREEN_PROP, propOf(static_cast<int>(mark)));
	SetPropW(hwnd, FULLSCREEN_X, propOf(raw.x()));
	SetPropW(hwnd, FULLSCREEN_Y, propOf(raw.y()));
	SetPropW(hwnd, FULLSCREEN_W, propOf(raw.width()));
	SetPropW(hwnd, FULLSCREEN_H, propOf(raw.height()));
}

void unmarkFullscreen(HWND hwnd) {
	for (const auto* name: {FULLSCREEN_PROP, FULLSCREEN_X, FULLSCREEN_Y, FULLSCREEN_W, FULLSCREEN_H}) {
		RemovePropW(hwnd, name);
	}
}

void setFullscreenLook(HWND hwnd, bool fullscreen) {
	DWORD corners = fullscreen ? DWMWCP_DONOTROUND : DWMWCP_DEFAULT;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
	COLORREF border = fullscreen ? DWMWA_COLOR_NONE : DWMWA_COLOR_DEFAULT;
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

}

void TilingManager::restoreLeftFullscreen(TrackedWindow* window) {
	auto* hwnd = window->hwnd();
	if (RemovePropW(hwnd, TOPMOST_PROP) != nullptr) {
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
	}
	auto mark = valueOf(hwnd, FULLSCREEN_PROP);
	if (mark == 0) return;
	auto raw = QRect(valueOf(hwnd, FULLSCREEN_X), valueOf(hwnd, FULLSCREEN_Y), valueOf(hwnd, FULLSCREEN_W), valueOf(hwnd, FULLSCREEN_H));
	unmarkFullscreen(hwnd);
	setFullscreenLook(hwnd, false);
	qCInfo(logTiling) << "Window" << window->addressHex() << window->appId()
	                  << "was left fullscreen by a previous shell; putting it back.";
	if (!raw.isEmpty()) setRawRect(hwnd, raw);
	if (mark == static_cast<int>(LeftMark::FullscreenOverMaximized)) ShowWindowAsync(hwnd, SW_MAXIMIZE);
}

bool TilingManager::toggleFullscreen(TrackedWindow* window, bool follow) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;
	auto& m = iter->second;
	auto* hwnd = window->hwnd();
	const auto floating = this->floats(m);
	if (!floating) m.mode = nextMode(m.mode, TileModeEvent::Restore);

	if (!m.fullscreen()) {
		if (m.elevated || window->minimized()) return false;
		auto special = Scratchpad::holds(window);
		auto* scratchpad = Scratchpad::instance();
		if (special && !scratchpad->shown()) return false;
		auto* monitor = special ? scratchpad->monitor() : monitorForScreen(window->screen());
		auto rects = monitorRects(monitor);
		if (!rects.valid) return false;
		auto wasMaximized = IsZoomed(hwnd) != FALSE;
		if (wasMaximized) ShowWindow(hwnd, SW_RESTORE);
		RECT raw {};
		if (!GetWindowRect(hwnd, &raw)) return false;
		auto before = QRect(QPoint(raw.left, raw.top), QPoint(raw.right - 1, raw.bottom - 1));
		if (floating) {
			if (!m.fullscreenRestore.isNull()) before = m.fullscreenRestore;
			else if (m.floatMoving && !m.floatFrame.isNull()) before = rawFor(hwnd, m.floatFrame);
			this->clearMaximized(this->floatHome(m, true), window, true);
		} else {
			this->clearMaximized(m.layout, nullptr, false);
		}
		for (auto& [other, existing]: this->managed) {
			if (!existing.fullscreen() || Scratchpad::holds(other) != special) continue;
			if (special || (other->desktop() == window->desktop() && other->screen() == window->screen())) {
				this->leaveFullscreen(existing);
			}
		}
		auto tiled = m.layout != nullptr && (wasMaximized || this->stateOf(m) == State::Tiled);
		m.fullscreenRestore = tiled ? QRect() : before;
		m.fullscreenWasMaximized = wasMaximized;
		markFullscreen(hwnd, before, wasMaximized ? LeftMark::FullscreenOverMaximized : LeftMark::Fullscreen);
		auto frame = fullscreenFrame(hwnd, rects.monitor);
		if (special) frame.translate(0, this->mSpecialOffset);
		if (!tiled && !floating) setFrame(hwnd, frame);

		m.mode = nextMode(m.mode, TileModeEvent::ToggleFullscreen);
		m.fullscreenToggled = true;
		if (floating) dropTopmost(m);
		this->coverOthers(m);
		if (floating) this->sendFloat(m);
		this->updateFullscreenLook(m);
		if (m.fullscreenTileRadius.has_value()) WinPanelWindow::prepareFullscreenBarHandoff(monitor);
		this->markLayout(m.layout);
		window->activate(follow);
		rememberFullscreen(window, true);
	} else {
		this->leaveFullscreen(m);
		rememberFullscreen(window, false);
	}

	qCWarning(logTiling) << "Window" << window->addressHex() << window->appId()
	                   << (m.fullscreen() ? "goes fullscreen" : "leaves fullscreen") << "state" << static_cast<int>(this->stateOf(m))
	                   << "layout" << (m.layout != nullptr);
	this->updateShellFullscreen();
	this->warnFullscreenConflict(window);
	return true;
}

void TilingManager::warnFullscreenConflict(TrackedWindow* source, qsizetype desktop) {
	if (!startup::settled()) return;
	if (source != nullptr) desktop = source->desktop();
	if (desktop < 0) return;
	for (const auto& [window, m]: this->managed) {
		if (window->minimized() || window->desktop() != desktop) continue;
		if (!this->mNativeFullscreen.contains(window->hwnd()) && !(m.appFullscreen && !m.fullscreen())) continue;
		if (window == source && this->mNativeFullscreen.contains(window->hwnd()) && !m.appFullscreen) continue;
		auto state = QUERY_USER_NOTIFICATION_STATE {};
		const auto exclusive = SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
		emit this->fullscreenConflict(window->appId(), window->title(), exclusive);
		return;
	}
}

void TilingManager::leaveFullscreen(Managed& m) {
	auto* hwnd = m.window->hwnd();
	const auto floating = this->floats(m) && !m.fullscreenWasMaximized && !m.fullscreenRestore.isNull();
	if (floating) m.floatFrame = frameFor(hwnd, m.fullscreenRestore);
	else if (!m.fullscreenRestore.isNull()) setRawRect(hwnd, m.fullscreenRestore);
	if (m.fullscreenWasMaximized) ShowWindowAsync(hwnd, SW_MAXIMIZE);
	m.fullscreenRestore = QRect();
	m.fullscreenWasMaximized = false;
	m.mode = nextMode(m.mode, TileModeEvent::LeaveFullscreen);
	m.fullscreenToggled = true;
	unmarkFullscreen(hwnd);
	if (floating) this->sendFloat(m);
	this->restackTimer.start();
	this->updateFullscreenLook(m);
	this->markLayout(m.layout);
}

void TilingManager::updateFullscreenLook(Managed& m) {
	auto* hwnd = m.window->hwnd();
	const auto floating = this->floats(m);
	auto* layout = floating ? this->floatHome(m, true) : m.layout;
	const auto animate = DwindleScene::enabled() && this->mAnimateMoves && layout != nullptr
	    && layout->tree->kind() == tiling::Kind::Dwindle
	    && this->sceneShows(layout, this->desktops->currentIndex())
	    && (floating || this->stateOf(m) == State::Tiled);
	if (!animate) m.fullscreenTileRadius.reset();
	else if (m.fullscreen() && !m.fullscreenTileRadius.has_value())
		m.fullscreenTileRadius = DwindleScene::cornerRadius(hwnd);

	if (m.fullscreen()) this->restoreTinyCorners(m);
	m.fullscreenLookPending = animate && !m.fullscreen();
	if (!m.fullscreenLookPending) setFullscreenLook(hwnd, m.fullscreen());
}

TrackedWindow* TilingManager::specialFullscreenWindow() const {
	for (const auto& [window, m]: this->managed) {
		if (m.fullscreen() && Scratchpad::holds(window) && !window->minimized()) return window;
	}
	return nullptr;
}

TrackedWindow* TilingManager::specialMaximizedWindow() const {
	for (const auto& [window, m]: this->managed) {
		if (m.focusMaximized() && Scratchpad::holds(window) && !window->minimized()) return window;
	}
	return nullptr;
}

TrackedWindow* TilingManager::specialCoveringWindow() const {
	auto* fullscreen = this->specialFullscreenWindow();
	return fullscreen != nullptr ? fullscreen : this->specialMaximizedWindow();
}

QRect TilingManager::settleFloat(TrackedWindow* window) {
	auto found = this->managed.find(window);
	if (found == this->managed.end()) return {};
	auto& m = found->second;
	if (m.layout != nullptr || m.mode != TileMode::Normal || !m.floatMoving || m.floatFrame.isNull()) return {};
	m.floatMoving = false;
	m.floatPending = false;
	m.floatOpening = false;
	this->forgetStage(m);
	m.placement = nextPlacement(m.placement, TilePlacementEvent::Release);
	return rawFor(window->hwnd(), m.floatFrame);
}

bool TilingManager::floatsInSpecial(TrackedWindow* window) const {
	auto found = this->managed.find(window);
	return found != this->managed.end() && Scratchpad::holds(window)
	    && this->stateOf(found->second) == State::Out;
}

bool TilingManager::overSpecialCover(TrackedWindow* window) const {
	auto* covering = this->specialCoveringWindow();
	auto cover = this->managed.find(covering);
	auto found = this->managed.find(window);
	if (cover == this->managed.end() || found == this->managed.end() || window == covering) return false;
	return found->second.layout == nullptr
	    && (found->second.managedAt > cover->second.coveredAt
	        || GetWindow(window->hwnd(), GW_OWNER) == covering->hwnd());
}

bool TilingManager::covers(TrackedWindow* window) const {
	auto found = this->managed.find(window);
	if (found == this->managed.end() || window->minimized()) return false;
	const auto& m = found->second;
	return m.mode != TileMode::Normal && (m.layout != nullptr || Scratchpad::holds(window) || this->floats(m));
}

void TilingManager::markCovering() {
	for (auto& [window, m]: this->managed) {
		if (m.mode == TileMode::Normal) continue;
		this->markLayout(m.layout != nullptr ? m.layout : this->floatHome(m, true));
	}
}

void TilingManager::updateShellFullscreen() {
	auto shown = this->desktops != nullptr ? this->desktops->shownIndex() : -1;
	auto* scratchpad = Scratchpad::instance();
	auto* specialMonitor = scratchpad->shown() ? scratchpad->monitor() : nullptr;
	QSet<HMONITOR> fullscreenMonitors;
	QHash<HMONITOR, HWND> managedFullscreenWindows;
	QSet<HWND> nativeFullscreen;
	for (auto& [window, m]: this->managed) {
		if (m.appFullscreen && !framelessWindow(window->hwnd())) m.appFullscreen = false;
	}
	for (const auto& [window, m]: this->managed) {
		if (window->minimized()) continue;
		if (!m.fullscreen() && !m.fullscreenLookPending && !m.fullscreenTileRadius.has_value()
		    && (window->borderlessFullscreen() || (window->fullscreen() && framelessCover(window->hwnd()))))
		{
			nativeFullscreen.insert(window->hwnd());
		}
		if (!m.fullscreen() && (m.fullscreenLookPending || m.fullscreenTileRadius.has_value()
		                      || !window->borderlessFullscreen()))
			continue;

		auto* monitor = Scratchpad::holds(window) ? specialMonitor
		               : window->desktop() == shown ? monitorForScreen(window->screen()) : nullptr;
		if (monitor == nullptr || (!Scratchpad::holds(window) && monitor == specialMonitor)) continue;
		fullscreenMonitors.insert(monitor);
		if (m.fullscreen()) managedFullscreenWindows.insert(monitor, window->hwnd());
	}
	QStringList fullscreenScreens;
	for (auto* screen: QGuiApplication::screens()) {
		if (fullscreenMonitors.contains(monitorForScreen(screen))) fullscreenScreens.append(screen->name());
	}
	if (fullscreenScreens != this->mFullscreenScreens) {
		this->mFullscreenScreens = fullscreenScreens;
		emit this->fullscreenScreensChanged();
	}
	this->mNativeFullscreen = nativeFullscreen;
	WinPanelWindow::setShellFullscreen(fullscreenMonitors, managedFullscreenWindows);
	WinPanelWindow::recheckFullscreenApp();
}

bool TilingManager::toggleSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->toggleSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::swapSplit(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->swapSplit(idOf(window))) this->markLayout(layout);
	return true;
}

bool TilingManager::splitRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->splitRatio(idOf(window), value, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::masterRatio(TrackedWindow* window, double value, bool exact) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	if (layout->tree->kind() != tiling::Kind::Master) return false;
	if (layout->tree->splitRatio(idOf(window), value * 2.0, exact)) this->markLayout(layout);
	return true;
}

bool TilingManager::swapWithMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* other = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (other != nullptr && other != window) this->swapWindows(m, this->managed.at(other));
	return true;
}

bool TilingManager::focusMaster(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.layout->tree->kind() != tiling::Kind::Master) return false;

	const auto& engine = static_cast<const tiling::MasterLayout&>(*m.layout->tree);
	auto master = engine.master();
	auto* target = this->windowOf(master == idOf(window) ? engine.firstInStack() : master);
	if (target != nullptr && target != window) target->activate();
	return true;
}

bool TilingManager::resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto* layout = iter->second.layout;
	auto dpr = layout->screen->devicePixelRatio();
	if (layout->tree->resize(idOf(window), toPhysical(dx, dpr), toPhysical(dy, dpr))) {
		this->markLayout(layout);
	}

	return true;
}

bool TilingManager::setFloating(TrackedWindow* window, bool floating, bool toggle) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return false;

	auto& m = iter->second;
	if (toggle) floating = m.layout != nullptr;

	if (!floating && m.elevated) {
		qCInfo(logTiling) << "Window" << window->addressHex()
		                  << "belongs to a more privileged process and can't be tiled.";
		return false;
	}

	m.override = floating ? Override::Float : Override::Tile;
	if (!floating) m.autoFloat = false;
	if (!floating && !Scratchpad::holds(window) && this->desktops->isWindowPinned(window->hwnd())) {
		this->desktops->pinWindow(window->hwnd(), false);
		SetWindowPos(window->hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}

	this->markWindow(window);
	return true;
}

bool TilingManager::beginEdgeDrag(TrackedWindow* window, bool left, bool top) {
	this->edgeDragState = EdgeDrag();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (m.fullscreen() || m.focusMaximized()) return false;
	if (this->stateOf(m) != State::Tiled) return false;
	this->clearMaximized(m.layout, nullptr, true);

	auto* layout = m.layout;
	auto id = idOf(window);
	auto pick = [&](Edge preferred, Edge other) -> std::optional<Edge> {
		if (layout->tree->hasEdge(id, preferred)) return preferred;
		if (layout->tree->hasEdge(id, other)) return other;
		return std::nullopt;
	};

	auto& drag = this->edgeDragState;
	drag.window = window;
	drag.layout = layout;
	drag.box = layout->tree->box(id);
	if (layout->tree->kind() == tiling::Kind::Dwindle) {
		const auto& tree = static_cast<const tiling::DwindleLayout&>(*layout->tree);
		drag.horizontal = tree.nearestEdge(id, true);
		drag.vertical = tree.nearestEdge(id, false);
		return true;
	}
	drag.horizontal = left ? pick(Edge::Left, Edge::Right) : pick(Edge::Right, Edge::Left);
	drag.vertical = top ? pick(Edge::Top, Edge::Bottom) : pick(Edge::Bottom, Edge::Top);
	return true;
}

void TilingManager::edgeDrag(const QPoint& delta) {
	auto& drag = this->edgeDragState;
	if (drag.window == nullptr) return;

	auto iter = this->managed.find(drag.window);
	if (iter == this->managed.end() || iter->second.layout != drag.layout
	    || !this->layoutAlive(drag.layout))
	{
		drag = EdgeDrag();
		return;
	}

	auto edgeOf = [&drag](Edge edge) {
		switch (edge) {
		case Edge::Left: return drag.box.left();
		case Edge::Right: return rightOf(drag.box);
		case Edge::Top: return drag.box.top();
		case Edge::Bottom: return bottomOf(drag.box);
		}
		return 0;
	};

	auto& tree = *drag.layout->tree;
	auto id = idOf(drag.window);
	auto changed = false;

	if (drag.horizontal) {
		changed |= tree.moveEdge(id, *drag.horizontal, edgeOf(*drag.horizontal) + delta.x());
	}

	if (drag.vertical) {
		changed |= tree.moveEdge(id, *drag.vertical, edgeOf(*drag.vertical) + delta.y());
	}

	if (changed) this->markLayout(drag.layout);
}

void TilingManager::endEdgeDrag() { this->edgeDragState = EdgeDrag(); }

QRect TilingManager::floatArea(TrackedWindow* window, const QRect& around) const {
	const auto native = toRECT(around);
	auto* monitor = MonitorFromRect(&native, MONITOR_DEFAULTTONEAREST);
	const auto work = monitorRects(monitor).work;
	auto* screen = screenForMonitor(monitor);
	if (!work.isValid() || screen == nullptr) return work;
	const auto gap = toPhysical(
	    Scratchpad::holds(window) ? SPECIAL_GAPS_OUT : this->mGapsOut,
	    screen->devicePixelRatio()
	);
	return work.adjusted(gap, gap, -gap, -gap);
}

std::optional<QRect> TilingManager::floatBase(TrackedWindow* window) const {
	if (window == nullptr || window->screen() == nullptr) return std::nullopt;
	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return std::nullopt;
	auto found = this->managed.find(window);
	if (found == this->managed.end()) return currentFrame(hwnd);
	const auto& m = found->second;
	if (m.mode != TileMode::Normal) return std::nullopt;
	return m.layout == nullptr && m.floatMoving && !m.floatFrame.isNull() ? m.floatFrame : currentFrame(hwnd);
}

void TilingManager::placeFloat(TrackedWindow* window, const QRect& frame) {
	auto* hwnd = window->hwnd();
	auto found = this->managed.find(window);
	if (found != this->managed.end() && this->mEnabled && this->mAnimateMoves && DwindleScene::enabled()) {
		auto& m = found->second;
		auto* home = this->floats(m) ? this->floatHome(m, true) : nullptr;
		if (home != nullptr && this->sceneShows(home, this->desktops->currentIndex())) {
			dropTopmost(m);
			m.floatFrame = frame;
			this->sendFloat(m);
			return;
		}
	}
	setFrame(hwnd, frame);
}

void TilingManager::resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact) {
	const auto base = this->floatBase(window);
	if (!base) return;

	auto dpr = window->screen()->devicePixelRatio();
	auto width = toPhysical(dx, dpr);
	auto height = toPhysical(dy, dpr);
	if (!exact) {
		width += base->width();
		height += base->height();
	}

	const auto size = QSize(qMax(1, width), qMax(1, height));
	const auto grown = size - base->size();
	this->placeFloat(window, QRect(base->topLeft() - QPoint(grown.width() / 2, grown.height() / 2), size));
}

void TilingManager::center(TrackedWindow* window) {
	const auto base = this->floatBase(window);
	if (!base) return;

	auto work = monitorRects(MonitorFromWindow(window->hwnd(), MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto frame = *base;
	frame.moveTo(
	    work.left() + (work.width() - frame.width()) / 2,
	    work.top() + (work.height() - frame.height()) / 2
	);
	this->placeFloat(window, frame);
}

void TilingManager::moveFloating(TrackedWindow* window, Edge direction) {
	const auto base = this->floatBase(window);
	if (!base) return;

	const auto work = this->floatArea(window, *base);
	if (!work.isValid()) return;

	auto frame = *base;
	switch (direction) {
	case Edge::Left: frame.moveLeft(work.left()); break;
	case Edge::Right: frame.moveRight(work.right()); break;
	case Edge::Top: frame.moveTop(work.top()); break;
	case Edge::Bottom: frame.moveBottom(work.bottom()); break;
	}

	this->placeFloat(window, frame);
}

Tiling::Tiling(QObject* parent): QObject(parent) {
	auto* manager = TilingManager::instance();
	QObject::connect(manager, &TilingManager::enabledChanged, this, &Tiling::enabledChanged);
	QObject::connect(manager, &TilingManager::suppressWindowShadowsChanged, this, &Tiling::suppressWindowShadowsChanged);
	QObject::connect(manager, &TilingManager::animateMovesChanged, this, &Tiling::animateMovesChanged);
	QObject::connect(manager, &TilingManager::gapsInChanged, this, &Tiling::gapsInChanged);
	QObject::connect(manager, &TilingManager::gapsOutChanged, this, &Tiling::gapsOutChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::preserveSplitChanged, this, &Tiling::preserveSplitChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::excludedChanged, this, &Tiling::excludedChanged);
	QObject::connect(manager, &TilingManager::fullscreenScreensChanged, this, &Tiling::fullscreenScreensChanged);
	QObject::connect(manager, &TilingManager::fullscreenConflict, this, &Tiling::fullscreenConflict);
	QObject::connect(manager, &TilingManager::layoutChanged, this, &Tiling::layoutChanged);
	QObject::connect(manager, &TilingManager::smartSplitChanged, this, &Tiling::smartSplitChanged);
	QObject::connect(manager, &TilingManager::forceSplitChanged, this, &Tiling::forceSplitChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::defaultSplitRatioChanged, this, &Tiling::defaultSplitRatioChanged);
	QObject::connect(manager, &TilingManager::masterNewStatusChanged, this, &Tiling::masterNewStatusChanged);
	QObject::connect(manager, &TilingManager::masterNewOnTopChanged, this, &Tiling::masterNewOnTopChanged);
	QObject::connect(manager, &TilingManager::masterOrientationChanged, this, &Tiling::masterOrientationChanged);
	// clang-format on
	QObject::connect(manager, &TilingManager::masterFactorChanged, this, &Tiling::masterFactorChanged);
	// clang-format off
	QObject::connect(manager, &TilingManager::scrollingColumnWidthChanged, this, &Tiling::scrollingColumnWidthChanged);
	QObject::connect(manager, &TilingManager::scrollingPresetsChanged, this, &Tiling::scrollingPresetsChanged);
	QObject::connect(manager, &TilingManager::scrollingCenterChanged, this, &Tiling::scrollingCenterChanged);
	// clang-format on
}

void Tiling::relayout() { TilingManager::instance()->relayout(); }
void Tiling::expectBarSpace(const QString& screenName, bool top, int reserved) {
	TilingManager::instance()->expectBarSpace(screenName, top, reserved);
}

bool Tiling::enabled() const { return TilingManager::instance()->enabled(); }
void Tiling::setEnabled(bool enabled) { TilingManager::instance()->setEnabled(enabled); }
bool Tiling::suppressWindowShadows() const { return TilingManager::instance()->suppressWindowShadows(); }
bool Tiling::animateMoves() const { return TilingManager::instance()->animateMoves(); }
void Tiling::setAnimateMoves(bool animate) { TilingManager::instance()->setAnimateMoves(animate); }
void Tiling::setSuppressWindowShadows(bool suppress) { TilingManager::instance()->setSuppressWindowShadows(suppress); }
qint32 Tiling::gapsIn() const { return TilingManager::instance()->gapsIn(); }
void Tiling::setGapsIn(qint32 gaps) { TilingManager::instance()->setGapsIn(gaps); }
qint32 Tiling::gapsOut() const { return TilingManager::instance()->gapsOut(); }
void Tiling::setGapsOut(qint32 gaps) { TilingManager::instance()->setGapsOut(gaps); }
bool Tiling::preserveSplit() const { return TilingManager::instance()->preserveSplit(); }
void Tiling::setPreserveSplit(bool preserve) {
	TilingManager::instance()->setPreserveSplit(preserve);
}
QStringList Tiling::excluded() const { return TilingManager::instance()->excluded(); }
QStringList Tiling::fullscreenScreens() const { return TilingManager::instance()->fullscreenScreens(); }
void Tiling::setExcluded(const QStringList& excluded) {
	TilingManager::instance()->setExcluded(excluded);
}

QString Tiling::layout() const { return TilingManager::instance()->layout(); }
void Tiling::setLayout(const QString& layout) { TilingManager::instance()->setLayout(layout); }
bool Tiling::smartSplit() const { return TilingManager::instance()->smartSplit(); }
void Tiling::setSmartSplit(bool smart) { TilingManager::instance()->setSmartSplit(smart); }
qint32 Tiling::forceSplit() const { return TilingManager::instance()->forceSplit(); }
void Tiling::setForceSplit(qint32 force) { TilingManager::instance()->setForceSplit(force); }
double Tiling::defaultSplitRatio() const { return TilingManager::instance()->defaultSplitRatio(); }
void Tiling::setDefaultSplitRatio(double ratio) {
	TilingManager::instance()->setDefaultSplitRatio(ratio);
}
QString Tiling::masterNewStatus() const { return TilingManager::instance()->masterNewStatus(); }
void Tiling::setMasterNewStatus(const QString& status) {
	TilingManager::instance()->setMasterNewStatus(status);
}
bool Tiling::masterNewOnTop() const { return TilingManager::instance()->masterNewOnTop(); }
void Tiling::setMasterNewOnTop(bool onTop) { TilingManager::instance()->setMasterNewOnTop(onTop); }
QString Tiling::masterOrientation() const { return TilingManager::instance()->masterOrientation(); }
void Tiling::setMasterOrientation(const QString& orientation) {
	TilingManager::instance()->setMasterOrientation(orientation);
}
double Tiling::masterFactor() const { return TilingManager::instance()->masterFactor(); }
void Tiling::setMasterFactor(double factor) { TilingManager::instance()->setMasterFactor(factor); }
double Tiling::scrollingColumnWidth() const {
	return TilingManager::instance()->scrollingColumnWidth();
}
void Tiling::setScrollingColumnWidth(double width) {
	TilingManager::instance()->setScrollingColumnWidth(width);
}
QList<double> Tiling::scrollingPresets() const {
	return TilingManager::instance()->scrollingPresets();
}
void Tiling::setScrollingPresets(const QList<double>& presets) {
	TilingManager::instance()->setScrollingPresets(presets);
}
QString Tiling::scrollingCenter() const { return TilingManager::instance()->scrollingCenter(); }
void Tiling::setScrollingCenter(const QString& center) {
	TilingManager::instance()->setScrollingCenter(center);
}

} // namespace qs::windows
