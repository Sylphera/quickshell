#pragma once

#include <qabstractnativeeventfilter.h>
#include <qbytearray.h>
#include <qobject.h>
#include <qt_windows.h>
#include <qtclasshelpermacros.h>
#include <qtimer.h>
#include <qtmetamacros.h>
#include <qtypes.h>

namespace qs::windows {

class SystemSwipe
    : public QObject
    , public QAbstractNativeEventFilter {
	Q_OBJECT;

public:
	static constexpr auto RESTORE_ARGUMENT = "--restore-touchpad-swipe";
	static int restoreWhenAbandoned();

	explicit SystemSwipe(QObject* parent = nullptr);
	~SystemSwipe() override;
	Q_DISABLE_COPY_MOVE(SystemSwipe);

	[[nodiscard]] bool suppressed() const { return this->mSuppressed; }
	void setSuppressRequested(bool requested);

	bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
	void suppressedChanged();

private:
	void acquire();
	void release();
	bool lock();
	bool startGuard();
	void setSuppressed(bool suppressed);

	QTimer retry;
	int attempts = 0;
	HANDLE mutex = nullptr;
	HANDLE guard = nullptr;
	bool requested = false;
	bool mSuppressed = false;
};

}
