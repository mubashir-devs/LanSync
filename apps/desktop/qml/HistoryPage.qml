import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root
    property bool errorsOnly: false
    readonly property var rows: {
        if (!errorsOnly) return appController.history
        var out = []
        var all = appController.history
        for (var i = 0; i < all.length; ++i) if (all[i].result === "failure") out.push(all[i])
        return out
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text { text: "Synchronization events, file operations, transfer results and errors."
                   color: Theme.textMuted; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ComboBox {
                id: filterBox
                implicitHeight: 34
                model: ["All events", "Errors only"]
                onActivated: root.errorsOnly = currentIndex === 1
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                spacing: 6
                model: root.rows
                ScrollBar.vertical: ScrollBar {}

                delegate: Card {
                    id: card
                    required property var modelData
                    width: ListView.view.width - 12
                    implicitHeight: row.implicitHeight + 20
                    GridLayout {
                        id: row
                        anchors.fill: parent
                        anchors.margins: 10
                        columns: width > 640 ? 4 : 2
                        columnSpacing: 12
                        rowSpacing: 2
                        Text { text: appController.formatTime(card.modelData.timestampMs); color: Theme.textMuted; font.pixelSize: 12; Layout.preferredWidth: 130 }
                        RowLayout {
                            spacing: 8
                            Text { text: card.modelData.operation; font.weight: Font.DemiBold; color: Theme.text; font.pixelSize: 13 }
                            Badge { text: card.modelData.result; tone: card.modelData.result === "success" ? "ok" : card.modelData.result === "failure" ? "error" : "info" }
                        }
                        Text {
                            text: [card.modelData.path, card.modelData.deviceName].filter(function(x) { return x.length > 0 }).join("  ·  ")
                            color: Theme.text; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true
                        }
                        Text {
                            text: card.modelData.detail
                            color: card.modelData.result === "failure" ? Theme.danger : Theme.textMuted
                            font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
                            Layout.columnSpan: row.columns > 2 ? 1 : 2
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                busy: appController.loading
                title: appController.loading ? "Loading…" : (root.errorsOnly ? "No errors" : "No history yet")
                description: appController.loading ? "" : "Events appear here as LanSync syncs your files."
            }
        }
    }
}
