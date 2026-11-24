#include "layer_plan.hpp"
#include <algorithm>

#include <qlist.h>
#include <qtypes.h>

namespace qs::windows {

namespace {

qsizetype positionOf(const QList<LayerWindow>& windows, quintptr id) {
	auto found = std::ranges::find(windows, id, &LayerWindow::id);
	return found == windows.end() ? -1 : std::distance(windows.begin(), found);
}

}

QList<LayerMove>
planLayers(const QList<LayerWindow>& current, quintptr anchor, const QList<quintptr>& chain) {
	auto order = current;
	QList<LayerMove> moves;
	auto previous = anchor;

	for (auto id: chain) {
		auto position = positionOf(order, id);
		if (position == -1) continue;
		auto above = previous == 0 ? -1 : positionOf(order, previous);
		if (order[position].topmost && position > above) {
			previous = id;
			continue;
		}

		moves.append({.id = id, .after = previous});
		order.removeAt(position);
		above = previous == 0 ? -1 : positionOf(order, previous);
		order.insert(above + 1, {.id = id, .topmost = true});
		previous = id;
	}
	return moves;
}

}
