#include "window_snap.hpp"
#include <cmath>
#include <cstdlib>

#include <qrect.h>

namespace qs::windows {

namespace {

constexpr int WINDOW_GAP = 4;
constexpr int MONITOR_GAP = 5;

struct Range {
	int start = 0;
	int end = 0;
};

struct Snapped {
	bool left = false;
	bool right = false;
	bool top = false;
	bool bottom = false;
};

class Snapper {
public:
	Snapper(const QRect& window, const SnapArea& area, const SnapEdges& edges, bool moving)
	    : x {.start = window.x(), .end = window.x() + window.width()}
	    , y {.start = window.y(), .end = window.y() + window.height()}
	    , area(area)
	    , edges(edges)
	    , moving(moving) {}

	QRect run() {
		for (const auto& other: this->area.windows) this->snapToWindow(other);
		this->snapToMonitor();
		return {this->x.start, this->y.start, this->x.end - this->x.start, this->y.end - this->y.start};
	}

private:
	[[nodiscard]] int scaled(int value) const {
		return static_cast<int>(std::lround(value * this->area.scale));
	}

	static bool canSnap(int a, int b, int gap) { return std::abs(a - b) < gap; }

	void snapStart(Range& range, int edge) const {
		if (this->moving) range.end = edge + (range.end - range.start);
		range.start = edge;
	}

	void snapEnd(Range& range, int edge) const {
		if (this->moving) range.start = edge - (range.end - range.start);
		range.end = edge;
	}

	bool snapEither(Range& range, bool startEdge, bool endEdge, Range to, int gap, bool& start, bool& end) {
		if (startEdge && canSnap(range.start, to.start, gap)) {
			this->snapStart(range, to.start);
			start = true;
			return true;
		}
		if (endEdge && canSnap(range.end, to.end, gap)) {
			this->snapEnd(range, to.end);
			end = true;
			return true;
		}
		return false;
	}

	void snapToWindow(const QRect& other) {
		auto gap = this->scaled(WINDOW_GAP);
		auto space = this->scaled(this->area.gapsIn * 2);
		auto otherX = Range {.start = other.x() - space, .end = other.x() + other.width() + space};
		auto otherY = Range {.start = other.y() - space, .end = other.y() + other.height() + space};
		auto& s = this->snapped;
		const auto& e = this->edges;

		if (this->y.start <= otherY.end && otherY.start <= this->y.end) {
			auto to = Range {.start = otherX.end, .end = otherX.start};
			this->snapEither(this->x, e.left, e.right, to, gap, s.left, s.right);
		}
		if (this->x.start <= otherX.end && otherX.start <= this->x.end) {
			auto to = Range {.start = otherY.end, .end = otherY.start};
			this->snapEither(this->y, e.top, e.bottom, to, gap, s.top, s.bottom);
		}

		if (this->x.start == otherX.end || otherX.start == this->x.end) {
			auto to = Range {.start = otherY.start + space, .end = otherY.end - space};
			this->snapEither(this->y, e.top && !s.top, e.bottom && !s.bottom, to, gap, s.top, s.bottom);
		}
		if (this->y.start == otherY.end || otherY.start == this->y.end) {
			auto to = Range {.start = otherX.start + space, .end = otherX.end - space};
			this->snapEither(this->x, e.left && !s.left, e.right && !s.right, to, gap, s.left, s.right);
		}
	}

	void snapToMonitor() {
		auto gap = this->scaled(MONITOR_GAP);
		auto out = this->scaled(this->area.gapsOut);
		const auto& work = this->area.work;
		const auto& reserved = this->area.reserved;
		if (work.isEmpty()) return;

		this->monitorStart(this->x, this->edges.left, work.x() + out, reserved.left(), gap);
		this->monitorEnd(this->x, this->edges.right, work.x() + work.width() - out, reserved.right(), gap);
		this->monitorStart(this->y, this->edges.top, work.y() + out, reserved.top(), gap);
		this->monitorEnd(this->y, this->edges.bottom, work.y() + work.height() - out, reserved.bottom(), gap);
	}

	void monitorStart(Range& range, bool edge, int inner, int reserved, int gap) const {
		if (!edge) return;
		if (reserved > 0 && canSnap(range.start, inner, gap)) this->snapStart(range, inner);
		else if (canSnap(range.start, inner - reserved, gap)) this->snapStart(range, inner - reserved);
	}

	void monitorEnd(Range& range, bool edge, int inner, int reserved, int gap) const {
		if (!edge) return;
		if (reserved > 0 && canSnap(range.end, inner, gap)) this->snapEnd(range, inner);
		else if (canSnap(range.end, inner + reserved, gap)) this->snapEnd(range, inner + reserved);
	}

	Range x;
	Range y;
	const SnapArea& area;
	const SnapEdges& edges;
	bool moving;
	Snapped snapped;
};

}

QRect snapMoved(const QRect& window, const SnapArea& area) {
	auto edges = SnapEdges();
	return Snapper(window, area, edges, true).run();
}

QRect snapResized(const QRect& window, const SnapArea& area, const SnapEdges& edges) {
	return Snapper(window, area, edges, false).run();
}

}
