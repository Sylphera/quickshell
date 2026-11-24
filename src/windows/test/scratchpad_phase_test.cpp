#include <qlist.h>
#include <qobject.h>
#include <qtest.h>
#include <qtestcase.h>

#include "scratchpad_phase.hpp"

using qs::windows::nextPhase;
using qs::windows::toggleEvent;
using Phase = qs::windows::ScratchpadPhase;
using Event = qs::windows::ScratchpadEvent;

namespace {

Phase run(const QList<Event>& events) {
	auto phase = Phase::Hidden;
	for (auto event: events) phase = nextPhase(phase, event);
	return phase;
}

constexpr Phase PHASES[] =
    {Phase::Hidden, Phase::Preparing, Phase::Entering, Phase::Shown, Phase::Leaving};
constexpr Event EVENTS[] = {Event::Show, Event::Hide, Event::HideNow, Event::Ready, Event::Settled};

}

class TestScratchpadPhase: public QObject {
	Q_OBJECT;

private slots:
	void opensAndCloses() {
		QCOMPARE(run({Event::Show}), Phase::Preparing);
		QCOMPARE(run({Event::Show, Event::Ready}), Phase::Entering);
		QCOMPARE(run({Event::Show, Event::Ready, Event::Settled}), Phase::Shown);
		QCOMPARE(run({Event::Show, Event::Ready, Event::Settled, Event::Hide}), Phase::Leaving);
		QCOMPARE(
		    run({Event::Show, Event::Ready, Event::Settled, Event::Hide, Event::Settled}),
		    Phase::Hidden
		);
	}

	void closingWhilePreparingHidesAtOnce() {
		QCOMPARE(run({Event::Show, Event::Hide}), Phase::Hidden);
	}

	void aLateReadyDoesNotBringItBack() {
		QCOMPARE(run({Event::Show, Event::Hide, Event::Ready}), Phase::Hidden);
		QCOMPARE(run({Event::Show, Event::Ready, Event::Hide, Event::Ready}), Phase::Leaving);
	}

	void aLateSettledOnlyEndsWhatIsGoing() {
		QCOMPARE(run({Event::Show, Event::Settled}), Phase::Preparing);
		QCOMPARE(run({Event::Settled}), Phase::Hidden);
	}

	void turningAroundMidway() {
		QCOMPARE(run({Event::Show, Event::Ready, Event::Hide}), Phase::Leaving);
		QCOMPARE(run({Event::Show, Event::Ready, Event::Hide, Event::Show}), Phase::Entering);
		QCOMPARE(run({Event::Show, Event::Ready, Event::Hide, Event::Show, Event::Hide}), Phase::Leaving);
	}

	void leavingTheDesktopHidesFromAnywhere() {
		for (auto phase: PHASES) QCOMPARE(nextPhase(phase, Event::HideNow), Phase::Hidden);
	}

	void askingTwiceChangesNothing() {
		for (auto phase: PHASES) {
			for (auto event: {Event::Show, Event::Hide}) {
				auto once = nextPhase(phase, event);
				QCOMPARE(nextPhase(once, event), once);
			}
		}
	}

	void togglingFastAlwaysEndsWhereItWasAskedTo() {
		for (auto presses = 1; presses <= 20; presses++) {
			for (auto between = 0; between < 3; between++) {
				auto phase = Phase::Hidden;
				auto wanted = false;
				for (auto i = 0; i < presses; i++) {
					auto event = toggleEvent(phase);
					wanted = event == Event::Show;
					phase = nextPhase(phase, event);
					if (between > 0) phase = nextPhase(phase, Event::Ready);
					if (between > 1) phase = nextPhase(phase, Event::Settled);
				}
				phase = nextPhase(nextPhase(phase, Event::Ready), Event::Settled);
				QCOMPARE(phase, wanted ? Phase::Shown : Phase::Hidden);
			}
		}
	}

	void everyPhaseCanAlwaysBeLeft() {
		for (auto phase: PHASES) {
			auto leaves = false;
			for (auto event: EVENTS) leaves = leaves || nextPhase(phase, event) != phase;
			QVERIFY(leaves);
		}
	}
};

QTEST_APPLESS_MAIN(TestScratchpadPhase);

#include "scratchpad_phase_test.moc"
