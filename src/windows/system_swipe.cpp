#include "system_swipe.hpp"
#include <cstddef>
#include <string>
#include <utility>

#include <qt_windows.h>
#include <combaseapi.h>
#include <servprov.h>
#include <wrl/client.h>

#include <qabstractnativeeventfilter.h>
#include <qbytearray.h>
#include <qcoreapplication.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qstring.h>
#include <qtimer.h>
#include <qtypes.h>

#include "../core/logcat.hpp"
#include "appbar.hpp"
#include "util.hpp"

namespace qs::windows {

namespace {

QS_LOGGING_CATEGORY(logSystemSwipe, "quickshell.windows.systemswipe", QtWarningMsg);

constexpr auto TOUCHPAD_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad";
constexpr auto SWIPE_VALUE = L"FourFingerSlideEnabled";
constexpr auto STATE_KEY = L"Software\\Quickshell";
constexpr auto SWIPE_BACKUP = L"TouchpadSwipeBackup";
constexpr auto SWIPE_MUTEX = L"Local\\QuickshellTouchpadSwipe";
constexpr int SWIPE_SETTING = 18;
constexpr int RETRY_MS = 1000;
constexpr int EXPLORER_SETTLE_MS = 2000;
constexpr int MAX_ATTEMPTS = 60;
constexpr size_t MAX_PATH_LENGTH = 32768;

struct SwipeSetting {
	DWORD present = 0;
	DWORD value = 0;
};

struct __declspec(uuid("4214f6fa-eb36-4e2f-9ca2-23fdc1832df7")) SwipeSettingsCache: IUnknown {
	virtual HRESULT STDMETHODCALLTYPE OnSettingChanged(int setting) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetBOOL(int setting, BOOL* enabled) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDWORD(int setting, DWORD* value) = 0;
};

bool supportsSettingsCache() {
	const auto build = windowsBuild();
	return build == 26100 || build == 26200 || build == 26300;
}

bool readSwipe(SwipeSetting& setting) {
	DWORD size = sizeof(setting.value);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    TOUCHPAD_KEY,
	    SWIPE_VALUE,
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &setting.value,
	    &size
	);
	setting.present = status == ERROR_SUCCESS ? 1 : 0;
	return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool writeSwipe(const SwipeSetting& setting) {
	if (setting.present == 0) {
		const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, TOUCHPAD_KEY, SWIPE_VALUE);
		return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
	}
	const auto status = RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    TOUCHPAD_KEY,
	    SWIPE_VALUE,
	    REG_DWORD,
	    &setting.value,
	    sizeof(setting.value)
	);
	return status == ERROR_SUCCESS;
}

bool restoreSwipeSetting() {
	SwipeSetting previous;
	DWORD size = sizeof(previous);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    SWIPE_BACKUP,
	    RRF_RT_REG_BINARY,
	    nullptr,
	    &previous,
	    &size
	);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
	if (status != ERROR_SUCCESS || size != sizeof(previous) || previous.present > 1) return false;

	SwipeSetting current;
	if (!readSwipe(current)) return false;
	if (current.present != 0 && current.value == 0 && !writeSwipe(previous)) return false;
	const auto removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, STATE_KEY, SWIPE_BACKUP);
	return removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
}

class SettingsCache {
public:
	SettingsCache() {
		constexpr CLSID immersiveShell =
		    {0xc2f03a33, 0x21f5, 0x47fa, {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
		constexpr GUID cacheService =
		    {0x53660488, 0x8855, 0x460b, {0xa9, 0xab, 0x5c, 0xfc, 0x6b, 0x50, 0x12, 0xca}};
		this->apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		if (FAILED(this->apartment) && this->apartment != RPC_E_CHANGED_MODE) return;

		Microsoft::WRL::ComPtr<IServiceProvider> shell;
		auto found = CoCreateInstance(immersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
		if (FAILED(found)) return;
		if (FAILED(shell->QueryService(cacheService, IID_PPV_ARGS(&this->cache)))) this->cache.Reset();
	}

	~SettingsCache() {
		this->cache.Reset();
		if (SUCCEEDED(this->apartment)) CoUninitialize();
	}

	Q_DISABLE_COPY_MOVE(SettingsCache);

	[[nodiscard]] bool valid() const { return this->cache != nullptr; }

	bool reload() { return this->valid() && SUCCEEDED(this->cache->OnSettingChanged(SWIPE_SETTING)); }

	bool swipeOff() {
		DWORD value = 1;
		return this->valid() && SUCCEEDED(this->cache->GetDWORD(SWIPE_SETTING, &value)) && value == 0;
	}

private:
	HRESULT apartment = E_FAIL;
	Microsoft::WRL::ComPtr<SwipeSettingsCache> cache;
};

bool turnOffSwipe() {
	SwipeSetting previous;
	if (!restoreSwipeSetting() || !readSwipe(previous)) return false;

	SettingsCache cache;
	if (!cache.valid()) return false;
	if (previous.present != 0 && previous.value == 0) return cache.reload() && cache.swipeOff();
	if (cache.swipeOff()) return true;

	const auto saved = RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    SWIPE_BACKUP,
	    REG_BINARY,
	    &previous,
	    sizeof(previous)
	);
	if (saved != ERROR_SUCCESS) return false;

	const auto reloaded = writeSwipe({.present = 1, .value = 0}) && cache.reload();
	const auto restored = restoreSwipeSetting();
	if (reloaded && restored && cache.swipeOff()) return true;
	cache.reload();
	return false;
}

bool turnOnSwipe() {
	const auto restored = restoreSwipeSetting();
	SettingsCache cache;
	return cache.reload() && restored;
}

}

SystemSwipe::SystemSwipe(QObject* parent): QObject(parent) {
	this->retry.setSingleShot(true);
	QObject::connect(&this->retry, &QTimer::timeout, this, &SystemSwipe::acquire);
	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
		this->setSuppressRequested(false);
	});
}

SystemSwipe::~SystemSwipe() {
	this->setSuppressRequested(false);
	if (this->guard != nullptr) CloseHandle(this->guard);
}

int SystemSwipe::restoreWhenAbandoned() {
	auto* mutex = CreateMutexW(nullptr, FALSE, SWIPE_MUTEX);
	if (mutex == nullptr) return 1;
	const auto ownership = WaitForSingleObject(mutex, INFINITE);
	if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) return 1;
	const auto restored = turnOnSwipe();
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return restored ? 0 : 1;
}

void SystemSwipe::setSuppressRequested(bool requested) {
	if (requested == this->requested) return;
	this->requested = requested;
	this->attempts = 0;
	if (!requested) {
		if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
		this->release();
		return;
	}
	if (!supportsSettingsCache()) {
		qCWarning(logSystemSwipe) << "Turning the Windows touchpad swipe off is not supported on build"
		                          << windowsBuild();
		return;
	}
	QCoreApplication::instance()->installNativeEventFilter(this);
	this->acquire();
}

bool SystemSwipe::nativeEventFilter(
    const QByteArray& /*eventType*/,
    void* message,
    qintptr* /*result*/
) {
	if (static_cast<MSG*>(message)->message == WinAppBar::taskbarCreatedMessage()) {
		this->attempts = 0;
		this->retry.start(EXPLORER_SETTLE_MS);
	}
	return false;
}

void SystemSwipe::acquire() {
	if (!this->requested) return;
	if (this->lock() && this->startGuard() && turnOffSwipe()) {
		this->setSuppressed(true);
		return;
	}
	if (++this->attempts < MAX_ATTEMPTS) {
		this->retry.start(RETRY_MS);
		return;
	}
	qCWarning(logSystemSwipe) << "Could not turn the Windows touchpad swipe off.";
	this->release();
}

void SystemSwipe::release() {
	this->retry.stop();
	if (this->mutex != nullptr) {
		if (!turnOnSwipe()) {
			qCWarning(logSystemSwipe) << "Could not turn the Windows touchpad swipe back on.";
		}
		ReleaseMutex(this->mutex);
		CloseHandle(std::exchange(this->mutex, nullptr));
	}
	this->setSuppressed(false);
}

bool SystemSwipe::lock() {
	if (this->mutex != nullptr) return true;
	auto* mutex = CreateMutexW(nullptr, FALSE, SWIPE_MUTEX);
	if (mutex == nullptr) return false;
	const auto ownership = WaitForSingleObject(mutex, 0);
	if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) {
		CloseHandle(mutex);
		return false;
	}
	this->mutex = mutex;
	return true;
}

bool SystemSwipe::startGuard() {
	if (this->guard != nullptr) {
		if (WaitForSingleObject(this->guard, 0) == WAIT_TIMEOUT) return true;
		CloseHandle(std::exchange(this->guard, nullptr));
	}

	auto path = std::wstring(MAX_PATH_LENGTH, L'\0');
	const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	if (length == 0 || length >= path.size()) return false;
	path.resize(length);
	auto command = L"\"" + path + L"\" " + QString::fromLatin1(RESTORE_ARGUMENT).toStdWString();

	auto startup = STARTUPINFOW {.cb = sizeof(STARTUPINFOW)};
	auto process = PROCESS_INFORMATION {};
	const auto start = [&](DWORD flags) {
		const auto started = CreateProcessW(
		    path.c_str(),
		    command.data(),
		    nullptr,
		    nullptr,
		    FALSE,
		    flags,
		    nullptr,
		    nullptr,
		    &startup,
		    &process
		);
		return started != FALSE;
	};
	if (!start(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB) && !start(CREATE_NO_WINDOW)) {
		qCWarning(logSystemSwipe) << "Could not start the touchpad swipe guard:" << GetLastError();
		return false;
	}
	CloseHandle(process.hThread);
	this->guard = process.hProcess;
	return true;
}

void SystemSwipe::setSuppressed(bool suppressed) {
	if (suppressed == this->mSuppressed) return;
	this->mSuppressed = suppressed;
	qCInfo(logSystemSwipe) << "Windows touchpad swipe" << (suppressed ? "off" : "back on");
	emit this->suppressedChanged();
}

}
