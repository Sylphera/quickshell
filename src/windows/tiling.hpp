#pragma once

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include <qt_windows.h>

#include <qdeadlinetimer.h>
#include <qlist.h>
#include <qmargins.h>
#include <qhash.h>
#include <qobject.h>
#include <qpointer.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include "dwindle_scene.hpp"
#include "tile_state.hpp"
#include "tiling_layout.hpp"

namespace qs::windows {

class TrackedWindow;
class WindowTracker;
class VirtualDesktops;

class TilingManager: public QObject {
	Q_OBJECT;

public:
	using Edge = tiling::Edge;

	static TilingManager* instance();
	static TilingManager* active();
	void markWindow(TrackedWindow* window);
	void prepareClose(TrackedWindow* window);
	void workAreaChanged() { this->markAll(); }
	[[nodiscard]] TrackedWindow* tileAt(const QPoint& point);
	void setSpecialOffset(int offset);
	[[nodiscard]] int specialOffset() const { return this->mSpecialOffset; }
	[[nodiscard]] QHash<HWND, QRect> specialFrames();

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);
	[[nodiscard]] bool suppressWindowShadows() const { return this->mSuppressWindowShadows; }
	void setSuppressWindowShadows(bool suppress);
	[[nodiscard]] bool animateMoves() const { return this->mAnimateMoves; }
	void setAnimateMoves(bool animate);
	[[nodiscard]] qint32 gapsIn() const { return this->mGapsIn; }
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const { return this->mGapsOut; }
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const { return this->mPreserveSplit; }
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const { return this->mExcluded; }
	[[nodiscard]] QStringList fullscreenScreens() const { return this->mFullscreenScreens; }
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QString layout() const { return this->mLayout; }
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const { return this->mSmartSplit; }
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const { return this->mForceSplit; }
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const { return this->mDefaultSplitRatio; }
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const { return this->mMasterNewStatus; }
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const { return this->mMasterNewOnTop; }
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const { return this->mMasterOrientation; }
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const { return this->mMasterFactor; }
	void setMasterFactor(double factor);
	[[nodiscard]] double scrollingColumnWidth() const { return this->mScrollingColumnWidth; }
	void setScrollingColumnWidth(double width);
	[[nodiscard]] QList<double> scrollingPresets() const { return this->mScrollingPresets; }
	void setScrollingPresets(const QList<double>& presets);
	[[nodiscard]] QString scrollingCenter() const { return this->mScrollingCenter; }
	void setScrollingCenter(const QString& center);

	static void restoreForCrash();

	void relayout();
	void expectBarSpace(const QString& screenName, bool top, int reserved);

	[[nodiscard]] bool isTiled(TrackedWindow* window) const;
	[[nodiscard]] bool allowsHoverFocus(TrackedWindow* window) const;
	[[nodiscard]] bool occupiesDwindleTile(HWND hwnd) const;
	[[nodiscard]] TrackedWindow* focusedWindow() const;
	[[nodiscard]] TrackedWindow* fullscreenShortcutTarget(TrackedWindow* focused) const;

	bool focusDirection(Edge direction);
	bool moveDirection(Edge direction);
	bool swapDirection(Edge direction);
	bool toggleSplit(TrackedWindow* window);
	bool swapSplit(TrackedWindow* window);
	bool splitRatio(TrackedWindow* window, double value, bool exact);
	bool masterRatio(TrackedWindow* window, double value, bool exact);
	bool swapWithMaster(TrackedWindow* window);
	bool focusMaster(TrackedWindow* window);
	bool resizeTiled(TrackedWindow* window, qint32 dx, qint32 dy);
	bool setFloating(TrackedWindow* window, bool floating, bool toggle);

	bool scrollColumns(TrackedWindow* window, qint32 delta);
	bool scrollView(TrackedWindow* window, qint32 pixels);
	bool columnWidth(TrackedWindow* window, double value, bool relative);
	bool cycleColumnWidth(TrackedWindow* window, qint32 direction);
	bool allColumnsWidth(TrackedWindow* window, double value);
	bool fitColumns(TrackedWindow* window, tiling::FitMode mode);
	bool focusColumn(TrackedWindow* window, Edge direction);
	bool focusColumnAt(POINT point, qint32 steps);
	bool moveIntoColumn(TrackedWindow* window, Edge direction);
	bool swapColumn(TrackedWindow* window, Edge direction);
	bool promote(TrackedWindow* window);
	bool centerColumn(TrackedWindow* window);
	bool toggleFit(TrackedWindow* window);
	bool toggleFullWidth(TrackedWindow* window);
	bool toggleMaximize(TrackedWindow* window);
	bool toggleFullscreen(TrackedWindow* window, bool follow = true);
	[[nodiscard]] TrackedWindow* specialFullscreenWindow() const;
	[[nodiscard]] TrackedWindow* specialMaximizedWindow() const;
	[[nodiscard]] TrackedWindow* specialCoveringWindow() const;
	[[nodiscard]] bool covers(TrackedWindow* window) const;
	[[nodiscard]] bool floatsInSpecial(TrackedWindow* window) const;
	[[nodiscard]] bool overSpecialCover(TrackedWindow* window) const;
	QRect settleFloat(TrackedWindow* window);
	[[nodiscard]] bool canPin(TrackedWindow* window) const;
	void updateShellFullscreen();
	void warnFullscreenConflict(TrackedWindow* source, qsizetype desktop = -1);
	void restoreLeftFullscreen(TrackedWindow* window);

	bool detachTile(TrackedWindow* window);
	bool dropOverviewTile(TrackedWindow* window, const QPointF& normalized);
	[[nodiscard]] QRect overviewTileRect(TrackedWindow* window) const;
	bool beginGestureMove(TrackedWindow* window);
	[[nodiscard]] bool coversScreen(TrackedWindow* window) const;
	void endGestureMove(TrackedWindow* window, std::optional<Edge> direction);
	bool beginEdgeDrag(TrackedWindow* window, bool left, bool top);
	void haltMotion(HWND hwnd, bool resize);
	void edgeDrag(const QPoint& delta);
	void endEdgeDrag();

	void resizeFloating(TrackedWindow* window, qint32 dx, qint32 dy, bool exact);
	void center(TrackedWindow* window);
	void moveFloating(TrackedWindow* window, Edge direction);

signals:
	void enabledChanged();
	void suppressWindowShadowsChanged();
	void animateMovesChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void fullscreenScreensChanged();
	void fullscreenConflict(const QString& appId, const QString& title, bool exclusive);
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
	void scrollingColumnWidthChanged();
	void scrollingPresetsChanged();
	void scrollingCenterChanged();
	void windowTilingChanged(TrackedWindow* window);

private:
	explicit TilingManager();
	~TilingManager() override = default;
	Q_DISABLE_COPY_MOVE(TilingManager);

	struct Layout {
		GUID desktop {};
		QPointer<QScreen> screen;
		std::unique_ptr<tiling::LayoutEngine> tree;
		TrackedWindow* lastFocused = nullptr;
		bool special = false;
		bool animateRearrangement = false;
		QDeadlineTimer nextOpening;
	};

	enum class State : quint8 {
		Out,
		Hidden,
		Suspended,
		Tiled,
	};

	enum class Override : quint8 { None, Float, Tile };

	struct EdgeDrag {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		QRect box;
		std::optional<Edge> horizontal;
		std::optional<Edge> vertical;
	};

	struct Managed {
		TrackedWindow* window = nullptr;
		Layout* layout = nullptr;
		Override override = Override::None;
		bool elevated = false;
		bool autoFloat = false;
		QRect target;
		QRect openingFrame;
		QRect before;
		QRect accepted;
		qint32 corrections = 0;
		qint64 lastCorrection = 0;
		bool verifying = false;
		bool wasHung = false;
		qint64 deadline = 0;
		bool parked = false;
		QRect preTiling;
		quint64 focusStamp = 0;
		bool offscreen = false;
		QRect restore;
		bool tinyCornersApplied = false;
		bool fresh = false;
		bool startupMove = false;
		bool appFullscreen = false;
		bool maximizedTile = false;
		bool maximizePending = false;
		qint64 managedAt = 0;
		bool tinySmallCorners = false;
		bool tinyChromiumCorners = false;
		bool nativeFrameRefreshRequested = false;
		bool tinyBorderSuppressed = false;
		int previousCornerPreference = 0;
		QRect tinyCornersRect;
		QMargins tinyFrameInsets;
		bool dropped = false;
		QRect fullscreenRestore;
		bool fullscreenWasMaximized = false;
		bool fullscreenToggled = false;
		bool fullscreenLookPending = false;
		std::optional<float> fullscreenTileRadius;
		TileMode mode = TileMode::Normal;
		TilePlacement placement = TilePlacement::Placed;
		bool stageNoted = false;
		qint64 unplacedSince = 0;
		QRect floatFrame;
		bool floatPending = false;
		bool floatMoving = false;
		bool floatOpening = false;
		bool wasTopmost = false;
		int unmaximized = 0;
		qint64 unmaximizedAt = 0;
		bool floated = false;
		QRect floatSeen;
		qint64 coveredAt = 0;

		[[nodiscard]] bool focusMaximized() const { return this->mode == TileMode::Maximized; }
		[[nodiscard]] bool fullscreen() const { return this->mode == TileMode::Fullscreen; }
		[[nodiscard]] bool staged() const { return isStaged(this->placement); }
		[[nodiscard]] bool contractionHeld() const { return isHeld(this->placement); }
	};

	bool start();
	void stop();
	void manage(TrackedWindow* window);
	void unmanage(TrackedWindow* window);

	[[nodiscard]] State stateOf(const Managed& m) const;
	[[nodiscard]] bool floatsByRule(const Managed& m) const;
	[[nodiscard]] bool isExcluded(TrackedWindow* window) const;
	[[nodiscard]] Layout* layoutFor(TrackedWindow* window, bool create);
	[[nodiscard]] Layout* layoutAt(const GUID& desktop, QScreen* screen, bool create);
	[[nodiscard]] bool layoutAlive(const Layout* layout) const;
	[[nodiscard]] bool sceneShows(const Layout* layout, int index) const;
	[[nodiscard]] static tiling::LayoutEngine::Id idOf(TrackedWindow* window);
	[[nodiscard]] TrackedWindow* windowOf(tiling::LayoutEngine::Id id) const;
	[[nodiscard]] tiling::Kind layoutKind() const;
	[[nodiscard]] tiling::Options options() const;
	void rebuildLayouts();

	void markLayout(Layout* layout);
	void schedule();
	void updateEarlyParking();
	void sync();
	void syncWindow(TrackedWindow* window);
	static bool ignoreMaximize(Managed& m);
	void insertInto(Managed& m, Layout* layout, TrackedWindow* target, const QPoint* cursor);
	void takeOut(Managed& m);
	void clearMaximized(Layout* layout, const TrackedWindow* except, bool animate);
	void dropEmptyLayouts();
	void apply(Layout* layout);
	void openStartupLayouts();
	QSet<Layout*> startupLayouts;
	bool dwindlePlan(Layout* layout, const QRect& work, QList<DwindleTile>& plan, bool& changed, bool still);
	[[nodiscard]] bool floatsFreely(TrackedWindow* window) const;
	[[nodiscard]] bool floats(const Managed& m) const;
	[[nodiscard]] Layout* floatHome(const Managed& m, bool create);
	[[nodiscard]] QList<Managed*> sceneFloats(Layout* layout);
	[[nodiscard]] QRect floatGoal(const Managed& m, const Layout* layout) const;
	void coverOthers(Managed& m);
	void markCovering();
	void sendFloat(Managed& m);
	bool openFloat(Managed& m);
	void floatTo(Managed& m, const QRect& raw);
	void prepareFloatClose(Managed& m);
	bool closeFloat(Managed& m);
	[[nodiscard]] std::optional<QRect> floatBase(TrackedWindow* window) const;
	[[nodiscard]] QRect floatArea(TrackedWindow* window, const QRect& around) const;
	void placeFloat(TrackedWindow* window, const QRect& frame);
	static void dropTopmost(Managed& m);
	static void restoreTopmost(Managed& m);
	void restoreFloat(Managed& m);
	void forgetFloat(Managed& m);
	void uncoverFor(const Managed& m);
	bool toggleFloatMaximize(Managed& m);
	void applyFloats(Layout* layout, bool presenting);
	void coverNewWindow(HWND hwnd);
	void stage(Managed& m, const QRect& tile);
	void finishMaximize(Managed& m);
	bool holdContraction(Managed& m, const QRect& tile);
	bool stageOccluding(HWND hwnd);
	void releaseStaged(const QList<HWND>& windows);
	void leaveFullscreen(Managed& m);
	void updateFullscreenLook(Managed& m);
	void yieldFullscreenTo(Managed& m);
	[[nodiscard]] QRect tileRect(const Layout* layout, TrackedWindow* window, bool fillLayout = false) const;
	void place(Managed& m, const QRect& tile);
	void hold(Managed& m, const QRect& tile);
	void cacheNextCloseFrame();
	void updateTinyCorners(Managed& m, const QRect& frame);
	void restoreTinyCorners(Managed& m);
	void reassertTinyDecorations();
	[[nodiscard]] QRect managedFrame(const Managed& m) const;
	bool needsNativeFrameRefresh(Managed& m);
	void send(Managed& m, const QRect& frame, bool refreshNativeFrame = false);
	void armDeadline();
	void restoreFloating(Managed& m, bool besideTile = false);
	[[nodiscard]] static QRect floatingRect(const QRect& tile, QSize size, const QRect& work);
	void restack();
	void collectLayers(QSet<HWND>& blocking, QSet<HWND>& tiled, QSet<HWND>& skipped) const;

	[[nodiscard]] static tiling::ScrollingLayout* scrollingOf(const Layout* layout);
	[[nodiscard]] tiling::ScrollingLayout* scrollingFor(TrackedWindow* window, Layout** layout);
	void prepareScrolling(Layout* layout) const;
	void settle(Layout* layout);
	void refocus(Layout* layout);
	bool focusIdleStrip();
	void syncWheel() const;
	void markOffscreen(Managed& m, bool offscreen, const QRect& restore);
	void unpark(Managed& m);
	void unparkAll();
	void stopMotions();
	void bringBack(Managed& m);
	void forgetStage(Managed& m);
	void heal();
	void auditTrees();
	static void rescueParked();

	void onActiveWindowChanged();
	void onRectChanged(TrackedWindow* window);
	void onMoveSizeStarted(TrackedWindow* window);
	void onMoveSizeEnded(TrackedWindow* window);
	void onScreensChanged();
	void verify();
	void onDeadline();
	void drop(Managed& m, const QPoint& cursor);
	bool insertAtCursor(Managed& m, const GUID& desktop, const QPoint& cursor);
	void giveUp(Managed& m, const QRect& frame);
	void markAll();

	[[nodiscard]] Managed* tiledActive();
	[[nodiscard]] Managed* neighbour(const Managed& from, Edge direction);
	[[nodiscard]] QScreen* screenInDirection(QScreen* from, Edge direction) const;
	void swapWindows(Managed& a, Managed& b);
	void moveInDwindle(Managed& m, Edge direction);
	void moveToLayout(Managed& m, Layout* layout, TrackedWindow* target);

	WindowTracker* tracker = nullptr;
	VirtualDesktops* desktops = nullptr;

	bool mEnabled = false;
	bool mSuppressWindowShadows = false;
	bool mAnimateMoves = true;
	bool barMoving = false;
	bool barRelayoutPending = false;
	QHash<TrackedWindow*, QRect> moving;
	qint32 mGapsIn = 4;
	qint32 mGapsOut = 5;
	int mSpecialOffset = 0;
	bool mSpecialShifting = false;
	bool mPreserveSplit = true;
	QStringList mExcluded;
	QStringList mFullscreenScreens;
	QSet<HWND> mNativeFullscreen;
	QString mLayout = QStringLiteral("dwindle");
	bool mSmartSplit = false;
	qint32 mForceSplit = 0;
	double mDefaultSplitRatio = 1.0;
	QString mMasterNewStatus = QStringLiteral("slave");
	bool mMasterNewOnTop = false;
	QString mMasterOrientation = QStringLiteral("left");
	double mMasterFactor = 0.55;
	double mScrollingColumnWidth = 0.5;
	QList<double> mScrollingPresets;
	QString mScrollingCenter = QStringLiteral("overflow");
	bool fitToggled = false;
	QList<QRegularExpression> excludedPatterns;
	QList<QRegularExpression> excludedTitles;

	std::unordered_map<TrackedWindow*, Managed> managed;
	std::map<QString, std::unique_ptr<Layout>> layouts;
	QSet<TrackedWindow*> dirtyWindows;
	QSet<Layout*> dirtyLayouts;
	QSet<TrackedWindow*> unverified;
	TrackedWindow* dragging = nullptr;
	QRect dragOrigin;
	QPointer<TrackedWindow> detached;
	Override detachedOverride = Override::None;
	QPointer<TrackedWindow> regrab;
	QSize regrabSize;
	QPointF regrabAt;
	void placeMoving(Managed& m, const Layout* layout, const QRect& tile);
	[[nodiscard]] bool canAnimate(const Managed& m, const Layout* layout, const QRect& tile) const;
	void onMotionLanded(HWND hwnd, const QRect& frame);
	EdgeDrag edgeDragState;
	quint64 focusCounter = 0;

	HANDLE ownerMutex = nullptr;
	QTimer syncTimer;
	QTimer restackTimer;
	QTimer verifyTimer;
	QTimer deadlineTimer;
	QTimer healTimer;
	QTimer closeCacheTimer;
	qsizetype nextCloseWindow = 0;
	QList<QMetaObject::Connection> connections;
};

class Tiling: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(bool suppressWindowShadows READ suppressWindowShadows WRITE setSuppressWindowShadows NOTIFY suppressWindowShadowsChanged);
	Q_PROPERTY(bool animateMoves READ animateMoves WRITE setAnimateMoves NOTIFY animateMovesChanged);
	Q_PROPERTY(qint32 gapsIn READ gapsIn WRITE setGapsIn NOTIFY gapsInChanged);
	Q_PROPERTY(qint32 gapsOut READ gapsOut WRITE setGapsOut NOTIFY gapsOutChanged);
	Q_PROPERTY(bool preserveSplit READ preserveSplit WRITE setPreserveSplit NOTIFY preserveSplitChanged);
	Q_PROPERTY(QStringList excluded READ excluded WRITE setExcluded NOTIFY excludedChanged);
	Q_PROPERTY(QStringList fullscreenScreens READ fullscreenScreens NOTIFY fullscreenScreensChanged);
	Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged);
	Q_PROPERTY(bool smartSplit READ smartSplit WRITE setSmartSplit NOTIFY smartSplitChanged);
	Q_PROPERTY(qint32 forceSplit READ forceSplit WRITE setForceSplit NOTIFY forceSplitChanged);
	Q_PROPERTY(double defaultSplitRatio READ defaultSplitRatio WRITE setDefaultSplitRatio NOTIFY defaultSplitRatioChanged);
	Q_PROPERTY(QString masterNewStatus READ masterNewStatus WRITE setMasterNewStatus NOTIFY masterNewStatusChanged);
	Q_PROPERTY(bool masterNewOnTop READ masterNewOnTop WRITE setMasterNewOnTop NOTIFY masterNewOnTopChanged);
	Q_PROPERTY(QString masterOrientation READ masterOrientation WRITE setMasterOrientation NOTIFY masterOrientationChanged);
	Q_PROPERTY(double masterFactor READ masterFactor WRITE setMasterFactor NOTIFY masterFactorChanged);
	Q_PROPERTY(double scrollingColumnWidth READ scrollingColumnWidth WRITE setScrollingColumnWidth NOTIFY scrollingColumnWidthChanged);
	Q_PROPERTY(QList<double> scrollingPresets READ scrollingPresets WRITE setScrollingPresets NOTIFY scrollingPresetsChanged);
	Q_PROPERTY(QString scrollingCenter READ scrollingCenter WRITE setScrollingCenter NOTIFY scrollingCenterChanged);
	// clang-format on

public:
	explicit Tiling(QObject* parent = nullptr);

	Q_INVOKABLE void relayout();
	Q_INVOKABLE void expectBarSpace(const QString& screenName, bool top, int reserved);

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] bool suppressWindowShadows() const;
	void setSuppressWindowShadows(bool suppress);
	[[nodiscard]] bool animateMoves() const;
	void setAnimateMoves(bool animate);
	[[nodiscard]] qint32 gapsIn() const;
	void setGapsIn(qint32 gaps);
	[[nodiscard]] qint32 gapsOut() const;
	void setGapsOut(qint32 gaps);
	[[nodiscard]] bool preserveSplit() const;
	void setPreserveSplit(bool preserve);
	[[nodiscard]] QStringList excluded() const;
	[[nodiscard]] QStringList fullscreenScreens() const;
	void setExcluded(const QStringList& excluded);
	[[nodiscard]] QString layout() const;
	void setLayout(const QString& layout);
	[[nodiscard]] bool smartSplit() const;
	void setSmartSplit(bool smart);
	[[nodiscard]] qint32 forceSplit() const;
	void setForceSplit(qint32 force);
	[[nodiscard]] double defaultSplitRatio() const;
	void setDefaultSplitRatio(double ratio);
	[[nodiscard]] QString masterNewStatus() const;
	void setMasterNewStatus(const QString& status);
	[[nodiscard]] bool masterNewOnTop() const;
	void setMasterNewOnTop(bool onTop);
	[[nodiscard]] QString masterOrientation() const;
	void setMasterOrientation(const QString& orientation);
	[[nodiscard]] double masterFactor() const;
	void setMasterFactor(double factor);
	[[nodiscard]] double scrollingColumnWidth() const;
	void setScrollingColumnWidth(double width);
	[[nodiscard]] QList<double> scrollingPresets() const;
	void setScrollingPresets(const QList<double>& presets);
	[[nodiscard]] QString scrollingCenter() const;
	void setScrollingCenter(const QString& center);

signals:
	void enabledChanged();
	void suppressWindowShadowsChanged();
	void animateMovesChanged();
	void gapsInChanged();
	void gapsOutChanged();
	void preserveSplitChanged();
	void excludedChanged();
	void fullscreenScreensChanged();
	void fullscreenConflict(const QString& appId, const QString& title, bool exclusive);
	void layoutChanged();
	void smartSplitChanged();
	void forceSplitChanged();
	void defaultSplitRatioChanged();
	void masterNewStatusChanged();
	void masterNewOnTopChanged();
	void masterOrientationChanged();
	void masterFactorChanged();
	void scrollingColumnWidthChanged();
	void scrollingPresetsChanged();
	void scrollingCenterChanged();
};

} // namespace qs::windows
