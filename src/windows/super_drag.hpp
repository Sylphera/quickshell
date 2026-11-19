#pragma once

#include <functional>
#include <thread>

#include <qt_windows.h>

#include <qobject.h>
#include <qqmlintegration.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>

namespace qs::windows {

class SuperDragManager: public QObject {
	Q_OBJECT;

public:
	~SuperDragManager() override;
	Q_DISABLE_COPY_MOVE(SuperDragManager);

	static SuperDragManager* instance();

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);
	void setWheelColumns(bool enabled);
	[[nodiscard]] bool focusFollowsMouse() const { return this->mFocusFollowsMouse; }
	// Hyprland's follow_mouse = 1: moving the pointer focuses the app window
	// under it.
	void setFocusFollowsMouse(bool enabled);
	// end-4's Super+wheel binds: the handler gets the steps (positive is down,
	// to the next workspace) and the modifier: none, Ctrl, or Shift/Alt.
	enum class WheelBind { Workspace, OccupiedWorkspace, MoveWindow };
	void setWorkspaceWheel(std::function<void(int steps, WheelBind bind)> handler);

	static bool onMouseHook(WPARAM message, const MSLLHOOKSTRUCT* info);
	static void onHookStopped();

signals:
	void enabledChanged();
	void focusFollowsMouseChanged();

private:
	explicit SuperDragManager(QObject* parent);
	void onHover(POINT point, bool byLayout = false);

	struct GuiDrag {
		HWND hwnd = nullptr;
		bool tiledResize = false;
		bool notified = false;
	};

	bool startWorker();
	void stopWorker();

	LRESULT onStarted(HWND hwnd, LPARAM flags);
	void onTiledResize();
	void onEnded(HWND hwnd, LPARAM delta);
	static bool onWheel(WPARAM message, const MSLLHOOKSTRUCT* info);
	void finishGuiDrag(const POINT* delta);

	static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	static void workerMain();

	HWND messageWindow = nullptr;
	std::thread worker;
	bool workerRunning = false;
	bool mEnabled = false;
	bool wheelColumns = false;
	bool mFocusFollowsMouse = false;
	std::function<void(int, WheelBind)> workspaceWheel;
	GuiDrag guiDrag;
};

class SuperDrag: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged);
	Q_PROPERTY(bool focusFollowsMouse READ focusFollowsMouse WRITE setFocusFollowsMouse NOTIFY focusFollowsMouseChanged);

public:
	explicit SuperDrag(QObject* parent = nullptr);

	[[nodiscard]] bool enabled() const;
	void setEnabled(bool enabled);
	[[nodiscard]] bool focusFollowsMouse() const;
	void setFocusFollowsMouse(bool enabled);

signals:
	void enabledChanged();
	void focusFollowsMouseChanged();
};

} // namespace qs::windows
