#pragma once
#include <functional>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>

namespace qs::windows {

bool hasWindowBackdrop(HWND hwnd);
HWND windowStill(HWND window, const QRect& raw, const QRect& visible, float radius);
class WindowMotion: public QObject {
	Q_OBJECT;

public:
	static WindowMotion* instance();
	Q_DISABLE_COPY_MOVE(WindowMotion);

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
	HWND windowAt(POINT point);
	QRect stopAt(HWND hwnd);
	[[nodiscard]] bool moving(HWND hwnd) const;
	[[nodiscard]] static bool travelsAsStill(HWND hwnd);

signals:
	void landed(HWND hwnd, QRect frame);

private:
	explicit WindowMotion(QObject* parent);
	~WindowMotion() override;

	struct Entry {
		HWND hwnd = nullptr;
		HTHUMBNAIL thumbnail = nullptr;
		HWND snapshot = nullptr;
		QSize snapshotSize;
		QRect from;
		QRect to;
		QRect current;
		QRect parked;
		bool pop = false;
		std::chrono::steady_clock::time_point start;
		bool waiting = true;
		int frames = 0;
		int lateFrames = 0;
		int resent = 0;
		bool painted = false;
		bool caughtUp = false;
		bool live = false;
		bool overlay = true;
		QSize realSize;
		bool done = false;
		int depth = 0;
		double progress = 0;
		bool retargeted = false;
		HWND hold = nullptr;
		HTHUMBNAIL holdThumbnail = nullptr;
		QPoint holdOffset;
		QSize holdSize;
	};

	bool ensureCover();
	void placeCover(HWND above);
	void closeCover();
	void run();
	void onFrameDone();
	void layout(Entry& entry, double progress);
	void restack();
	static void moveTo(HWND hwnd, const QRect& frame, bool wait);
	static void slideTo(HWND hwnd, const QPoint& topLeft);
	static void clipTo(HWND hwnd, const QSize& visible);
	static void release(const Entry& entry);

	HWND cover = nullptr;
	QRect coverRect;
	QList<Entry> entries;
	QList<Entry> lingering;
	QList<Entry> stills;
	mutable std::mutex mutex;
	std::thread worker;
	std::atomic<bool> stop = false;
	std::atomic<bool> donePending = false;
};

class WindowPop {
public:
	static void popIn(HWND hwnd);
	static int popOut(HWND hwnd);
	static void start(HWND hwnd, bool in);
	static bool closeOut(HWND hwnd, const std::function<void()>& close);
	static void popInAt(HWND hwnd, const QRect& frame, const std::function<void()>& land);
};

}
