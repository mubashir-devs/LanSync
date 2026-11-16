import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root

    function stateTone(f) {
        return !f.enabled || f.paused ? "muted" : f.state === "Failed" ? "error" : f.state === "Conflict" ? "warn"
             : f.state === "Completed" || f.state === "Idle" ? "ok" : "info"
    }
    function directionText(d) {
        return d === "send_only" ? "Send only" : d === "receive_only" ? "Receive only" : "Two-way"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Folders are matched between devices by name. Register a folder with the same name on both computers."
                color: Theme.textMuted; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            AppButton {
                text: "Add folder"; kind: "primary"
                onClicked: { addDialog.clearFields(); addDialog.open() }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                spacing: 10
                model: appController.folders
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
                            spacing: 8
                            Text { text: card.modelData.name; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text }
                            Badge { text: card.modelData.state; tone: root.stateTone(card.modelData) }
                            Badge { text: root.directionText(card.modelData.direction); tone: "info" }
                            Item { Layout.fillWidth: true }
                            Switch {
                                checked: card.modelData.enabled
                                text: "Enabled"
                                onToggled: appController.setFolderEnabled(card.modelData.id, checked)
                            }
                        }
                        Text { text: card.modelData.path; font.family: Theme.mono; font.pixelSize: 12; color: Theme.textMuted; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text {
                            text: "With " + card.modelData.remoteDeviceName + "  ·  " + card.modelData.fileCount + " files"
                                  + (card.modelData.pendingCount > 0 ? "  ·  " + card.modelData.pendingCount + " pending" : "")
                                  + (card.modelData.conflictCount > 0 ? "  ·  " + card.modelData.conflictCount + " conflict(s)" : "")
                                  + "  ·  last sync " + appController.formatAgo(card.modelData.lastSyncMs)
                            font.pixelSize: 12; color: Theme.textMuted; wrapMode: Text.WordWrap; Layout.fillWidth: true
                        }
                        Text {
                            visible: card.modelData.error.length > 0
                            text: "⚠ " + card.modelData.error
                            font.pixelSize: 12; color: Theme.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 8
                            AppButton { text: "Sync now"; kind: "primary"; enabled: card.modelData.enabled
                                        onClicked: appController.syncNow(card.modelData.id, false) }
                            AppButton { text: "Verify content"; enabled: card.modelData.enabled
                                        onClicked: appController.syncNow(card.modelData.id, true) }
                            AppButton { text: card.modelData.paused ? "Resume" : "Pause"; enabled: card.modelData.enabled
                                        onClicked: appController.setFolderPaused(card.modelData.id, !card.modelData.paused) }
                            ComboBox {
                                id: dirBox
                                implicitHeight: 34
                                model: ["bidirectional", "send_only", "receive_only"]
                                displayText: root.directionText(currentValue)
                                delegate: ItemDelegate { text: root.directionText(modelData); width: dirBox.width }
                                currentIndex: model.indexOf(card.modelData.direction)
                                onActivated: appController.setFolderDirection(card.modelData.id, model[currentIndex])
                            }
                            AppButton { text: "Versions…"; onClicked: { versionsDialog.openFor(card.modelData.id, "") } }
                            AppButton { text: "Remove"; kind: "danger"
                                        onClicked: { confirmRemove.folderId = card.modelData.id
                                                     confirmRemove.title = "Stop syncing “" + card.modelData.name + "”?"
                                                     confirmRemove.open() } }
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                busy: appController.loading
                title: appController.loading ? "Loading folders…" : "No folders registered"
                description: appController.loading ? "" :
                    appController.pairedDevices.length === 0
                    ? "Pair a device first, then add a folder to synchronize with it."
                    : "Add a folder to start synchronizing it with a paired device."
            }
        }
    }

    ConfirmDialog {
        id: confirmRemove
        property string folderId
        message: "The folder stops syncing and LanSync forgets its index and saved versions. Your files on disk are not touched."
        confirmText: "Remove"
        onConfirmed: appController.removeFolder(folderId)
    }

    VersionsDialog { id: versionsDialog }

    FolderDialog {
        id: picker
        title: "Choose a folder to synchronize"
        onAccepted: pathField.text = appController.urlToPath(selectedFolder)
    }

    Dialog {
        id: addDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, (Overlay.overlay ? Overlay.overlay.width : 520) - 32)
        padding: 20
        title: "Add folder"
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.border }
        header: Text { text: "Add folder"; font.pixelSize: 17; font.weight: Font.DemiBold; color: Theme.text; leftPadding: 20; topPadding: 18 }

        function clearFields() { pathField.text = ""; nameField.text = ""; nameEdited = false; deviceBox.currentIndex = 0; dirBox2.currentIndex = 0 }
        property bool nameEdited: false
        readonly property bool valid: pathField.text.length > 0 && /^[A-Za-z0-9 _.-]{1,64}$/.test(nameField.text) && appController.pairedDevices.length > 0

        contentItem: ColumnLayout {
            spacing: 8
            Text { text: "Folder on this computer"; color: Theme.textMuted; font.pixelSize: 13 }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: pathField
                    Layout.fillWidth: true
                    placeholderText: "/path/to/folder"
                    onTextEdited: if (!addDialog.nameEdited) nameField.text = appController.baseName(text)
                }
                AppButton { text: "Browse…"; onClicked: picker.open() }
            }
            Text { text: "Shared name (must match on both devices)"; color: Theme.textMuted; font.pixelSize: 13 }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "e.g. Documents"
                onTextEdited: addDialog.nameEdited = true
            }
            Text { text: "Sync with"; color: Theme.textMuted; font.pixelSize: 13 }
            ComboBox {
                id: deviceBox
                Layout.fillWidth: true
                model: appController.pairedDevices
                textRole: "name"
                valueRole: "id"
            }
            Text { text: "Direction"; color: Theme.textMuted; font.pixelSize: 13 }
            ComboBox {
                id: dirBox2
                Layout.fillWidth: true
                model: [{ t: "Two-way (send and receive)", v: "bidirectional" }, { t: "Send only", v: "send_only" }, { t: "Receive only", v: "receive_only" }]
                textRole: "t"
                valueRole: "v"
            }
            Text {
                visible: appController.pairedDevices.length === 0
                text: "No paired devices. Pair a device on the Devices page first."
                color: Theme.danger; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
        }
        footer: DialogFooter {
            AppButton { text: "Cancel"; onClicked: addDialog.close() }
            AppButton {
                text: "Add folder"; kind: "primary"; enabled: addDialog.valid
                onClicked: {
                    appController.addFolder(pathField.text, nameField.text, deviceBox.currentValue, dirBox2.currentValue)
                    addDialog.close()
                }
            }
        }
    }
}
