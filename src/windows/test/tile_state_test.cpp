#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>

#include "tile_state.hpp"

using qs::windows::isHeld;
using qs::windows::isStaged;
using qs::windows::nextMode;
using qs::windows::nextPlacement;
using Mode = qs::windows::TileMode;
using ModeEvent = qs::windows::TileModeEvent;
using Placement = qs::windows::TilePlacement;
using PlacementEvent = qs::windows::TilePlacementEvent;

namespace {

constexpr Mode MODES[] = {Mode::Normal, Mode::Maximized, Mode::Fullscreen};
constexpr Placement PLACEMENTS[] =
    {Placement::Placed, Placement::Staged, Placement::Held, Placement::StagedHeld};

}

class TestTileState: public QObject {
	Q_OBJECT;

private slots:
	void togglingTwiceReturns() {
		QCOMPARE(nextMode(nextMode(Mode::Normal, ModeEvent::ToggleMaximize), ModeEvent::ToggleMaximize), Mode::Normal);
		QCOMPARE(
		    nextMode(nextMode(Mode::Normal, ModeEvent::ToggleFullscreen), ModeEvent::ToggleFullscreen),
		    Mode::Normal
		);
	}

	void oneModeReplacesTheOther() {
		QCOMPARE(nextMode(Mode::Maximized, ModeEvent::ToggleFullscreen), Mode::Fullscreen);
		QCOMPARE(nextMode(Mode::Fullscreen, ModeEvent::ToggleMaximize), Mode::Maximized);
	}

	void restoringOnlyEndsMaximize() {
		QCOMPARE(nextMode(Mode::Maximized, ModeEvent::Restore), Mode::Normal);
		QCOMPARE(nextMode(Mode::Fullscreen, ModeEvent::Restore), Mode::Fullscreen);
	}

	void leavingFullscreenOnlyEndsFullscreen() {
		QCOMPARE(nextMode(Mode::Fullscreen, ModeEvent::LeaveFullscreen), Mode::Normal);
		QCOMPARE(nextMode(Mode::Maximized, ModeEvent::LeaveFullscreen), Mode::Maximized);
	}

	void everyModeCanReturnToNormal() {
		for (auto mode: MODES) {
			auto next = nextMode(nextMode(mode, ModeEvent::LeaveFullscreen), ModeEvent::Restore);
			QCOMPARE(next, Mode::Normal);
		}
	}

	void releasingAlwaysPlaces() {
		for (auto placement: PLACEMENTS) {
			QCOMPARE(nextPlacement(placement, PlacementEvent::Release), Placement::Placed);
		}
	}

	void stagingKeepsTheHold() {
		for (auto placement: PLACEMENTS) {
			auto next = nextPlacement(placement, PlacementEvent::Stage);
			QVERIFY(isStaged(next));
			QCOMPARE(isHeld(next), isHeld(placement));
		}
	}

	void aStagedWindowIsNotHeldLater() {
		QCOMPARE(nextPlacement(Placement::Staged, PlacementEvent::Hold), Placement::Staged);
		QCOMPARE(nextPlacement(Placement::Placed, PlacementEvent::Hold), Placement::Held);
	}

	void unholdingKeepsTheStage() {
		for (auto placement: PLACEMENTS) {
			auto next = nextPlacement(placement, PlacementEvent::Unhold);
			QVERIFY(!isHeld(next));
			QCOMPARE(isStaged(next), isStaged(placement));
		}
	}
};

QTEST_APPLESS_MAIN(TestTileState);

#include "tile_state_test.moc"
