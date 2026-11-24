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

enum class TileStep : quint8 { Settled, Leave, Send, Correct, GiveUp };

struct TileFrame {
	bool moved = false;
	bool refresh = false;
	bool inPlace = false;
	bool accepted = false;
	bool waiting = false;
	bool exhausted = false;
};

[[nodiscard]] constexpr TileStep placeTile(const TileFrame& frame) {
	if (frame.refresh) return TileStep::Send;
	if (frame.inPlace) return TileStep::Settled;
	if (frame.moved) return TileStep::Send;
	if (frame.accepted || frame.waiting) return TileStep::Leave;
	return frame.exhausted ? TileStep::GiveUp : TileStep::Correct;
}

enum class TileHeal : quint8 { None, Wait, Release, EndAnimation, Replace };

struct TileHealth {
	TilePlacement placement = TilePlacement::Placed;
	bool busy = false;
	bool animated = false;
	bool lost = false;
	qint64 unplacedMs = 0;
};

constexpr qint64 TILE_WAIT_MS = 1500;
constexpr qint64 TILE_ANIMATION_MS = 8000;

[[nodiscard]] constexpr TileHeal healTile(const TileHealth& health) {
	if (health.placement == TilePlacement::Placed) {
		if (!health.lost) return TileHeal::None;
		return health.busy || health.animated ? TileHeal::Wait : TileHeal::Replace;
	}
	if (health.busy) return TileHeal::Wait;
	if (health.animated) {
		return health.unplacedMs >= TILE_ANIMATION_MS ? TileHeal::EndAnimation : TileHeal::Wait;
	}
	return health.unplacedMs >= TILE_WAIT_MS ? TileHeal::Release : TileHeal::Wait;
}

}
