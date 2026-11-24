#include "touchpad_swipe.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include <qlist.h>
#include <qtypes.h>

namespace qs::windows {

namespace {

constexpr qsizetype SWIPE_FINGERS = 4;
constexpr qsizetype MOVE_FINGERS = 3;
constexpr double SWIPE_DISTANCE = 700;
constexpr double SWIPE_FORCE_SPEED = 5;
constexpr double SWIPE_MINIMUM = 2;
constexpr double START_THRESHOLD = 5;
constexpr double MOVE_START_THRESHOLD = 16;
constexpr double PINCH_START_RATIO = 0.1;
constexpr double PINCH_DELTA_SCALE = 400;
constexpr double PINCH_CLOSING_FACTOR = 1.6;
constexpr double PINCH_DISTANCE = 250;
constexpr double PINCH_COMPLETION = 0.2;
constexpr double DIRECTION_LOCK_THRESHOLD = 10;
constexpr double RETURN_THRESHOLD = 35;
constexpr double FINGER_SWITCH_MS = 100;
constexpr double LIBINPUT_BASELINE = 0.9;
constexpr double LIBINPUT_TOUCHPAD_SLOWDOWN = 0.2968;
constexpr double UNITS_PER_MM = 1000 / 25.4 * LIBINPUT_BASELINE * LIBINPUT_TOUCHPAD_SLOWDOWN;

double spreadOf(const QList<TouchpadContact>& contacts) {
	auto x = 0.0;
	auto y = 0.0;
	for (const auto& contact: contacts) {
		x += contact.x / static_cast<double>(contacts.length());
		y += contact.y / static_cast<double>(contacts.length());
	}
	auto spread = 0.0;
	for (const auto& contact: contacts) spread += std::hypot(contact.x - x, contact.y - y);
	return spread / static_cast<double>(contacts.length()) * UNITS_PER_MM;
}

}

qsizetype swipeDestination(
    qsizetype from,
    int way,
    qsizetype count,
    const std::function<bool(qsizetype)>& occupied
) {
	for (auto to = from + way; to >= 0 && to < count; to += way) {
		if (occupied(to)) return to;
	}
	return way > 0 && occupied(from) ? from + 1 : -1;
}

TouchpadSwipe::TouchpadSwipe(std::function<bool(int)> canGo): canGo(std::move(canGo)) {}

TouchpadSwipe::Step TouchpadSwipe::frame(const QList<TouchpadContact>& contacts, double timeMs) {
	if (contacts.isEmpty()) {
		auto step = this->finish();
		this->reset();
		this->spent = false;
		return step;
	}

	auto before = std::exchange(this->previous, contacts);
	if (this->fingersSwitched(contacts.length(), timeMs)) {
		auto step = this->finish();
		auto threeFingered = step.action == Action::MoveEnd || step.action == Action::Pinch;
		if (threeFingered && contacts.length() > this->fingers) step.cancelled = true;
		if (step.action != Action::None) this->spent = true;
		this->reset();
		this->previous = contacts;
		this->fingers = contacts.length();
		this->startSpread = spreadOf(contacts);
		return step;
	}

	auto dx = 0.0;
	auto dy = 0.0;
	auto matched = 0;
	for (const auto& contact: contacts) {
		auto old = std::ranges::find(before, contact.id, &TouchpadContact::id);
		if (old == before.end()) continue;
		dx += contact.x - old->x;
		dy += contact.y - old->y;
		matched++;
	}
	if (!this->enabledFor(this->fingers) || matched == 0) return {};
	if (contacts.length() == MOVE_FINGERS && this->fingers == MOVE_FINGERS) this->trackPinch();
	if (dx == 0 && dy == 0) return {};
	dx *= UNITS_PER_MM / matched;
	dy *= UNITS_PER_MM / matched;

	if (this->state == State::Waiting) {
		this->totalX += dx;
		this->totalY += dy;
		if (this->fingers == MOVE_FINGERS) this->state = this->startMoving();
	}
	if (this->state == State::Moving) {
		this->moveX += dx;
		this->moveY += dy;
		return {.action = Action::Move, .dx = this->moveX, .dy = this->moveY};
	}
	if (this->fingers == MOVE_FINGERS) return {};

	if (this->state == State::Waiting) {
		auto horizontal = std::abs(this->totalX);
		auto vertical = std::abs(this->totalY);
		if (std::max(horizontal, vertical) < START_THRESHOLD) return {};
		this->state = horizontal > vertical ? State::Horizontal : State::Vertical;
	}
	if (this->state == State::Vertical) return {};

	this->advance(-dx);
	this->trackReturn();
	return {.action = Action::Update, .offset = this->delta / SWIPE_DISTANCE};
}

void TouchpadSwipe::setEnabled(bool fourFingers, bool threeFingers) {
	this->fourFingers = fourFingers;
	this->threeFingers = threeFingers;
}

bool TouchpadSwipe::enabledFor(qsizetype count) const {
	if (count == SWIPE_FINGERS) return this->fourFingers;
	return count == MOVE_FINGERS && this->threeFingers && !this->spent;
}

TouchpadSwipe::State TouchpadSwipe::startMoving() const {
	auto moved = std::hypot(this->totalX, this->totalY);
	if (moved < MOVE_START_THRESHOLD) return State::Waiting;
	return std::abs(spreadOf(this->previous) - this->startSpread) > moved ? State::Waiting : State::Moving;
}

void TouchpadSwipe::trackPinch() {
	if (this->startSpread <= 0) return;
	auto spread = spreadOf(this->previous);
	auto ratio = spread / this->startSpread;
	if (this->state == State::Waiting) {
		auto moved = std::hypot(this->totalX, this->totalY);
		if (std::abs(ratio - 1) < PINCH_START_RATIO || std::abs(spread - this->startSpread) <= moved) return;
		this->state = State::Pinching;
		this->opening = ratio > 1;
	}
	if (this->state != State::Pinching) return;
	auto change = this->opening ? ratio - 1 : (1 - ratio) * PINCH_CLOSING_FACTOR;
	this->pinchProgress = change * PINCH_DELTA_SCALE / PINCH_DISTANCE;
}

void TouchpadSwipe::reset() {
	this->previous.clear();
	this->state = State::Waiting;
	this->fingers = 0;
	this->pendingFingers = -1;
	this->startSpread = 0;
	this->opening = false;
	this->pinchProgress = 0;
	this->moveX = 0;
	this->moveY = 0;
	this->totalX = 0;
	this->totalY = 0;
	this->delta = 0;
	this->direction = 0;
	this->turningPoint = 0;
	this->returning = false;
	this->averageSpeed = 0;
	this->speedPoints = 0;
}

TouchpadSwipe::Step TouchpadSwipe::finish() const {
	if (this->state == State::Vertical) return {.action = Action::Vertical};
	if (this->state == State::Pinching) {
		return {.action = Action::Pinch, .cancelled = this->pinchProgress < PINCH_COMPLETION};
	}
	if (this->state == State::Moving) {
		return {.action = Action::MoveEnd, .dx = this->moveX, .dy = this->moveY};
	}
	if (this->state != State::Horizontal) return {};
	return {
	    .action = Action::End,
	    .offset = this->delta / SWIPE_DISTANCE,
	    .forced = this->averageSpeed >= SWIPE_FORCE_SPEED && std::abs(this->delta) >= SWIPE_MINIMUM,
	    .cancelled = this->returning,
	};
}

bool TouchpadSwipe::fingersSwitched(qsizetype count, double timeMs) {
	if (count == this->fingers) {
		this->pendingFingers = -1;
		return false;
	}
	if (this->state == State::Waiting) return true;
	if (count != this->pendingFingers) {
		this->pendingFingers = count;
		this->pendingSince = timeMs;
	}
	return timeMs - this->pendingSince >= FINGER_SWITCH_MS;
}

void TouchpadSwipe::advance(double moved) {
	this->averageSpeed =
	    (this->averageSpeed * this->speedPoints + std::abs(moved)) / (this->speedPoints + 1);
	this->speedPoints++;
	this->delta = std::clamp(this->delta + moved, -SWIPE_DISTANCE, SWIPE_DISTANCE);

	auto way = this->delta < 0 ? -1 : 1;
	if (this->delta != 0 && !this->canGo(way)) {
		this->delta = 0;
		return;
	}
	if (this->direction != 0 && this->direction != way) {
		this->delta = 0;
	} else if (this->direction == 0 && std::abs(this->delta) > DIRECTION_LOCK_THRESHOLD) {
		this->direction = way;
	}
}

void TouchpadSwipe::trackReturn() {
	auto reach = std::abs(this->delta);
	auto behind = this->returning ? reach - this->turningPoint : this->turningPoint - reach;
	if (behind >= RETURN_THRESHOLD) {
		this->returning = !this->returning;
		this->turningPoint = reach;
	} else if (behind < 0) {
		this->turningPoint = reach;
	}
}

}
