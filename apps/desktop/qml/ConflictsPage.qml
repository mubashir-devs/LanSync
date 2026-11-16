import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        spacing: 12

        Text {
            text: "These files were changed on two devices at once. Both versions were kept: the newer edit has the original name and the other is saved as a “sync-conflict” copy next to it."
            color: Theme.textMuted; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                spacing: 10
                model: appController.conflicts
                ScrollBar.vertical: ScrollBar {}

                delegate: Card {
                    id: card
                    required property var modelData
                    width: ListView.view.width - 12
                    implicitHeight: col.implicitHeight + 28

                    ColumnLayout {
                        id: col
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: card.modelData.path; font.pixelSize: 15; font.weight: Font.DemiBold; color: Theme.text; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            Badge { text: card.modelData.folderName; tone: "info" }
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: width > 560 ? 2 : 1
                            columnSpacing: 12
                            rowSpacing: 8
                            Repeater {
                                model: [
                                    { title: "This device", ms: card.modelData.localModifiedMs, hash: card.modelData.localHash },
                                    { title: "Other device", ms: card.modelData.remoteModifiedMs, hash: card.modelData.remoteHash }
                                ]
                                delegate: Rectangle {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    implicitHeight: v.implicitHeight + 20
                                    radius: 6
                                    color: Theme.hover
                                    ColumnLayout {
                                        id: v
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 2
                                        Text { text: modelData.title; font.weight: Font.DemiBold; color: Theme.text; font.pixelSize: 13 }
                                        Text { text: "Modified " + appController.formatTime(modelData.ms); color: Theme.textMuted; font.pixelSize: 12 }
                                        Text { text: "Content " + modelData.hash.substring(0, 12); color: Theme.textMuted; font.pixelSize: 11; font.family: Theme.mono }
                                    }
                                }
                            }
                        }
                        Text {
                            text: "Conflict copy: " + card.modelData.copyPath
                            font.pixelSize: 12; color: Theme.textMuted; elide: Text.ElideMiddle; Layout.fillWidth: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 8
                            AppButton { text: "Keep this device's version"; onClicked: ask("keep_local", card.modelData) }
                            AppButton { text: "Keep other device's version"; onClicked: ask("keep_remote", card.modelData) }
                            AppButton { text: "Keep both"; kind: "primary"; onClicked: appController.resolveConflict(card.modelData.id, "keep_both") }
                            AppButton { text: "Versions…"; onClicked: versionsDialog.openFor(card.modelData.folderId, card.modelData.path) }
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                busy: appController.loading
                title: appController.loading ? "Loading…" : "No conflicts"
                description: appController.loading ? "" : "When the same file is edited on two devices at once, both versions are kept and listed here."
            }
        }
    }

    function ask(resolution, c) {
        confirm.conflictId = c.id
        confirm.resolution = resolution
        confirm.title = resolution === "keep_local" ? "Keep this device's version?" : "Keep the other device's version?"
        confirm.open()
    }

    ConfirmDialog {
        id: confirm
        property int conflictId
        property string resolution
        message: "The chosen version takes the original file name and the other one is removed from the folder. The removed version stays available under Versions."
        confirmText: "Keep this version"
        destructive: false
        onConfirmed: appController.resolveConflict(conflictId, resolution)
    }

    VersionsDialog { id: versionsDialog }
}
