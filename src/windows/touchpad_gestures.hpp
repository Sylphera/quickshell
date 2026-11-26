#pragma once

#include <qelapsedtimer.h>
#include <qlist.h>
#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

#include "system_swipe.hpp"
#include "touchpad_move.hpp"
#include "touchpad_reader.hpp"
#include "touchpad_swipe.hpp"

namespace qs::windows {

class TouchpadGestures: public QObject {
	Q_OBJECT;
	Q_PROPERTY(bool swipeEnabled READ swipeEnabled WRITE setSwipeEnabled NOTIFY swipeEnabledChanged);
	Q_PROPERTY(bool moveEnabled READ moveEnabled WRITE setMoveEnabled NOTIFY moveEnabledChanged);
	QML_ELEMENT;
	QML_SINGLETON;

public:
	explicit TouchpadGestures(QObject* parent = nullptr);
	~TouchpadGestures() override;
	Q_DISABLE_COPY_MOVE(TouchpadGestures);

	[[nodiscard]] bool swipeEnabled() const { return this->mSwipeEnabled; }
	void setSwipeEnabled(bool enabled);
	[[nodiscard]] bool moveEnabled() const { return this->mMoveEnabled; }
	void setMoveEnabled(bool enabled);

	Q_INVOKABLE void simulateSwipe(double distance, int durationMs);

signals:
	void swipeEnabledChanged();
	void moveEnabledChanged();
	void verticalSwiped();
	void pinched();

private:
	void step();
	void onFrame(const QList<TouchpadContact>& contacts, double timeMs);
	void scrollTape(double dx, double timeMs);
	void pushTape(double delta, double timeMs);
	[[nodiscard]] double tapeVelocity() const;
	void cancelSwipe();
	void onSystemSwipeChanged();
	void updateRequested();

	SystemSwipe systemSwipe;
	TouchpadReader reader;
	TouchpadSwipe swipe;
	TouchpadMove move;
	enum class MoveTarget : quint8 { None, Window, Tape, Ignored };
	MoveTarget moveTarget = MoveTarget::None;
	double tapeDx = 0;
	double tapeRaw = 0;
	QList<QPair<double, double>> tapeHistory;
	QTimer watchdog;
	bool mSwipeEnabled = false;
	bool mMoveEnabled = false;

	QTimer simulation;
	QElapsedTimer clock;
	double distance = 0;
	double durationMs = 0;
};

}
