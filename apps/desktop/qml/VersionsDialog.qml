import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

// Browse and restore previous versions of one file.
Dialog {
    id: dlg
    property string folderId
    property var versions: []
    property bool loaded: false

    function openFor(folder, path) {
        folderId = folder
        pathField.text = path
        versions = []
        loaded = false
        open()
        if (path.length > 0) load()
    }
    function load() {
        loaded = false
        appController.loadVersions(folderId, pathField.text)
    }

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, (Overlay.overlay ? Overlay.overlay.width : 560) - 32)
    height: Math.min(520, (Overlay.overlay ? Overlay.overlay.height : 520) - 32)
    padding: 20
    background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.border }
    header: Text { text: "File versions"; font.pixelSize: 17; font.weight: Font.DemiBold; color: Theme.text; leftPadding: 20; topPadding: 18 }

    Connections {
        target: appController
        function onVersionsLoaded(list) { dlg.versions = list; dlg.loaded = true }
    }

    contentItem: ColumnLayout {
        spacing: 8
        Text { text: "Path inside the folder"; color: Theme.textMuted; font.pixelSize: 13 }
        RowLayout {
            Layout.fillWidth: true
            TextField { id: pathField; Layout.fillWidth: true; placeholderText: "e.g. reports/2024.txt"; onAccepted: dlg.load() }
            AppButton { text: "Show versions"; kind: "primary"; enabled: pathField.text.length > 0; onClicked: dlg.load() }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            ListView {
                id: vlist
                anchors.fill: parent
                clip: true
                spacing: 6
                model: dlg.versions
                delegate: Card {
                    required property var modelData
                    width: ListView.view.width
                    implicitHeight: 52
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text { text: "Replaced " + appController.formatTime(modelData.createdMs); color: Theme.text; font.pixelSize: 13 }
                            Text { text: appController.formatBytes(modelData.size) + " · modified " + appController.formatTime(modelData.modifiedMs) + " · " + modelData.hash
                                   color: Theme.textMuted; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                        AppButton { text: "Restore"; onClicked: { confirmRestore.versionId = modelData.id; confirmRestore.open() } }
                    }
                }
            }
            EmptyState {
                anchors.centerIn: parent
                visible: vlist.count === 0
                title: dlg.loaded ? "No saved versions" : "Enter a file path"
                description: dlg.loaded ? "Older versions appear here after a file is replaced or deleted by synchronization."
                                        : "Previous versions of replaced or deleted files are kept according to your retention settings."
            }
        }
    }
    footer: DialogFooter {
        AppButton { text: "Close"; onClicked: dlg.close() }
    }

    ConfirmDialog {
        id: confirmRestore
        property int versionId
        title: "Restore this version?"
        message: "The current file is saved as a version first, then replaced by the selected one. The restored file syncs to your other devices."
        confirmText: "Restore"
        destructive: false
        onConfirmed: appController.restoreVersion(versionId)
    }
}
