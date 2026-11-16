import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import LanSync.UI

Item {
    id: root
    property bool dirty: false

    // Copies current settings into the editors (after load or "Revert").
    function load() {
        var s = appController.settings
        if (s.logLevel === undefined) return
        deviceName.text = s.deviceName
        listenPort.value = s.listenPort
        publicNets.checked = s.allowPublicNetworks
        discoveryOn.checked = s.discoveryEnabled
        discoveryPort.value = s.discoveryPort
        announce.value = s.announceIntervalSec
        concurrency.value = s.maxConcurrentTransfers
        chunk.value = s.chunkSizeKb
        maxSize.value = s.maxFileSizeMb
        reconcile.value = s.reconcileIntervalSec
        versionCount.value = s.versionMaxCount
        versionAge.value = s.versionMaxAgeDays
        logLevel.currentIndex = Math.max(0, logLevel.model.indexOf(s.logLevel))
        dirty = false
    }
    Component.onCompleted: load()
    Connections { target: appController; function onLoadingChanged() { if (!root.dirty) root.load() } }

    component Field: RowLayout {
        property string label
        property string hint: ""
        default property alias content: slot.data
        Layout.fillWidth: true
        spacing: 12
        ColumnLayout {
            Layout.preferredWidth: 260
            Layout.maximumWidth: 260
            spacing: 0
            Text { text: label; color: Theme.text; font.pixelSize: 14 }
            Text { visible: hint.length > 0; text: hint; color: Theme.textMuted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        RowLayout { id: slot; Layout.fillWidth: true }
    }
    component Section: Card {
        property string title
        default property alias body: inner.data
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 56
        Text { x: 16; y: 14; text: title; font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text }
        ColumnLayout { id: inner; x: 16; y: 44; width: parent.width - 32; spacing: 10 }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.spacing

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.pagePadding
                spacing: Theme.spacing

                Section {
                    title: "Network"
                    Field { label: "Device name"; hint: "Shown to other devices"
                        TextField { id: deviceName; Layout.fillWidth: true; placeholderText: "Default: this computer's name"; maximumLength: 100; onTextEdited: root.dirty = true } }
                    Field { label: "Listening port (TCP)"; hint: "Needs a restart"
                        SpinBox { id: listenPort; from: 0; to: 65535; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Allow non-local peers"; hint: "Off keeps LanSync to private networks only (recommended)"
                        Switch { id: publicNets; onToggled: root.dirty = true } }
                }
                Section {
                    title: "Discovery"
                    Field { label: "Discover devices automatically"; hint: "Needs a restart"
                        Switch { id: discoveryOn; onToggled: root.dirty = true } }
                    Field { label: "Discovery port (UDP)"; hint: "Needs a restart"
                        SpinBox { id: discoveryPort; from: 1; to: 65535; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Announce interval (seconds)"
                        SpinBox { id: announce; from: 1; to: 300; editable: true; onValueModified: root.dirty = true } }
                }
                Section {
                    title: "Transfers and sync"
                    Field { label: "Concurrent transfers"
                        SpinBox { id: concurrency; from: 1; to: 16; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Chunk size (KB)"; hint: "Larger chunks use fewer round trips"
                        SpinBox { id: chunk; from: 16; to: 4096; stepSize: 64; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Largest file accepted (MB)"
                        SpinBox { id: maxSize; from: 1; to: 16777216; stepSize: 1024; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Full rescan interval (seconds)"; hint: "Safety net in case filesystem events are missed"
                        SpinBox { id: reconcile; from: 5; to: 86400; stepSize: 30; editable: true; onValueModified: root.dirty = true } }
                }
                Section {
                    title: "Version history"
                    Field { label: "Versions kept per file"; hint: "0 = no count limit"
                        SpinBox { id: versionCount; from: 0; to: 1000; editable: true; onValueModified: root.dirty = true } }
                    Field { label: "Keep versions for (days)"; hint: "0 = no age limit. With both at 0 nothing is ever deleted."
                        SpinBox { id: versionAge; from: 0; to: 3650; editable: true; onValueModified: root.dirty = true } }
                }
                Section {
                    title: "Storage and logging"
                    Field { label: "Data folder"; hint: "Database, identity, versions, logs"
                        Text { text: appController.dataDir; color: Theme.textMuted; font.family: Theme.mono; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                    Field { label: "Log level"
                        ComboBox { id: logLevel; model: ["trace", "debug", "info", "warn", "error", "critical", "off"]; onActivated: root.dirty = true } }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    AppButton { text: "Revert"; enabled: root.dirty; onClicked: root.load() }
                    AppButton {
                        text: "Save settings"; kind: "primary"; enabled: root.dirty
                        onClicked: {
                            appController.saveSettings({
                                deviceName: deviceName.text, listenPort: listenPort.value, allowPublicNetworks: publicNets.checked,
                                discoveryEnabled: discoveryOn.checked, discoveryPort: discoveryPort.value,
                                announceIntervalSec: announce.value, maxConcurrentTransfers: concurrency.value,
                                chunkSizeKb: chunk.value, maxFileSizeMb: maxSize.value, reconcileIntervalSec: reconcile.value,
                                versionMaxCount: versionCount.value, versionMaxAgeDays: versionAge.value,
                                logLevel: logLevel.currentText })
                            root.dirty = false
                        }
                    }
                }
            }
        }
    }
}
