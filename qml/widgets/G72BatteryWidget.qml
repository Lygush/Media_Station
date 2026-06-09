// G72BatteryWidget.qml
// ─────────────────────────────────────────────────────────────────────────────
// Индикатор заряда пульта G72.
// Встраивается в topRow в Main.qml между часами и кнопкой настроек.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root

    width:  Settings.us(64)
    height: Settings.us(64)

    readonly property bool isConnected: (typeof G72Battery !== "undefined"
                                         && G72Battery !== null
                                         && G72Battery.connected)

    // ── Иконка пульта ─────────────────────────────────────────────────────────
    Image {
        source: "qrc:/assets/icons/ic_remote_control.png"
        width: Settings.us(36); height: Settings.us(36)
        sourceSize.width: 100; sourceSize.height: 100
        smooth: true
        fillMode: Image.PreserveAspectFit
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Settings.us(6)
        opacity: root.isConnected ? 1.0 : 0.35
        Behavior on opacity { NumberAnimation { duration: 400 } }
    }

    // ── Полоска заряда (только когда подключено) ──────────────────────────────
    Item {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Settings.us(7)
        anchors.horizontalCenter: parent.horizontalCenter
        width: Settings.us(36)
        height: Settings.us(6)
        visible: root.isConnected

        Rectangle {
            anchors.fill: parent
            radius: 3
            color: "#40FFFFFF"
        }

        Rectangle {
            id: barFill
            anchors.left:   parent.left
            anchors.top:    parent.top
            anchors.bottom: parent.bottom
            radius: 3

            width: {
                if (!root.isConnected) return 0
                var pct = (G72Battery.battery >= 0) ? G72Battery.battery : 0
                return Math.round(parent.width * pct / 100)
            }
            Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }

            color: {
                if (!root.isConnected) return "#888888"
                switch (G72Battery.zone) {
                    case 0:  return "#4CAF50"
                    case 1:  return "#FFC107"
                    case 2:  return "#F44336"
                    default: return "#888888"
                }
            }
            Behavior on color { ColorAnimation { duration: 300 } }
        }
    }

    // ── Тултип ────────────────────────────────────────────────────────────────
    ToolTip.visible: hoverArea.containsMouse
    ToolTip.delay:   600
    ToolTip.text: {
        if (!root.isConnected) return qsTr("G72 remote: not connected")
        var pct = G72Battery.battery
        if (pct < 0) return qsTr("G72 remote: reading...")
        return qsTr("G72 remote: %1%").arg(pct)
    }

    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
    }
}
