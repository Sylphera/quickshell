#pragma once

#include <qobject.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>

namespace qs::windows {

class KillMode: public QObject {
	Q_OBJECT;

public:
	static KillMode* instance();
	Q_DISABLE_COPY_MOVE(KillMode);

	void start();

private:
	explicit KillMode(QObject* parent);

	void stop();
	static void killAt(POINT point);
	static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	HWND cover = nullptr;
	QTimer escape;
};

}
