import QtQuick
import Quickshell
import Quickshell.Wayland

Scope {
	id: root

	property bool locked: false
	readonly property bool secure: false
	property Component surface: null
	property bool preview: false

	signal unlock()

	Variants {
		model: root.locked ? Quickshell.screens : []

		delegate: PanelWindow {
			id: lockWindow

			required property var modelData

			screen: modelData
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
