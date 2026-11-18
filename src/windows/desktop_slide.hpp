#pragma once

#include <qt_windows.h>
#include <atomic>
#include <thread>

#include <dwmapi.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qtclasshelpermacros.h>
#include <qtypes.h>

namespace qs::windows {

// Hyprland slides workspaces in its compositor. Windows switches virtual
// desktops at once, so this covers the switch with live DWM thumbnails of the
// wallpaper and of both desktops' windows, slid under the shell's own panels.
class DesktopSlide: public QObject {
	Q_OBJECT;

public:
	static DesktopSlide* instance();
	Q_DISABLE_COPY_MOVE(DesktopSlide);

	// Shows the cover for a switch from one desktop index to another. Returns
	// false when nothing was set up and the switch should just happen.
	bool begin(qsizetype from, qsizetype to);
	[[nodiscard]] bool running() const { return this->mRunning.load(); }

private:
	explicit DesktopSlide(QObject* parent);
	~DesktopSlide() override;

	struct Thumb {
		HTHUMBNAIL thumbnail = nullptr;
		QRect rect;
		int lane = 0;
	};

	void finish();
	void run(int direction, int travel, qint64 durationMs);
	void raisePanels();

	HWND overlay = nullptr;
	QRect monitor;
	QList<Thumb> thumbs;
	std::thread worker;
	std::atomic<bool> mRunning = false;
};

} // namespace qs::windows
