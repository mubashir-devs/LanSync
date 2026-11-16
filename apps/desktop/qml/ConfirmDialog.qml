import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Dialog {
    id: dlg
    property string message
    property string confirmText: "Confirm"
    property bool destructive: true
    signal confirmed()

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(440, (Overlay.overlay ? Overlay.overlay.width : 440) - 32)
    padding: 20
    background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.border }
    header: Text {
        text: dlg.title
        font.pixelSize: 17
        font.weight: Font.DemiBold
        color: Theme.text
        leftPadding: 20; topPadding: 18; rightPadding: 20
    }
    contentItem: Text {
        text: dlg.message
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: 14
    }
    footer: DialogFooter {
        AppButton { text: "Cancel"; onClicked: dlg.reject() }
        AppButton {
            text: dlg.confirmText
            kind: dlg.destructive ? "danger" : "primary"
            onClicked: dlg.accept()
        }
    }
    onAccepted: confirmed()
}
