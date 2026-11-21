#pragma once

#include <functional>

#include <qt_windows.h>

#include <qlist.h>
#include <qrect.h>

namespace qs::windows {

struct DwindleTile {
	HWND window;
	QRect previous;
	QRect target;
	bool settled = false;
};

class DwindleScene {
public:
	static bool enabled();
	static void warmUp();
	static void present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed);
	static void cover(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles);
	static void dismiss(HMONITOR monitor);
	static bool presenting(HMONITOR monitor);
	static void onRelease(std::function<void(const QList<HWND>&)> release);
	static void dismissAll();
};

}
