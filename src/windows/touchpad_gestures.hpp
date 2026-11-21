#pragma once

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

class TouchpadGestures: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit TouchpadGestures(QObject* parent = nullptr);

	Q_INVOKABLE void simulateSwipe(double distance, int durationMs);

private:
	void step();

	QTimer simulation;
	QElapsedTimer clock;
	double distance = 0;
	double durationMs = 0;
};

}
