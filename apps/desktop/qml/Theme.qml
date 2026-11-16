pragma Singleton
import QtQuick

QtObject {
    readonly property color sidebar: "#1b2430"
    readonly property color sidebarHover: "#263243"
    readonly property color sidebarActive: "#2f6fed"
    readonly property color sidebarText: "#c9d3e0"
    readonly property color background: "#f4f6f9"
    readonly property color card: "#ffffff"
    readonly property color border: "#dfe4ec"
    readonly property color hover: "#eef2f8"
    readonly property color disabledBg: "#eceff4"
    readonly property color text: "#1b2430"
    readonly property color textMuted: "#5d6a7e"
    readonly property color accent: "#2f6fed"
    readonly property color accentHover: "#4a82f0"
    readonly property color accentDark: "#2559c4"
    readonly property color danger: "#c62f3e"
    readonly property color dangerBg: "#fdecee"
    readonly property color ok: "#1c7c4b"
    readonly property color okBg: "#e3f5ec"
    readonly property color warn: "#9a6200"
    readonly property color warnBg: "#fff2d9"
    readonly property color info: "#2559c4"
    readonly property color infoBg: "#e6eefc"

    readonly property int radius: 8
    readonly property int spacing: 16
    readonly property int pagePadding: 24
    readonly property string mono: "monospace"
}
