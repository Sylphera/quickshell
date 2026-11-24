#pragma once

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class VirtualDesktops;

class DesktopTrimmer: public QObject {
	Q_OBJECT;

public:
	static DesktopTrimmer* instance();
	Q_DISABLE_COPY_MOVE(DesktopTrimmer);

	[[nodiscard]] bool enabled() const { return this->mEnabled; }
	void setEnabled(bool enabled);

signals:
	void enabledChanged();

private:
	explicit DesktopTrimmer(QObject* parent);
	~DesktopTrimmer() override = default;

	void schedule();
	void trim();
	[[nodiscard]] bool busy() const;
	[[nodiscard]] qsizetype lastOccupied() const;

	VirtualDesktops* desktops = nullptr;
	QTimer timer;
	QElapsedTimer uptime;
	bool mEnabled = false;
	bool watching = false;
};

}
