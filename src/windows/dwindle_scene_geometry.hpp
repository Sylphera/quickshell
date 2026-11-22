#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

#include <qlist.h>
#include <qrect.h>

namespace qs::windows::dwindle {

constexpr double CurveX1 = 0.05;
constexpr double CurveY1 = 0.7;
constexpr double CurveX2 = 0.1;
constexpr double CurveY2 = 1.0;
constexpr int CurveSegments = 16;

inline double bezierAt(double a, double b, double u) {
	const auto v = 1.0 - u;
	return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
}

inline double bezierSlopeAt(double a, double b, double u) {
	const auto v = 1.0 - u;
	return 3 * v * v * a + 6 * v * u * (b - a) + 3 * u * u * (1 - b);
}

inline double curveParameter(double t) {
	auto low = 0.0;
	auto high = 1.0;
	for (auto step = 0; step < 48; ++step) {
		const auto middle = (low + high) / 2;
		if (bezierAt(CurveX1, CurveX2, middle) < t) low = middle;
		else high = middle;
	}
	return (low + high) / 2;
}

inline double ease(double t) {
	if (t <= 0) return 0;
	if (t >= 1) return 1;
	return bezierAt(CurveY1, CurveY2, curveParameter(t));
}

inline double easeSlope(double t) {
	const auto u = curveParameter(std::clamp(t, 0.0, 1.0));
	const auto dx = bezierSlopeAt(CurveX1, CurveX2, u);
	if (dx <= 1e-9) return 0;
	return bezierSlopeAt(CurveY1, CurveY2, u) / dx;
}

struct CurveSegment {
	double begin;
	double value;
	double linear;
	double quadratic;
	double cubic;
};

inline QList<CurveSegment> curveSegments(double from, double to, double duration) {
	struct Sample {
		double time;
		double value;
		double slope;
	};
	static const auto samples = [] {
		std::array<Sample, CurveSegments + 1> result {};
		for (auto i = 0; i <= CurveSegments; ++i) {
			const auto position = static_cast<double>(i) / CurveSegments;
			const auto time = position * position;
			result[i] = {time, ease(time), easeSlope(time)};
		}
		return result;
	}();

	QList<CurveSegment> segments;
	segments.reserve(CurveSegments);
	const auto distance = to - from;
	for (auto i = 0; i < CurveSegments; ++i) {
		const auto& first = samples[i];
		const auto& last = samples[i + 1];
		const auto t0 = duration * first.time;
		const auto span = duration * last.time - t0;
		const auto y0 = from + distance * first.value;
		const auto y1 = from + distance * last.value;
		const auto m0 = distance * first.slope / duration;
		const auto m1 = distance * last.slope / duration;
		const auto rise = (y1 - y0) / span;
		segments.append({
		    t0,
		    y0,
		    m0,
		    (3 * rise - 2 * m0 - m1) / span,
		    (m0 + m1 - 2 * rise) / (span * span),
		});
	}
	return segments;
}

inline double curveValue(const QList<CurveSegment>& segments, double time) {
	const auto* active = &segments.first();
	for (const auto& segment: segments) {
		if (segment.begin > time) break;
		active = &segment;
	}
	const auto s = time - active->begin;
	return active->value + s * (active->linear + s * (active->quadratic + s * active->cubic));
}

inline QRectF interpolate(const QRectF& from, const QRectF& to, double t) {
	const auto weight = ease(t);
	return {
	    std::lerp(from.x(), to.x(), weight),
	    std::lerp(from.y(), to.y(), weight),
	    std::lerp(from.width(), to.width(), weight),
	    std::lerp(from.height(), to.height(), weight),
	};
}

struct Motion {
	QRectF from;
	QRectF target;
};

inline bool stayApart(const Motion& a, const Motion& b) {
	constexpr auto epsilon = 0.001;
	return (a.from.right() <= b.from.left() + epsilon && a.target.right() <= b.target.left())
	    || (b.from.right() <= a.from.left() + epsilon && b.target.right() <= a.target.left())
	    || (a.from.bottom() <= b.from.top() + epsilon && a.target.bottom() <= b.target.top())
	    || (b.from.bottom() <= a.from.top() + epsilon && b.target.bottom() <= a.target.top());
}

inline std::optional<QRectF> openingOrigin(const QRectF& target, const QList<Motion>& placed) {
	const auto center = target.center();
	QList<qreal> xs {center.x()};
	QList<qreal> ys {center.y()};
	for (const auto& motion: placed) {
		if (motion.target.right() <= target.left()) xs.append(motion.from.right());
		if (target.right() <= motion.target.left()) xs.append(motion.from.left());
		if (motion.target.bottom() <= target.top()) ys.append(motion.from.bottom());
		if (target.bottom() <= motion.target.top()) ys.append(motion.from.top());
	}
	auto best = std::optional<QRectF>();
	auto cost = std::numeric_limits<qreal>::max();
	for (auto x: xs) {
		for (auto y: ys) {
			const auto dx = x - center.x();
			const auto dy = y - center.y();
			const auto distance = dx * dx + dy * dy;
			if (distance >= cost) continue;
			const Motion opening {QRectF(x, y, 0, 0), target};
			if (std::ranges::all_of(placed, [&opening](const Motion& existing) {
				    return stayApart(opening, existing);
			    }))
			{
				best = opening.from;
				cost = distance;
			}
		}
	}
	return best;
}

inline void dropStackedOrigins(QList<std::optional<QRectF>>& origins, const QList<bool>& movable) {
	constexpr auto epsilon = 0.5;
	QList<qsizetype> stacked;
	for (qsizetype i = 0; i < origins.size(); ++i) {
		if (!movable[i] || !origins[i].has_value()) continue;
		for (qsizetype j = 0; j < origins.size(); ++j) {
			if (i == j || !origins[j].has_value()) continue;
			const auto overlap = origins[i]->intersected(*origins[j]);
			if (overlap.width() > epsilon && overlap.height() > epsilon) {
				stacked.append(i);
				break;
			}
		}
	}
	for (auto i: stacked) origins[i].reset();
}

inline bool planOrigins(const QList<QRect>& targets, QList<std::optional<QRectF>>& origins) {
	QList<Motion> placed;
	placed.reserve(targets.size());
	for (qsizetype i = 0; i < targets.size(); ++i) {
		if (!origins[i].has_value()) continue;
		const Motion motion {*origins[i], targets[i]};
		if (!std::ranges::all_of(placed, [&motion](const Motion& previous) {
			    return stayApart(motion, previous);
		    }))
			return false;
		placed.append(motion);
	}
	for (qsizetype i = 0; i < targets.size(); ++i) {
		if (origins[i].has_value()) continue;
		origins[i] = openingOrigin(targets[i], placed);
		if (!origins[i].has_value()) return false;
		placed.append(Motion {*origins[i], targets[i]});
	}
	return true;
}

}
