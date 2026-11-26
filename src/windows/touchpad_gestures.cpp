#include "touchpad_gestures.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>

#include "desktop_slide.hpp"
#include "system_swipe.hpp"
#include "tiling.hpp"
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

TouchpadGestures::~TouchpadGestures() {
	this->setSwipeEnabled(false);
	this->setMoveEnabled(false);
}

void TouchpadGestures::setSwipeEnabled(bool enabled) {
	if (enabled == this->mSwipeEnabled) return;
	this->mSwipeEnabled = enabled;
	this->updateRequested();
	emit this->swipeEnabledChanged();
}

void TouchpadGestures::setMoveEnabled(bool enabled) {
	if (enabled == this->mMoveEnabled) return;
	this->mMoveEnabled = enabled;
	this->updateRequested();
	emit this->moveEnabledChanged();
}

void TouchpadGestures::updateRequested() {
	auto fingers = (this->mSwipeEnabled ? SystemSwipe::FourFingers : 0)
	             | (this->mMoveEnabled ? SystemSwipe::ThreeFingers : 0);
	if (fingers != 0 && gSwiping != this) {
		if (auto* other = gSwiping.data()) {
			other->setSwipeEnabled(false);
			other->setMoveEnabled(false);
		}
		gSwiping = this;
	} else if (fingers == 0 && gSwiping == this) {
		gSwiping = nullptr;
	}
	this->systemSwipe.setRequested(fingers);
}

void TouchpadGestures::onSystemSwipeChanged() {
	auto fingers = this->systemSwipe.suppressed();
	this->swipe.setEnabled(
	    (fingers & SystemSwipe::FourFingers) != 0,
	    (fingers & SystemSwipe::ThreeFingers) != 0
	);
	if (fingers != 0) {
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
	} else if (step.action == TouchpadSwipe::Action::Move) {
		if (this->moveTarget == MoveTarget::None) {
			auto* tiling = TilingManager::active();
			const auto horizontal = this->swipe.movedHorizontally() && tiling != nullptr && tiling->scrollingHere();
			if (!horizontal) this->moveTarget = MoveTarget::Window;
			else this->moveTarget = tiling->beginGestureScroll() ? MoveTarget::Tape : MoveTarget::Ignored;
			this->tapeHistory.clear();
			this->tapeDx = 0;
			this->tapeRaw = 0;
		}
		if (this->moveTarget == MoveTarget::Tape) this->scrollTape(step.dx, timeMs);
		else if (this->moveTarget == MoveTarget::Window) this->move.update(step.dx, step.dy);
	} else if (step.action == TouchpadSwipe::Action::MoveEnd) {
		const auto target = std::exchange(this->moveTarget, MoveTarget::None);
		if (target == MoveTarget::Tape) {
			this->pushTape(0, timeMs);
			TilingManager::instance()->endGestureScroll(this->tapeVelocity(), step.cancelled);
		}
		else if (target == MoveTarget::Window) this->move.end(step.dx, step.dy, step.cancelled);
	} else if (step.action == TouchpadSwipe::Action::Pinch && !step.cancelled) {
		emit this->pinched();
	}
	if (this->swipe.swiping() || this->swipe.moving()) this->watchdog.start();
	else this->watchdog.stop();
}

void TouchpadGestures::scrollTape(double dx, double timeMs) {
	constexpr double BASELINE = 0.9;
	constexpr double THRESHOLD = 254;
	constexpr double INCLINE = 0.011;
	constexpr double MAX_ACCEL = 9;
	constexpr double SLOW_LIMIT = 7;
	const auto raw = dx - this->tapeRaw;
	this->tapeRaw = dx;
	auto moved = raw;
	if (!this->tapeHistory.isEmpty() && timeMs > this->tapeHistory.last().second) {
		const auto seconds = (timeMs - this->tapeHistory.last().second) / 1000.0;
		const auto speed = std::abs(raw) / TouchpadSwipe::unitsPerMm() / seconds;
		auto factor = BASELINE;
		if (speed < SLOW_LIMIT) factor = std::min(BASELINE, 0.1 * speed + 0.3);
		else if (speed >= THRESHOLD) factor = INCLINE * (speed - THRESHOLD) + BASELINE;
		moved = raw * std::min(MAX_ACCEL, factor) / BASELINE;
	}
	this->pushTape(moved, timeMs);
	TilingManager::instance()->gestureScroll(this->tapeDx);
}

void TouchpadGestures::pushTape(double delta, double timeMs) {
	constexpr double HISTORY_MS = 150;
	if (!this->tapeHistory.isEmpty() && timeMs < this->tapeHistory.last().second) return;
	this->tapeHistory.append({delta, timeMs});
	this->tapeDx += delta;
	while (!this->tapeHistory.isEmpty() && timeMs > this->tapeHistory.first().second + HISTORY_MS)
		this->tapeHistory.removeFirst();
}

double TouchpadGestures::tapeVelocity() const {
	if (this->tapeHistory.size() < 2) return 0;
	const auto seconds = (this->tapeHistory.last().second - this->tapeHistory.first().second) / 1000.0;
	if (seconds <= 0) return 0;
	auto total = 0.0;
	for (const auto& [delta, time]: this->tapeHistory) total += delta;
	return total / seconds;
}

void TouchpadGestures::cancelSwipe() {
	this->watchdog.stop();
	auto swiping = this->swipe.swiping();
	if (this->swipe.moving()) {
		const auto target = std::exchange(this->moveTarget, MoveTarget::None);
		if (target == MoveTarget::Tape) TilingManager::instance()->endGestureScroll(0, true);
		else if (target == MoveTarget::Window) this->move.end(0, 0, true);
	}
	this->swipe.reset();
	if (!swiping) return;
	qCDebug(logGestures) << "Swipe cancelled";
	DesktopSlide::instance()->swipeUpdate(0);
	DesktopSlide::instance()->swipeEnd(SwipeRelease::Cancel);
}

void TouchpadGestures::simulateSwipe(double distance, int durationMs) {
	qCDebug(logGestures) << "Simulating a swipe of" << distance << "desktops in" << durationMs << "ms";
	if (this->simulation.isActive() || this->swipe.swiping()) return;
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
