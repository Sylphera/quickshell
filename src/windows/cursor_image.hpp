#pragma once

#include <qt_windows.h>
#include <qimage.h>
#include <qpoint.h>

namespace qs::windows {

struct CursorImage {
	HCURSOR handle = nullptr;
	QImage image;
	QPoint hotspot;
};

HCURSOR currentCursorHandle();
CursorImage cursorImage(HCURSOR cursor);

}
