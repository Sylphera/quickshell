#pragma once

#include <qelapsedtimer.h>
#include <qpointer.h>
#include <qt_windows.h>

namespace qs::windows {

class TrackedWindow;

class TouchpadMove {
public:
	void update(double dx, double dy);
	void end(double dx, double dy, bool cancelled);

private:
	bool begin();
	void moveTo(double dx, double dy, bool force);

	bool started = false;
	bool tiled = false;
	QPointer<TrackedWindow> window;
	double scale = 1;
	POINT origin {};
	POINT sent {};
	QElapsedTimer sentAt;
};

}
