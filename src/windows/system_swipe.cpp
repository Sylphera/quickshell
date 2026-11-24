#include "system_swipe.hpp"
#include <array>
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
constexpr auto UNTOUCHED_VALUE = L"ThreeFingerTapEnabled";
constexpr auto STATE_KEY = L"Software\\Quickshell";
constexpr auto SWIPE_MUTEX = L"Local\\QuickshellTouchpadSwipe";
constexpr int UNTOUCHED_SETTING = 15;
constexpr DWORD UNTOUCHED_DEFAULT = 1;
constexpr DWORD MINIMUM_BUILD = 26100;
constexpr int RETRY_MS = 1000;
constexpr int EXPLORER_SETTLE_MS = 2000;
constexpr int MAX_ATTEMPTS = 60;
constexpr size_t MAX_PATH_LENGTH = 32768;

struct Swipe {
	SystemSwipe::Fingers fingers;
	const wchar_t* value;
	const wchar_t* backup;
	int setting;
};

constexpr std::array SWIPES = {
    Swipe {SystemSwipe::FourFingers, L"FourFingerSlideEnabled", L"TouchpadSwipeBackup", 18},
    Swipe {SystemSwipe::ThreeFingers, L"ThreeFingerSlideEnabled", L"TouchpadMoveBackup", 17},
};

struct SwipeSetting {
	DWORD present = 0;
	DWORD value = 0;
};

struct __declspec(uuid("4214f6fa-eb36-4e2f-9ca2-23fdc1832df7")) SwipeSettingsCache: IUnknown {
	virtual HRESULT STDMETHODCALLTYPE OnSettingChanged(int setting) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetBOOL(int setting, BOOL* enabled) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetDWORD(int setting, DWORD* value) = 0;
};

bool readSetting(const wchar_t* name, SwipeSetting& setting) {
	DWORD size = sizeof(setting.value);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    TOUCHPAD_KEY,
	    name,
	    RRF_RT_REG_DWORD,
	    nullptr,
	    &setting.value,
	    &size
	);
	setting.present = status == ERROR_SUCCESS ? 1 : 0;
	return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool writeSetting(const wchar_t* name, const SwipeSetting& setting) {
	if (setting.present == 0) {
		const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, TOUCHPAD_KEY, name);
		return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
	}
	const auto status = RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    TOUCHPAD_KEY,
	    name,
	    REG_DWORD,
	    &setting.value,
	    sizeof(setting.value)
	);
	return status == ERROR_SUCCESS;
}

bool restoreSetting(const Swipe& swipe) {
	SwipeSetting previous;
	DWORD size = sizeof(previous);
	const auto status = RegGetValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    swipe.backup,
	    RRF_RT_REG_BINARY,
	    nullptr,
	    &previous,
	    &size
	);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
	if (status != ERROR_SUCCESS || size != sizeof(previous) || previous.present > 1) return false;

	SwipeSetting current;
	if (!readSetting(swipe.value, current)) return false;
	if (current.present != 0 && current.value == 0 && !writeSetting(swipe.value, previous)) {
		return false;
	}
	const auto removed = RegDeleteKeyValueW(HKEY_CURRENT_USER, STATE_KEY, swipe.backup);
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

	bool understood() {
		SwipeSetting untouched;
		DWORD value = 0;
		if (!this->valid() || !readSetting(UNTOUCHED_VALUE, untouched)) return false;
		if (FAILED(this->cache->GetDWORD(UNTOUCHED_SETTING, &value))) return false;
		return value == (untouched.present != 0 ? untouched.value : UNTOUCHED_DEFAULT);
	}

	bool reload(const Swipe& swipe) {
		return this->valid() && SUCCEEDED(this->cache->OnSettingChanged(swipe.setting));
	}

	bool off(const Swipe& swipe) {
		DWORD value = 1;
		return this->valid() && SUCCEEDED(this->cache->GetDWORD(swipe.setting, &value)) && value == 0;
	}

private:
	HRESULT apartment = E_FAIL;
	Microsoft::WRL::ComPtr<SwipeSettingsCache> cache;
};

bool turnOff(const Swipe& swipe) {
	SwipeSetting previous;
	if (!restoreSetting(swipe) || !readSetting(swipe.value, previous)) return false;

	SettingsCache cache;
	if (!cache.understood()) return false;
	if (previous.present != 0 && previous.value == 0) return cache.reload(swipe) && cache.off(swipe);
	if (cache.off(swipe)) return true;

	const auto saved = RegSetKeyValueW(
	    HKEY_CURRENT_USER,
	    STATE_KEY,
	    swipe.backup,
	    REG_BINARY,
	    &previous,
	    sizeof(previous)
	);
	if (saved != ERROR_SUCCESS) return false;

	const auto reloaded = writeSetting(swipe.value, {.present = 1, .value = 0}) && cache.reload(swipe);
	const auto restored = restoreSetting(swipe);
	if (reloaded && restored && cache.off(swipe)) return true;
	cache.reload(swipe);
	return false;
}

bool turnOn(int fingers) {
	SettingsCache cache;
	auto done = true;
	for (const auto& swipe: SWIPES) {
		if ((fingers & swipe.fingers) == 0) continue;
		const auto restored = restoreSetting(swipe);
		done = cache.reload(swipe) && restored && done;
	}
	return done;
}

}

SystemSwipe::SystemSwipe(QObject* parent): QObject(parent) {
	this->retry.setSingleShot(true);
	QObject::connect(&this->retry, &QTimer::timeout, this, &SystemSwipe::acquire);
	QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
		this->setRequested(0);
	});
}

SystemSwipe::~SystemSwipe() {
	this->setRequested(0);
	if (this->guard != nullptr) CloseHandle(this->guard);
}

int SystemSwipe::restoreWhenAbandoned() {
	auto* mutex = CreateMutexW(nullptr, FALSE, SWIPE_MUTEX);
	if (mutex == nullptr) return 1;
	const auto ownership = WaitForSingleObject(mutex, INFINITE);
	if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) return 1;
	const auto restored = turnOn(FourFingers | ThreeFingers);
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	return restored ? 0 : 1;
}

void SystemSwipe::setRequested(int fingers) {
	if (fingers == this->requested) return;
	const auto before = std::exchange(this->requested, fingers);
	this->attempts = 0;
	if (fingers == 0) {
		if (auto* app = QCoreApplication::instance()) app->removeNativeEventFilter(this);
		this->release();
		return;
	}
	if (windowsBuild() < MINIMUM_BUILD) {
		qCWarning(logSystemSwipe) << "Turning the Windows touchpad swipe off is not supported on build"
		                          << windowsBuild();
		return;
	}
	if (before == 0) QCoreApplication::instance()->installNativeEventFilter(this);
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
	if (this->requested == 0) return;
	if (this->lock() && this->startGuard()) {
		const auto dropped = this->mSuppressed & ~this->requested;
		if (dropped != 0) turnOn(dropped);
		auto off = 0;
		for (const auto& swipe: SWIPES) {
			if ((this->requested & swipe.fingers) != 0 && turnOff(swipe)) off |= swipe.fingers;
		}
		this->setSuppressed(off);
		if (off == this->requested) return;
	}
	if (++this->attempts < MAX_ATTEMPTS) {
		this->retry.start(RETRY_MS);
		return;
	}
	qCWarning(logSystemSwipe) << "Could not turn every Windows touchpad swipe off.";
	if (this->mSuppressed == 0) this->release();
}

void SystemSwipe::release() {
	this->retry.stop();
	if (this->mutex != nullptr) {
		if (!turnOn(FourFingers | ThreeFingers)) {
			qCWarning(logSystemSwipe) << "Could not turn the Windows touchpad swipe back on.";
		}
		ReleaseMutex(this->mutex);
		CloseHandle(std::exchange(this->mutex, nullptr));
	}
	this->setSuppressed(0);
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

void SystemSwipe::setSuppressed(int fingers) {
	if (fingers == this->mSuppressed) return;
	this->mSuppressed = fingers;
	qCInfo(logSystemSwipe) << "Windows touchpad swipes turned off:" << fingers;
	emit this->suppressedChanged();
}

}
