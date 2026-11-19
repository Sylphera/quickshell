#include "touchpad_gestures.hpp"
#include <algorithm>

#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>

#include "desktop_slide.hpp"

namespace qs::windows {

namespace {
Q_LOGGING_CATEGORY(logGestures, "quickshell.windows.gestures", QtWarningMsg);
// About a touchpad's report rate.
constexpr int SIMULATION_STEP_MS = 8;
} // namespace

TouchpadGestures::TouchpadGestures(QObject* parent): QObject(parent) {
	this->simulation.setInterval(SIMULATION_STEP_MS);
	QObject::connect(&this->simulation, &QTimer::timeout, this, &TouchpadGestures::step);
}

void TouchpadGestures::simulateSwipe(double distance, int durationMs) {
	qCDebug(logGestures) << "Simulating a swipe of" << distance << "desktops in" << durationMs << "ms";
	if (this->simulation.isActive()) return;
	this->distance = distance;
	this->durationMs = std::max(1, durationMs);
	this->clock.start();
	this->simulation.start();
}

void TouchpadGestures::step() {
	auto progress = std::clamp(static_cast<double>(this->clock.elapsed()) / this->durationMs, 0.0, 1.0);
	// Fingers ease in and out like a hand does.
	auto eased = progress * progress * (3 - 2 * progress);
	DesktopSlide::instance()->swipeUpdate(this->distance * eased);
	if (progress < 1.0) return;

	this->simulation.stop();
	// Letting go at the end of a gentle stroke: no flick, so distance decides.
	DesktopSlide::instance()->swipeEnd(0);
}

} // namespace qs::windows
