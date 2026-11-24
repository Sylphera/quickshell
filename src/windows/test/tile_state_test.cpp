#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>

#include "tile_state.hpp"

using qs::windows::healTile;
using qs::windows::isHeld;
using qs::windows::isStaged;
using qs::windows::nextMode;
using qs::windows::nextPlacement;
using qs::windows::placeTile;
using qs::windows::TILE_ANIMATION_MS;
using qs::windows::TILE_WAIT_MS;
using Heal = qs::windows::TileHeal;
using Mode = qs::windows::TileMode;
using ModeEvent = qs::windows::TileModeEvent;
using Step = qs::windows::TileStep;
using Placement = qs::windows::TilePlacement;
using PlacementEvent = qs::windows::TilePlacementEvent;

namespace {

constexpr Mode MODES[] = {Mode::Normal, Mode::Maximized, Mode::Fullscreen};
constexpr Placement PLACEMENTS[] =
    {Placement::Placed, Placement::Staged, Placement::Held, Placement::StagedHeld};

Step placedBefore(bool moved, bool refresh, bool inPlace, bool accepted, bool waiting, bool exhausted) {
	if (moved) {
		if (inPlace && !refresh) return Step::Settled;
		return Step::Send;
	}
	if (refresh) return Step::Send;
	if (inPlace) return Step::Settled;
	if (accepted) return Step::Leave;
	if (waiting) return Step::Leave;
	if (exhausted) return Step::GiveUp;
	return Step::Correct;
}

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

	void aPlacedTileOnScreenIsLeftAlone() {
		for (auto busy: {false, true}) {
			for (auto animated: {false, true}) {
				QCOMPARE(healTile({.busy = busy, .animated = animated}), Heal::None);
			}
		}
	}

	void aLostTileComesBackOnceNothingMoves() {
		QCOMPARE(healTile({.lost = true}), Heal::Replace);
		QCOMPARE(healTile({.busy = true, .lost = true}), Heal::Wait);
		QCOMPARE(healTile({.animated = true, .lost = true}), Heal::Wait);
	}

	void aWaitingTileIsAlwaysWatchedUntilItIsPlaced() {
		for (auto placement: {Placement::Staged, Placement::Held, Placement::StagedHeld}) {
			for (auto busy: {false, true}) {
				for (auto animated: {false, true}) {
					for (auto lost: {false, true}) {
						for (qint64 ms: {qint64(0), TILE_WAIT_MS, TILE_ANIMATION_MS}) {
							auto heal = healTile({
							    .placement = placement,
							    .busy = busy,
							    .animated = animated,
							    .lost = lost,
							    .unplacedMs = ms,
							});
							QVERIFY(heal != Heal::None);
							if (busy) QCOMPARE(heal, Heal::Wait);
							if (!busy && ms >= TILE_ANIMATION_MS) QVERIFY(heal != Heal::Wait);
						}
					}
				}
			}
		}
	}

	void placingDecidesWhatItAlwaysDid() {
		for (auto bits = 0; bits < 64; bits++) {
			auto moved = (bits & 1) != 0;
			auto refresh = (bits & 2) != 0;
			auto inPlace = (bits & 4) != 0;
			auto accepted = (bits & 8) != 0;
			auto waiting = (bits & 16) != 0;
			auto exhausted = (bits & 32) != 0;
			QCOMPARE(
			    placeTile({moved, refresh, inPlace, accepted, waiting, exhausted}),
			    placedBefore(moved, refresh, inPlace, accepted, waiting, exhausted)
			);
		}
	}

	void anAnimationIsNotCutEarly() {
		QCOMPARE(
		    healTile({.placement = Placement::Staged, .animated = true, .unplacedMs = TILE_ANIMATION_MS - 1}),
		    Heal::Wait
		);
		QCOMPARE(
		    healTile({.placement = Placement::Held, .unplacedMs = TILE_WAIT_MS - 1}),
		    Heal::Wait
		);
		QCOMPARE(healTile({.placement = Placement::Held, .unplacedMs = TILE_WAIT_MS}), Heal::Release);
	}
};

QTEST_APPLESS_MAIN(TestTileState);

#include "tile_state_test.moc"
