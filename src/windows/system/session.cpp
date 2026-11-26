#include "session.hpp"

#include <qt_windows.h>

#include <mmsystem.h>
#include <powrprof.h>
#include <winbase.h>
#include <winreg.h>
#include <wtsapi32.h>

#include <thread>

#include <bcrypt.h>

#include <userconsentverifierinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.UI.h>

#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qtimer.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qstandardpaths.h>
#include <qhash.h>
#include <qlogging.h>
#include <qloggingcategory.h>

#include "../../core/logcat.hpp"

namespace qs::windows::sys {

namespace {
QS_LOGGING_CATEGORY(logSession, "quickshell.windows.session", QtWarningMsg);

constexpr auto SESSION_WINDOW_CLASS = L"QuickshellSessionNotifications";

LRESULT CALLBACK sessionWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
	if (message == WM_WTSSESSION_CHANGE && wparam == WTS_SESSION_LOCK) {
		auto* session = reinterpret_cast<Session*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); // NOLINT
		if (session != nullptr) emit session->nativeSessionLocked();
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool enablePrivilege(LPCWSTR name) {
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
		return false;
	}

	LUID luid {};
	if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
		CloseHandle(token);
		return false;
	}

	TOKEN_PRIVILEGES tp {};
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	auto ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) != FALSE;
	CloseHandle(token);
	return ok && GetLastError() == ERROR_SUCCESS;
}

constexpr ULONGLONG LOCK_PBKDF2_ITERATIONS = 210000;
constexpr int LOCK_SALT_BYTES = 16;
constexpr int LOCK_HASH_BYTES = 32;

QString lockPasswordPath() {
	return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
	     + QStringLiteral("/illogical-impulse/lock-password");
}

QByteArray pbkdf2(const QString& password, const QByteArray& salt, ULONGLONG iterations) {
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
		return {};
	}
	auto secret = password.toUtf8();
	QByteArray hash(LOCK_HASH_BYTES, Qt::Uninitialized);
	auto status = BCryptDeriveKeyPBKDF2(
	    algorithm,
	    reinterpret_cast<PUCHAR>(secret.data()), // NOLINT
	    static_cast<ULONG>(secret.size()),
	    reinterpret_cast<PUCHAR>(const_cast<char*>(salt.constData())), // NOLINT
	    static_cast<ULONG>(salt.size()),
	    iterations,
	    reinterpret_cast<PUCHAR>(hash.data()), // NOLINT
	    static_cast<ULONG>(hash.size()),
	    0
	);
	SecureZeroMemory(secret.data(), static_cast<SIZE_T>(secret.size()));
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return status == 0 ? hash : QByteArray();
}

HWND credentialPrompt() {
	HWND found = nullptr;
	EnumWindows(
	    [](HWND hwnd, LPARAM param) -> BOOL {
		    if (!IsWindowVisible(hwnd)) return TRUE;
		    DWORD pid = 0;
		    GetWindowThreadProcessId(hwnd, &pid);
		    auto* process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		    if (process == nullptr) return TRUE;
		    wchar_t path[MAX_PATH] {};
		    DWORD length = MAX_PATH;
		    auto named = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
		    CloseHandle(process);
		    if (!named) return TRUE;
		    auto name = QString::fromWCharArray(path, static_cast<int>(length)).toLower();
		    if (!name.endsWith(QStringLiteral("credentialuibroker.exe"))) return TRUE;
		    *reinterpret_cast<HWND*>(param) = hwnd; // NOLINT
		    return FALSE;
	    },
	    reinterpret_cast<LPARAM>(&found)
	);
	return found;
}

void raiseCredentialPrompt(int tries) {
	if (auto* prompt = credentialPrompt()) {
		SetWindowPos(prompt, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
		SetForegroundWindow(prompt);
		return;
	}
	if (tries <= 0) return;
	QTimer::singleShot(100, QCoreApplication::instance(), [tries]() { raiseCredentialPrompt(tries - 1); });
}

bool sameBytes(const QByteArray& a, const QByteArray& b) {
	if (a.size() != b.size() || a.isEmpty()) return false;
	unsigned char diff = 0;
	for (qsizetype i = 0; i < a.size(); i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
	return diff == 0;
}

} // namespace

bool Session::setLockPassword(const QString& password) {
	if (password.isEmpty()) return false;
	QByteArray salt(LOCK_SALT_BYTES, Qt::Uninitialized);
	if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(salt.data()), LOCK_SALT_BYTES, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { // NOLINT
		return false;
	}
	auto hash = pbkdf2(password, salt, LOCK_PBKDF2_ITERATIONS);
	if (hash.isEmpty()) return false;
	auto path = lockPasswordPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
	file.write(QByteArrayLiteral("pbkdf2-sha256$") + QByteArray::number(LOCK_PBKDF2_ITERATIONS) + '$' + salt.toHex()
	           + '$' + hash.toHex());
	return file.flush();
}

bool Session::hasLockPassword() { return QFile::exists(lockPasswordPath()); }

void Session::clearLockPassword() { QFile::remove(lockPasswordPath()); }

void Session::verifyLockPassword(const QString& password) {
	QFile file(lockPasswordPath());
	if (!file.open(QIODevice::ReadOnly)) {
		emit this->userVerified(false, 101);
		return;
	}
	auto parts = file.readAll().trimmed().split('$');
	std::thread([this, password, parts]() {
		auto verified = false;
		if (parts.size() == 4 && parts[0] == "pbkdf2-sha256") {
			auto iterations = parts[1].toULongLong();
			auto expected = QByteArray::fromHex(parts[3]);
			verified = iterations > 0 && sameBytes(pbkdf2(password, QByteArray::fromHex(parts[2]), iterations), expected);
		}
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [this, verified]() { emit this->userVerified(verified, verified ? 0 : 100); },
		    Qt::QueuedConnection
		);
	}).detach();
}

void Session::verifyPassword(const QString& password) {
	auto secret = password.toStdWString();
	std::thread([this, secret]() mutable {
		wchar_t user[256] {};
		DWORD length = 256;
		auto verified = false;
		if (GetUserNameW(user, &length)) {
			HANDLE token = nullptr;
			verified = LogonUserW(
			               user,
			               L".",
			               secret.c_str(),
			               LOGON32_LOGON_INTERACTIVE,
			               LOGON32_PROVIDER_DEFAULT,
			               &token
			           )
			        != FALSE;
			if (token != nullptr) CloseHandle(token);
			if (!verified) qCInfo(logSession) << "Password check failed:" << GetLastError();
		}
		SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [this, verified]() { emit this->userVerified(verified, verified ? 0 : 100); },
		    Qt::QueuedConnection
		);
	}).detach();
}

void Session::verifyUser(const QString& message) {
	auto* window = GetForegroundWindow();
	AllowSetForegroundWindow(ASFW_ANY);
	QTimer::singleShot(150, QCoreApplication::instance(), []() { raiseCredentialPrompt(30); });
	auto text = message.toStdWString();
	std::thread([this, window, text]() {
		using namespace winrt::Windows::Security::Credentials::UI;
		auto verified = false;
		auto result = -1;
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			auto interop = winrt::get_activation_factory<UserConsentVerifier, IUserConsentVerifierInterop>();
			winrt::Windows::Foundation::IAsyncOperation<UserConsentVerificationResult> operation {nullptr};
			winrt::hstring prompt(text);
			winrt::check_hresult(interop->RequestVerificationForWindowAsync(
			    window,
			    static_cast<HSTRING>(winrt::get_abi(prompt)),
			    winrt::guid_of<decltype(operation)>(),
			    winrt::put_abi(operation)
			));
			auto answer = operation.get();
			result = static_cast<int>(answer);
			verified = answer == UserConsentVerificationResult::Verified;
		} catch (const winrt::hresult_error& e) {
			qCWarning(logSession) << "Windows Hello verification failed:" << Qt::hex
			                      << static_cast<quint32>(e.code().value);
		}
		QMetaObject::invokeMethod(
		    QCoreApplication::instance(),
		    [this, verified, result]() { emit this->userVerified(verified, result); },
		    Qt::QueuedConnection
		);
	}).detach();
}

Session::Session(QObject* parent): QObject(parent) {
	auto* instance = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.lpfnWndProc = sessionWindowProc;
	cls.hInstance = instance;
	cls.lpszClassName = SESSION_WINDOW_CLASS;
	if (RegisterClassW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		qCWarning(logSession) << "Could not register Windows session notification class:" << GetLastError();
		return;
	}
	auto* window = CreateWindowExW(
	    0, SESSION_WINDOW_CLASS, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr
	);
	if (window == nullptr) {
		qCWarning(logSession) << "Could not create Windows session notification window:" << GetLastError();
		return;
	}
	this->notificationWindow = reinterpret_cast<quintptr>(window); // NOLINT
	SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this)); // NOLINT
	this->registerNativeNotifications();
}

void Session::registerNativeNotifications() {
	if (this->nativeNotificationsReady || this->notificationWindow == 0) return;
	auto* window = reinterpret_cast<HWND>(this->notificationWindow); // NOLINT
	this->nativeNotificationsReady = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
	if (this->nativeNotificationsReady) return;
	auto error = GetLastError();
	qCWarning(logSession) << "Could not subscribe to Windows lock confirmation:" << error;
	if (++this->nativeNotificationAttempts < 10) {
		QTimer::singleShot(3000, this, [this]() { this->registerNativeNotifications(); });
	}
}

Session::~Session() {
	auto* window = reinterpret_cast<HWND>(this->notificationWindow); // NOLINT
	if (window == nullptr) return;
	if (this->nativeNotificationsReady) WTSUnRegisterSessionNotification(window);
	SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	DestroyWindow(window);
}

bool Session::requestNativeLock() {
	if (!this->nativeNotificationsReady) this->registerNativeNotifications();
	if (!this->nativeNotificationsReady) return false;
	if (!LockWorkStation()) {
		qCWarning(logSession) << "LockWorkStation failed:" << GetLastError();
		return false;
	}
	return true;
}

void Session::lock() {
	if (!LockWorkStation()) {
		qCWarning(logSession) << "LockWorkStation failed:" << GetLastError();
	}
}

void Session::logout() {
	if (!ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER)) {
		qCWarning(logSession) << "ExitWindowsEx(EWX_LOGOFF) failed:" << GetLastError();
	}
}

void Session::shutdown() {
	enablePrivilege(SE_SHUTDOWN_NAME);

	auto result = InitiateShutdownW(
	    nullptr,
	    nullptr,
	    0,
	    SHUTDOWN_POWEROFF,
	    SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER
	);

	if (result != ERROR_SUCCESS) {
		qCWarning(logSession) << "InitiateShutdownW(SHUTDOWN_POWEROFF) failed:" << result;
	}
}

void Session::reboot() {
	enablePrivilege(SE_SHUTDOWN_NAME);

	auto result = InitiateShutdownW(
	    nullptr,
	    nullptr,
	    0,
	    SHUTDOWN_RESTART,
	    SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_OTHER
	);

	if (result != ERROR_SUCCESS) {
		qCWarning(logSession) << "InitiateShutdownW(SHUTDOWN_RESTART) failed:" << result;
	}
}

void Session::suspend() {
	if (!SetSuspendState(FALSE, FALSE, FALSE)) {
		qCWarning(logSession) << "SetSuspendState(suspend) failed:" << GetLastError();
	}
}

void Session::hibernate() {
	if (!SetSuspendState(TRUE, FALSE, FALSE)) {
		qCWarning(logSession) << "SetSuspendState(hibernate) failed:" << GetLastError();
	}
}

void Session::rebootToFirmware() {
	STARTUPINFOW si {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi {};

	wchar_t cmdLine[] = L"shutdown.exe /r /fw /t 0"; // NOLINT

	if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
		qCWarning(logSession) << "CreateProcessW(shutdown /r /fw) failed:" << GetLastError();
		return;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}

bool Session::canHibernate() {
	SYSTEM_POWER_CAPABILITIES caps {};
	if (!GetPwrCapabilities(&caps)) return false;
	return caps.HiberFilePresent != FALSE;
}

void Session::playSystemSound(const QString& name) {
	static const QHash<QString, const wchar_t*> EVENTS = {
	    {"dialog-warning", L"SystemExclamation"},
	    {"dialog-error", L"SystemHand"},
	    {"dialog-information", L"SystemAsterisk"},
	    {"suspend-error", L"CriticalBatteryAlarm"},
	    {"battery-low", L"LowBatteryAlarm"},
	    {"battery-caution", L"CriticalBatteryAlarm"},
	    {"complete", L"Notification.Default"},
	    {"message", L"Notification.Default"},
	    {"message-new-instant", L"Notification.IM"},
	    {"power-plug", L"DeviceConnect"},
	    {"power-unplug", L"DeviceDisconnect"},
	    {"device-added", L"DeviceConnect"},
	    {"device-removed", L"DeviceDisconnect"},
	    {"alarm-clock-elapsed", L"Notification.Looping.Alarm"},
	    {"bell", L"SystemDefault"},
	};

	const auto* alias = EVENTS.value(name, L"Notification.Default");
	PlaySoundW(alias, nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

QVariantMap Session::osInfo() {
	auto readString = [](const wchar_t* name) {
		wchar_t buffer[256] {};
		DWORD size = sizeof(buffer);
		auto status = RegGetValueW(
		    HKEY_LOCAL_MACHINE,
		    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
		    name,
		    RRF_RT_REG_SZ,
		    nullptr,
		    buffer,
		    &size
		);
		return status == ERROR_SUCCESS ? QString::fromWCharArray(buffer) : QString();
	};

	DWORD ubr = 0;
	DWORD ubrSize = sizeof(ubr);
	auto hasUbr = RegGetValueW(
	                  HKEY_LOCAL_MACHINE,
	                  L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
	                  L"UBR",
	                  RRF_RT_REG_DWORD,
	                  nullptr,
	                  &ubr,
	                  &ubrSize
	              )
	           == ERROR_SUCCESS;

	auto build = readString(L"CurrentBuildNumber");
	auto name = readString(L"ProductName");

	if (build.toInt() >= 22000) name.replace("Windows 10", "Windows 11");
	if (name.isEmpty()) name = "Windows";

	wchar_t uiLanguage[LOCALE_NAME_MAX_LENGTH] {};
	if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), uiLanguage, LOCALE_NAME_MAX_LENGTH, 0) == 0)
		uiLanguage[0] = 0;

	return {
	    {"name", name},
	    {"uiLanguage", QString::fromWCharArray(uiLanguage)},
	    {"version", readString(L"DisplayVersion")},
	    {"build", hasUbr ? build + "." + QString::number(ubr) : build},
	    {"edition", readString(L"EditionID")},
	};
}

} // namespace qs::windows::sys
