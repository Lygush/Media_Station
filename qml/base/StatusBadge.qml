// StatusBadge.qml
// ─────────────────────────────────────────────────────────────────────────────
// Цветная точка + текст статуса. Вынесено из GameLibraryScreen.
//
// Использование:
//   StatusBadge {
//       dotColor: "#4CAF50"
//       label:    "Installed"
//   }
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import AppSettings 1.0

Row {
    id: root

    spacing: Settings.us(6)

    // ── Публичные свойства ───────────────────────────────────────────────────
    property color  dotColor: Settings.accent(1.0)
    property string label:    ""
    property int    dotSize:  Settings.us(8)

    // ── Точка ────────────────────────────────────────────────────────────────
    Rectangle {
        width:  root.dotSize
        height: root.dotSize
        radius: root.dotSize / 2
        color:  root.dotColor
        anchors.verticalCenter: parent.verticalCenter
        Behavior on color { ColorAnimation { duration: 200 } }
    }

    // ── Текст ────────────────────────────────────────────────────────────────
    Text {
        text: root.label
        color: "white"
        font.pixelSize: Settings.fs(13)
        anchors.verticalCenter: parent.verticalCenter
    }
}
