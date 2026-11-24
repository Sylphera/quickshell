#pragma once

#include <qtypes.h>

namespace qs::windows {

enum class TileMode : quint8 { Normal, Maximized, Fullscreen };

enum class TileModeEvent : quint8 {
	ToggleMaximize,
	ToggleFullscreen,
	LeaveFullscreen,
	Restore,
};

enum class TilePlacement : quint8 { Placed, Staged, Held, StagedHeld };

enum class TilePlacementEvent : quint8 {
	Stage,
	Hold,
	Unhold,
	Release,
};

[[nodiscard]] constexpr TileMode nextMode(TileMode mode, TileModeEvent event) {
	switch (event) {
	case TileModeEvent::ToggleMaximize:
		return mode == TileMode::Maximized ? TileMode::Normal : TileMode::Maximized;
	case TileModeEvent::ToggleFullscreen:
		return mode == TileMode::Fullscreen ? TileMode::Normal : TileMode::Fullscreen;
	case TileModeEvent::LeaveFullscreen:
		return mode == TileMode::Fullscreen ? TileMode::Normal : mode;
	case TileModeEvent::Restore: return mode == TileMode::Maximized ? TileMode::Normal : mode;
	}
	return mode;
}

[[nodiscard]] constexpr bool isStaged(TilePlacement placement) {
	return placement == TilePlacement::Staged || placement == TilePlacement::StagedHeld;
}

[[nodiscard]] constexpr bool isHeld(TilePlacement placement) {
	return placement == TilePlacement::Held || placement == TilePlacement::StagedHeld;
}

[[nodiscard]] constexpr TilePlacement nextPlacement(TilePlacement placement, TilePlacementEvent event) {
	switch (event) {
	case TilePlacementEvent::Stage:
		return isHeld(placement) ? TilePlacement::StagedHeld : TilePlacement::Staged;
	case TilePlacementEvent::Hold:
		return placement == TilePlacement::Placed ? TilePlacement::Held : placement;
	case TilePlacementEvent::Unhold:
		return isStaged(placement) ? TilePlacement::Staged : TilePlacement::Placed;
	case TilePlacementEvent::Release: return TilePlacement::Placed;
	}
	return placement;
}

}
