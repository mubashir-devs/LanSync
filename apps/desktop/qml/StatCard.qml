import QtQuick
import QtQuick.Layouts
import LanSync.UI

Rectangle {
    id: root
    property string label
    property string value
    property string hint: ""

    color: Theme.card
    radius: Theme.radius
    border.color: Theme.border
    implicitHeight: 104
    implicitWidth: 180

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 2
        Text { text: root.label; color: Theme.textMuted; font.pixelSize: 13 }
        Text {
            text: root.value
            color: Theme.text
            font.pixelSize: 28
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Text {
            visible: root.hint.length > 0
            text: root.hint
            color: Theme.textMuted
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Item { Layout.fillHeight: true }
    }
}
