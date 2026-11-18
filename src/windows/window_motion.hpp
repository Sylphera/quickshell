#pragma once

#include <qt_windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qtclasshelpermacros.h>

namespace qs::windows {

// end-4's windowsMove (slide, 300 ms, emphasizedDecel) for tiled windows.
// Windows can't hide another app's window with a documented API, so while a
// window travels it waits off screen at its new size and a DWM thumbnail of it
// slides from where it was to where it goes. When the thumbnail arrives, the
// window jumps into place in the same frame the thumbnail goes away. A new
// target in the middle of a slide carries on from where the thumbnail is.
class WindowMotion: public QObject {
	Q_OBJECT;

public:
	static WindowMotion* instance();
	Q_DISABLE_COPY_MOVE(WindowMotion);

	// Frames are DWM extended frame bounds, as tiling uses them. `parked` is
	// where the window waits; the caller has already decided it may animate.
	bool begin(HWND hwnd, const QRect& from, const QRect& to, const QRect& parked);
	void retarget(HWND hwnd, const QRect& to, const QRect& parked);
	void cancel(HWND hwnd);
	[[nodiscard]] bool moving(HWND hwnd) const;

signals:
	// The window is in place at `frame`; tiling takes it back from here.
	void landed(HWND hwnd, QRect frame);

private:
	explicit WindowMotion(QObject* parent);
	~WindowMotion() override;

	struct Entry {
		HWND hwnd = nullptr;
		HTHUMBNAIL thumbnail = nullptr;
		QRect from;
		QRect to;
		QRect current;
		QRect parked;
		std::chrono::steady_clock::time_point start;
		// Until the window has actually left, the thumbnail holds still over it.
		bool waiting = true;
		bool done = false;
	};

	bool ensureCover();
	void placeCover(HWND above);
	void closeCover();
	void run();
	void onFrameDone();
	void layout(Entry& entry, double progress);
	static void moveTo(HWND hwnd, const QRect& frame, bool wait);

	HWND cover = nullptr;
	QRect coverRect;
	QList<Entry> entries;
	mutable std::mutex mutex;
	std::thread worker;
	std::atomic<bool> stop = false;
	std::atomic<bool> donePending = false;
};

} // namespace qs::windows
