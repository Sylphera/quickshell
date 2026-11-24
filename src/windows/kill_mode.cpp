#include "kill_mode.hpp"
#include <string>

#include <qt_windows.h>

#include <qcoreapplication.h>
#include <qfileinfo.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qobject.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>
#include <qtimer.h>

#include "util.hpp"

namespace qs::windows {

namespace {

Q_LOGGING_CATEGORY(logKill, "quickshell.windows.kill", QtInfoMsg);

constexpr auto* COVER_CLASS = L"QuickshellKillMode";
constexpr int ESCAPE_POLL_MS = 30;

QString classOf(HWND hwnd) {
	wchar_t name[128] {};
	return QString::fromWCharArray(name, GetClassNameW(hwnd, name, 128));
}

bool isShell(HWND hwnd) {
	static const auto classes = QSet<QString> {
	    QStringLiteral("Progman"),
	    QStringLiteral("WorkerW"),
	    QStringLiteral("Shell_TrayWnd"),
	    QStringLiteral("Shell_SecondaryTrayWnd"),
	    QStringLiteral("Windows.UI.Core.CoreWindow"),
	};
	return classes.contains(classOf(hwnd));
}

bool hasOtherWindows(HWND hwnd, DWORD pid) {
	struct Search {
		HWND skip;
		DWORD pid;
		bool found;
	};
	auto search = Search {hwnd, pid, false};
	EnumWindows(
	    [](HWND other, LPARAM param) -> BOOL {
		    auto* search = reinterpret_cast<Search*>(param);
		    if (other == search->skip || !IsWindowVisible(other) || GetWindow(other, GW_OWNER) != nullptr) return TRUE;
		    if ((GetWindowLongPtrW(other, GWL_STYLE) & WS_CAPTION) != WS_CAPTION) return TRUE;
		    DWORD pid = 0;
		    GetWindowThreadProcessId(other, &pid);
		    search->found = pid == search->pid;
		    return search->found ? FALSE : TRUE;
	    },
	    reinterpret_cast<LPARAM>(&search)
	);
	return search.found;
}

HWND appWindow(HWND hwnd) {
	if (classOf(hwnd) != QStringLiteral("ApplicationFrameWindow")) return hwnd;
	auto* content = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
	return content != nullptr ? content : hwnd;
}

}

KillMode* KillMode::instance() {
	static QPointer<KillMode> mode; // NOLINT
	if (mode.isNull()) mode = new KillMode(QCoreApplication::instance());
	return mode.data();
}

KillMode::KillMode(QObject* parent): QObject(parent) {
	this->escape.setInterval(ESCAPE_POLL_MS);
	QObject::connect(&this->escape, &QTimer::timeout, this, [this] {
		if (GetAsyncKeyState(VK_ESCAPE) < 0) this->stop();
	});
}

void KillMode::start() {
	if (this->cover != nullptr) {
		this->stop();
		return;
	}

	auto* module = GetModuleHandleW(nullptr);
	WNDCLASSW cls {};
	cls.hInstance = module;
	cls.lpfnWndProc = &KillMode::proc;
	cls.hCursor = LoadCursorW(nullptr, IDC_CROSS);
	cls.lpszClassName = COVER_CLASS;
	RegisterClassW(&cls);

	this->cover = CreateWindowExW(
	    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
	    COVER_CLASS,
	    L"",
	    WS_POPUP,
	    GetSystemMetrics(SM_XVIRTUALSCREEN),
	    GetSystemMetrics(SM_YVIRTUALSCREEN),
	    GetSystemMetrics(SM_CXVIRTUALSCREEN),
	    GetSystemMetrics(SM_CYVIRTUALSCREEN),
	    nullptr,
	    nullptr,
	    module,
	    nullptr
	);
	if (this->cover == nullptr) {
		qCWarning(logKill) << "Could not create the kill overlay" << GetLastError();
		return;
	}
	SetLayeredWindowAttributes(this->cover, 0, 1, LWA_ALPHA);
	ShowWindow(this->cover, SW_SHOWNOACTIVATE);
	this->escape.start();
}

void KillMode::stop() {
	this->escape.stop();
	if (this->cover != nullptr) DestroyWindow(this->cover);
	this->cover = nullptr;
}

void KillMode::killAt(POINT point) {
	auto* hit = WindowFromPhysicalPoint(point);
	auto* root = hit == nullptr ? nullptr : GetAncestor(hit, GA_ROOT);
	if (root == nullptr || isOwnProcessWindow(root) || isShell(root)) return;

	DWORD pid = 0;
	GetWindowThreadProcessId(appWindow(root), &pid);
	if (pid == 0) return;

	auto* process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (process == nullptr) {
		qCWarning(logKill) << "Could not open process" << pid << GetLastError();
		return;
	}
	wchar_t path[MAX_PATH * 2] {};
	DWORD length = MAX_PATH * 2;
	QueryFullProcessImageNameW(process, 0, path, &length);
	const auto exe = QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).fileName();

	if (exe.compare(QStringLiteral("explorer.exe"), Qt::CaseInsensitive) == 0 || hasOtherWindows(root, pid)) {
		PostMessageW(root, WM_CLOSE, 0, 0);
		qCInfo(logKill) << "Closed one window of" << exe << pid;
	} else if (!TerminateProcess(process, 1)) {
		qCWarning(logKill) << "Could not kill" << exe << pid << GetLastError();
	} else {
		qCInfo(logKill) << "Killed" << exe << pid;
	}
	CloseHandle(process);
}

LRESULT CALLBACK KillMode::proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_LBUTTONUP: {
		POINT point {};
		GetPhysicalCursorPos(&point);
		KillMode::instance()->stop();
		KillMode::killAt(point);
		return 0;
	}
	case WM_RBUTTONUP: KillMode::instance()->stop(); return 0;
	default: return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

}
