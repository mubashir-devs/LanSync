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
            text: "Files being sent or received, queued downloads, and recent results."
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
                model: appController.transfers
                ScrollBar.vertical: ScrollBar {}
                delegate: TransferRow {
                    required property var modelData
                    t: modelData
                    width: ListView.view.width - 12
                    onPauseRequested: appController.pauseTransfer(modelData.id)
                    onResumeRequested: appController.resumeTransfer(modelData.id)
                    onCancelRequested: { confirmCancel.transferId = modelData.id; confirmCancel.title = "Cancel “" + appController.baseName(modelData.path) + "”?"; confirmCancel.open() }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                busy: appController.loading
                title: appController.loading ? "Loading…" : "No transfers"
                description: appController.loading ? "" : "Transfers appear here while files move between your devices."
            }
        }
    }

    ConfirmDialog {
        id: confirmCancel
        property string transferId
        message: "The partially received data is discarded. The file is not retried automatically for the next 10 minutes; “Sync now” retries it."
        confirmText: "Cancel transfer"
        onConfirmed: appController.cancelTransfer(transferId)
    }
}
