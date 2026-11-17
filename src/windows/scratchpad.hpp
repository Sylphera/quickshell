#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qrect.h>
#include <qtclasshelpermacros.h>
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

	void toggle();
	void add(TrackedWindow* window, bool follow);
	void remove(TrackedWindow* window);

	static void restoreForCrash();

signals:
	void shownChanged();

private:
	explicit Scratchpad(QObject* parent);
	~Scratchpad() override = default;

	struct Entry {
		QPointer<TrackedWindow> window;
		HWND hwnd = nullptr;
		QRect rect;
	};

	void setShown(bool shown);
	void park(Entry& entry);
	void unpark(Entry& entry, bool front);
	void releaseAll();
	void prune();

	QList<Entry> entries;
	bool mShown = false;
	HMONITOR mMonitor = nullptr;
};

} // namespace qs::windows
