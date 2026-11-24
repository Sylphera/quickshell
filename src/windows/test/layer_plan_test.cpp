#include <qlist.h>
#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>

#include "layer_plan.hpp"

using qs::windows::LayerMove;
using qs::windows::LayerWindow;
using qs::windows::planLayers;

namespace {

constexpr quintptr PANEL = 1;
constexpr quintptr PICTURE = 2;
constexpr quintptr COVER = 3;
constexpr quintptr FIRST = 4;
constexpr quintptr SECOND = 5;
constexpr quintptr DIM = 6;
constexpr quintptr TILE = 7;

QList<LayerWindow> order(const QList<quintptr>& topmost, const QList<quintptr>& normal) {
	QList<LayerWindow> list;
	for (auto id: topmost) list.append({.id = id, .topmost = true});
	for (auto id: normal) list.append({.id = id, .topmost = false});
	return list;
}

QList<quintptr> after(QList<LayerWindow> windows, const QList<LayerMove>& moves) {
	for (const auto& move: moves) {
		windows.removeIf([&move](const LayerWindow& window) { return window.id == move.id; });
		qsizetype at = 0;
		for (qsizetype i = 0; i < windows.length(); i++) {
			if (windows[i].id == move.after) at = i + 1;
		}
		windows.insert(at, {.id = move.id, .topmost = true});
	}
	QList<quintptr> ids;
	for (const auto& window: windows) ids.append(window.id);
	return ids;
}

}

class TestLayerPlan: public QObject {
	Q_OBJECT;

private slots:
	void nothingMovesWhenTheOrderIsRight() {
		auto windows = order({PANEL, PICTURE, FIRST, DIM}, {TILE});
		QVERIFY(planLayers(windows, PICTURE, {FIRST, DIM}).isEmpty());
	}

	void theDimComesDownFromOverTheWindow() {
		auto windows = order({PANEL, DIM, FIRST}, {TILE});
		auto moves = planLayers(windows, PANEL, {FIRST, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({PANEL, FIRST, DIM, TILE}));
	}

	void aWindowThatIsNotTopmostIsRaised() {
		auto windows = order({PANEL, DIM}, {TILE, FIRST});
		auto moves = planLayers(windows, PANEL, {FIRST, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({PANEL, FIRST, DIM, TILE}));
	}

	void aWindowOverThePictureGoesUnderIt() {
		auto windows = order({FIRST, PANEL, PICTURE, DIM}, {TILE});
		auto moves = planLayers(windows, PICTURE, {FIRST, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({PANEL, PICTURE, FIRST, DIM, TILE}));
	}

	void everythingGoesUnderALowPicture() {
		auto windows = order({PANEL, FIRST, DIM, PICTURE}, {TILE});
		auto moves = planLayers(windows, PICTURE, {FIRST, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({PANEL, PICTURE, FIRST, DIM, TILE}));
	}

	void theCoverStaysOverEveryWindow() {
		auto windows = order({PANEL, SECOND, FIRST, COVER, DIM}, {TILE});
		auto moves = planLayers(windows, PANEL, {COVER, FIRST, SECOND, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({PANEL, COVER, FIRST, SECOND, DIM, TILE}));
	}

	void theFirstWindowOfTheChainEndsOnTop() {
		auto windows = order({PANEL, SECOND, FIRST, DIM}, {TILE});
		auto moves = planLayers(windows, PANEL, {FIRST, SECOND, DIM});
		auto ids = after(windows, moves);
		QVERIFY(ids.indexOf(FIRST) < ids.indexOf(SECOND));
		QVERIFY(ids.indexOf(SECOND) < ids.indexOf(DIM));
	}

	void withoutAnAnchorTheChainIsStillOrdered() {
		auto windows = order({DIM}, {TILE, FIRST});
		auto moves = planLayers(windows, 0, {FIRST, DIM});
		QCOMPARE(after(windows, moves), QList<quintptr>({FIRST, DIM, TILE}));
	}

	void missingWindowsAreSkipped() {
		auto windows = order({PANEL, DIM}, {TILE});
		QVERIFY(planLayers(windows, PANEL, {FIRST, DIM}).isEmpty());
	}

	void aSecondPassNeverMovesAnything() {
		const QList<QList<quintptr>> starts = {
		    {PANEL, DIM, FIRST, SECOND, PICTURE, COVER},
		    {FIRST, DIM, PICTURE, PANEL, SECOND, COVER},
		    {COVER, SECOND, FIRST, DIM, PANEL, PICTURE},
		};
		for (const auto& start: starts) {
			auto windows = order(start, {TILE});
			auto lowest = start.indexOf(PANEL) > start.indexOf(PICTURE) ? PANEL : PICTURE;
			auto chain = QList<quintptr>({COVER, FIRST, SECOND, DIM});
			auto ids = after(windows, planLayers(windows, lowest, chain));
			ids.removeOne(TILE);
			QVERIFY(planLayers(order(ids, {TILE}), lowest, chain).isEmpty());
		}
	}
};

QTEST_APPLESS_MAIN(TestLayerPlan);

#include "layer_plan_test.moc"
