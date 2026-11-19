#pragma once

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

// end-4's touchpad gestures. A four-finger horizontal swipe switches desktops,
// with the slide following the fingers (Hyprland's workspace swipe).
class TouchpadGestures: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit TouchpadGestures(QObject* parent = nullptr);

	// Plays a four-finger horizontal swipe as a touchpad would: `distance` in
	// desktop widths (positive toward the next desktop) over `durationMs`, then
	// lets go. For trying the gesture without a touchpad.
	Q_INVOKABLE void simulateSwipe(double distance, int durationMs);

private:
	void step();

	QTimer simulation;
	QElapsedTimer clock;
	double distance = 0;
	double durationMs = 0;
};

} // namespace qs::windows
