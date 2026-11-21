#pragma once

#include <functional>
#include <optional>

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
	static void snapshotClose(HMONITOR monitor, const QRect& work, HWND window);
	static void cacheCloseFrame(HWND window, const QRect& bounds);
	static void useCachedCloseFrame(HMONITOR monitor, const QRect& work, HWND window, quint32 pid,
	                               const QRect& bounds, bool destroyedOnly);
	static void clearCloseCache();
	static bool hasClosingTile(HMONITOR monitor, const QRect& work);
	static void present(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles, bool changed);
	static void cover(HMONITOR monitor, const QRect& work, const QList<DwindleTile>& tiles);
	static void dismiss(HMONITOR monitor);
	static bool presenting(HMONITOR monitor);
	static std::optional<HWND> windowAt(POINT point, HWND nativeHit);
	static QRect frameForDrag(HWND window);
	static void onOcclusion(std::function<bool(HWND)> stage);
	static void onRelease(std::function<void(const QList<HWND>&)> release);
	static void dismissAll();
};

}
