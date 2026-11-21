#pragma once

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
#include <qtimer.h>
#include <qtypes.h>

namespace qs::windows {

void waitForCompositorFrame();
double compositorRefreshMs();

class DesktopSlide: public QObject {
	Q_OBJECT;

public:
	static DesktopSlide* instance();
	Q_DISABLE_COPY_MOVE(DesktopSlide);

	bool begin(qsizetype from, qsizetype to);

	void swipeUpdate(double offset);
	void swipeEnd(double velocity);

private:
	explicit DesktopSlide(QObject* parent);
	~DesktopSlide() override;

	struct Thumb {
		HTHUMBNAIL thumbnail = nullptr;
		QRect rect;
		qsizetype desktop = -1;
		HWND window = nullptr;
		QRect visible;
		QRect source;
		bool fullscreen = false;
	};

	bool start(qsizetype from, qsizetype to);
	void retarget(qsizetype to);
	bool addLiveDesktop(qsizetype desktop);
	void warmLiveThumbs();
	bool prepareLiveOverlay(const QRect& monitor, const QList<HWND>& windows);
	HTHUMBNAIL liveThumbnail(HWND window);
	void clearLiveOverlay();
	void releaseThumb(const Thumb& thumb);
	bool carryPanels();
	void liftFullscreen(qsizetype desktop = -1);
	void prepareBackdrop();
	void releaseBackdrop();
	void run();
	void layoutThumbs(double camera);
	void onSettled();
	void onCommit();
	void setClickThrough(bool through);
	void finish();
	void placeUnderPanels();

	HWND overlay = nullptr;
	HWND liveOverlay = nullptr;
	QRect liveMonitor;
	QList<HWND> liveBackdropSources;
	QHash<HWND, HTHUMBNAIL> liveThumbnails;
	QList<HWND> liveThumbnailOrder;
	QTimer liveWarmTimer;
	bool liveWarmPending = false;
	QRect monitor;
	bool panelsCarried = false;
	int travel = 0;

	std::mutex mutex;
	QList<Thumb> thumbs;
	QHash<qsizetype, int> bases;
	qsizetype target = -1;
	double cameraFrom = 0;
	double cameraTo = 0;
	std::chrono::steady_clock::time_point startedAt;
	bool settledSent = false;
	bool commitSent = false;
	bool manual = false;
	double manualCamera = 0;
	bool swiping = false;
	qsizetype swipeFrom = -1;
	double swipeOffset = 0;

	std::thread worker;
	std::atomic<bool> stop = false;
	int settleRetries = 0;
	QList<HWND> raised;
	HDC wallDc = nullptr;
	HBITMAP wallBitmap = nullptr;
};

}
