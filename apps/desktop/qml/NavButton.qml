import QtQuick
import QtQuick.Controls.Basic
import LanSync.UI

AbstractButton {
    id: root
    property bool active: false
    property bool compact: false
    property string shortText: ""
    property int badgeCount: 0
    implicitHeight: 40
    hoverEnabled: true

    background: Rectangle {
        radius: Theme.radius
        color: root.active ? Theme.sidebarActive : root.hovered ? Theme.sidebarHover : "transparent"
    }
    contentItem: Item {
        Text {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: root.compact ? 0 : 12
            anchors.horizontalCenter: root.compact ? parent.horizontalCenter : undefined
            text: root.compact ? (root.shortText.length > 0 ? root.shortText : root.text.charAt(0)) : root.text
            color: root.active ? "white" : Theme.sidebarText
            font.pixelSize: root.compact ? 14 : 14
            font.weight: root.active ? Font.DemiBold : Font.Normal
        }
        Rectangle {
            visible: root.badgeCount > 0
            anchors.verticalCenter: parent.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: root.compact ? 2 : 8
            height: 18
            width: Math.max(18, countText.implicitWidth + 10)
            radius: 9
            color: Theme.danger
            Text { id: countText; anchors.centerIn: parent; text: root.badgeCount; color: "white"; font.pixelSize: 11; font.weight: Font.Bold }
        }
    }
    Accessible.role: Accessible.Button
    Accessible.name: text
}
