#include <qlist.h>
#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>
#include <qtypes.h>

#include "touchpad_swipe.hpp"

using qs::windows::swipeDestination;
using qs::windows::TouchpadContact;
using qs::windows::TouchpadSwipe;
using Action = TouchpadSwipe::Action;

namespace {

constexpr double FRAME_MS = 7.2;

struct Hand {
	TouchpadSwipe swipe;
	double x = 60;
	double y = 30;
	double time = 0;
	TouchpadSwipe::Step last;
	TouchpadSwipe::Step end;
	int ends = 0;

	explicit Hand(bool left = true, bool right = true)
	    : swipe([left, right](int way) { return way < 0 ? left : right; }) {}

	void frame(int fingers) {
		auto contacts = QList<TouchpadContact>();
		for (auto i = 0; i < fingers; i++) {
			contacts.append({.id = static_cast<quint32>(i), .x = this->x + i * 15, .y = this->y});
		}
		this->time += FRAME_MS;
		this->last = this->swipe.frame(contacts, this->time);
		if (this->last.action != Action::End) return;
		this->end = this->last;
		this->ends++;
	}

	void move(double dx, double dy, int frames, int fingers = 4) {
		for (auto i = 0; i < frames; i++) {
			this->x += dx / frames;
			this->y += dy / frames;
			this->frame(fingers);
		}
	}

	void lift() { this->frame(0); }
};

}

class TestTouchpadSwipe: public QObject {
	Q_OBJECT;

private slots:
	void fingersLeftGoToTheNextDesktop() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-33.3, 0, 100);
		QCOMPARE(hand.last.action, Action::Update);
		QVERIFY(hand.last.offset > 0.45 && hand.last.offset < 0.51);
		hand.lift();
		QCOMPARE(hand.ends, 1);
		QVERIFY(hand.end.offset > 0.45);
	}

	void slowShortSwipeIsNotForced() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(8, 0, 60);
		hand.lift();
		QCOMPARE(hand.ends, 1);
		QVERIFY(!hand.end.forced);
		QVERIFY(hand.end.offset < 0 && hand.end.offset > -0.2);
	}

	void fastShortSwipeIsForced() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-8, 0, 8);
		hand.lift();
		QVERIFY(hand.end.forced);
		QVERIFY(hand.end.offset < 0.2);
	}

	void turningBackStopsAtTheOrigin() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-20, 0, 60);
		hand.move(50, 0, 60);
		QCOMPARE(hand.last.offset, 0.0);
		hand.lift();
		QCOMPARE(hand.end.offset, 0.0);
		QVERIFY(!hand.end.forced);
	}

	void turningBackAgainResumes() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-20, 0, 60);
		hand.move(50, 0, 60);
		hand.move(-20, 0, 60);
		QVERIFY(hand.last.offset > 0.25);
	}

	void fastReturnIsCancelled() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-30, 0, 20);
		hand.move(20, 0, 15);
		hand.lift();
		QVERIFY(hand.end.offset > 0.1);
		QVERIFY(hand.end.cancelled);
	}

	void pausingAfterAReturnStaysCancelled() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(40, 0, 40);
		hand.move(-15, 0, 20);
		hand.move(0, 0, 40);
		hand.lift();
		QVERIFY(hand.end.offset < -0.2);
		QVERIFY(hand.end.cancelled);
	}

	void goingForwardAfterAReturnIsNotCancelled() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-30, 0, 20);
		hand.move(20, 0, 15);
		hand.move(-15, 0, 15);
		hand.lift();
		QVERIFY(!hand.end.cancelled);
	}

	void aSlightReturnWhileLiftingIsNotCancelled() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-30, 0, 20);
		hand.move(2, 0, 4);
		hand.lift();
		QVERIFY(!hand.end.cancelled);
	}

	void theEdgeDoesNotLockTheDirection() {
		auto hand = Hand(false, true);
		hand.frame(4);
		hand.move(20, 0, 60);
		QCOMPARE(hand.last.offset, 0.0);
		hand.move(-20, 0, 60);
		QVERIFY(hand.last.offset > 0.25);
	}

	void verticalIsIgnoredUntilTheFingersLift() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(0, 10, 20);
		hand.move(-30, 0, 20);
		hand.lift();
		QCOMPARE(hand.ends, 0);
		hand.frame(4);
		hand.move(-30, 0, 20);
		hand.lift();
		QCOMPARE(hand.ends, 1);
	}

	void onlyFourFingersSwipe() {
		for (auto fingers: {1, 2, 3, 5}) {
			auto hand = Hand();
			hand.frame(fingers);
			hand.move(-30, 0, 30, fingers);
			hand.lift();
			QCOMPARE(hand.ends, 0);
		}
	}

	void aBriefLostFingerKeepsTheSwipe() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-15, 0, 30);
		hand.move(-5, 0, 10, 3);
		QCOMPARE(hand.ends, 0);
		hand.move(-10, 0, 20);
		QVERIFY(hand.last.offset > 0.4);
		hand.lift();
		QCOMPARE(hand.ends, 1);
	}

	void aLastingLostFingerEndsTheSwipe() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-15, 0, 30);
		hand.move(0, 0, 20, 3);
		QCOMPARE(hand.ends, 1);
		hand.lift();
		QCOMPARE(hand.ends, 1);
	}

	void fingersLandingOneByOneDoNotMove() {
		auto hand = Hand();
		hand.move(-10, 0, 10, 1);
		hand.move(-10, 0, 10, 2);
		hand.move(-10, 0, 10, 3);
		hand.frame(4);
		QCOMPARE(hand.ends, 0);
		hand.move(-20, 0, 40);
		QVERIFY(hand.last.offset < 0.31);
	}

	void swipingSkipsEmptyDesktops() {
		auto occupied = [](qsizetype desktop) { return desktop == 0 || desktop == 3; };
		QCOMPARE(swipeDestination(0, 1, 4, occupied), 3);
		QCOMPARE(swipeDestination(3, -1, 4, occupied), 0);
		QCOMPARE(swipeDestination(1, 1, 4, occupied), 3);
		QCOMPARE(swipeDestination(2, -1, 4, occupied), 0);
	}

	void swipingPastTheLastOccupiedDesktopGoesToANewOne() {
		auto occupied = [](qsizetype desktop) { return desktop <= 2; };
		QCOMPARE(swipeDestination(2, 1, 3, occupied), 3);
		QCOMPARE(swipeDestination(2, 1, 10, occupied), 3);
	}

	void swipingPastAnEmptyDesktopGoesNowhere() {
		auto occupied = [](qsizetype desktop) { return desktop <= 2; };
		QCOMPARE(swipeDestination(3, 1, 4, occupied), -1);
		QCOMPARE(swipeDestination(3, 1, 10, occupied), -1);
		QCOMPARE(swipeDestination(3, -1, 4, occupied), 2);
	}

	void swipingBeforeTheFirstDesktopGoesNowhere() {
		QCOMPARE(swipeDestination(0, -1, 3, [](qsizetype) { return true; }), -1);
		QCOMPARE(swipeDestination(2, -1, 3, [](qsizetype desktop) { return desktop == 2; }), -1);
	}

	void theOffsetStopsAtOneDesktop() {
		auto hand = Hand();
		hand.frame(4);
		hand.move(-100, 0, 80);
		QCOMPARE(hand.last.offset, 1.0);
	}
};

QTEST_APPLESS_MAIN(TestTouchpadSwipe);

#include "touchpad_swipe_test.moc"
