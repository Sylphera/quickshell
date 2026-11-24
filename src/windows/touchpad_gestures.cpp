#include "touchpad_gestures.hpp"
#include <algorithm>

#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>

#include "desktop_slide.hpp"
#include "system_swipe.hpp"
#include "touchpad_reader.hpp"
#include "touchpad_swipe.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logGestures, "quickshell.windows.gestures", QtWarningMsg);
constexpr int SIMULATION_STEP_MS = 8;
constexpr int SWIPE_WATCHDOG_MS = 300;
QPointer<TouchpadGestures> gSwiping; // NOLINT
}

TouchpadGestures::TouchpadGestures(QObject* parent)
    : QObject(parent)
    , swipe([](int way) { return DesktopSlide::instance()->swipeCanGo(way); }) {
	this->simulation.setInterval(SIMULATION_STEP_MS);
	QObject::connect(&this->simulation, &QTimer::timeout, this, &TouchpadGestures::step);

	this->watchdog.setSingleShot(true);
	this->watchdog.setInterval(SWIPE_WATCHDOG_MS);
	QObject::connect(&this->watchdog, &QTimer::timeout, this, &TouchpadGestures::cancelSwipe);
	QObject::connect(&this->reader, &TouchpadReader::frame, this, &TouchpadGestures::onFrame);
	QObject::connect(
	    &this->systemSwipe,
	    &SystemSwipe::suppressedChanged,
	    this,
	    &TouchpadGestures::onSystemSwipeChanged
	);
}

TouchpadGestures::~TouchpadGestures() { this->setSwipeEnabled(false); }

void TouchpadGestures::setSwipeEnabled(bool enabled) {
	if (enabled == this->mSwipeEnabled) return;
	if (enabled) {
		if (gSwiping != nullptr) gSwiping->setSwipeEnabled(false);
		gSwiping = this;
	} else if (gSwiping == this) {
		gSwiping = nullptr;
	}
	this->mSwipeEnabled = enabled;
	this->systemSwipe.setSuppressRequested(enabled);
	emit this->swipeEnabledChanged();
}

void TouchpadGestures::onSystemSwipeChanged() {
	if (this->systemSwipe.suppressed()) {
		this->reader.start();
	} else {
		this->reader.stop();
		this->cancelSwipe();
	}
}

void TouchpadGestures::onFrame(const QList<TouchpadContact>& contacts, double timeMs) {
	if (this->simulation.isActive()) return;
	auto step = this->swipe.frame(contacts, timeMs);
	if (step.action == TouchpadSwipe::Action::Update) {
		DesktopSlide::instance()->swipeUpdate(step.offset);
	} else if (step.action == TouchpadSwipe::Action::End) {
		qCDebug(logGestures) << "Swipe let go at" << step.offset << "forced" << step.forced << "cancelled"
		                     << step.cancelled;
		auto release = step.forced ? SwipeRelease::Force : SwipeRelease::Settle;
		if (step.cancelled) release = SwipeRelease::Cancel;
		DesktopSlide::instance()->swipeEnd(release);
	} else if (step.action == TouchpadSwipe::Action::Vertical) {
		emit this->verticalSwiped();
	}
	if (this->swipe.active()) this->watchdog.start();
	else this->watchdog.stop();
}

void TouchpadGestures::cancelSwipe() {
	this->watchdog.stop();
	auto swiping = this->swipe.active();
	this->swipe.reset();
	if (!swiping) return;
	qCDebug(logGestures) << "Swipe cancelled";
	DesktopSlide::instance()->swipeUpdate(0);
	DesktopSlide::instance()->swipeEnd(SwipeRelease::Cancel);
}

void TouchpadGestures::simulateSwipe(double distance, int durationMs) {
	qCDebug(logGestures) << "Simulating a swipe of" << distance << "desktops in" << durationMs << "ms";
	if (this->simulation.isActive() || this->swipe.active()) return;
	this->distance = distance;
	this->durationMs = std::max(1, durationMs);
	this->clock.start();
	this->simulation.start();
}

void TouchpadGestures::step() {
	auto progress = std::clamp(static_cast<double>(this->clock.elapsed()) / this->durationMs, 0.0, 1.0);
	auto eased = progress * progress * (3 - 2 * progress);
	DesktopSlide::instance()->swipeUpdate(this->distance * eased);
	if (progress < 1.0) return;

	this->simulation.stop();
	DesktopSlide::instance()->swipeEnd(SwipeRelease::Settle);
}

}
