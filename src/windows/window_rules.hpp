#pragma once

#include <qobject.h>
#include <qset.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class TrackedWindow;

class WindowRules: public QObject {
	Q_OBJECT;

public:
	enum class Kind : quint8 {
		None,
		Dialog,
		FileDialog,
		PictureInPicture,
		SharingBar,
	};

	static WindowRules* instance();
	Q_DISABLE_COPY_MOVE(WindowRules);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

signals:
	void enabledChanged();

private:
	explicit WindowRules(QObject* parent);
	~WindowRules() override = default;

	void onWindowAdded(TrackedWindow* window);
	void onForeground(HWND hwnd);
	void watch(HWND hwnd);
	[[nodiscard]] static Kind kindOf(HWND hwnd);
	static void apply(HWND hwnd, Kind kind);

	QSet<HWND> seenDialogs;

	bool mEnabled = false;
	bool watching = false;
};

}
