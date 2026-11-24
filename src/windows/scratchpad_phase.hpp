#pragma once

#include <qtypes.h>

namespace qs::windows {

enum class ScratchpadPhase : quint8 {
	Hidden,
	Preparing,
	Entering,
	Shown,
	Leaving,
};

enum class ScratchpadEvent : quint8 {
	Show,
	Hide,
	HideNow,
	Ready,
	Settled,
};

[[nodiscard]] constexpr ScratchpadPhase nextPhase(ScratchpadPhase phase, ScratchpadEvent event) {
	using Phase = ScratchpadPhase;
	using Event = ScratchpadEvent;

	if (event == Event::HideNow) return Phase::Hidden;

	switch (phase) {
	case Phase::Hidden: return event == Event::Show ? Phase::Preparing : phase;
	case Phase::Preparing:
		if (event == Event::Ready) return Phase::Entering;
		return event == Event::Hide ? Phase::Hidden : phase;
	case Phase::Entering:
		if (event == Event::Settled) return Phase::Shown;
		return event == Event::Hide ? Phase::Leaving : phase;
	case Phase::Shown: return event == Event::Hide ? Phase::Leaving : phase;
	case Phase::Leaving:
		if (event == Event::Settled) return Phase::Hidden;
		return event == Event::Show ? Phase::Entering : phase;
	}
	return phase;
}

[[nodiscard]] constexpr ScratchpadEvent toggleEvent(ScratchpadPhase phase) {
	auto hidden = phase == ScratchpadPhase::Hidden || phase == ScratchpadPhase::Leaving;
	return hidden ? ScratchpadEvent::Show : ScratchpadEvent::Hide;
}

}
