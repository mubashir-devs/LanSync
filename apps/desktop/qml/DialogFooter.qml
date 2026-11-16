import QtQuick
import QtQuick.Layouts

// Right-aligned button row with consistent padding for dialog footers.
Item {
    default property alias buttons: row.data
    implicitHeight: 60
    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        anchors.topMargin: 6
        anchors.bottomMargin: 16
        spacing: 8
        Item { Layout.fillWidth: true }
    }
}
