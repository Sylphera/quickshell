#include "window_rules.hpp"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <memory>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qstring.h>
#include <qtimer.h>

#include "scratchpad.hpp"
#include "startup.hpp"
#include "util.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logRules, "quickshell.windows.rules", QtWarningMsg);

constexpr int CLASS_LENGTH = 64;
constexpr int TITLE_LENGTH = 256;
constexpr auto DIALOG_CLASS = L"#32770";
constexpr int RETRIES_MS[] = {0, 80, 250, 700};
constexpr double FILE_DIALOG_WIDTH = 0.60;
constexpr double FILE_DIALOG_HEIGHT = 0.65;
constexpr double PICTURE_SIZE = 0.25;
constexpr double PICTURE_X = 0.73;
constexpr double PICTURE_Y = 0.72;
constexpr LONG SHARING_MARGIN = 12;

bool hasClass(HWND hwnd, const wchar_t* name) {
	wchar_t cls[CLASS_LENGTH] {};
	GetClassNameW(hwnd, cls, CLASS_LENGTH);
	return wcscmp(cls, name) == 0;
}

QString titleOf(HWND hwnd) {
	wchar_t title[TITLE_LENGTH] {};
	auto length = GetWindowTextW(hwnd, title, TITLE_LENGTH);
	return QString::fromWCharArray(title, length);
}

bool showsFiles(HWND hwnd) {
	auto found = false;
	EnumChildWindows(
	    hwnd,
	    [](HWND child, LPARAM param) -> BOOL {
		    if (!hasClass(child, L"DUIViewWndClassName")) return TRUE;
		    *reinterpret_cast<bool*>(param) = true;
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&found)
	);
	return found;
}

const QRegularExpression& pictureTitle() {
	static const auto expression = QRegularExpression(
	    QStringLiteral(
	        "^(picture[- ]?in[- ]?picture|imagen en imagen|imagen incrustada|pantalla en pantalla)"
	    ),
	    QRegularExpression::CaseInsensitiveOption
	);
	return expression;
}

const QRegularExpression& sharingTitle() {
	static const auto expression = QRegularExpression(
	    QStringLiteral(
	        "is sharing (a window|your screen)|está compartiendo (una ventana|tu pantalla)"
	    ),
	    QRegularExpression::CaseInsensitiveOption
	);
	return expression;
}

void place(HWND hwnd, LONG x, LONG y, LONG width, LONG height, bool resize) {
	auto flags = UINT(SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	if (!resize) flags |= SWP_NOSIZE;
	SetWindowPos(hwnd, nullptr, x, y, width, height, flags);
}

}

WindowRules* WindowRules::instance() {
	static QPointer<WindowRules> rules; // NOLINT
	if (rules.isNull()) rules = new WindowRules(QCoreApplication::instance());
	return rules.data();
}

WindowRules::WindowRules(QObject* parent): QObject(parent) {}

void WindowRules::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	if (enabled && !this->watching) {
		this->watching = true;
		EnumWindows(
		    [](HWND hwnd, LPARAM param) -> BOOL {
			    if (hasClass(hwnd, DIALOG_CLASS)) reinterpret_cast<QSet<HWND>*>(param)->insert(hwnd);
			    return TRUE;
		    },
		    reinterpret_cast<LPARAM>(&this->seenDialogs)
		);
		auto* tracker = WindowTracker::instance();
		QObject::connect(tracker, &WindowTracker::windowAdded, this, &WindowRules::onWindowAdded);
		QObject::connect(tracker, &WindowTracker::foregroundChanged, this, &WindowRules::onForeground);
	}
	emit this->enabledChanged();
}

void WindowRules::onWindowAdded(TrackedWindow* window) {
	if (window->desktop() != VirtualDesktops::instance()->currentIndex()) return;
	if (hasClass(window->hwnd(), DIALOG_CLASS)) this->onForeground(window->hwnd());
	else this->watch(window->hwnd());
}

void WindowRules::onForeground(HWND hwnd) {
	if (!hasClass(hwnd, DIALOG_CLASS) || isOwnProcessWindow(hwnd)) return;
	this->seenDialogs.removeIf([](HWND seen) { return IsWindow(seen) == FALSE; });
	if (this->seenDialogs.contains(hwnd)) return;
	this->seenDialogs.insert(hwnd);
	this->watch(hwnd);
}

void WindowRules::watch(HWND hwnd) {
	if (!this->mEnabled || !startup::settled()) return;
	auto* desktops = VirtualDesktops::instance();
	if (desktops->shownIndex() != desktops->currentIndex()) return;

	auto applied = std::make_shared<Kind>(Kind::None);
	for (auto delay: RETRIES_MS) {
		QTimer::singleShot(delay, this, [this, hwnd, applied] {
			if (!this->mEnabled || !IsWindow(hwnd)) return;
			auto kind = WindowRules::kindOf(hwnd);
			if (kind == Kind::None || kind <= *applied) return;
			*applied = kind;
			WindowRules::apply(hwnd, kind);
		});
	}
}

WindowRules::Kind WindowRules::kindOf(HWND hwnd) {
	if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) || IsHungAppWindow(hwnd)) {
		return Kind::None;
	}
	auto* window = WindowTracker::instance()->windowFor(hwnd);
	if (Scratchpad::holds(window) || (window != nullptr && window->fullscreen())) return Kind::None;

	if (!hasClass(hwnd, DIALOG_CLASS)) {
		auto title = titleOf(hwnd);
		if (sharingTitle().match(title).hasMatch()) return Kind::SharingBar;
		return pictureTitle().match(title).hasMatch() ? Kind::PictureInPicture : Kind::None;
	}

	auto sizable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0;
	return sizable && showsFiles(hwnd) ? Kind::FileDialog : Kind::Dialog;
}

void WindowRules::apply(HWND hwnd, Kind kind) {
	auto* owner = GetWindow(hwnd, GW_OWNER);
	auto* anchor = owner != nullptr && IsWindowVisible(owner) && !IsIconic(owner) ? owner : hwnd;

	RECT rect {};
	MONITORINFO info {.cbSize = sizeof(MONITORINFO)};
	if (!GetWindowRect(hwnd, &rect)) return;
	if (!GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &info)) return;

	const auto& work = info.rcWork;
	const auto& monitor = info.rcMonitor;
	auto monitorWidth = monitor.right - monitor.left;
	auto monitorHeight = monitor.bottom - monitor.top;
	auto width = rect.right - rect.left;
	auto height = rect.bottom - rect.top;
	if (width <= 0 || height <= 0) return;
	qCDebug(logRules) << "Rule" << static_cast<int>(kind) << "for" << titleOf(hwnd);

	if (kind == Kind::Dialog || kind == Kind::FileDialog) {
		auto resize = kind == Kind::FileDialog;
		if (resize) {
			width = std::lround(monitorWidth * FILE_DIALOG_WIDTH);
			height = std::lround(monitorHeight * FILE_DIALOG_HEIGHT);
		}
		width = std::min(width, work.right - work.left);
		height = std::min(height, work.bottom - work.top);
		auto x = work.left + (work.right - work.left - width) / 2;
		auto y = work.top + (work.bottom - work.top - height) / 2;
		place(hwnd, x, y, width, height, resize);
		return;
	}

	if (kind == Kind::PictureInPicture) {
		auto scaled = std::lround(monitorWidth * PICTURE_SIZE);
		height = MulDiv(height, static_cast<int>(scaled), width);
		width = scaled;
		auto x = monitor.left + std::lround(monitorWidth * PICTURE_X);
		auto y = monitor.top + std::lround(monitorHeight * PICTURE_Y);
		x = std::max(work.left, std::min<LONG>(x, work.right - width));
		y = std::max(work.top, std::min<LONG>(y, work.bottom - height));
		place(hwnd, x, y, width, height, true);
	} else {
		auto x = monitor.left + (monitorWidth - width) / 2;
		auto y = std::min<LONG>(monitor.bottom - height - SHARING_MARGIN, work.bottom - height);
		place(hwnd, x, y, width, height, false);
	}
	if (!VirtualDesktops::instance()->pinWindow(hwnd, true)) {
		qCDebug(logRules) << "Could not pin" << titleOf(hwnd);
	}
}

}
