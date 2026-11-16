import QtQuick
import QtQuick.Controls.Basic
import LanSync.UI

Button {
    id: control
    property string kind: "default"   // default | primary | danger
    implicitHeight: 34
    leftPadding: 14
    rightPadding: 14
    font.pixelSize: 13
    hoverEnabled: true

    background: Rectangle {
        radius: 6
        color: !control.enabled ? Theme.disabledBg
             : control.kind === "primary" ? (control.down ? Theme.accentDark : control.hovered ? Theme.accentHover : Theme.accent)
             : control.kind === "danger" ? (control.hovered ? Theme.dangerBg : Theme.card)
             : (control.hovered ? Theme.hover : Theme.card)
        border.width: control.kind === "primary" ? 0 : 1
        border.color: control.kind === "danger" ? Theme.danger : Theme.border
    }
    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.textMuted
             : control.kind === "primary" ? "white"
             : control.kind === "danger" ? Theme.danger : Theme.text
    }
    Accessible.name: text
}
