#pragma once

#include <qelapsedtimer.h>
#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

#include "system_swipe.hpp"
#include "touchpad_reader.hpp"
#include "touchpad_swipe.hpp"

namespace qs::windows {

class TouchpadGestures: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool swipeEnabled READ swipeEnabled WRITE setSwipeEnabled NOTIFY swipeEnabledChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit TouchpadGestures(QObject* parent = nullptr);
	~TouchpadGestures() override;
	Q_DISABLE_COPY_MOVE(TouchpadGestures);

	[[nodiscard]] bool swipeEnabled() const { return this->mSwipeEnabled; }
	void setSwipeEnabled(bool enabled);

	Q_INVOKABLE void simulateSwipe(double distance, int durationMs);

signals:
	void swipeEnabledChanged();

private:
	void step();
	void onFrame(const QList<TouchpadContact>& contacts, double timeMs);
	void cancelSwipe();
	void onSystemSwipeChanged();

	SystemSwipe systemSwipe;
	TouchpadReader reader;
	TouchpadSwipe swipe;
	QTimer watchdog;
	bool mSwipeEnabled = false;

	QTimer simulation;
	QElapsedTimer clock;
	double distance = 0;
	double durationMs = 0;
};

}
