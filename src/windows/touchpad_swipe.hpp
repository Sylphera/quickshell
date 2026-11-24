#pragma once

#include <functional>

#include <qlist.h>
#include <qtypes.h>

namespace qs::windows {

struct TouchpadContact {
	quint32 id = 0;
	double x = 0;
	double y = 0;
};

[[nodiscard]] qsizetype swipeDestination(
    qsizetype from,
    int way,
    qsizetype count,
    const std::function<bool(qsizetype)>& occupied
);

class TouchpadSwipe {
public:
	enum class Action : quint8 {
		None,
		Update,
		End,
	};

	struct Step {
		Action action = Action::None;
		double offset = 0;
		bool forced = false;
		bool cancelled = false;
	};

	explicit TouchpadSwipe(std::function<bool(int)> canGo);

	Step frame(const QList<TouchpadContact>& contacts, double timeMs);
	void reset();

	[[nodiscard]] bool active() const { return this->state == State::Horizontal; }

private:
	enum class State : quint8 {
		Waiting,
		Horizontal,
		Ignored,
	};

	[[nodiscard]] Step finish() const;
	[[nodiscard]] bool fingersSwitched(qsizetype count, double timeMs);
	void advance(double moved);
	void trackReturn();

	std::function<bool(int)> canGo;
	QList<TouchpadContact> previous;
	State state = State::Waiting;
	qsizetype fingers = 0;
	qsizetype pendingFingers = -1;
	double pendingSince = 0;
	double totalX = 0;
	double totalY = 0;
	double delta = 0;
	int direction = 0;
	double turningPoint = 0;
	bool returning = false;
	double averageSpeed = 0;
	int speedPoints = 0;
};

}
