import QtQuick
import QtQuick.Layouts
import LanSync.UI

// tone: ok | warn | error | info | muted
Rectangle {
    id: root
    property string text
    property string tone: "muted"
    implicitHeight: 22
    implicitWidth: label.implicitWidth + 16
    radius: 11
    color: tone === "ok" ? Theme.okBg : tone === "warn" ? Theme.warnBg : tone === "error" ? Theme.dangerBg
         : tone === "info" ? Theme.infoBg : Theme.hover
    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        font.pixelSize: 11
        font.weight: Font.DemiBold
        color: root.tone === "ok" ? Theme.ok : root.tone === "warn" ? Theme.warn : root.tone === "error" ? Theme.danger
             : root.tone === "info" ? Theme.info : Theme.textMuted
    }
    Accessible.role: Accessible.StaticText
    Accessible.name: text
}
