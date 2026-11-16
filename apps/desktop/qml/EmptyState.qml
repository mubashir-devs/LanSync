import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

ColumnLayout {
    id: root
    property string title
    property string description
    property bool busy: false
    spacing: 8

    BusyIndicator {
        running: root.busy
        visible: root.busy
        Layout.alignment: Qt.AlignHCenter
    }
    Text {
        text: root.title
        color: Theme.text
        font.pixelSize: 18
        font.weight: Font.DemiBold
        Layout.alignment: Qt.AlignHCenter
    }
    Text {
        text: root.description
        color: Theme.textMuted
        font.pixelSize: 14
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        Layout.preferredWidth: 380
        Layout.maximumWidth: 380
        Layout.alignment: Qt.AlignHCenter
    }
}
