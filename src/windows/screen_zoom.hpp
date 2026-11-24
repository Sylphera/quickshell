#pragma once

#include <qt_windows.h>
#include <qpoint.h>

namespace qs::windows {

struct ScreenZoomView {
	HMONITOR monitor = nullptr;
	float factor = 1.0f;
	QPointF offset;
};

ScreenZoomView screenZoomView();

void adjustScreenZoom(bool increase);
void resetScreenZoom();

}
