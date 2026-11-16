import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

// One transfer (also used compactly on the dashboard).
Card {
    id: root
    property var t
    property bool showActions: true
    signal pauseRequested()
    signal resumeRequested()
    signal cancelRequested()

    implicitHeight: col.implicitHeight + 24

    readonly property string status: t ? t.status : ""
    readonly property string tone: status === "completed" ? "ok" : status === "failed" ? "error"
                                  : status === "paused" || status === "cancelled" ? "warn" : "info"

    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: 12
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: (root.t.direction === "upload" ? "↑ " : "↓ ") + appController.baseName(root.t.path)
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: Theme.text
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            Badge { text: root.status; tone: root.tone }
        }
        Text {
            text: root.t.folderName + " · " + (root.t.direction === "upload" ? "to " : "from ") + root.t.peerName
            font.pixelSize: 12
            color: Theme.textMuted
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        ProgressBar {
            id: pb
            Layout.fillWidth: true
            from: 0; to: 1
            value: root.t.progress
            visible: root.status !== "failed" && root.status !== "cancelled"
            background: Rectangle { implicitHeight: 6; radius: 3; color: Theme.hover }
            contentItem: Item {
                implicitHeight: 6
                Rectangle {
                    width: pb.visualPosition * parent.width
                    height: parent.height
                    radius: 3
                    color: root.status === "completed" ? Theme.ok : root.status === "paused" ? Theme.warn : Theme.accent
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text {
                text: root.t.error.length > 0 ? root.t.error : root.t.detail
                font.pixelSize: 12
                color: root.t.error.length > 0 ? Theme.danger : Theme.textMuted
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            AppButton {
                visible: root.showActions && root.t.direction === "download" && (root.status === "transferring" || root.status === "queued")
                text: "Pause"; implicitHeight: 28
                onClicked: root.pauseRequested()
            }
            AppButton {
                visible: root.showActions && root.status === "paused"
                text: "Resume"; kind: "primary"; implicitHeight: 28
                onClicked: root.resumeRequested()
            }
            AppButton {
                visible: root.showActions && (root.status === "transferring" || root.status === "queued" || root.status === "paused")
                text: "Cancel"; kind: "danger"; implicitHeight: 28
                onClicked: root.cancelRequested()
            }
        }
    }
}
