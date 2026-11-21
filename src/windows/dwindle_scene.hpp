#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qrect.h>

namespace qs::windows {

struct DwindleTile {
	HWND window;
	QRect previous;
	QRect target;
};

class DwindleScene {
public:
	static bool enabled();
	static void warmUp();
	static void present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed);
	static void dismiss(HMONITOR monitor);
	static void dismissAll();
};

}
