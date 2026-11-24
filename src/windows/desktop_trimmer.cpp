#include "desktop_trimmer.hpp"
#include <algorithm>
#include <cwchar>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qelapsedtimer.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qtimer.h>
#include <qtypes.h>

#include "desktop_slide.hpp"
#include "virtual_desktops.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logTrimmer, "quickshell.windows.trimmer", QtWarningMsg);

constexpr int SETTLE_MS = 1000;
constexpr int BUSY_RETRY_MS = 250;
constexpr int NEXT_MS = 100;
constexpr qint64 STARTUP_GRACE_MS = 15000;
constexpr int CLASS_LENGTH = 64;

bool taskViewOpen() {
	wchar_t name[CLASS_LENGTH] {};
	GetClassNameW(GetForegroundWindow(), name, CLASS_LENGTH);
	return wcscmp(name, L"XamlExplorerHostIslandWindow") == 0;
}

struct OccupiedSearch {
	const VirtualDesktops* desktops = nullptr;
	qsizetype last = -1;
};

BOOL CALLBACK findOccupied(HWND hwnd, LPARAM param) {
	if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
	if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return TRUE;
	auto* search = reinterpret_cast<OccupiedSearch*>(param);
	auto index = search->desktops->indexOf(search->desktops->windowDesktopId(hwnd));
	search->last = std::max(search->last, index);
	return TRUE;
}

}

DesktopTrimmer* DesktopTrimmer::instance() {
	static QPointer<DesktopTrimmer> trimmer; // NOLINT
	if (trimmer.isNull()) trimmer = new DesktopTrimmer(QCoreApplication::instance());
	return trimmer.data();
}

DesktopTrimmer::DesktopTrimmer(QObject* parent)
    : QObject(parent)
    , desktops(VirtualDesktops::instance()) {
	this->uptime.start();
	this->timer.setSingleShot(true);
	QObject::connect(&this->timer, &QTimer::timeout, this, &DesktopTrimmer::trim);
}

void DesktopTrimmer::setEnabled(bool enabled) {
	if (enabled == this->mEnabled) return;
	this->mEnabled = enabled;
	if (!enabled) this->timer.stop();

	if (enabled && !this->watching) {
		this->watching = true;
		auto changed = &DesktopTrimmer::schedule;
		QObject::connect(this->desktops, &VirtualDesktops::desktopsChanged, this, changed);
		QObject::connect(this->desktops, &VirtualDesktops::currentChanged, this, changed);
		QObject::connect(this->desktops, &VirtualDesktops::accessorReady, this, changed);
		auto* tracker = WindowTracker::instance();
		QObject::connect(tracker, &WindowTracker::windowRemoved, this, changed);
		auto watch = [this](TrackedWindow* window) {
			QObject::connect(window, &TrackedWindow::desktopChanged, this, &DesktopTrimmer::schedule);
		};
		QObject::connect(tracker, &WindowTracker::windowAdded, this, watch);
		for (auto* window: tracker->windows()) watch(window);
	}
	this->schedule();
	emit this->enabledChanged();
}

void DesktopTrimmer::schedule() {
	if (!this->mEnabled) return;
	auto grace = STARTUP_GRACE_MS - this->uptime.elapsed();
	this->timer.start(static_cast<int>(std::max<qint64>(SETTLE_MS, grace)));
}

bool DesktopTrimmer::busy() const {
	return DesktopSlide::instance()->busy() || this->desktops->creating()
	    || this->desktops->shownIndex() != this->desktops->currentIndex()
	    || this->desktops->nativeCurrentIndex() != this->desktops->currentIndex() || taskViewOpen();
}

qsizetype DesktopTrimmer::lastOccupied() const {
	auto search = OccupiedSearch {.desktops = this->desktops};
	EnumWindows(&findOccupied, reinterpret_cast<LPARAM>(&search));
	for (auto* window: WindowTracker::instance()->windows()) {
		search.last = std::max(search.last, static_cast<qsizetype>(window->desktop()));
		auto known = IsEqualGUID(this->desktops->windowDesktopId(window->hwnd()), GUID {}) == FALSE;
		if (known || this->desktops->isWindowPinned(window->hwnd())) continue;
		qCDebug(logTrimmer) << "Keeping every desktop, unknown desktop for" << window->title();
		return this->desktops->count() - 1;
	}
	return search.last;
}

void DesktopTrimmer::trim() {
	if (!this->mEnabled || !this->desktops->accessorLoaded()) return;
	this->desktops->refresh();
	if (this->busy()) {
		this->timer.start(BUSY_RETRY_MS);
		return;
	}

	auto last = this->desktops->count() - 1;
	auto current = this->desktops->currentIndex();
	if (last <= std::max(current, this->lastOccupied())) {
		this->timer.stop();
		return;
	}

	QElapsedTimer spent;
	spent.start();
	if (!this->desktops->removeDesktop(last, current)) {
		this->timer.stop();
		return;
	}
	qCDebug(logTrimmer) << "Removed empty desktop" << last << "in" << spent.elapsed() << "ms";
	this->timer.start(NEXT_MS);
}

}
