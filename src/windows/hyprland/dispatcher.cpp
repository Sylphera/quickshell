#include "dispatcher.hpp"
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include <qdir.h>
#include <qhash.h>
#include <qlist.h>
#include <qlogging.h>
#include <qloggingcategory.h>
#include <qpointer.h>
#include <qpoint.h>
#include <qregularexpression.h>
#include <qscreen.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qtimer.h>
#include <qtypes.h>

#include "../keyboard_hook.hpp"
#include "../kill_mode.hpp"
#include "../scratchpad.hpp"
#include "../screen_zoom.hpp"
#include "../tiling.hpp"
#include "../util.hpp"
#include "../virtual_desktops.hpp"
#include "../window_manager.hpp"
#include "../window_motion.hpp"
#include "../window_tracker.hpp"
#include "connection.hpp"
#include "workspace.hpp"

#include <dwmapi.h>
#include <shellapi.h>

using namespace qs::windows;

namespace qs::hyprland::ipc {

namespace {
Q_LOGGING_CATEGORY(logDispatch, "quickshell.hyprland.dispatch", QtWarningMsg);

void sendWinShortcut(WORD key) {
	qs::windows::hotkeys::KeyboardHook::sendChord(
	    qs::windows::hotkeys::ModSuper,
	    static_cast<uint8_t>(key)
	);
}

std::optional<TilingManager::Edge> directionEdge(const QString& direction) {
	auto d = direction.trimmed();
	if (d.startsWith('l')) return TilingManager::Edge::Left;
	if (d.startsWith('r')) return TilingManager::Edge::Right;
	if (d.startsWith('u') || d.startsWith('t')) return TilingManager::Edge::Top;
	if (d.startsWith('d') || d.startsWith('b')) return TilingManager::Edge::Bottom;
	return std::nullopt;
}

std::optional<double> parseFraction(QString text) {
	text = text.trimmed();
	auto percent = text.endsWith('%');
	if (percent) text.chop(1);

	auto value = 0.0;
	auto slash = text.indexOf('/');

	if (slash != -1) {
		auto okTop = false;
		auto okBottom = false;
		auto top = text.left(slash).toDouble(&okTop);
		auto bottom = text.mid(slash + 1).toDouble(&okBottom);
		if (!okTop || !okBottom || bottom == 0.0) return std::nullopt;
		value = top / bottom;
	} else {
		auto ok = false;
		value = text.toDouble(&ok);
		if (!ok) return std::nullopt;
	}

	return percent ? value / 100.0 : value;
}

std::optional<qs::windows::tiling::FitMode> fitMode(const QString& name) {
	using qs::windows::tiling::FitMode;
	if (name == "active") return FitMode::Active;
	if (name == "visible") return FitMode::Visible;
	if (name == "all") return FitMode::All;
	if (name == "toend") return FitMode::ToEnd;
	if (name == "tobeg" || name == "tobeginning") return FitMode::ToBeginning;
	return std::nullopt;
}

bool isRelativeWorkspace(const QString& arg) {
	auto a = arg.trimmed();
	if (a.startsWith('e') || a.startsWith('r') || a.startsWith('m')) a = a.mid(1);
	return a.startsWith('+') || a.startsWith('-');
}

WORD arrowKey(const QString& direction) {
	if (direction.startsWith('l')) return VK_LEFT;
	if (direction.startsWith('r')) return VK_RIGHT;
	if (direction.startsWith('u')) return VK_UP;
	if (direction.startsWith('d')) return VK_DOWN;
	return 0;
}

void launchCommandLine(const QString& line) {
	auto commandLine = line.toStdWString();
	auto home = QDir::toNativeSeparators(QDir::homePath()).toStdWString();

	STARTUPINFOW startup {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process {};

	auto ok = CreateProcessW(
	    nullptr,
	    commandLine.data(),
	    nullptr,
	    nullptr,
	    FALSE,
	    CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
	    nullptr,
	    home.c_str(),
	    &startup,
	    &process
	);

	if (ok) {
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		return;
	}

	QString file;
	QString params;

	if (line.startsWith('"')) {
		auto end = line.indexOf('"', 1);
		file = line.mid(1, end - 1);
		if (end != -1) params = line.mid(end + 1).trimmed();
	} else {
		auto space = line.indexOf(' ');
		file = space == -1 ? line : line.left(space);
		if (space != -1) params = line.mid(space + 1).trimmed();
	}

	auto wfile = file.toStdWString();
	auto wparams = params.toStdWString();

	auto result = ShellExecuteW(
	    nullptr,
	    L"open",
	    wfile.c_str(),
	    params.isEmpty() ? nullptr : wparams.c_str(),
	    home.c_str(),
	    SW_SHOWNORMAL
	);

	if (reinterpret_cast<INT_PTR>(result) <= 32) {
		qCWarning(logDispatch) << "exec failed for" << line << "error"
		                       << reinterpret_cast<INT_PTR>(result);
	}
}

void moveWindowTo(TrackedWindow* window, qsizetype index, bool follow) {
	if (!window->moveToDesktop(index)) return;

	auto* behavior = WorkspaceBehavior::instance();
	if (!follow) {
		behavior->expectFocus(VirtualDesktops::instance()->currentIndex());
		return;
	}

	behavior->prepareWarp(index, true);
	window->activate();
}

bool closeShellWindow(HWND hwnd) {
	if (hwnd == nullptr || !isOwnProcessWindow(hwnd)) return false;
	auto exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	if ((exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) return false;
	if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
	PostMessageW(hwnd, WM_CLOSE, 0, 0);
	return true;
}

constexpr int WARP_AFTER_MOVE_MS = 120;

void warpToWindow(HWND hwnd) {
	RECT frame {};
	if (hwnd == nullptr || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
		return;
	}
	POINT cursor {};
	if (GetCursorPos(&cursor) && PtInRect(&frame, cursor)) return;
	SetCursorPos((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
}

void warpToWindowLater(TrackedWindow* window, int delayMs) {
	if (window == nullptr) return;
	QTimer::singleShot(delayMs, window, [window = QPointer(window)]() {
		if (window != nullptr) warpToWindow(window->hwnd());
	});
}

constexpr int WARP_FOCUS_TIMEOUT_MS = 300;

void warpToFocusChange(WindowTracker* tracker) {
	auto* before = tracker->activeWindow();
	auto* context = new QObject(tracker);
	QObject::connect(tracker, &WindowTracker::activeWindowChanged, context, [tracker, before, context]() {
		auto* active = tracker->activeWindow();
		if (active == nullptr || active == before) return;
		warpToWindow(active->hwnd());
		context->deleteLater();
	});
	QTimer::singleShot(WARP_FOCUS_TIMEOUT_MS, context, [context]() { context->deleteLater(); });
}

bool aloneOnDesktop(WindowTracker* tracker, TrackedWindow* window) {
	auto special = Scratchpad::holds(window);
	for (auto* other: tracker->windows()) {
		if (other == window || other->minimized()) continue;
		if (Scratchpad::holds(other) != special) continue;
		if (!special && other->desktop() != window->desktop()) continue;
		return false;
	}
	return true;
}

void closeFocused(WindowTracker* tracker) {
	tracker->syncActive();
	if (auto* window = tracker->activeWindow()) {
		window->close();
	} else {
		closeShellWindow(GetForegroundWindow());
	}
}

} // namespace

void Dispatcher::dispatch(const QString& request) {
	auto trimmed = request.trimmed();
	if (trimmed.isEmpty()) return;

	qCDebug(logDispatch) << "dispatch" << trimmed;

	if (trimmed.startsWith("hl.")) {
		auto call = Dispatcher::parseLua(trimmed);
		if (!call.ok) {
			qCWarning(logDispatch) << "Could not parse dispatch request" << trimmed;
			return;
		}

		this->dispatchLua(call);
		return;
	}

	auto space = trimmed.indexOf(' ');
	auto name = space == -1 ? trimmed : trimmed.left(space);
	auto args = space == -1 ? QString() : trimmed.mid(space + 1).trimmed();
	this->dispatchClassic(name, args);
}

QString Dispatcher::unquote(QString value) {
	value = value.trimmed();
	if (value.length() >= 2) {
		auto first = value.front();
		if ((first == '"' || first == '\'') && value.back() == first) {
			return value.mid(1, value.length() - 2);
		}
	}

	return value;
}

Dispatcher::LuaCall Dispatcher::parseLua(const QString& request) {
	LuaCall call;

	auto open = request.indexOf('(');
	auto close = request.lastIndexOf(')');
	if (open == -1 || close == -1 || close < open) return call;

	call.function = request.left(open).trimmed();
	call.ok = true;

	auto inner = request.mid(open + 1, close - open - 1).trimmed();
	if (inner.isEmpty()) return call;

	if (!inner.startsWith('{')) {
		call.scalar = Dispatcher::unquote(inner);
		return call;
	}

	auto end = inner.lastIndexOf('}');
	auto body = inner.mid(1, end - 1);

	QStringList fields;
	qsizetype start = 0;
	auto depth = 0;
	auto quote = QChar();

	for (qsizetype i = 0; i < body.length(); i++) {
		auto c = body[i];

		if (!quote.isNull()) {
			if (c == quote) quote = QChar();
			continue;
		}

		if (c == '"' || c == '\'') quote = c;
		else if (c == '{') depth++;
		else if (c == '}') depth--;
		else if (c == ',' && depth == 0) {
			fields.append(body.mid(start, i - start));
			start = i + 1;
		}
	}

	fields.append(body.mid(start));

	for (const auto& field: fields) {
		auto eq = field.indexOf('=');
		if (eq == -1) {
			auto value = Dispatcher::unquote(field);
			if (!value.isEmpty()) call.positional.append(value);
			continue;
		}

		call.table.insert(field.left(eq).trimmed(), Dispatcher::unquote(field.mid(eq + 1)));
	}

	return call;
}

void Dispatcher::dispatchLua(const LuaCall& call) {
	const auto& fn = call.function;
	const auto& t = call.table;
	auto windowArg = t.value("window");

	if (fn == "hl.dsp.focus") {
		if (t.contains("workspace")) this->focusWorkspace(t.value("workspace"));
		else if (t.contains("direction")) this->moveFocus(t.value("direction"));
		else if (t.contains("window")) {
			if (auto* window = this->resolveWindow(windowArg)) window->activate();
		} else if (t.contains("monitor")) this->unsupported("focus monitor");
		else this->unsupported(fn);
	} else if (fn == "hl.dsp.window.move") {
		auto* window = this->resolveWindow(windowArg);
		if (t.contains("workspace")) {
			if (!t.contains("window")) {
				this->ipc->tracker()->syncActive();
				window = this->ipc->tracker()->activeWindow();
			}
			this->moveToWorkspace(window, t.value("workspace"), t.value("follow") != "false");
		} else if (t.contains("x") || t.contains("y")) {
			this->moveWindowPixel(window, t.value("x"), t.value("y"));
		} else if (t.contains("direction")) {
			this->moveWindow(t.value("direction"));
		} else {
			this->unsupported(fn);
		}
	} else if (fn == "hl.dsp.window.close") {
			if (t.contains("window")) {
				if (auto* window = this->resolveWindow(windowArg)) window->close();
			} else {
				closeFocused(this->ipc->tracker());
			}
	} else if (fn == "hl.dsp.window.kill_active" || fn == "hl.dsp.kill_active") {
			closeFocused(this->ipc->tracker());
	} else if (fn == "hl.dsp.window.pin") {
		this->pin(this->resolveWindow(windowArg));
	} else if (fn == "hl.dsp.window.fullscreen") {
		auto mode = t.value("mode");
		this->fullscreen(windowArg, mode == "maximized" || mode == "maximize" || mode == "1" ? 1 : 0);
	} else if (fn == "hl.dsp.window.maximize") {
		this->fullscreen(windowArg, 1);
	} else if (fn == "hl.dsp.global") {
		emit this->ipc->dispatchGlobal(call.scalar);
	} else if (fn == "hl.dsp.exec" || fn == "hl.dsp.exec_cmd") {
		this->exec(call.scalar);
	} else if (fn.startsWith("hl.config")) {
		qCDebug(logDispatch) << "Ignoring config change" << fn;
	} else if (fn == "hl.dsp.window.toggle_float") {
		this->toggleFloating(this->resolveWindow(windowArg), "toggle");
	} else if (fn == "hl.dsp.window.float" || fn == "hl.dsp.window.tile") {
		auto action = t.value("action");
		auto off = action == "disable" || action == "unset" || action == "off" || action == "false";
		auto floating = (fn == "hl.dsp.window.float") != off;
		auto mode = QStringLiteral("toggle");
		if (action != "toggle") mode = floating ? QStringLiteral("float") : QStringLiteral("tile");
		this->toggleFloating(this->resolveWindow(windowArg), mode);
	} else if (fn == "hl.dsp.window.swap") {
		if (t.contains("direction")) this->swapWindow(t.value("direction"));
		else this->unsupported(fn);
	} else if (fn == "hl.dsp.window.resize") {
		if (t.contains("x") || t.contains("y")) {
			auto exact = call.positional.contains("exact") || t.value("exact") == "true";
			auto x = t.value("x", "0");
			auto y = t.value("y", "0");
			this->resizeActive((exact ? "exact " : "") + x + ' ' + y);
		} else {
			this->unsupported(fn + " (mouse)");
		}
	} else if (fn == "hl.dsp.window.center") {
		this->centerWindow();
	} else if (fn == "hl.dsp.layout") {
		this->layoutMessage(call.scalar);
	} else if (fn == "hl.dsp.workspace.toggle_special") {
		Scratchpad::instance()->toggle();
	} else if (fn == "hl.dsp.window.drag") {
		this->unsupported(fn);
	} else {
		this->unknown(fn);
	}
}

void Dispatcher::dispatchClassic(const QString& name, const QString& args) {
	auto* tracker = this->ipc->tracker();

	if (name == "workspace") {
		this->focusWorkspace(args);
	} else if (name == "movetoworkspace" || name == "movetoworkspacesilent") {
		auto comma = args.indexOf(',');
		auto workspace = comma == -1 ? args : args.left(comma);
		auto window = comma == -1 ? QString() : args.mid(comma + 1);
		if (window.trimmed().isEmpty()) tracker->syncActive();
		auto* target = window.trimmed().isEmpty() ? tracker->activeWindow() : this->resolveWindow(window);
		this->moveToWorkspace(target, workspace, name == "movetoworkspace");
	} else if (name == "killactive") {
			closeFocused(tracker);
	} else if (name == "closewindow") {
			if (args.isEmpty()) {
				closeFocused(tracker);
			} else if (auto* window = this->resolveWindow(args)) {
				window->close();
			}
	} else if (name == "focuswindow") {
		if (auto* window = this->resolveWindow(args)) window->activate();
	} else if (name == "fullscreen" || name == "fullscreenstate") {
		this->fullscreen({}, args.trimmed() == "1" ? 1 : 0);
	} else if (name == "pin") {
		this->pin(this->resolveWindow(args));
	} else if (name == "movefocus") {
		this->moveFocus(args);
	} else if (name == "cyclenext") {
		this->cycleFocus(args == "prev");
	} else if (name == "movewindow") {
		this->moveWindow(args);
	} else if (name == "movewindowpixel") {
		auto comma = args.indexOf(',');
		auto coords = (comma == -1 ? args : args.left(comma)).split(' ', Qt::SkipEmptyParts);
		auto window = comma == -1 ? QString() : args.mid(comma + 1);
		if (coords.length() >= 3 && coords[0] == "exact") {
			this->moveWindowPixel(this->resolveWindow(window), coords[1], coords[2]);
		} else {
			this->unsupported("movewindowpixel (relative)");
		}
	} else if (name == "exec") {
		this->exec(args);
	} else if (name == "global") {
		emit this->ipc->dispatchGlobal(args.trimmed());
	} else if (name == "swapwindow") {
		this->swapWindow(args);
	} else if (name == "togglefloating") {
		this->toggleFloating(this->resolveWindow(args), "toggle");
	} else if (name == "setfloating") {
		this->toggleFloating(this->resolveWindow(args), "float");
	} else if (name == "settiled") {
		this->toggleFloating(this->resolveWindow(args), "tile");
	} else if (name == "togglesplit") {
		this->toggleSplit();
	} else if (name == "layoutmsg") {
		this->layoutMessage(args);
	} else if (name == "resizeactive") {
		this->resizeActive(args);
	} else if (name == "centerwindow") {
		this->centerWindow();
	} else if (name == "togglespecialworkspace") {
		Scratchpad::instance()->toggle();
	} else if (name == "kill") {
		KillMode::instance()->start();
	} else if (name == "screenzoom") {
		if (args == "in" || args == "out") adjustScreenZoom(args == "in");
		else if (args == "reset") resetScreenZoom();
	} else if (name == "pseudo" || name == "focusmonitor"
	           || name == "movecurrentworkspacetomonitor"
	           || name == "swapnext" || name == "togglegroup" || name == "submap")
	{
		this->unsupported(name);
	} else {
		this->unknown(name);
	}
}

qsizetype Dispatcher::resolveWorkspace(const QString& arg, bool& create) const {
	auto a = arg.trimmed();
	create = false;

	auto* desktops = this->ipc->desktops();
	auto current = desktops->shownIndex();
	auto count = desktops->count();

	if (a == "previous" || a == "previous_per_monitor") {
		return WorkspaceBehavior::instance()->previousDesktop();
	}

	if (a.isEmpty() || a.startsWith("special")) return -1;

	if (a.startsWith("name:")) {
		auto* workspace = this->ipc->workspaceByName(a.mid(5));
		return workspace == nullptr ? -1 : workspace->bindableId().value() - 1;
	}

	if (a == "empty") {
		for (qsizetype i = 0; i < count; i++) {
			auto* workspace = this->ipc->workspaceById(static_cast<qint32>(i + 1));
			if (workspace != nullptr && workspace->toplevels()->valueList().isEmpty()) return i;
		}

		create = true;
		return count;
	}

	auto relative = a;
	auto onlyExisting = a.startsWith('e') || a.startsWith('m');
	if (onlyExisting || a.startsWith('r')) relative = a.mid(1);

	if (relative.startsWith('+') || relative.startsWith('-')) {
		auto ok = false;
		auto delta = relative.toInt(&ok);
		if (!ok) return -1;
		if (onlyExisting) return this->existingWorkspace(current, delta);

		create = true;
		return qMax(qsizetype(0), current + delta);
	}

	auto ok = false;
	auto number = a.toInt(&ok);
	if (!ok || number < 1) return -1;

	create = true;
	return number - 1;
}

qsizetype Dispatcher::existingWorkspace(qsizetype current, int delta) const {
	QList<qsizetype> existing;
	for (qsizetype i = 0; i < this->ipc->desktops()->count(); i++) {
		auto* workspace = this->ipc->workspaceById(static_cast<qint32>(i + 1));
		auto occupied = workspace != nullptr && !workspace->toplevels()->valueList().isEmpty();
		if (i == current || occupied) existing.append(i);
	}

	auto position = existing.indexOf(current);
	if (position == -1) return -1;
	auto count = existing.length();
	return existing[((position + delta) % count + count) % count];
}

TrackedWindow* Dispatcher::resolveWindow(const QString& arg) const {
	auto a = arg.trimmed();
	auto* tracker = this->ipc->tracker();

	if (a.isEmpty() || a == "activewindow") return tracker->activeWindow();

	if (a.startsWith("address:")) {
		auto hex = a.mid(8);
		if (hex.startsWith("0x")) hex = hex.mid(2);
		auto ok = false;
		auto address = hex.toULongLong(&ok, 16);
		if (!ok) return nullptr;
		auto* hwnd = reinterpret_cast<HWND>(static_cast<quintptr>(address)); // NOLINT
		return tracker->windowFor(hwnd);
	}

	if (a.startsWith("pid:")) {
		auto pid = a.mid(4).toUInt();
		for (auto* window: tracker->windows()) {
			if (window->pid() == pid) return window;
		}

		return nullptr;
	}

	auto colon = a.indexOf(':');
	if (colon != -1) {
		auto kind = a.left(colon);
		auto pattern = QRegularExpression(a.mid(colon + 1));
		auto byTitle = kind == "title" || kind == "initialtitle";
		auto byClass = kind == "class" || kind == "initialclass";

		if (byTitle || byClass) {
			for (auto* window: tracker->windows()) {
				auto value = byClass ? window->appId() : window->title();
				if (value == a.mid(colon + 1) || pattern.match(value).hasMatch()) return window;
			}

			return nullptr;
		}
	}

	qCWarning(logDispatch) << "Unsupported window selector" << a;
	return nullptr;
}

void Dispatcher::focusWorkspace(const QString& arg) {
	auto create = false;
	auto index = this->resolveWorkspace(arg, create);

	if (index < 0) {
		if (!arg.trimmed().startsWith("previous")) this->unsupported("workspace " + arg.trimmed());
		return;
	}

	auto* desktops = this->ipc->desktops();
	auto* behavior = WorkspaceBehavior::instance();

	if (index == desktops->shownIndex()) {
		auto previous = behavior->previousDesktop();
		if (!behavior->backAndForth() || isRelativeWorkspace(arg) || previous < 0 || previous == index) {
			behavior->warpToActive();
			return;
		}

		index = previous;
	}

	if (index >= desktops->count()) {
		if (!create) return;

		desktops->ensureCount(index + 1, [desktops, index](bool ok) {
			if (!ok) return;
			WorkspaceBehavior::instance()->prepareWarp(index);
			WorkspaceBehavior::instance()->expectFocus(index);
			desktops->switchToAsync(index);
		});

		return;
	}

	behavior->prepareWarp(index);
	behavior->expectFocus(index);
	desktops->switchToAsync(index);
}

void Dispatcher::moveToWorkspace(TrackedWindow* window, const QString& arg, bool follow) {
	if (window == nullptr) return;

	auto* scratchpad = Scratchpad::instance();
	if (arg.trimmed().startsWith("special")) {
		scratchpad->add(window, follow);
		return;
	}
	if (scratchpad->contains(window)) scratchpad->remove(window);

	auto create = false;
	auto index = this->resolveWorkspace(arg, create);

	if (index < 0) {
		if (!arg.trimmed().startsWith("previous")) this->unsupported("movetoworkspace " + arg.trimmed());
		return;
	}

	auto* desktops = this->ipc->desktops();
	if (index >= desktops->count()) {
		if (!create) return;

		desktops->ensureCount(index + 1, [window = QPointer(window), index, follow](bool ok) {
			if (ok && window) moveWindowTo(window, index, follow);
		});

		return;
	}

	moveWindowTo(window, index, follow);
}

void Dispatcher::fullscreen(const QString& selector, int mode) {
	auto target = selector.trimmed();
	if (target.isEmpty() || target == "activewindow") this->ipc->tracker()->syncActive();

	auto* window = this->resolveWindow(target);
	auto* tiling = TilingManager::active();
	if (tiling != nullptr && target.isEmpty()) window = tiling->fullscreenShortcutTarget(window);
	if (window == nullptr || window->minimized()) return;

	auto special = Scratchpad::holds(window);
	if (special && !Scratchpad::instance()->shown()) return;

	if (tiling != nullptr) {
		if (mode == 1 ? tiling->toggleFullWidth(window) || tiling->toggleMaximize(window)
		              : tiling->toggleFullscreen(window))
			return;
	}

	if (!special) window->setMaximized(!window->maximized());
}

void Dispatcher::pin(TrackedWindow* window) {
	if (window == nullptr) return;

	auto* desktops = this->ipc->desktops();
	if (!desktops->accessorLoaded()) {
		this->unsupported("pin (needs VirtualDesktopAccessor.dll)");
		return;
	}

	auto* hwnd = window->hwnd();
	const auto pin = !desktops->isWindowPinned(hwnd);
	auto* tiling = TilingManager::active();
	if (tiling != nullptr && !tiling->canPin(window)) return;
	auto* scratchpad = Scratchpad::instance();
	if (scratchpad->contains(window)) {
		scratchpad->remove(window);
		desktops->pinWindow(hwnd, true);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
		             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
		return;
	}
	if (!pin && scratchpad->shown()) {
		scratchpad->add(window, false);
		return;
	}
	if (!desktops->pinWindow(hwnd, pin) || tiling == nullptr) return;
	SetWindowPos(hwnd, pin ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void Dispatcher::cycleFocus(bool backwards) {
	auto* tracker = this->ipc->tracker();
	auto* desktops = this->ipc->desktops();
	tracker->syncActive();
	auto* active = tracker->activeWindow();
	const auto inSpecial = Scratchpad::holds(active) && Scratchpad::instance()->shown();

	QList<TrackedWindow*> candidates;
	for (auto* window: tracker->windows()) {
		auto* hwnd = window->hwnd();
		if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd) || window->minimized() || IsIconic(hwnd))
			continue;
		if (Scratchpad::holds(window) != inSpecial || !desktops->isWindowOnCurrent(hwnd)) continue;

		DWORD cloaked = 0;
		if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0)
			continue;

		candidates.append(window);
	}

	if (candidates.isEmpty()) return;
	const auto current = candidates.indexOf(active);
	const auto count = candidates.size();
	const auto next = current < 0 ? (backwards ? count - 1 : 0)
	                              : (current + count + (backwards ? -1 : 1)) % count;
	if (auto* target = candidates.at(next); target != active) {
		warpToFocusChange(tracker);
		target->activate(false);
	}
}

void Dispatcher::moveFocus(const QString& direction) {
	auto edge = directionEdge(direction);
	auto* tiling = TilingManager::active();
	if (tiling != nullptr && edge) {
		warpToFocusChange(this->ipc->tracker());
		if (tiling->focusDirection(*edge)) return;
	}

	auto* tracker = this->ipc->tracker();
	auto* desktops = this->ipc->desktops();
	auto* active = tracker->activeWindow();
	auto current = desktops->currentIndex();

	QList<TrackedWindow*> candidates;
	for (auto* window: tracker->windows()) {
		if (window == active || window->minimized()) continue;
		if (window->desktop() != -1 && window->desktop() != current) continue;
		candidates.append(window);
	}

	if (candidates.isEmpty()) return;

	if (active == nullptr) {
		candidates.first()->activate();
		warpToWindowLater(candidates.first(), 0);
		return;
	}

	auto origin = active->rect().center();
	TrackedWindow* best = nullptr;
	auto bestScore = std::numeric_limits<qint64>::max();

	for (auto* window: candidates) {
		auto delta = window->rect().center() - origin;
		qint64 primary = 0;
		qint64 secondary = 0;

		if (direction.startsWith('l')) {
			if (delta.x() >= 0) continue;
			primary = -delta.x();
			secondary = qAbs(delta.y());
		} else if (direction.startsWith('r')) {
			if (delta.x() <= 0) continue;
			primary = delta.x();
			secondary = qAbs(delta.y());
		} else if (direction.startsWith('u')) {
			if (delta.y() >= 0) continue;
			primary = -delta.y();
			secondary = qAbs(delta.x());
		} else if (direction.startsWith('d')) {
			if (delta.y() <= 0) continue;
			primary = delta.y();
			secondary = qAbs(delta.x());
		} else {
			this->unsupported("movefocus " + direction);
			return;
		}

		auto score = primary + secondary * 2;
		if (score < bestScore) {
			bestScore = score;
			best = window;
		}
	}

	if (best != nullptr) {
		best->activate();
		warpToWindowLater(best, 0);
	}
}

void Dispatcher::moveWindow(const QString& direction) {
	if (direction.startsWith("mon:")) {
		this->unsupported("movewindow mon:");
		return;
	}

	auto edge = directionEdge(direction);
	auto* tiling = TilingManager::active();
	if (tiling != nullptr && edge) {
		auto* active = this->ipc->tracker()->activeWindow();
		if (!tiling->moveDirection(*edge)) tiling->moveFloating(active, *edge);
		warpToWindowLater(active, WARP_AFTER_MOVE_MS);
		return;
	}

	auto key = arrowKey(direction.trimmed());
	if (key == 0) {
		this->unsupported("movewindow " + direction);
		return;
	}

	sendWinShortcut(key);
}

void Dispatcher::swapWindow(const QString& direction) {
	auto edge = directionEdge(direction);
	auto* tiling = TilingManager::active();

	if (tiling == nullptr || !edge) {
		this->unsupported("swapwindow (without tiling)");
		return;
	}

	tiling->swapDirection(*edge);
}

void Dispatcher::toggleFloating(TrackedWindow* window, const QString& action) {
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) {
		this->unsupported("togglefloating (without tiling)");
		return;
	}

	if (window == nullptr) return;
	tiling->setFloating(window, action == "float", action == "toggle");
}

void Dispatcher::toggleSplit() {
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) {
		this->unsupported("togglesplit (without tiling)");
		return;
	}

	if (auto* window = this->ipc->tracker()->activeWindow()) tiling->toggleSplit(window);
}

void Dispatcher::layoutMessage(const QString& message) {
	auto* tiling = TilingManager::active();
	if (tiling == nullptr) {
		this->unsupported("layoutmsg (without tiling)");
		return;
	}

	auto* window = tiling->focusedWindow();
	if (window == nullptr) return;

	auto parts = message.split(' ', Qt::SkipEmptyParts);
	if (parts.isEmpty()) return;
	const auto& command = parts.first();
	auto argument = parts.value(1).trimmed();
	auto handled = true;

	if (command == "togglesplit") {
		tiling->toggleSplit(window);
	} else if (command == "swapsplit") {
		tiling->swapSplit(window);
	} else if ((command == "splitratio" || command == "mfact") && parts.length() >= 2) {
		auto exact = parts[1] == "exact";
		auto ok = false;
		auto value = parts.value(exact ? 2 : 1).toDouble(&ok);
		if (!ok) return;

		if (command == "splitratio") tiling->splitRatio(window, value, exact);
		else handled = tiling->masterRatio(window, value, exact);
	} else if (command == "swapwithmaster") {
		handled = tiling->swapWithMaster(window);
	} else if (command == "focusmaster") {
		handled = tiling->focusMaster(window);
	} else if (command == "move" && !argument.isEmpty()) {
		if (argument == "+col" || argument == "col") {
			handled = tiling->scrollColumns(window, 1);
		} else if (argument == "-col") {
			handled = tiling->scrollColumns(window, -1);
		} else {
			auto ok = false;
			auto pixels = argument.toDouble(&ok);
			handled = ok && tiling->scrollView(window, qRound(pixels));
		}
	} else if (command == "colresize" && !argument.isEmpty()) {
		if (argument == "+conf" || argument == "-conf") {
			handled = tiling->cycleColumnWidth(window, argument.startsWith('+') ? 1 : -1);
		} else if (argument == "all") {
			auto value = parseFraction(parts.value(2));
			handled = value && tiling->allColumnsWidth(window, *value);
		} else {
			auto relative = argument.startsWith('+') || argument.startsWith('-');
			auto value = parseFraction(argument);
			handled = value && tiling->columnWidth(window, *value, relative);
		}
	} else if (command == "fit" && !argument.isEmpty()) {
		auto mode = fitMode(argument);
		handled = mode && tiling->fitColumns(window, *mode);
	} else if (command == "focus" && !argument.isEmpty()) {
		auto edge = directionEdge(argument);
		handled = edge && tiling->focusColumn(window, *edge);
	} else if (command == "movewindowto" && !argument.isEmpty()) {
		auto edge = directionEdge(argument);
		handled = edge.has_value();
		if (edge && !tiling->moveIntoColumn(window, *edge)) this->moveWindow(argument);
	} else if (command == "swapcol" && !argument.isEmpty()) {
		auto edge = directionEdge(argument);
		handled = edge && tiling->swapColumn(window, *edge);
	} else if (command == "promote") {
		handled = tiling->promote(window);
	} else if (command == "center" || command == "centercolumn") {
		handled = tiling->centerColumn(window);
	} else if (command == "togglefit") {
		handled = tiling->toggleFit(window);
	} else {
		handled = false;
	}

	if (!handled) this->unsupported("layoutmsg " + command + " (" + tiling->layout() + ")");
}

void Dispatcher::resizeActive(const QString& args) {
	auto* window = this->ipc->tracker()->activeWindow();
	if (window == nullptr || window->screen() == nullptr) return;

	auto parts = args.split(' ', Qt::SkipEmptyParts);
	auto exact = !parts.isEmpty() && parts.first() == "exact";
	if (exact) parts.removeFirst();
	if (parts.length() < 2) {
		this->unsupported("resizeactive " + args);
		return;
	}

	auto reference = exact ? window->screen()->geometry().size() : window->rect().size();

	auto parse = [](QString value, int whole, bool& ok) {
		auto percent = value.endsWith('%');
		if (percent) value.chop(1);
		auto number = value.toDouble(&ok);
		return qRound(percent ? number * whole / 100.0 : number);
	};

	auto okX = false;
	auto okY = false;
	auto x = parse(parts[0], reference.width(), okX);
	auto y = parse(parts[1], reference.height(), okY);
	if (!okX || !okY) {
		this->unsupported("resizeactive " + args);
		return;
	}

	if (auto* tiling = TilingManager::active()) {
		auto dx = exact ? x - window->rect().width() : x;
		auto dy = exact ? y - window->rect().height() : y;
		if (tiling->resizeTiled(window, dx, dy)) return;
	}

	TilingManager::instance()->resizeFloating(window, x, y, exact);
}

void Dispatcher::centerWindow() {
	auto* tiling = TilingManager::active();
	auto* window = tiling != nullptr ? tiling->focusedWindow() : this->ipc->tracker()->activeWindow();
	if (window == nullptr) return;

	if (tiling != nullptr && tiling->isTiled(window)) {
		tiling->centerColumn(window);
		return;
	}

	TilingManager::instance()->center(window);
}

void Dispatcher::moveWindowPixel(TrackedWindow* window, const QString& x, const QString& y) {
	if (window == nullptr) return;

	auto* screen = window->screen();
	if (screen == nullptr) return;

	auto origin = screen->geometry().topLeft();
	auto logical = origin + QPoint(qRound(x.toDouble()), qRound(y.toDouble()));
	window->moveTo(logical);
}

void Dispatcher::exec(const QString& command) {
	auto line = command.trimmed();
	if (line.isEmpty()) return;

	if (line.contains("%focused_window%")) {
		auto* active = this->ipc->tracker()->activeWindow();
		line.replace("%focused_window%", QString::number(active == nullptr ? 0 : active->pid()));
	}

	qs::windows::runLauncher([line]() { launchCommandLine(line); });
}

void Dispatcher::unknown(const QString& what) {
	if (this->warned.contains(what)) return;
	this->warned.insert(what);
	qCWarning(logDispatch) << "Unknown dispatcher" << what << "(further uses are not logged)";
}

void Dispatcher::unsupported(const QString& what) {
	if (this->warned.contains(what)) return;
	this->warned.insert(what);
	qCInfo(logDispatch) << "Dispatcher" << what
	                    << "has no Windows equivalent; ignored (further uses are not logged)";
}

} // namespace qs::hyprland::ipc
