#pragma once

#include <qlist.h>
#include <qrect.h>
#include <qregion.h>
#include <qtypes.h>

namespace qs::windows {

struct DimWindow {
	quintptr id = 0;
	QRect rect;
};

struct DimArea {
	quintptr id = 0;
	QRect rect;
	QRegion region;
	bool dimmed = false;
};

[[nodiscard]] QList<DimArea> planDim(const QList<DimWindow>& topToBottom, quintptr focused);
[[nodiscard]] double dimCurve(double progress);

}
