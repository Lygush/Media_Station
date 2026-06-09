// NumStepper.qml
// ─────────────────────────────────────────────────────────────────────────────
// Счётчик с кнопками − / значение / +.
// Использование:
//   NumStepper {
//       value: profileData.cores
//       minValue: 1; maxValue: 16
//       onDecremented: saveAndRefresh(id, "cores", Math.max(1, value - 1))
//       onIncremented: saveAndRefresh(id, "cores", Math.min(16, value + 1))
//   }
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import AppSettings 1.0

Row {
    id: root
    spacing: Settings.us(8)

    // ── Публичные свойства ───────────────────────────────────────────────────
    property int    value:         0
    property int    minValue:      0
    property int    maxValue:      100
    property int    step:          1
    property string displaySuffix: ""

    // Ширина центрального поля значения. По умолчанию подстраивается под контент.
    property int    valueWidth:    Settings.us(60)

    // ── Сигналы ──────────────────────────────────────────────────────────────
    signal decremented()
    signal incremented()

    // ── Кнопка − ─────────────────────────────────────────────────────────────
    Item {
        width: Settings.us(30); height: Settings.us(34)

        Rectangle {
            anchors.fill: parent; radius: 8
            color: minusMA.containsMouse ? Settings.accent(0.35) : Settings.accent(0.15)
            border.color: Settings.accent(0.4); border.width: 1
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        Image {
            anchors.centerIn: parent
            source: "qrc:/assets/icons/ic_minus.png"
            width: Settings.us(16); height: Settings.us(16)
            sourceSize.width: 100; sourceSize.height: 100
            fillMode: Image.PreserveAspectFit
            opacity: root.value > root.minValue ? 1.0 : 0.3
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            id: minusMA
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.value > root.minValue
            onClicked: root.decremented()
        }
    }

    // ── Отображение значения ─────────────────────────────────────────────────
    Item {
        width: root.valueWidth; height: Settings.us(34)

        Rectangle {
            anchors.fill: parent; radius: 8
            color: Settings.accent(0.1)
            border.color: Settings.accent(0.3); border.width: 1
        }

        Text {
            anchors.centerIn: parent
            text: root.value + root.displaySuffix
            color: "white"
            font.pixelSize: Settings.fs(13)
        }
    }

    // ── Кнопка + ─────────────────────────────────────────────────────────────
    Item {
        width: Settings.us(30); height: Settings.us(34)

        Rectangle {
            anchors.fill: parent; radius: 8
            color: plusMA.containsMouse ? Settings.accent(0.35) : Settings.accent(0.15)
            border.color: Settings.accent(0.4); border.width: 1
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        Image {
            anchors.centerIn: parent
            source: "qrc:/assets/icons/ic_add.png"
            width: Settings.us(16); height: Settings.us(16)
            sourceSize.width: 100; sourceSize.height: 100
            fillMode: Image.PreserveAspectFit
            opacity: root.value < root.maxValue ? 1.0 : 0.3
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            id: plusMA
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.value < root.maxValue
            onClicked: root.incremented()
        }
    }
}
