#include "dim_plan.hpp"
#include <algorithm>

#include <qlist.h>
#include <qregion.h>
#include <qtypes.h>

namespace qs::windows {

namespace {

double bezier(double t, double first, double second) {
	auto rest = 1.0 - t;
	return 3 * rest * rest * t * first + 3 * rest * t * t * second + t * t * t;
}

}

QList<DimArea> planDim(const QList<DimWindow>& topToBottom, quintptr focused) {
	auto known = std::ranges::any_of(topToBottom, [focused](const auto& window) {
		return window.id == focused;
	});
	QList<DimArea> areas;
	QRegion covered;
	for (const auto& window: topToBottom) {
		auto region = QRegion(window.rect).subtracted(covered);
		covered += window.rect;
		if (region.isEmpty()) continue;
		areas.append({
		    .id = window.id,
		    .rect = window.rect,
		    .region = region,
		    .dimmed = known && window.id != focused,
		});
	}
	return areas;
}

double dimCurve(double progress) {
	if (progress <= 0) return 0;
	if (progress >= 1) return 1;
	auto low = 0.0;
	auto high = 1.0;
	for (auto i = 0; i < 30; i++) {
		auto middle = (low + high) / 2;
		if (bezier(middle, 0.0, 0.15) < progress) low = middle;
		else high = middle;
	}
	return bezier((low + high) / 2, 0.75, 1.0);
}

}
