#pragma once

#include <qlist.h>
#include <qmargins.h>
#include <qrect.h>

namespace qs::windows {

struct SnapArea {
	QList<QRect> windows;
	QRect work;
	QMargins reserved;
	double scale = 1;
	int gapsIn = 0;
	int gapsOut = 0;
};

struct SnapEdges {
	bool left = true;
	bool right = true;
	bool top = true;
	bool bottom = true;
};

[[nodiscard]] QRect snapMoved(const QRect& window, const SnapArea& area);
[[nodiscard]] QRect snapResized(const QRect& window, const SnapArea& area, const SnapEdges& edges);

}
