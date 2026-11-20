import QtQuick
import Quickshell
import Quickshell.Wayland

// Windows has no session lock protocol: while locked, the lock surface fills a
// panel on each screen, over everything and holding the keyboard. It isn't
// secure (Ctrl+Alt+Del still reaches Task Manager), hence `secure`.
Scope {
	id: root

	property bool locked: false
	readonly property bool secure: false
	property Component surface: null
	// A trial run: the keyboard isn't held, so nothing is kept away from it.
	property bool preview: false

	signal unlock()

	Variants {
		model: root.locked ? Quickshell.screens : []

		delegate: PanelWindow {
			id: lockWindow

			required property var modelData

			screen: modelData
			// Not quite transparent: Windows passes clicks on fully clear pixels
			// through to the window behind (the desktop's widgets, the clock).
			color: Qt.rgba(0, 0, 0, 1 / 255)
			exclusionMode: ExclusionMode.Ignore
			WlrLayershell.layer: WlrLayer.Overlay
			WlrLayershell.keyboardFocus: root.preview ? WlrKeyboardFocus.OnDemand : WlrKeyboardFocus.Exclusive
			WlrLayershell.namespace: "quickshell:lockscreen"

			anchors {
				top: true
				bottom: true
				left: true
				right: true
			}

			// The surface is made once the window is on screen: its entry
			// animations run on this window's frames, and started before the
			// window drew they stayed frozen (the password island invisible)
			// until something made it repaint.
			Timer {
				interval: 80
				running: lockWindow.visible
				onTriggered: {
					const item = root.surface?.createObject(lockWindow.contentItem, {
						"screen": lockWindow.modelData
					});
					if (item)
						item.anchors.fill = lockWindow.contentItem;
				}
			}
		}
	}
}
