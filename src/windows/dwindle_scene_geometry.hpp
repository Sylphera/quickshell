#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include <qlist.h>
#include <qrect.h>

namespace qs::windows::dwindle {

inline QRectF interpolate(const QRectF& from, const QRectF& to, double t) {
	const auto f = 1.0 - t;
	const auto weight = 1.0 - f * f * f;
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
