#include <qobject.h>
#include <qrect.h>
#include <qtest.h>
#include <qtestcase.h>

#include "window_snap.hpp"

using qs::windows::SnapArea;
using qs::windows::snapMoved;
using qs::windows::snapResized;

namespace {

SnapArea emptyArea() {
	return {
	    .windows = {},
	    .work = QRect(0, 40, 1920, 1040),
	    .reserved = QMargins(0, 40, 0, 0),
	    .scale = 1,
	    .gapsIn = 4,
	    .gapsOut = 5,
	};
}

SnapArea areaWithWindow() {
	auto area = emptyArea();
	area.windows = {QRect(500, 300, 400, 300)};
	return area;
}

}

class TestWindowSnap: public QObject {
	Q_OBJECT;

private slots:
	void farFromEverythingStaysWhereItIs() {
		auto window = QRect(100, 700, 300, 200);
		QCOMPARE(snapMoved(window, areaWithWindow()), window);
	}

	void snapsToTheMonitorWithItsGap() {
		QCOMPARE(snapMoved(QRect(8, 400, 300, 200), emptyArea()), QRect(5, 400, 300, 200));
		QCOMPARE(snapMoved(QRect(1612, 400, 300, 200), emptyArea()), QRect(1615, 400, 300, 200));
		QCOMPARE(snapMoved(QRect(100, 872, 300, 200), emptyArea()), QRect(100, 875, 300, 200));
	}

	void snapsUnderTheBarAndToTheScreenItself() {
		QCOMPARE(snapMoved(QRect(100, 47, 300, 200), emptyArea()), QRect(100, 45, 300, 200));
		QCOMPARE(snapMoved(QRect(100, 3, 300, 200), emptyArea()), QRect(100, 5, 300, 200));
	}

	void fivePixelsAreTooFar() {
		QCOMPARE(snapMoved(QRect(10, 400, 300, 200), emptyArea()), QRect(10, 400, 300, 200));
	}

	void snapsBesideAWindowWithBothGaps() {
		QCOMPARE(snapMoved(QRect(911, 400, 300, 100), areaWithWindow()), QRect(908, 400, 300, 100));
		QCOMPARE(snapMoved(QRect(190, 400, 300, 100), areaWithWindow()), QRect(192, 400, 300, 100));
	}

	void snapsBelowAWindow() {
		QCOMPARE(snapMoved(QRect(550, 610, 300, 100), areaWithWindow()), QRect(550, 608, 300, 100));
	}

	void alignsTheCornerOnceBeside() {
		QCOMPARE(snapMoved(QRect(910, 302, 300, 100), areaWithWindow()), QRect(908, 300, 300, 100));
		QCOMPARE(snapMoved(QRect(910, 498, 300, 100), areaWithWindow()), QRect(908, 500, 300, 100));
	}

	void windowsThatDoNotOverlapAreIgnored() {
		auto window = QRect(911, 700, 300, 100);
		QCOMPARE(snapMoved(window, areaWithWindow()), window);
	}

	void resizingOnlyMovesTheDraggedEdge() {
		auto edges = qs::windows::SnapEdges {.left = false, .right = true, .top = false, .bottom = true};
		QCOMPARE(
		    snapResized(QRect(100, 350, 390, 100), areaWithWindow(), edges),
		    QRect(100, 350, 392, 100)
		);
		QCOMPARE(snapResized(QRect(8, 400, 300, 200), emptyArea(), edges), QRect(8, 400, 300, 200));
	}

	void distancesFollowTheScale() {
		auto area = emptyArea();
		area.scale = 2;
		QCOMPARE(snapMoved(QRect(18, 400, 300, 200), area), QRect(10, 400, 300, 200));
	}
};

QTEST_APPLESS_MAIN(TestWindowSnap);

#include "window_snap_test.moc"
