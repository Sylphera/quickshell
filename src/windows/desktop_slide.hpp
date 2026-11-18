#pragma once

#include <qt_windows.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <dwmapi.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qrect.h>
#include <qtclasshelpermacros.h>
#include <qtypes.h>

namespace qs::windows {

// Hyprland slides workspaces in its compositor. Windows switches virtual
// desktops at once, so this covers the switch with live DWM thumbnails of the
// wallpaper and of the desktops' windows, slid under the shell's own panels.
// Desktops sit on a strip and a camera moves along it, so a new switch in the
// middle of a slide just gives the camera a new target.
class DesktopSlide: public QObject {
	Q_OBJECT;

public:
	static DesktopSlide* instance();
	Q_DISABLE_COPY_MOVE(DesktopSlide);

	void begin(qsizetype from, qsizetype to);

private:
	explicit DesktopSlide(QObject* parent);
	~DesktopSlide() override;

	struct Thumb {
		HTHUMBNAIL thumbnail = nullptr;
		QRect rect;
		QRect source;
		qsizetype desktop = -1;
	};

	bool start(qsizetype from, qsizetype to);
	void retarget(qsizetype to);
	void addDesktop(qsizetype desktop);
	void run();
	void layoutThumbs(double camera);
	void onSettled();
	void finish();
	void placeUnderPanels();

	HWND overlay = nullptr;
	QRect monitor;
	int travel = 0;

	std::mutex mutex;
	QList<Thumb> thumbs;
	QHash<qsizetype, int> bases;
	qsizetype target = -1;
	double cameraFrom = 0;
	double cameraTo = 0;
	std::chrono::steady_clock::time_point startedAt;
	bool settledSent = false;

	std::thread worker;
	std::atomic<bool> stop = false;
	int settleRetries = 0;
	QList<HWND> raised;
};

} // namespace qs::windows
