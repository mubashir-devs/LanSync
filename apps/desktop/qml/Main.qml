import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

ApplicationWindow {
    id: window
    width: 1120
    height: 740
    minimumWidth: 560
    minimumHeight: 480
    visible: true
    title: "LanSync"
    color: Theme.background

    readonly property var sections: ["Dashboard", "Devices", "Sync Folders", "Transfers", "Conflicts", "History", "Settings"]
    readonly property var shortNames: ["Da", "De", "Fo", "Tr", "Cf", "Hi", "St"]
    property int currentIndex: 0
    readonly property bool compact: width < 860

    function badgeFor(i) {
        return i === 4 ? appController.stats.openConflicts : 0
    }

    // ---------------------------------------------------------------- toasts
    ListModel { id: toasts }
    property int toastSeq: 0
    function showToast(title, text, isError) {
        toasts.append({ seq: ++toastSeq, title: title, text: text, isError: isError, expires: Date.now() + (isError ? 9000 : 5000) })
    }
    Timer {
        interval: 500; repeat: true; running: toasts.count > 0
        onTriggered: { for (var i = toasts.count - 1; i >= 0; --i) if (toasts.get(i).expires < Date.now()) toasts.remove(i) }
    }

    // ---------------------------------------------------------------- pairing prompts (queued)
    property var pairingQueue: []
    function enqueuePairing(p) {
        if (pairDialog.visible && pairDialog.deviceId === p.deviceId) return
        for (var i = 0; i < pairingQueue.length; ++i) if (pairingQueue[i].deviceId === p.deviceId) return
        pairingQueue.push(p)
        showNextPairing()
    }
    function showNextPairing() {
        if (pairDialog.visible || pairingQueue.length === 0) return
        var p = pairingQueue.shift()
        pairDialog.deviceId = p.deviceId; pairDialog.deviceName = p.deviceName
        pairDialog.code = p.code; pairDialog.incoming = p.incoming
        pairDialog.open()
    }

    Connections {
        target: appController
        function onNotification(title, text, isError) { window.showToast(title, text, isError) }
        function onPairingRequested(deviceId, deviceName, code, incoming) {
            window.enqueuePairing({ deviceId: deviceId, deviceName: deviceName, code: code, incoming: incoming })
        }
        function onPairingFinished(deviceId, deviceName, text, isError) {
            if (pairDialog.visible && pairDialog.deviceId === deviceId) pairDialog.close()
            var q = []
            for (var i = 0; i < window.pairingQueue.length; ++i) if (window.pairingQueue[i].deviceId !== deviceId) q.push(window.pairingQueue[i])
            window.pairingQueue = q
            window.showToast(text === "paired" ? "Paired" : "Pairing", text === "paired" ? deviceName + " is now a trusted device." : deviceName + ": " + text, isError)
            window.showNextPairing()
        }
    }

    Dialog {
        id: pairDialog
        property string deviceId
        property string deviceName
        property string code
        property bool incoming
        modal: true
        closePolicy: Popup.NoAutoClose
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, window.width - 32)
        padding: 20
        background: Rectangle { color: Theme.card; radius: Theme.radius; border.color: Theme.border }
        header: Text {
            text: pairDialog.incoming ? "“" + pairDialog.deviceName + "” wants to pair" : "Pair with “" + pairDialog.deviceName + "”"
            font.pixelSize: 17; font.weight: Font.DemiBold; color: Theme.text; leftPadding: 20; topPadding: 18; wrapMode: Text.WordWrap
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                text: "Check that this code is identical on both screens. If it differs, someone may be intercepting the connection — choose Reject."
                wrapMode: Text.WordWrap; color: Theme.textMuted; font.pixelSize: 14; Layout.fillWidth: true
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 76
                radius: 8
                color: Theme.infoBg
                Text {
                    anchors.centerIn: parent
                    text: pairDialog.code.length === 6 ? pairDialog.code.substring(0, 3) + " " + pairDialog.code.substring(3) : pairDialog.code
                    font.pixelSize: 40; font.weight: Font.Bold; font.letterSpacing: 4; color: Theme.info
                }
            }
            Text {
                text: "Both people must approve. Once paired, this device can sync only the folders you share with it."
                wrapMode: Text.WordWrap; color: Theme.textMuted; font.pixelSize: 12; Layout.fillWidth: true
            }
        }
        footer: DialogFooter {
            AppButton { text: "Reject"; kind: "danger"
                        onClicked: { appController.approvePairing(pairDialog.deviceId, false); pairDialog.close(); window.showNextPairing() } }
            AppButton { text: "Codes match — Approve"; kind: "primary"
                        onClicked: { appController.approvePairing(pairDialog.deviceId, true); pairDialog.close(); window.showNextPairing() } }
        }
    }

    // ---------------------------------------------------------------- layout
    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: window.compact ? 64 : 220
            color: Theme.sidebar

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 4

                Text {
                    text: window.compact ? "L" : "LanSync"
                    color: "white"; font.pixelSize: 20; font.weight: Font.Bold
                    Layout.topMargin: 8; Layout.bottomMargin: 14
                    Layout.alignment: window.compact ? Qt.AlignHCenter : Qt.AlignLeft
                    Layout.leftMargin: window.compact ? 0 : 12
                }
                Repeater {
                    model: window.sections
                    NavButton {
                        required property string modelData
                        required property int index
                        Layout.fillWidth: true
                        text: modelData
                        compact: window.compact
                        shortText: window.shortNames[index]
                        badgeCount: window.badgeFor(index)
                        active: window.currentIndex === index
                        onClicked: window.currentIndex = index
                    }
                }
                Item { Layout.fillHeight: true }
                Text {
                    visible: !window.compact
                    text: appController.deviceName
                    color: Theme.sidebarText; font.pixelSize: 12; elide: Text.ElideRight
                    Layout.fillWidth: true; Layout.preferredWidth: 0; Layout.leftMargin: 12
                }
                Text {
                    visible: !window.compact
                    text: "v" + appController.appVersion
                    color: Theme.sidebarText; font.pixelSize: 11; opacity: 0.7
                    Layout.leftMargin: 12
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Text {
                text: window.sections[window.currentIndex]
                color: Theme.text; font.pixelSize: 24; font.weight: Font.DemiBold
                Layout.leftMargin: Theme.pagePadding; Layout.topMargin: Theme.pagePadding
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: window.currentIndex

                DashboardPage { onNavigate: (i) => window.currentIndex = i }
                DevicesPage {}
                FoldersPage {}
                TransfersPage {}
                ConflictsPage {}
                HistoryPage {}
                SettingsPage {}
            }
        }
    }

    // ---------------------------------------------------------------- toast stack
    Column {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        spacing: 8
        z: 100
        Repeater {
            model: toasts
            delegate: Rectangle {
                required property string title
                required property string text
                required property bool isError
                required property int index
                width: Math.min(380, window.width - 32)
                implicitHeight: tcol.implicitHeight + 24
                radius: 8
                color: isError ? Theme.dangerBg : Theme.card
                border.color: isError ? Theme.danger : Theme.border
                ColumnLayout {
                    id: tcol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 2
                    Text { text: title; font.weight: Font.DemiBold; color: isError ? Theme.danger : Theme.text; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                    Text { text: parent.parent.text; color: Theme.text; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                MouseArea { anchors.fill: parent; onClicked: toasts.remove(index) }
                Accessible.role: Accessible.AlertMessage
                Accessible.name: title + ". " + parent.parent.text
            }
        }
    }
}
