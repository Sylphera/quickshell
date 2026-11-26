#pragma once

#include <cmath>
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
	[[nodiscard]] static double unitsPerMm();
	[[nodiscard]] bool movedHorizontally() const { return std::abs(this->totalX) > std::abs(this->totalY); }

	enum class Action : quint8 {
		None,
		Update,
		End,
		Vertical,
		Move,
		MoveEnd,
		Pinch,
	};

	struct Step {
		Action action = Action::None;
		double offset = 0;
		double dx = 0;
		double dy = 0;
		bool forced = false;
		bool cancelled = false;
	};

	explicit TouchpadSwipe(std::function<bool(int)> canGo);

	Step frame(const QList<TouchpadContact>& contacts, double timeMs);
	void reset();
	void setEnabled(bool fourFingers, bool threeFingers);

	[[nodiscard]] bool swiping() const { return this->state == State::Horizontal; }
	[[nodiscard]] bool moving() const { return this->state == State::Moving; }

private:
	enum class State : quint8 {
		Waiting,
		Horizontal,
		Vertical,
		Moving,
		Pinching,
	};

	[[nodiscard]] Step finish() const;
	[[nodiscard]] bool enabledFor(qsizetype count) const;
	[[nodiscard]] State startMoving() const;
	void trackPinch();
	[[nodiscard]] bool fingersSwitched(qsizetype count, double timeMs);
	void advance(double moved);
	void trackReturn();

	std::function<bool(int)> canGo;
	QList<TouchpadContact> previous;
	State state = State::Waiting;
	qsizetype fingers = 0;
	bool fourFingers = true;
	bool threeFingers = true;
	bool spent = false;
	double startSpread = 0;
	bool opening = false;
	double pinchProgress = 0;
	double moveX = 0;
	double moveY = 0;
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
