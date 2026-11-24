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
	enum Fingers : int {
		FourFingers = 1,
		ThreeFingers = 2,
	};

	static constexpr auto RESTORE_ARGUMENT = "--restore-touchpad-swipe";
	static int restoreWhenAbandoned();

	explicit SystemSwipe(QObject* parent = nullptr);
	~SystemSwipe() override;
	Q_DISABLE_COPY_MOVE(SystemSwipe);

	[[nodiscard]] int suppressed() const { return this->mSuppressed; }
	void setRequested(int fingers);

	bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

signals:
	void suppressedChanged();

private:
	void acquire();
	void release();
	bool lock();
	bool startGuard();
	void setSuppressed(int fingers);

	QTimer retry;
	int attempts = 0;
	HANDLE mutex = nullptr;
	HANDLE guard = nullptr;
	int requested = 0;
	int mSuppressed = 0;
};

}
