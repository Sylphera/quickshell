#pragma once

#include <qt_windows.h>
#include <chrono>
#include <memory>

#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qrect.h>
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
	void slide(bool in);
	void stepSlide();
	void finishHide();
	[[nodiscard]] int slideDistance() const;
	void park(Entry& entry);
	void releaseAll();
	void prune();
	void showDim();
	void hideDim();
	void stepDim();
	void relayout();
	void onWindowAdded(TrackedWindow* window);
	void onWindowRemoved(TrackedWindow* window);
	void onActiveWindowChanged();
	static LRESULT CALLBACK dimProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	QList<Entry> entries;
	bool mShown = false;
	HMONITOR mMonitor = nullptr;
	ULONGLONG shownAt = 0;
	HWND dim = nullptr;
	int dimAlpha = 0;
	int dimTarget = 0;
	QTimer dimTimer;
	QTimer slideTimer;
	std::chrono::steady_clock::time_point slideStart;
	double slideFrom = 0;
	double slideTo = 0;
	bool slideIn = true;
	bool hiding = false;
	int slideOffset = 0;
};

} // namespace qs::windows
