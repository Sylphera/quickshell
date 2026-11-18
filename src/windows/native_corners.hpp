#pragma once

#include <array>

#include <qt_windows.h>

#include <qcolor.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qscreen.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>

namespace qs::windows {

///! The screen's fake rounded corners and its corner hot zones, natively.
/// end-4 draws each corner of each screen as its own layer surface. On Windows
/// each Qt window brings a whole render context along (about 19 MB), so the
/// four corners cost more than the video wallpaper. These are four tiny
/// layered windows painted once, receiving the mouse only on the hot zones.
/// Corners follow RoundCorner.CornerEnum: TopLeft, TopRight, BottomLeft,
/// BottomRight.
class NativeScreenCorners: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	/// Name of the screen, as QScreen and Quickshell report it.
	Q_PROPERTY(QString screenName READ screenName WRITE setScreenName NOTIFY changed);
	/// Rounding radius, in logical pixels.
	Q_PROPERTY(qint32 radius READ radius WRITE setRadius NOTIFY changed);
	Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY changed);
	/// Whether the rounding is drawn.
	Q_PROPERTY(bool shown READ shown WRITE setShown NOTIFY changed);
	/// Whether the hot zones take the mouse.
	Q_PROPERTY(bool hotEnabled READ hotEnabled WRITE setHotEnabled NOTIFY changed);
	/// Hot zones on the bottom corners instead of the top ones.
	Q_PROPERTY(bool hotBottom READ hotBottom WRITE setHotBottom NOTIFY changed);
	Q_PROPERTY(qint32 hotWidth READ hotWidth WRITE setHotWidth NOTIFY changed);
	Q_PROPERTY(qint32 hotHeight READ hotHeight WRITE setHotHeight NOTIFY changed);
	/// Extend the right and bottom corners one pixel past the screen.
	Q_PROPERTY(bool deadPixel READ deadPixel WRITE setDeadPixel NOTIFY changed);

public:
	explicit NativeScreenCorners(QObject* parent = nullptr);
	~NativeScreenCorners() override;
	Q_DISABLE_COPY_MOVE(NativeScreenCorners);

	[[nodiscard]] QString screenName() const { return this->mScreenName; }
	void setScreenName(const QString& name);
	[[nodiscard]] qint32 radius() const { return this->mRadius; }
	void setRadius(qint32 radius);
	[[nodiscard]] QColor color() const { return this->mColor; }
	void setColor(const QColor& color);
	[[nodiscard]] bool shown() const { return this->mShown; }
	void setShown(bool shown);
	[[nodiscard]] bool hotEnabled() const { return this->mHotEnabled; }
	void setHotEnabled(bool enabled);
	[[nodiscard]] bool hotBottom() const { return this->mHotBottom; }
	void setHotBottom(bool bottom);
	[[nodiscard]] qint32 hotWidth() const { return this->mHotWidth; }
	void setHotWidth(qint32 width);
	[[nodiscard]] qint32 hotHeight() const { return this->mHotHeight; }
	void setHotHeight(qint32 height);
	[[nodiscard]] bool deadPixel() const { return this->mDeadPixel; }
	void setDeadPixel(bool deadPixel);

signals:
	void changed();
	/// The pointer came onto a corner's hot zone.
	void entered(qint32 corner);
	/// The pointer moved within a hot zone, in logical pixels from the zone's
	/// top left; width and height are the zone's.
	void moved(qint32 corner, qreal x, qreal y, qreal width, qreal height);
	void pressed(qint32 corner);
	/// Positive steps scroll up.
	void scrolled(qint32 corner, qint32 steps);
	void left(qint32 corner);

private:
	struct Corner {
		HWND hwnd = nullptr;
		QRect hot; // in the window's own pixels
		bool tracking = false;
	};

	void scheduleRebuild();
	void rebuild();
	void destroyWindows();
	[[nodiscard]] bool isHot(int corner) const;
	static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	LRESULT handle(int corner, UINT msg, WPARAM wParam, LPARAM lParam);

	QString mScreenName;
	QPointer<QScreen> screen;
	qint32 mRadius = 25;
	QColor mColor = Qt::black;
	bool mShown = false;
	bool mHotEnabled = false;
	bool mHotBottom = false;
	qint32 mHotWidth = 250;
	qint32 mHotHeight = 5;
	bool mDeadPixel = false;
	qreal scale = 1;
	std::array<Corner, 4> corners {};
	QTimer rebuildTimer;
	QTimer topmostTimer;
};

} // namespace qs::windows
