#include <qlist.h>
#include <qobject.h>
#include <qregion.h>
#include <qtest.h>
#include <qtestcase.h>

#include "dim_plan.hpp"

using qs::windows::dimCurve;
using qs::windows::planDim;

class TestDimPlan: public QObject {
	Q_OBJECT;

private slots:
	void onlyTheUnfocusedAreDimmed() {
		auto areas = planDim({{.id = 1, .rect = {0, 0, 10, 10}}, {.id = 2, .rect = {20, 0, 10, 10}}}, 2);
		QCOMPARE(areas.size(), 2);
		QVERIFY(areas[0].dimmed);
		QVERIFY(!areas[1].dimmed);
	}

	void aSingleFocusedWindowIsNot() {
		auto areas = planDim({{.id = 1, .rect = {0, 0, 10, 10}}}, 1);
		QCOMPARE(areas.size(), 1);
		QVERIFY(!areas[0].dimmed);
	}

	void nothingIsDimmedWithoutFocus() {
		for (const auto& area: planDim({{.id = 1, .rect = {0, 0, 10, 10}}}, 7)) QVERIFY(!area.dimmed);
	}

	void theWindowAboveCutsTheOneBelow() {
		auto areas = planDim({{.id = 1, .rect = {0, 0, 10, 10}}, {.id = 2, .rect = {0, 0, 30, 10}}}, 1);
		QCOMPARE(areas[1].region, QRegion(10, 0, 20, 10));
		QVERIFY(areas[1].dimmed);
	}

	void anUnfocusedFloatOverTheFocusedIsDimmedWhole() {
		auto areas = planDim({{.id = 1, .rect = {5, 5, 4, 4}}, {.id = 2, .rect = {0, 0, 30, 10}}}, 2);
		QCOMPARE(areas[0].region, QRegion(5, 5, 4, 4));
		QVERIFY(areas[0].dimmed);
	}

	void aFullyCoveredWindowIsLeftOut() {
		auto areas = planDim({{.id = 1, .rect = {0, 0, 99, 99}}, {.id = 2, .rect = {0, 0, 30, 10}}}, 1);
		QCOMPARE(areas.size(), 1);
	}

	void noPixelIsDimmedTwice() {
		auto areas = planDim(
		    {{.id = 1, .rect = {0, 0, 10, 10}}, {.id = 2, .rect = {5, 5, 10, 10}}, {.id = 3, .rect = {2, 2, 20, 20}}},
		    0
		);
		QRegion seen;
		for (const auto& area: areas) {
			QVERIFY(!seen.intersects(area.region));
			seen += area.region;
		}
	}

	void theCurveGoesFromZeroToOne() {
		QCOMPARE(dimCurve(0), 0.0);
		QCOMPARE(dimCurve(1), 1.0);
		auto previous = 0.0;
		for (auto i = 1; i <= 100; i++) {
			auto value = dimCurve(i / 100.0);
			QVERIFY(value >= previous);
			previous = value;
		}
		QVERIFY(dimCurve(0.25) > 0.7);
	}
};

QTEST_APPLESS_MAIN(TestDimPlan);

#include "dim_plan_test.moc"
