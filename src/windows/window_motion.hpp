#pragma once
#include <functional>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>

namespace qs::windows {

bool hasWindowBackdrop(HWND hwnd);
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
	// With pop, the window pops in at `to` (end-4's windowsIn: from 80 % with a
	// fade) instead of sliding from `from`. With still, it travels as a copy of
	// how it looked at `from`, for an app that won't paint while it waits.
	bool begin(
	    HWND hwnd,
	    const QRect& from,
	    const QRect& to,
	    const QRect& parked,
	    bool pop = false,
	    bool still = false
	);
	void retarget(HWND hwnd, const QRect& to, const QRect& parked);
	void cancel(HWND hwnd);
	[[nodiscard]] bool moving(HWND hwnd) const;
	// Apps that stop painting off screen (Chromium) travel as a still taken
	// before they leave, at the size they had; the rest take their new size
	// right away and their live thumbnail shows it, as in Hyprland.
	[[nodiscard]] static bool travelsAsStill(HWND hwnd);

signals:
	// The window is in place at `frame`; tiling takes it back from here.
	void landed(HWND hwnd, QRect frame);

private:
	explicit WindowMotion(QObject* parent);
	~WindowMotion() override;

	struct Entry {
		HWND hwnd = nullptr;
		HTHUMBNAIL thumbnail = nullptr;
		// A still of the window, for apps that stop painting off screen.
		HWND snapshot = nullptr;
		// The still's size: its picture is always shown whole.
		QSize snapshotSize;
		QRect from;
		QRect to;
		QRect current;
		QRect parked;
		bool pop = false;
		std::chrono::steady_clock::time_point start;
		// Until the window has actually left, the thumbnail holds still over it.
		bool waiting = true;
		bool done = false;
		// Windows above it when it set off, and how far along it is: the
		// thumbnails are stacked as the windows were.
		int depth = 0;
		double progress = 0;
		// How it looked when it set off, shown over its live picture for its
		// first frames: resized under the cover, the app presented an empty
		// frame before it repainted, and a translucent one went see-through.
		HWND hold = nullptr;
		HTHUMBNAIL holdThumbnail = nullptr;
		std::chrono::steady_clock::time_point holdStart;
		// Given a new target on its way: resized while nearly all off screen.
		bool retargeted = false;
	};

	bool ensureCover();
	void placeCover(HWND above);
	void closeCover();
	void run();
	void onFrameDone();
	void layout(Entry& entry, double progress);
	void restack();
	void placeCoverUnderPanels();
	void updateHoles();
	static void moveTo(HWND hwnd, const QRect& frame, bool wait);
	static void release(const Entry& entry);
	static void releaseHold(const Entry& entry);

	HWND cover = nullptr;
	QRect coverRect;
	QList<Entry> entries;
	// Stills that stay over their landed window until it has painted again.
	QList<Entry> lingering;
	// Thumbnails of the windows that stay put, drawn with the moving ones.
	QHash<HWND, HTHUMBNAIL> scene;
	// Live pictures still over their landed windows.
	int landingLive = 0;
	// Those pictures, until they join the scene or go.
	QList<Entry> handoff;
	mutable std::mutex mutex;
	std::thread worker;
	std::atomic<bool> stop = false;
	std::atomic<bool> donePending = false;
};

// end-4's windowsIn and windowsOut (popin 80% / 90% with a fade) for the
// shell's own windows, such as settings. The window is cloaked (it is ours, so
// DWMWA_CLOAK is allowed) while a thumbnail of it scales and fades on a small
// cover above it; popping in, the window shows again once the thumbnail lands.
class WindowPop {
public:
	// Hides the window now; pops it in once it has painted, or after a moment.
	static void popIn(HWND hwnd);
	// Pops the window out and leaves it cloaked; returns the duration in ms.
	static int popOut(HWND hwnd);
	// Starts the pop-in of a window cloaked by popIn.
	static void start(HWND hwnd, bool in);
	// end-4's windowsOut for an app window: a still of it pops out over it while
	// `close` closes it. False if it couldn't, and then `close` hasn't run.
	static bool closeOut(HWND hwnd, const std::function<void()>& close);
	// end-4's windowsIn for an app window waiting off screen at the size of
	// `frame`: a live thumbnail of it pops in at `frame`, then `land` puts the
	// window there.
	static void popInAt(HWND hwnd, const QRect& frame, const std::function<void()>& land);
};

} // namespace qs::windows
