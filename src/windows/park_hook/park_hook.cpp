
#include <windows.h>

#pragma section(".qsshared", read, write, shared)
__declspec(allocate(".qsshared")) volatile LONG gEnabled = 0;   // NOLINT
__declspec(allocate(".qsshared")) volatile DWORD gShellPid = 0; // NOLINT

namespace {

constexpr const wchar_t* PROP_PARKED = L"qs.parked";
constexpr const wchar_t* PROP_X = L"qs.px";
constexpr const wchar_t* PROP_Y = L"qs.py";
constexpr DWORD GIVE_UP_MS = 5000;

int parkedTop() {
	return GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) + 100;
}

bool qualifies(HWND hwnd) {
	if (GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER) != nullptr) return false;
	auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((style & WS_THICKFRAME) == 0 || (style & WS_CHILD) != 0) return false;
	if ((exStyle & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) {
		return false;
	}
	return (style & WS_MINIMIZE) == 0;
}

void remember(HWND hwnd, int x, int y) {
	SetPropW(hwnd, PROP_X, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(x))); // NOLINT
	SetPropW(hwnd, PROP_Y, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(y))); // NOLINT
}

void forget(HWND hwnd) {
	RemovePropW(hwnd, PROP_PARKED);
	RemovePropW(hwnd, PROP_X);
	RemovePropW(hwnd, PROP_Y);
}

void park(HWND hwnd, int x) {
	SetWindowPos(
	    hwnd,
	    nullptr,
	    x,
	    parkedTop(),
	    0,
	    0,
	    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER
	);
}

bool givenUp(HWND hwnd, ULONG_PTR parkedAt) {
	if (GetTickCount() - static_cast<DWORD>(parkedAt) <= GIVE_UP_MS) return false;
	forget(hwnd);
	return true;
}

void onPosChanging(HWND hwnd, const WINDOWPOS* pos) {
	if ((pos->flags & SWP_SHOWWINDOW) == 0 || IsWindowVisible(hwnd)) return;
	if (GetPropW(hwnd, PROP_PARKED) != nullptr) return;
	if (GetCurrentProcessId() == gShellPid || !qualifies(hwnd)) return;

	RECT rect {};
	GetWindowRect(hwnd, &rect);
	auto x = (pos->flags & SWP_NOMOVE) == 0 ? pos->x : rect.left;
	auto y = (pos->flags & SWP_NOMOVE) == 0 ? pos->y : rect.top;
	SetPropW(
	    hwnd,
	    PROP_PARKED,
	    reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetTickCount() | 1))
	); // NOLINT
	remember(hwnd, x, y);
	park(hwnd, x);
}

void onPosChanged(HWND hwnd, const WINDOWPOS* pos) {
	if ((pos->flags & SWP_NOMOVE) != 0) return;
	auto parkedAt = reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, PROP_PARKED)); // NOLINT
	if (parkedAt == 0 || givenUp(hwnd, parkedAt)) return;
	if (pos->y >= parkedTop() - 50) return;
	remember(hwnd, pos->x, pos->y);
	park(hwnd, pos->x);
}
}

extern "C" {

__declspec(dllexport) LRESULT CALLBACK qsParkCallWndProc(int code, WPARAM wParam, LPARAM lParam) {
	if (code == HC_ACTION && gEnabled != 0) {
		const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam); // NOLINT
		if (cwp->lParam != 0) {
			const auto* pos = reinterpret_cast<const WINDOWPOS*>(cwp->lParam); // NOLINT
			if (cwp->message == WM_WINDOWPOSCHANGING) onPosChanging(cwp->hwnd, pos);
			else if (cwp->message == WM_WINDOWPOSCHANGED) onPosChanged(cwp->hwnd, pos);
		}
	}
	return CallNextHookEx(nullptr, code, wParam, lParam);
}

__declspec(dllexport) void qsParkSetEnabled(BOOL enabled, DWORD shellPid) {
	gShellPid = shellPid;
	InterlockedExchange(&gEnabled, enabled != FALSE ? 1 : 0);
}

}

BOOL WINAPI DllMain(HINSTANCE /*instance*/, DWORD /*reason*/, LPVOID /*reserved*/) { return TRUE; }
