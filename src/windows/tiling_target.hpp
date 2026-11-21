#pragma once

#include <limits>

#include <qpoint.h>
#include <qrect.h>
#include <qtypes.h>

#include "tiling_layout.hpp"

namespace qs::windows::tiling {

[[nodiscard]] inline LayoutEngine::Id
cursorInsertionTarget(const LayoutEngine& layout, const QPoint& cursor) {
	auto chosen = LayoutEngine::Id(0);
	auto closest = std::numeric_limits<qint64>::max();

	for (auto id: layout.ids()) {
		if (layout.isHidden(id) || layout.isParked(id)) continue;
		auto box = layout.box(id);
		if (!box.isValid()) continue;

		auto dx = qint64(0);
		if (cursor.x() < box.left()) dx = qint64(box.left()) - cursor.x();
		else if (cursor.x() > box.right()) dx = qint64(cursor.x()) - box.right();
		auto dy = qint64(0);
		if (cursor.y() < box.top()) dy = qint64(box.top()) - cursor.y();
		else if (cursor.y() > box.bottom()) dy = qint64(cursor.y()) - box.bottom();

		auto distance = dx * dx + dy * dy;
		if (distance < closest) {
			chosen = id;
			closest = distance;
			if (distance == 0) break;
		}
	}
	return chosen;
}

}
