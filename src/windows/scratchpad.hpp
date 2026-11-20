#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qrect.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

class TrackedWindow;

class Scratchpad: public QObject {
	Q_OBJECT;

public:
	static Scratchpad* instance();
	Q_DISABLE_COPY_MOVE(Scratchpad);

	[[nodiscard]] bool shown() const { return this->mShown; }
	[[nodiscard]] HMONITOR monitor() const { return this->mMonitor; }
	[[nodiscard]] bool contains(const TrackedWindow* window) const;
	[[nodiscard]] static bool holds(const TrackedWindow* window);

	void toggle();
	void add(TrackedWindow* window, bool follow);
	void remove(TrackedWindow* window);

	static void restoreForCrash();

signals:
	void shownChanged();
	void membershipChanged();

private:
	explicit Scratchpad(QObject* parent);
	~Scratchpad() override;

	struct Entry {
		QPointer<TrackedWindow> window;
		HWND hwnd = nullptr;
		QRect rect;
	};

	void setShown(bool shown);
	void hideNow();
	void slide(bool in);
	void closeCover();
	void placeCover();
	void layoutCover(double offset);
	void runSlide();
	void onSlideDone();
	void waitForTakeoff(int generation, int tries);
	void moveReal(int offset);
	void jumpReal(int offset);
	void openCover(bool show);
	void finishHide();
	[[nodiscard]] int slideDistance() const;
	void park(Entry& entry);
	void releaseAll();
	void prune();
	void showDim();
	void hideDim();
	void stepDim();
	void relayout();
	[[nodiscard]] HWND focusCandidate() const;
	void onWindowAdded(TrackedWindow* window);
	void onWindowRemoved(TrackedWindow* window);
	void onActiveWindowChanged();
	static LRESULT CALLBACK dimProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Entry> entries;
	bool mShown = false;
	qsizetype openedDesktop = -1;
	HMONITOR mMonitor = nullptr;
	ULONGLONG shownAt = 0;
	HWND dim = nullptr;
	int dimAlpha = 0;
	int dimTarget = 0;
	QTimer dimTimer;
	struct CoverThumb {
		HWND hwnd = nullptr;
		HTHUMBNAIL thumbnail = nullptr;
		QRect frame; // where the window rests, in screen coordinates
	};
	HWND cover = nullptr;
	QPoint coverOrigin;
	QList<CoverThumb> coverThumbs;
	std::mutex slideMutex;
	std::thread slideWorker;
	std::atomic<bool> slideStop = false;
	bool slideDoneSent = false;
	int slideGeneration = 0;
	int realOffset = 0;
	std::chrono::steady_clock::time_point slideStart;
	double slideFrom = 0;
	double slideTo = 0;
	bool slideIn = true;
	bool hiding = false;
	bool preparing = false;
	int slideOffset = 0;
	int slideSpan = 0;
	[[nodiscard]] bool slideDrivesDim() const;
};

} // namespace qs::windows
