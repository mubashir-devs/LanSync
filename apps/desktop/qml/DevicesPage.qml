import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root
    property var details: null

    function statusTone(d) {
        return d.pairingStatus === "paired" ? "ok" : d.pairingStatus === "pending" ? "warn" : d.pairingStatus === "revoked" ? "error" : "muted"
    }
    function statusText(d) {
        return d.pairingStatus === "paired" ? "Paired" : d.pairingStatus === "pending" ? "Waiting for approval"
             : d.pairingStatus === "revoked" ? "Revoked" : "Not paired"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pagePadding
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "Computers on your local network. Pairing needs approval on both devices."
                color: Theme.textMuted; font.pixelSize: 14; wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            AppButton { text: "Discover devices"; kind: "primary"; onClicked: appController.discoverNow() }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                clip: true
                spacing: 10
                model: appController.devices
                ScrollBar.vertical: ScrollBar {}

                delegate: Card {
                    id: card
                    required property var modelData
                    width: ListView.view.width - 12
                    implicitHeight: grid.implicitHeight + 28

                    GridLayout {
                        id: grid
                        anchors.fill: parent
                        anchors.margins: 14
                        columns: width > 620 ? 2 : 1
                        columnSpacing: 12
                        rowSpacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            RowLayout {
                                spacing: 8
                                Text { text: card.modelData.name; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text; elide: Text.ElideRight }
                                Badge { text: card.modelData.connection; tone: card.modelData.connected ? "ok" : card.modelData.discovered ? "info" : "muted" }
                                Badge { text: root.statusText(card.modelData); tone: root.statusTone(card.modelData) }
                            }
                            Text {
                                text: card.modelData.id
                                font.family: Theme.mono; font.pixelSize: 11; color: Theme.textMuted
                                elide: Text.ElideMiddle; Layout.fillWidth: true
                            }
                            Text {
                                text: (card.modelData.address.length > 0 ? card.modelData.address + ":" + card.modelData.port : "address unknown")
                                      + "  ·  last seen " + appController.formatAgo(card.modelData.lastSeenMs)
                                font.pixelSize: 12; color: Theme.textMuted
                            }
                        }
                        Flow {
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            spacing: 8
                            AppButton {
                                visible: card.modelData.pairingStatus !== "paired" && card.modelData.pairingStatus !== "pending"
                                enabled: card.modelData.discovered
                                text: "Pair"; kind: "primary"
                                onClicked: appController.pairDevice(card.modelData.id)
                            }
                            AppButton { text: "Details"; onClicked: { root.details = card.modelData; detailsDialog.open() } }
                            AppButton {
                                visible: card.modelData.pairingStatus === "revoked" || (card.modelData.pairingStatus === "unpaired" && !card.modelData.discovered)
                                text: "Remove"; kind: "danger"
                                onClicked: appController.removeDevice(card.modelData.id)
                            }
                            AppButton {
                                visible: card.modelData.pairingStatus === "paired"
                                text: "Unpair"; kind: "danger"
                                onClicked: { confirmUnpair.deviceId = card.modelData.id; confirmUnpair.title = "Unpair " + card.modelData.name + "?"; confirmUnpair.open() }
                            }
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: list.count === 0
                busy: appController.loading
                title: appController.loading ? "Looking for devices…" : "No devices found yet"
                description: appController.loading ? "" :
                    "Make sure LanSync is running on another computer on the same network, then press “Discover devices”."
            }
        }
    }

    ConfirmDialog {
        id: confirmUnpair
        property string deviceId
        message: "This device will no longer be able to sync with you. Folders shared with it stay registered but stop syncing. You can pair again later."
        confirmText: "Unpair"
        onConfirmed: appController.unpairDevice(deviceId)
    }

    Dialog {
        id: detailsDialog
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(520, (Overlay.overlay ? Overlay.overlay.width : 520) - 32)
        padding: 20
        title: root.details ? root.details.name : ""
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.border }
        header: Text { text: detailsDialog.title; font.pixelSize: 17; font.weight: Font.DemiBold; color: Theme.text; leftPadding: 20; topPadding: 18 }
        contentItem: GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 6
            Repeater {
                model: root.details ? [
                    ["Device ID", root.details.id],
                    ["Status", root.statusText(root.details)],
                    ["Connection", root.details.connection],
                    ["Address", root.details.address.length > 0 ? root.details.address + ":" + root.details.port : "unknown"],
                    ["Last seen", appController.formatTime(root.details.lastSeenMs)],
                    ["App version", root.details.appVersion.length > 0 ? root.details.appVersion : "—"],
                    ["Pinned certificate", root.details.fingerprint.length > 0 ? root.details.fingerprint : "— (not paired)"]
                ] : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    Text { text: modelData[0]; color: Theme.textMuted; font.pixelSize: 13; Layout.preferredWidth: 130 }
                    Text { text: modelData[1]; color: Theme.text; font.pixelSize: 13; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true
                           font.family: modelData[0] === "Device ID" || modelData[0] === "Pinned certificate" ? Theme.mono : font.family }
                }
            }
        }
        footer: DialogFooter {
            AppButton { text: "Close"; onClicked: detailsDialog.close() }
        }
    }
}
