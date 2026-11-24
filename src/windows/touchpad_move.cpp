#include "touchpad_move.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <qt_windows.h>

#include "tiling.hpp"
#include "window_tracker.hpp"

namespace qs::windows {

namespace {
constexpr double TILED_FACTOR = 0.5;
constexpr double TILED_LIMIT = 100;
constexpr double MINIMUM_DISTANCE = 0.1;
constexpr qint64 IN_FLIGHT_MS = 50;
}

bool TouchpadMove::begin() {
	this->started = true;
	this->tiled = false;
	this->window = WindowTracker::instance()->activeWindow();
	if (this->window == nullptr) return false;

	auto* hwnd = this->window->hwnd();
	auto movable = !IsIconic(hwnd) && !IsZoomed(hwnd) && !IsHungAppWindow(hwnd)
	            && !this->window->fullscreen() && !this->window->borderlessFullscreen();
	if (movable) {
		auto* tiling = TilingManager::active();
		if (tiling != nullptr && tiling->isTiled(this->window)) {
			this->tiled = tiling->beginGestureMove(this->window);
			movable = this->tiled;
		}
	}

	RECT rect {};
	if (!movable || !GetWindowRect(hwnd, &rect)) {
		if (this->tiled) TilingManager::instance()->endGestureMove(this->window, std::nullopt);
		this->window = nullptr;
		return false;
	}
	this->origin = {.x = rect.left, .y = rect.top};
	this->sent = this->origin;
	this->sentAt.invalidate();
	auto dpi = GetDpiForWindow(hwnd);
	this->scale = dpi == 0 ? 1 : static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
	return true;
}

void TouchpadMove::moveTo(double dx, double dy, bool force) {
	auto* hwnd = this->window->hwnd();
	auto target = POINT {
	    .x = this->origin.x + std::lround(dx * this->scale),
	    .y = this->origin.y + std::lround(dy * this->scale),
	};
	if (target.x == this->sent.x && target.y == this->sent.y) return;

	RECT rect {};
	auto arrived = GetWindowRect(hwnd, &rect) && rect.left == this->sent.x && rect.top == this->sent.y;
	auto waiting = !arrived && this->sentAt.isValid() && this->sentAt.elapsed() < IN_FLIGHT_MS;
	if (waiting && !force) return;

	this->sent = target;
	this->sentAt.start();
	constexpr UINT flags =
	    SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
	SetWindowPos(hwnd, nullptr, target.x, target.y, 0, 0, flags);
}

void TouchpadMove::update(double dx, double dy) {
	if (!this->started && !this->begin()) return;
	if (this->window == nullptr) return;
	if (this->tiled) {
		dx = std::clamp(dx * TILED_FACTOR, -TILED_LIMIT, TILED_LIMIT);
		dy = std::clamp(dy * TILED_FACTOR, -TILED_LIMIT, TILED_LIMIT);
	}
	this->moveTo(dx, dy, false);
}

void TouchpadMove::end(double dx, double dy, bool cancelled) {
	if (!std::exchange(this->started, false)) return;
	auto window = std::exchange(this->window, nullptr);
	auto wasTiled = std::exchange(this->tiled, false);
	if (window == nullptr) return;

	if (!wasTiled) {
		this->window = window;
		this->moveTo(cancelled ? 0 : dx, cancelled ? 0 : dy, true);
		this->window = nullptr;
		return;
	}

	using Edge = TilingManager::Edge;
	std::optional<Edge> edge;
	if (!cancelled && std::hypot(dx, dy) >= MINIMUM_DISTANCE) {
		if (std::abs(dx) > std::abs(dy)) edge = dx > 0 ? Edge::Right : Edge::Left;
		else edge = dy > 0 ? Edge::Bottom : Edge::Top;
	}
	TilingManager::instance()->endGestureMove(window, edge);
}

}
