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
#include <qset.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

#include "inactive_dim.hpp"
#include "scratchpad_phase.hpp"

namespace qs::windows {

class TrackedWindow;

class Scratchpad: public QObject {
	Q_OBJECT;

public:
	static Scratchpad* instance();
	Q_DISABLE_COPY_MOVE(Scratchpad);

	[[nodiscard]] bool shown() const { return this->phase != ScratchpadPhase::Hidden; }
	[[nodiscard]] bool moving() const {
		return this->shown() && this->phase != ScratchpadPhase::Shown;
	}
	[[nodiscard]] HMONITOR monitor() const { return this->mMonitor; }
	[[nodiscard]] HWND dimWindow() const { return this->dim; }
	[[nodiscard]] int dimLevel() const { return this->dimAlpha; }
	[[nodiscard]] bool contains(const TrackedWindow* window) const;
	[[nodiscard]] bool containsWindow(HWND hwnd) const;
	[[nodiscard]] static bool holds(const TrackedWindow* window);
	static void raiseWindow(HWND hwnd);

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

	bool handle(ScratchpadEvent event);
	void prepare();
	void enter();
	void leave();
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
	void placeDim();
	void order();
	[[nodiscard]] QList<HWND> layerChain() const;
	void focusBelow();
	void onForeground(HWND hwnd);
	void dropLifted();
	void hideDim();
	void stepDim();
	void relayout();
	[[nodiscard]] HWND focusCandidate() const;
	void onWindowAdded(TrackedWindow* window);
	void onWindowRemoved(TrackedWindow* window);
	void onActiveWindowChanged();
	static LRESULT CALLBACK dimProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Entry> entries;
	ScratchpadPhase phase = ScratchpadPhase::Hidden;
	qsizetype openedDesktop = -1;
	HMONITOR mMonitor = nullptr;
	ULONGLONG shownAt = 0;
	HWND dim = nullptr;
	int dimAlpha = 0;
	int dimTarget = 0;
	QTimer dimTimer;
	QTimer orderTimer;
	QSet<HWND> lifted;
	int orderRetries = 0;
	struct CoverThumb {
		HWND hwnd = nullptr;
		HTHUMBNAIL thumbnail = nullptr;
		QRect frame;
		HTHUMBNAIL shade = nullptr;
		DimLevel level;
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
	int slideOffset = 0;
	int slideSpan = 0;
	[[nodiscard]] bool slideDrivesDim() const;
};

}
