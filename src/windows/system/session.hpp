#pragma once

#include <qobject.h>
#include <qstring.h>
#include <qvariant.h>
#include <qqmlintegration.h>
#include <qtmetamacros.h>

namespace qs::windows::sys {

class Session: public QObject {
	Q_OBJECT;
	QML_ELEMENT;
	QML_SINGLETON;
	Q_PROPERTY(bool canHibernate READ canHibernate CONSTANT);

public:
	explicit Session(QObject* parent = nullptr);
	~Session() override;

	Q_INVOKABLE static void lock();
	// Request the Windows lock only when session notifications are available.
	// nativeSessionLocked confirms the desktop actually switched to Winlogon.
	Q_INVOKABLE bool requestNativeLock();
	Q_INVOKABLE static void logout();
	Q_INVOKABLE static void shutdown();
	Q_INVOKABLE static void reboot();
	Q_INVOKABLE static void suspend();
	Q_INVOKABLE static void hibernate();
	Q_INVOKABLE static void rebootToFirmware();
	Q_INVOKABLE static void playSystemSound(const QString& name);
	Q_INVOKABLE static QVariantMap osInfo();
	// Asks Windows Hello (PIN, fingerprint, face or password) to confirm the
	// signed-in user, over the foreground window; answers with userVerified.
	Q_INVOKABLE void verifyUser(const QString& message);
	// Checks the signed-in user's password without any Windows prompt (a PIN
	// can't be checked this way); answers with userVerified, 0 or 100 (wrong).
	Q_INVOKABLE void verifyPassword(const QString& password);
	// The lock screen's own password, kept as a salted PBKDF2-SHA256 hash
	// beside the shell's settings, never the password itself.
	Q_INVOKABLE static bool setLockPassword(const QString& password);
	Q_INVOKABLE static bool hasLockPassword();
	Q_INVOKABLE static void clearLockPassword();
	// Answers with userVerified: 0 right, 100 wrong, 101 none set.
	Q_INVOKABLE void verifyLockPassword(const QString& password);

	[[nodiscard]] static bool canHibernate();

signals:
	void nativeSessionLocked();
	// `result` is UserConsentVerificationResult (0 = verified), or -1 when the
	// verifier couldn't be asked at all.
	void userVerified(bool verified, int result);

private:
	void registerNativeNotifications();
	quintptr notificationWindow = 0;
	bool nativeNotificationsReady = false;
	int nativeNotificationAttempts = 0;
};

} // namespace qs::windows::sys
