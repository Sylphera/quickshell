#pragma once

#include <qt_windows.h>

#include <qlist.h>
#include <qobject.h>
#include <qpoint.h>
#include <qqmlintegration.h>
#include <qrect.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

enum class TaskbarEdge : quint8 { Left, Top, Right, Bottom };

class TaskbarManager: public QObject {
	Q_OBJECT;

public:
	static TaskbarManager* instance();
	~TaskbarManager() override;
	Q_DISABLE_COPY_MOVE(TaskbarManager);

	[[nodiscard]] bool hoverOnly() const { return this->mHoverOnly; }
	void setHoverOnly(bool hoverOnly);
	[[nodiscard]] bool alwaysHidden() const { return this->mAlwaysHidden; }
	void setAlwaysHidden(bool alwaysHidden);
	void turnOffAutoHide();
	[[nodiscard]] bool suppressSnapBar() const { return this->snapBarRequested; }
	void setSuppressSnapBar(bool enabled);

	static void restoreForCrash();
	static void showAfterShell();

signals:
	void hoverOnlyChanged();
	void alwaysHiddenChanged();
	void suppressSnapBarChanged();

private:
	explicit TaskbarManager(QObject* parent);

	struct Bar {
		HWND hwnd = nullptr;
		QRect monitor;
		TaskbarEdge edge = TaskbarEdge::Bottom;
		int thickness = 0;
	};

	void enable();
	void disable(bool restoreAutoHide);
	void refreshMode();
	void releaseStaleAutoHide();
	void acquireSnapBar();
	void findBars();
	void onCursorMoved(QPoint position);
	void onCheck();
	void onDisplayChanged();
	void reveal();
	void conceal();
	[[nodiscard]] bool atTrigger(QPoint position) const;
	[[nodiscard]] bool overBar(QPoint position) const;
	[[nodiscard]] static bool taskbarPopupActive();
	void watchExplorer();
	void unwatchExplorer();
	void onBarShown(HWND hwnd);
	static void CALLBACK onWinEvent(
	    HWINEVENTHOOK hook,
	    DWORD event,
	    HWND hwnd,
	    LONG idObject,
	    LONG idChild,
	    DWORD thread,
	    DWORD time
	);

	bool mHoverOnly = false;
	bool mAlwaysHidden = false;
	bool enabled = false;
	bool revealed = false;
	qint64 leftAt = 0;
	QList<Bar> bars;
	QTimer checkTimer;
	QTimer displayTimer;
	QTimer snapBarRetry;
	HANDLE snapBarMutex = nullptr;
	bool snapBarRequested = false;
	HWINEVENTHOOK showHook = nullptr;
	DWORD watchedPid = 0;
	qint64 burstStart = 0;
	int burstHides = 0;
};

class Taskbar: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	// clang-format off
	Q_PROPERTY(bool hoverOnly READ hoverOnly WRITE setHoverOnly NOTIFY hoverOnlyChanged);
	Q_PROPERTY(bool alwaysHidden READ alwaysHidden WRITE setAlwaysHidden NOTIFY alwaysHiddenChanged);
	Q_PROPERTY(bool suppressSnapBar READ suppressSnapBar WRITE setSuppressSnapBar NOTIFY suppressSnapBarChanged);
	// clang-format on

public:
	explicit Taskbar(QObject* parent = nullptr);

	[[nodiscard]] bool hoverOnly() const;
	void setHoverOnly(bool hoverOnly);
	[[nodiscard]] bool alwaysHidden() const;
	void setAlwaysHidden(bool alwaysHidden);
	Q_INVOKABLE void turnOffAutoHide();
	[[nodiscard]] bool suppressSnapBar() const;
	void setSuppressSnapBar(bool enabled);

signals:
	void hoverOnlyChanged();
	void alwaysHiddenChanged();
	void suppressSnapBarChanged();
};

} // namespace qs::windows
