import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root
    signal navigate(int index)

    readonly property var activeTransfers: {
        var out = []
        var all = appController.transfers
        for (var i = 0; i < all.length; ++i)
            if (all[i].status === "transferring" || all[i].status === "queued" || all[i].status === "verifying") out.push(all[i])
        return out.slice(0, 5)
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.spacing

            // Startup failure: surfaced prominently, never swallowed.
            Card {
                visible: !appController.ready
                color: Theme.dangerBg
                border.color: Theme.danger
                Layout.fillWidth: true
                Layout.margins: Theme.pagePadding
                implicitHeight: errCol.implicitHeight + 32
                ColumnLayout {
                    id: errCol
                    anchors.fill: parent
                    anchors.margins: 16
                    Text { text: "LanSync could not start"; color: Theme.danger; font.pixelSize: 16; font.weight: Font.DemiBold }
                    Text { text: appController.errorText; color: Theme.text; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
            }

            GridLayout {
                visible: appController.ready
                Layout.fillWidth: true
                Layout.margins: Theme.pagePadding
                Layout.bottomMargin: 0
                columns: width > 980 ? 5 : width > 640 ? 3 : 2
                columnSpacing: Theme.spacing
                rowSpacing: Theme.spacing

                StatCard { Layout.fillWidth: true; label: "Paired devices"; value: appController.stats.pairedDevices
                           hint: appController.stats.pairedDevices === 0 ? "Pair a device to start" : "trusted" }
                StatCard { Layout.fillWidth: true; label: "Active folders"; value: appController.stats.activeFolders
                           hint: appController.folders.length + " registered" }
                StatCard { Layout.fillWidth: true; label: "Files synchronized"; value: appController.stats.filesSynced }
                StatCard { Layout.fillWidth: true; label: "Pending transfers"; value: appController.stats.pendingTransfers
                           hint: appController.stats.activeTransfers + " active" }
                StatCard { Layout.fillWidth: true; label: "Open conflicts"; value: appController.stats.openConflicts
                           hint: appController.stats.openConflicts > 0 ? "needs your attention" : "none" }
            }

            GridLayout {
                visible: appController.ready
                Layout.fillWidth: true
                Layout.leftMargin: Theme.pagePadding
                Layout.rightMargin: Theme.pagePadding
                Layout.bottomMargin: Theme.pagePadding
                columns: width > 820 ? 2 : 1
                columnSpacing: Theme.spacing
                rowSpacing: Theme.spacing

                // Current transfer activity
                Card {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: actCol.implicitHeight + 32
                    ColumnLayout {
                        id: actCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 10
                        Text { text: "Current activity"; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text }
                        Text {
                            visible: root.activeTransfers.length === 0
                            text: appController.stats.pairedDevices === 0
                                  ? "No paired devices yet. Open Devices to pair a computer on your network."
                                  : "Nothing is transferring right now."
                            color: Theme.textMuted; wrapMode: Text.WordWrap; Layout.fillWidth: true
                        }
                        Repeater {
                            model: root.activeTransfers
                            delegate: TransferRow {
                                required property var modelData
                                t: modelData
                                showActions: false
                                Layout.fillWidth: true
                            }
                        }
                    }
                }

                // Recent events
                Card {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: evCol.implicitHeight + 32
                    ColumnLayout {
                        id: evCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: "Recent events"; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text; Layout.fillWidth: true }
                            AppButton { text: "All history"; implicitHeight: 28; onClicked: root.navigate(5) }
                        }
                        Text {
                            visible: appController.history.length === 0
                            text: "No synchronization events yet."
                            color: Theme.textMuted
                        }
                        Repeater {
                            model: appController.history.slice(0, 8)
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8
                                Rectangle {
                                    width: 8; height: 8; radius: 4
                                    color: modelData.result === "success" ? Theme.ok : modelData.result === "failure" ? Theme.danger : Theme.accent
                                }
                                Text {
                                    text: modelData.operation + (modelData.path.length > 0 ? " · " + appController.baseName(modelData.path) : "")
                                    color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true
                                }
                                Text { text: appController.formatAgo(modelData.timestampMs); color: Theme.textMuted; font.pixelSize: 12 }
                            }
                        }
                    }
                }

                // This device
                Card {
                    Layout.fillWidth: true
                    Layout.columnSpan: parent.columns
                    implicitHeight: devCol.implicitHeight + 32
                    ColumnLayout {
                        id: devCol
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 4
                        Text { text: "This device"; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text }
                        Text { text: appController.deviceName + "  ·  listening on port " + appController.listenPort; color: Theme.text }
                        Text { text: "Device ID " + appController.deviceId; color: Theme.textMuted; font.pixelSize: 12; font.family: Theme.mono; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text { text: "Data: " + appController.dataDir + "  ·  v" + appController.appVersion; color: Theme.textMuted; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    }
                }
            }
        }
    }

    EmptyState {
        anchors.centerIn: parent
        visible: appController.loading
        busy: true
        title: "Starting LanSync…"
        description: "Opening the database and starting device discovery."
    }
}
