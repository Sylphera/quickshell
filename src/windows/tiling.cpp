#include "tiling.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <utility>

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qfileinfo.h>
#include <qguiapplication.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpoint.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>

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

// SPI_SETDROPSHADOW changes the current user's Windows appearance setting.
// Remember only changes made by this process, including for crash recovery.
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

constexpr int SYNC_DELAY_MS = 30;
constexpr int VERIFY_DELAY_MS = 150;
constexpr int DEADLINE_MS = 2000;
constexpr int HUNG_RETRY_MS = 500;
constexpr qint32 MAX_CORRECTIONS = 2;
constexpr qint64 CORRECTION_RESET_MS = 1500;
constexpr int EDGE_TOLERANCE = 2;
constexpr int TINY_CORNER_LIMIT = 80;
constexpr int SYSTEM_CAPTION_MIN = 8;
constexpr int TINY_CORNER_RADIUS = 12;
constexpr qint32 SPECIAL_GAPS_OUT = 30;
constexpr GUID SPECIAL_DESKTOP = {0x5ec1a100, 0x0000, 0x0000, {0x73, 0x70, 0x65, 0x63, 0x69, 0x61, 0x6c, 0x00}};
constexpr std::size_t TINY_CORNER_SLOTS = 256;
constexpr auto* TINY_CORNER_PROP = L"QuickshellTilingTinyRounded";

// Restore the process-owned clipping on abnormal exit, just like parked tiles.
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

// A window region survives if the shell is forcibly terminated. Store only
// our original corner preference as a window property, so a new shell can
// distinguish its stale clip from a region installed by the application.
bool markTinyCornerOwner(HWND hwnd, int preference) {
	if (preference < 0 || preference > 3) return false;
	auto value = static_cast<UINT_PTR>(preference + 1);
	return SetPropW(hwnd, TINY_CORNER_PROP, reinterpret_cast<HANDLE>(value)) != FALSE;
}

constexpr UINT_PTR TINY_CORNER_NC_DISABLED = 0x100;

void recoverOrphanedTinyCorners() {
	int restored = 0;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
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

void setFrame(HWND hwnd, const QRect& target, bool tiled = false, const QMargins* insets = nullptr) {
	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;

	// SetWindowRgn makes DWM report its frame as the full native HWND
	// rectangle. Use the saved unmasked frame margins while tiling a clipped
	// window, or Windows moves its surface by several pixels on each retry.
	auto left = insets ? insets->left() : frame.left() - raw.left();
	auto top = insets ? insets->top() : frame.top() - raw.top();
	auto right = insets ? insets->right() : rightOf(raw) - rightOf(frame);
	auto bottom = insets ? insets->bottom() : bottomOf(raw) - bottomOf(frame);

	// Tiling must honor the exact rectangle generated by the layout, even
	// when it is smaller than the application's interactive resize minimum.
	// DefWindowProc enforces WM_GETMINMAXINFO during WM_WINDOWPOSCHANGING;
	// bypass that check only for managed tile placement, not floating windows.
	auto flags = UINT(SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	if (tiled) flags |= SWP_NOSENDCHANGING;
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

void setRawRect(HWND hwnd, const QRect& rect) {
	SetWindowPos(
	    hwnd,
	    nullptr,
	    rect.left(),
	    rect.top(),
	    rect.width(),
	    rect.height(),
	    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
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

	this->verifyTimer.setSingleShot(true);
	this->verifyTimer.setInterval(VERIFY_DELAY_MS);
	QObject::connect(&this->verifyTimer, &QTimer::timeout, this, &TilingManager::verify);

	this->deadlineTimer.setSingleShot(true);
	this->deadlineTimer.setInterval(DEADLINE_MS);
	QObject::connect(&this->deadlineTimer, &QTimer::timeout, this, &TilingManager::onDeadline);

	if (auto* app = QCoreApplication::instance()) {
		QObject::connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
			this->unparkAll();
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

	this->syncWheel();
	emit this->enabledChanged();
}

void TilingManager::setAnimateMoves(bool animate) {
	if (animate == this->mAnimateMoves) return;
	this->mAnimateMoves = animate;
	emit this->animateMovesChanged();
}

namespace {
// Where a sliding window waits: past the right edge of every screen, at the
// size of its new tile, so it is laid out and repainted before it shows.
QRect parkedFrame(const QRect& tile) {
	auto x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 100;
	return QRect(QPoint(x, tile.top()), tile.size());
}
} // namespace

// end-4's windowsMove: a tiled window that settled somewhere and gets a new
// tile slides there instead of jumping. While it slides, tiling's own target
// for it is the parked spot, so its checks don't pull it back on screen.
void TilingManager::placeMoving(Managed& m, const Layout* layout, const QRect& tile) {
	auto* motion = WindowMotion::instance();
	auto found = this->moving.find(m.window);
	if (found != this->moving.end()) {
		// It keeps waiting where it is, at the size it had: only the thumbnail
		// heads somewhere else.
		auto parked = m.target.isNull() ? parkedFrame(tile) : m.target;
		if (*found != tile) {
			*found = tile;
			motion->retarget(m.window->hwnd(), tile, parked);
		}
		this->place(m, parked);
		return;
	}
	// end-4's windowsIn: a window that just opened pops in at its tile.
	if (m.target.isNull() && this->canPop(m, layout, tile)) {
		m.fresh = false;
		if (motion->begin(m.window->hwnd(), tile, tile, parkedFrame(tile), true)) {
			this->moving.insert(m.window, tile);
			this->place(m, parkedFrame(tile));
			return;
		}
	}
	m.fresh = false;
	if (this->canAnimate(m, layout, tile)) {
		// As in Hyprland, the app takes its new size right away and its live
		// thumbnail shows it while it travels.
		auto from = this->managedFrame(m);
		auto parked = parkedFrame(tile);
		if (motion->begin(m.window->hwnd(), from, tile, parked)) {
			this->moving.insert(m.window, tile);
			this->place(m, parked);
			return;
		}
	}
	this->place(m, tile);
}

bool TilingManager::canPop(const Managed& m, const Layout* layout, const QRect& tile) const {
	if (!this->mAnimateMoves || !m.fresh || layout->special || m.window == this->dragging) return false;
	if (QDateTime::currentMSecsSinceEpoch() - m.managedAt > 2000) return false;
	auto* hwnd = m.window->hwnd();
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsHungAppWindow(hwnd)) return false;
	if (this->desktops->shownIndex() != this->desktops->currentIndex()) return false;
	if (m.window->desktop() != this->desktops->currentIndex()) return false;
	auto rects = monitorRects(monitorForScreen(layout->screen));
	return rects.valid && rects.monitor.contains(tile);
}

bool TilingManager::canAnimate(const Managed& m, const Layout* layout, const QRect& tile) const {
	// Special too: closing or moving a window there slides the others, as on a
	// normal desktop.
	if (!this->mAnimateMoves || m.target.isNull() || tile == m.target) return false;
	if ((m.verifying && !this->barMoving) || m.offscreen || m.tinyCornersApplied || m.window == this->dragging) return false;

	auto* hwnd = m.window->hwnd();
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsHungAppWindow(hwnd)) return false;
	// Still where tiling left it. A few pixels off is the app nudging itself
	// when the work area changes (Chromium does), which shouldn't cost the
	// slide; anything more is someone else's move.
	auto frame = this->managedFrame(m);
	if (frame != m.target) {
		auto off = std::max({
		    std::abs(frame.left() - m.target.left()),
		    std::abs(frame.top() - m.target.top()),
		    std::abs(frame.right() - m.target.right()),
		    std::abs(frame.bottom() - m.target.bottom()),
		});
		if (off > 48) return false;
	}

	// Only what is on screen now: not a desktop on its way out or in.
	if (this->desktops->shownIndex() != this->desktops->currentIndex()) return false;
	if (m.window->desktop() != this->desktops->currentIndex()) return false;

	auto rects = monitorRects(monitorForScreen(layout->screen));
	return rects.valid && rects.monitor.contains(m.target) && rects.monitor.contains(tile) ;
}

void TilingManager::onMotionLanded(HWND hwnd, const QRect& /*frame*/) {
	for (auto iter = this->moving.begin(); iter != this->moving.end(); ++iter) {
		if (iter.key()->hwnd() != hwnd) continue;
		auto* window = iter.key();
		auto tile = iter.value();
		this->moving.erase(iter);
		auto found = this->managed.find(window);
		if (found != this->managed.end()) this->place(found->second, tile);
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

	// Every window is sent to its tile again, but keeps where it was: a window
	// whose tile changed (the bar toggled) slides there (windowsMove).
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

	// Measured from the screen's edge: only the bar's edge changes, the others
	// keep what Windows reserves there (the dock, say).
	auto physical = std::max(0, static_cast<int>(std::lround(reserved * screen->devicePixelRatio())));
	auto work = rects.work;
	if (top) work.setTop(std::min(rects.monitor.top() + physical, work.bottom()));
	else work.setBottom(std::max(rects.monitor.bottom() - physical, work.top()));
	expectWorkArea(monitor, work, 1500);

	this->barMoving = true;
	this->relayout();
	this->sync();
	this->barMoving = false;

	// Maximized windows aren't tiled: Windows fits them to the work area by
	// itself, without a slide. They slide to it here like the tiled ones; the
	// motion leaves them at the new work area, still maximized.
	if (!this->mAnimateMoves || this->desktops->shownIndex() != this->desktops->currentIndex()) return;
	auto* motion = WindowMotion::instance();
	for (auto& [window, m]: this->managed) {
		if (!window->maximized() || window->minimized() || m.elevated) continue;
		if (window->desktop() != this->desktops->currentIndex() || Scratchpad::holds(window)) continue;
		if (monitorForScreen(window->screen()) != monitor) continue;
		auto* hwnd = window->hwnd();
		if (!IsWindowVisible(hwnd) || IsHungAppWindow(hwnd)) continue;
		RECT bounds {};
		if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) continue;
		auto from = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
		if (from == work && !motion->moving(hwnd)) continue;
		motion->begin(hwnd, from, work, parkedFrame(work));
	}
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

bool TilingManager::start() {
	this->ownerMutex = CreateMutexW(nullptr, TRUE, L"Local\\QuickshellTiling");
	if (this->ownerMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
		qCWarning(logTiling) << "Another Quickshell process already tiles windows;"
		                     << "tiling stays off in this one.";
		if (this->ownerMutex != nullptr) CloseHandle(this->ownerMutex);
		this->ownerMutex = nullptr;
		return false;
	}

	// Only the mutex owner can clear stale per-window regions from a previous
	// instance; never touch regions owned by the target application.
	recoverOrphanedTinyCorners();

	this->tracker = WindowTracker::instance();
	this->desktops = this->tracker->desktops();

	auto& c = this->connections;
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowAdded, this, &TilingManager::manage)
	);
	c.append(
	    QObject::connect(this->tracker, &WindowTracker::windowRemoved, this, &TilingManager::unmanage)
	);
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
	c.append(QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, [this]() {
		this->markAll();
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
	return true;
}

void TilingManager::stop() {
	for (const auto& connection: this->connections) QObject::disconnect(connection);
	this->connections.clear();

	for (auto* screen: QGuiApplication::screens()) {
		QObject::disconnect(screen, nullptr, this, nullptr);
	}

	QList<TrackedWindow*> tiled;
	for (auto& [window, m]: this->managed) {
		this->restoreTinyCorners(m);
		QObject::disconnect(window, nullptr, this, nullptr);
		if (m.layout == nullptr) continue;

		tiled.append(window);
		this->restoreFloating(m);
	}

	this->managed.clear();
	this->layouts.clear();
	this->dirtyWindows.clear();
	this->dirtyLayouts.clear();
	this->unverified.clear();
	this->dragging = nullptr;
	this->edgeDragState = EdgeDrag();
	this->syncTimer.stop();
	this->verifyTimer.stop();
	this->deadlineTimer.stop();

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

	if (m.elevated) {
		qCDebug(logTiling) << "Window" << window->addressHex() << window->appId()
		                   << "belongs to a more privileged process; it floats.";
	}

	auto mark = [this, window]() { this->markWindow(window); };
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

	auto* from = iter->second.layout;
	auto refocus = from != nullptr && from->lastFocused == window
	            && TilingManager::scrollingOf(from) != nullptr;

	QObject::disconnect(window, nullptr, this, nullptr);
	if (auto moved = this->moving.find(window); moved != this->moving.end()) {
		// A window leaving tiling mid-slide goes straight to where it was going.
		WindowMotion::instance()->cancel(window->hwnd());
		if (IsWindow(window->hwnd())) setFrame(window->hwnd(), moved.value());
		this->moving.erase(moved);
	}
	this->takeOut(iter->second);
	if (iter->second.offscreen) forgetParked(window->hwnd());
	this->managed.erase(iter);

	this->dirtyWindows.remove(window);
	this->unverified.remove(window);
	if (this->dragging == window) this->dragging = nullptr;

	for (auto& [key, layout]: this->layouts) {
		if (layout->lastFocused == window) layout->lastFocused = nullptr;
	}

	if (refocus) this->refocus(from);
}

TilingManager::State TilingManager::stateOf(const Managed& m) const {
	auto* window = m.window;

	if (Scratchpad::holds(window)) {
		if (m.elevated) return State::Out;
		if (window->minimized() || !Scratchpad::instance()->shown()) return State::Hidden;
		return State::Tiled;
	}
	if (window->desktop() < 0 || window->screen() == nullptr) return State::Out;
	if (!QGuiApplication::screens().contains(window->screen())) return State::Out;
	if (m.elevated) return State::Out;
	if (m.override == Override::Float) return State::Out;
	if (m.override != Override::Tile && (m.autoFloat || this->floatsByRule(m))) return State::Out;

	if (window->minimized()) return State::Hidden;
	if (window->maximized()) return State::Suspended;

	if (window->fullscreen()) {
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

	constexpr auto FLOATING_EX =
	    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_DLGMODALFRAME;
	if ((exStyle & FLOATING_EX) != 0) return true;

	if ((style & WS_THICKFRAME) == 0) return true;

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
	if (!this->syncTimer.isActive()) this->syncTimer.start();
}

void TilingManager::sync() {
	setParkNewWindows(
	    this->mEnabled && this->mAnimateMoves && startup::settled()
	    && this->desktops->shownIndex() == this->desktops->currentIndex()
	);
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
				this->unpark(m);
				m.layout = nullptr;
				m.target = QRect();
				this->dirtyWindows.insert(window);
			}
		}

		this->dirtyLayouts.remove(layout);
		iter = this->layouts.erase(iter);
	}

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
}

void TilingManager::syncWindow(TrackedWindow* window) {
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;
	auto& m = iter->second;

	if (window == this->dragging) return;

	auto state = this->stateOf(m);
	if (state != State::Tiled) {
		releaseEarlyPark(window->hwnd());
		m.verifying = false;
		this->restoreTinyCorners(m);
	}

	if (state == State::Out) {
		if (m.layout == nullptr) return;

		auto floated = !window->minimized() && window->desktop() >= 0 && !m.elevated;
		this->takeOut(m);
		if (floated) this->restoreFloating(m);
		else this->unpark(m);
		return;
	}

	auto special = Scratchpad::holds(window);
	auto index = window->desktop();
	if (!special && index >= this->desktops->count()) return;
	auto desktop = special ? SPECIAL_DESKTOP : this->desktops->desktops().at(index).id;

	Layout* want = nullptr;
	auto* current = m.layout;
	if (current != nullptr && this->layoutAlive(current) && IsEqualGUID(current->desktop, desktop)) {
		want = current;

		if (!special && state == State::Tiled && !m.verifying && !m.offscreen
		    && window->screen() != current->screen)
		{
			this->moveToLayout(m, this->layoutFor(window, true), nullptr);
			return;
		}
	} else if (state == State::Tiled) {
		want = this->layoutFor(window, true);
	}

	if (want == m.layout) {
		if (m.parked && state == State::Tiled) m.target = QRect();
		m.parked = state != State::Tiled;
		this->markLayout(m.layout);
		return;
	}

	if (m.layout != nullptr) this->takeOut(m);

	if (want != nullptr) {
		auto cursor = cursorPos();
		this->insertInto(m, want, nullptr, &cursor);
	}
}

void TilingManager::insertInto(
    Managed& m,
    Layout* layout,
    TrackedWindow* target,
    const QPoint* cursor
) {
	auto* window = m.window;

	if (m.preTiling.isNull()) {
		QRect raw;
		QRect frame;
		if (readFrame(window->hwnd(), raw, frame)) m.preTiling = raw;
	}

	if (target == nullptr && cursor != nullptr && layout->tree->kind() == tiling::Kind::Dwindle) {
		// Automatic insertion follows the pointer, not lastFocused or the
		// rightmost leaf. Explicit targets used on startup remain unchanged.
		// Refresh the visible boxes before hit testing: minimization and
		// changes to split options may have occurred since the last layout.
		layout->tree->configure(this->options());
		for (auto id: layout->tree->ids()) {
			auto* existing = this->windowOf(id);
			if (existing == nullptr) continue;
			layout->tree->setHidden(id, this->stateOf(this->managed.at(existing)) == State::Hidden);
		}
		auto rects = monitorRects(monitorForScreen(layout->screen));
		if (rects.valid) layout->tree->compute(rects.work);
		auto hovered = tiling::cursorInsertionTarget(*layout->tree, *cursor);
		target = hovered == 0 ? nullptr : this->windowOf(hovered);
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

	if (auto* scroll = TilingManager::scrollingOf(layout)) {
		this->prepareScrolling(layout);
		auto focused = this->tracker->activeWindow() == window
		            || GetForegroundWindow() == window->hwnd();
		if (focused) scroll->activate(idOf(window));
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

void TilingManager::takeOut(Managed& m) {
	this->restoreTinyCorners(m);
	auto* layout = m.layout;
	if (layout == nullptr) return;

	layout->tree->remove(idOf(m.window));
	if (layout->lastFocused == m.window) layout->lastFocused = nullptr;

	m.layout = nullptr;
	m.target = QRect();
	m.accepted = QRect();
	m.verifying = false;
	this->unverified.remove(m.window);

	this->markLayout(layout);
	emit this->windowTilingChanged(m.window);
}

void TilingManager::dropEmptyLayouts() {
	for (auto iter = this->layouts.begin(); iter != this->layouts.end();) {
		if (iter->second->tree->isEmpty()) {
			this->dirtyLayouts.remove(iter->second.get());
			iter = this->layouts.erase(iter);
		} else {
			++iter;
		}
	}
}

void TilingManager::apply(Layout* layout) {
	if (!this->layoutAlive(layout)) return;

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

	auto* scroll = TilingManager::scrollingOf(layout);
	auto in = toPhysical(this->mGapsIn, layout->screen->devicePixelRatio());

	for (auto id: layout->tree->ids()) {
		auto* window = this->windowOf(id);
		if (window == nullptr || window == this->dragging) continue;

		auto& m = this->managed.at(window);
		if (this->stateOf(m) != State::Tiled) continue;

		auto parked = scroll != nullptr && scroll->isParked(id);
		auto restore = QRect();
		if (parked) restore = clampInto(scroll->visibleBox(id).adjusted(in, in, -in, -in), rects.work);
		this->markOffscreen(m, parked, restore);

		this->placeMoving(m, layout, this->tileRect(layout, window));
	}
}

QRect TilingManager::tileRect(const Layout* layout, TrackedWindow* window) const {
	auto box = layout->tree->box(idOf(window));
	auto area = layout->tree->area();
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
			// Windows 11's separately rendered DWM frame border is rectangular
			// even when the window region has genuinely rounded corners.
			// DWMWA_BORDER_COLOR is set-only. Restore the currently configured
			// decoration color rather than erasing a user-selected border color.
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
		// Region belongs to this manager only if we installed it. Never clear
		// pre-existing regions: those windows are not opted in.
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
	if (m.layout == nullptr || m.offscreen || frame.isEmpty()
	    || (frame.width() >= TINY_CORNER_LIMIT && frame.height() >= TINY_CORNER_LIMIT))
	{
		this->restoreTinyCorners(m);
		return;
	}

	auto* hwnd = m.window->hwnd();
	if (!IsWindow(hwnd) || IsHungAppWindow(hwnd)) return;
	if (!m.tinyCornersApplied && hasSystemCaption(hwnd)) {
		// A region takes the DWM frame away from windows with a system title
		// bar, so they keep their frame and only ask DWM for small corners.
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

	// DWM reports the *entire* raw HWND as EXTENDED_FRAME_BOUNDS as soon
	// as SetWindowRgn is active. Recomputing insets from that reading
	// progressively expands the mask to the native, square frame.
	// Freeze the pre-mask insets and reuse them until we remove our region.
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
		// The target app can repaint the native frame after our asynchronous
		// SetWindowPos. Reassert tiny DWM rounding and border suppression.
		// A one-time setting leaves a square outline on Windows Terminal.
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
		// Respect apps that manage their own nonrectangular window region.
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

	// Limit the *visual* corner radius to half the smaller tile dimension.
	// Rounded corners remain enabled in appearance, down to pixel resolution.
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
		// Windows Terminal composes a child DirectComposition surface outside
		// the GDI region. DONOTROUND leaves a dark square surrounding that
		// surface. ROUNDSMALL constrains the native surface as well; the GDI
		// region above still provides a radius proportional to the tiny tile.
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

		// Keep the adaptive GDI region intact. Only reassert the DWM settings
		// which Win32 can repaint when a tiny window gains or loses focus.
		auto corner = DWMWCP_ROUNDSMALL;
		DwmSetWindowAttribute(window->hwnd(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		auto border = COLORREF(DWMWA_COLOR_NONE);
		DwmSetWindowAttribute(window->hwnd(), DWMWA_BORDER_COLOR, &border, sizeof(border));
	}
}

void TilingManager::place(Managed& m, const QRect& tile) {
	claimEarlyPark(m.window->hwnd());
	auto frame = this->managedFrame(m);
	this->updateTinyCorners(m, frame);

	if (tile != m.target) {
		m.target = tile;
		m.accepted = QRect();
		m.corrections = 0;

		if (frame == tile) m.verifying = false;
		else this->send(m, frame);
		return;
	}

	if (frame == tile) {
		m.verifying = false;
		return;
	}

	if (!m.accepted.isNull() && frame == m.accepted) return;
	if (m.verifying && frame == m.before) return;

	auto now = QDateTime::currentMSecsSinceEpoch();
	if (now - m.lastCorrection > CORRECTION_RESET_MS) m.corrections = 0;

	if (m.corrections >= MAX_CORRECTIONS) {
		this->giveUp(m, frame);
		return;
	}

	m.corrections++;
	m.lastCorrection = now;
	this->send(m, frame);
}

void TilingManager::send(Managed& m, const QRect& frame) {
	m.before = frame;
	m.verifying = true;
	m.wasHung = false;
	m.deadline = QDateTime::currentMSecsSinceEpoch() + DEADLINE_MS;
	setFrame(m.window->hwnd(), m.target, true,
	         m.tinyCornersApplied ? &m.tinyFrameInsets : nullptr);
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

void TilingManager::restoreFloating(Managed& m) {
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
	if (previous.isNull()) return;

	if (IsIconic(hwnd) || IsZoomed(hwnd)) {
		restorePlacement(hwnd, onScreen(previous));
		return;
	}

	QRect raw;
	QRect frame;
	if (!readFrame(hwnd, raw, frame)) return;
	if (!parkedAt.isNull()) raw = parkedAt;

	auto rect = previous;
	auto previousRect = toRECT(previous);
	auto rawRect = toRECT(raw);
	auto* then = MonitorFromRect(&previousRect, MONITOR_DEFAULTTONEAREST);
	auto* now = MonitorFromRect(&rawRect, MONITOR_DEFAULTTONEAREST);

	if (then != now) {
		rect.moveCenter(raw.center());
		rect = clampInto(rect, monitorRects(now).work);
	}

	setRawRect(hwnd, onScreen(rect));
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
	this->sync();
	for (auto& [key, layout]: this->layouts) {
		if (layout->special) this->apply(layout.get());
	}
}

QHash<HWND, QRect> TilingManager::specialFrames() const {
	QHash<HWND, QRect> frames;
	for (const auto& [key, layout]: this->layouts) {
		if (!layout->special) continue;
		for (auto id: layout->tree->ids()) {
			if (layout->tree->isHidden(id)) continue;
			auto* window = this->windowOf(id);
			if (window == nullptr) continue;
			auto found = this->managed.find(window);
			if (found == this->managed.end() || this->stateOf(found->second) != State::Tiled) continue;
			frames.insert(window->hwnd(), this->tileRect(layout.get(), window).translated(0, -this->mSpecialOffset));
		}
	}
	return frames;
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

	auto raw = restore;
	QRect current;
	QRect frame;
	if (readFrame(hwnd, current, frame)) {
		raw = restore.adjusted(
		    current.left() - frame.left(),
		    current.top() - frame.top(),
		    rightOf(current) - rightOf(frame),
		    bottomOf(current) - bottomOf(frame)
		);
	}

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
	for (auto& [window, m]: this->managed) this->unpark(m);
}

void TilingManager::restoreForCrash() {
	setWindowShadowsSuppressed(false);
	for (auto& slot: gTinyCornerSlots) {
		auto* hwnd = slot.hwnd.exchange(nullptr);
		if (hwnd == nullptr || !IsWindow(hwnd)) continue;
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

void TilingManager::onActiveWindowChanged() {
	this->reassertTinyDecorations();
	QTimer::singleShot(40, this, [this]() { this->reassertTinyDecorations(); });

	auto* window = this->tracker->activeWindow();
	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	auto& m = iter->second;
	m.focusStamp = ++this->focusCounter;
	if (m.layout == nullptr) return;

	m.layout->lastFocused = window;

	if (auto* scroll = TilingManager::scrollingOf(m.layout)) {
		this->prepareScrolling(m.layout);
		if (scroll->activate(idOf(window))) this->markLayout(m.layout);
	}
}

void TilingManager::onRectChanged(TrackedWindow* window) {
	if (window == this->dragging) return;

	auto iter = this->managed.find(window);
	if (iter == this->managed.end()) return;

	const auto& m = iter->second;
	if (m.layout == nullptr || m.target.isNull()) return;

	this->unverified.insert(window);
	if (!this->verifyTimer.isActive()) this->verifyTimer.start();
}

void TilingManager::verify() {
	auto pending = std::move(this->unverified);
	this->unverified.clear();

	for (auto* window: pending) {
		auto iter = this->managed.find(window);
		if (iter == this->managed.end() || window == this->dragging) continue;

		auto& m = iter->second;
		if (m.layout == nullptr || m.target.isNull()) continue;
		if (this->stateOf(m) != State::Tiled) continue;

		this->place(m, m.target);
	}
}

void TilingManager::giveUp(Managed& m, const QRect& frame) {
	m.verifying = false;

	if (m.offscreen) {
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

		if (m.offscreen) {
			m.verifying = false;
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
	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return;
	this->restoreTinyCorners(iter->second);

	auto state = this->stateOf(iter->second);
	if (state != State::Tiled && !(state == State::Suspended && window->maximized())) return;

	this->dragging = window;
	this->unverified.remove(window);
}

void TilingManager::onMoveSizeEnded(TrackedWindow* window) {
	if (window != this->dragging) return;
	this->dragging = nullptr;

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

	auto movedLeft = qAbs(frame.left() - target.left()) > EDGE_TOLERANCE;
	auto movedTop = qAbs(frame.top() - target.top()) > EDGE_TOLERANCE;
	auto movedRight = qAbs(rightOf(frame) - rightOf(target)) > EDGE_TOLERANCE;
	auto movedBottom = qAbs(bottomOf(frame) - bottomOf(target)) > EDGE_TOLERANCE;
	auto moved = int(movedLeft) + int(movedTop) + int(movedRight) + int(movedBottom);
	auto resized = qAbs(frame.width() - target.width()) > EDGE_TOLERANCE
	            || qAbs(frame.height() - target.height()) > EDGE_TOLERANCE;

	m.target = QRect();

	if (moved == 0) {
		this->markLayout(m.layout);
		return;
	}

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

void TilingManager::drop(Managed& m, const QPoint& cursor) {
	auto* from = m.layout;
	auto self = idOf(m.window);

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
		a.layout = layoutB;
		b.layout = layoutA;
		if (layoutA->lastFocused == a.window) layoutA->lastFocused = b.window;
		if (layoutB->lastFocused == b.window) layoutB->lastFocused = a.window;
	}

	this->markLayout(layoutA);
	this->markLayout(layoutB);
}

void TilingManager::moveToLayout(Managed& m, Layout* layout, TrackedWindow* target) {
	auto* from = m.layout;
	if (from == nullptr || layout == nullptr || from == layout) return;

	from->tree->remove(idOf(m.window));
	if (from->lastFocused == m.window) from->lastFocused = nullptr;
	this->markLayout(from);

	if (target == nullptr || !layout->tree->contains(idOf(target))) target = layout->lastFocused;
	if (target != nullptr && layout->tree->isHidden(idOf(target))) target = nullptr;
	layout->tree->configure(this->options());
	layout->tree->insert(idOf(m.window), target == nullptr ? 0 : idOf(target), nullptr);
	layout->lastFocused = m.window;
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

	// Special is a workspace of its own: its windows trade places with each
	// other and never leave it for a desktop or screen behind it. Moving one
	// to a desktop unpinned it, and special then dragged the user there.
	if (m->layout->special) {
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

	if (next != nullptr && next->layout == m->layout) {
		this->swapWindows(*m, *next);
	} else if (next != nullptr) {
		this->moveToLayout(*m, next->layout, next->window);
	} else if (auto* screen = this->screenInDirection(m->layout->screen, direction)) {
		this->moveToLayout(*m, this->layoutAt(m->layout->desktop, screen, true), nullptr);
	}

	return true;
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

bool TilingManager::toggleFullWidth(TrackedWindow* window) {
	Layout* layout = nullptr;
	auto* scroll = this->scrollingFor(window, &layout);
	if (scroll == nullptr) return false;

	scroll->toggleFullWidth(idOf(window));
	this->settle(layout);
	return true;
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

	this->markWindow(window);
	return true;
}

bool TilingManager::beginEdgeDrag(TrackedWindow* window, bool left, bool top) {
	this->edgeDragState = EdgeDrag();

	auto iter = this->managed.find(window);
	if (iter == this->managed.end() || iter->second.layout == nullptr) return false;

	auto& m = iter->second;
	if (this->stateOf(m) != State::Tiled) return false;

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

void TilingManager::resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact) {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto dpr = window->screen()->devicePixelRatio();
	auto frame = currentFrame(hwnd);
	auto width = toPhysical(dx, dpr);
	auto height = toPhysical(dy, dpr);
	if (!exact) {
		width += frame.width();
		height += frame.height();
	}

	setFrame(hwnd, QRect(frame.topLeft(), QSize(qMax(1, width), qMax(1, height))));
}

void TilingManager::center(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto frame = currentFrame(hwnd);
	frame.moveTo(
	    work.left() + (work.width() - frame.width()) / 2,
	    work.top() + (work.height() - frame.height()) / 2
	);
	setFrame(hwnd, frame);
}

void TilingManager::moveFloating(TrackedWindow* window, Edge direction) const {
	if (window == nullptr || window->screen() == nullptr) return;

	auto* hwnd = window->hwnd();
	if (IsIconic(hwnd) || IsZoomed(hwnd)) return;

	auto work = monitorRects(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).work;
	if (!work.isValid()) return;

	auto gap = toPhysical(this->mGapsOut, window->screen()->devicePixelRatio());
	work.adjust(gap, gap, -gap, -gap);

	auto frame = currentFrame(hwnd);
	switch (direction) {
	case Edge::Left: frame.moveLeft(work.left()); break;
	case Edge::Right: frame.moveRight(work.right()); break;
	case Edge::Top: frame.moveTop(work.top()); break;
	case Edge::Bottom: frame.moveBottom(work.bottom()); break;
	}

	setFrame(hwnd, frame);
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
