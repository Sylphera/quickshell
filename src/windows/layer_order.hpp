#pragma once

#include <functional>

#include <qlist.h>
#include <qt_windows.h>

namespace qs::windows::layers {

[[nodiscard]] bool staysAbove(HWND hwnd);
[[nodiscard]] HWND anchor(const std::function<bool(HWND)>& skip = {});
bool apply(const QList<HWND>& chain);

}
