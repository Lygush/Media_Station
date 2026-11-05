// PillButton.qml
// ─────────────────────────────────────────────────────────────────────────────
// Небольшая кнопка для строк настроек. Ширина подстраивается под текст
// (кегль задаётся отдельно от масштаба интерфейса, поэтому фиксированная
// ширина в Settings.us() не годится — текст вылезал за рамку).
//
//   PillButton { text: "Очистить данные"; role: "danger"; onClicked: ... }
//
// role:  "accent" (по умолчанию) | "strong" (основное действие) | "danger"
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import AppSettings 1.0

Item {
    id: root

    property string text:       ""
    property string iconSource: ""
    property string role:       "accent"
    property bool   bold:       false
    property int    fontSize:   Settings.fs(13)
    property int    minWidth:   Settings.us(70)
    property int    hPadding:   Settings.us(14)
    readonly property alias containsMouse: ma.containsMouse

    signal clicked()

    implicitWidth:  Math.max(minWidth, content.implicitWidth + 2 * hPadding)
    implicitHeight: Math.max(Settings.us(34), Math.ceil(label.implicitHeight) + Settings.us(12))
    width:  implicitWidth
    height: implicitHeight

    Rectangle {
        anchors.fill: parent
        radius: Math.min(height / 2, Settings.us(10))
        color: {
            var hov = ma.containsMouse && root.enabled
            if (root.role === "danger") return hov ? "#55FF4444" : "#33FF4444"
            if (root.role === "strong") return hov ? Settings.accent(0.55) : Settings.accent(0.28)
            return hov ? Settings.accent(0.4) : Settings.accent(0.18)
        }
        border.color: root.role === "danger" ? "#88FF4444"
                    : root.role === "strong" ? Settings.accent(0.7) : Settings.accent(0.5)
        border.width: 1.5
        opacity: root.enabled ? 1.0 : 0.4
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Settings.us(6)
        opacity: root.enabled ? 1.0 : 0.4

        Image {
            visible: root.iconSource !== ""
            source: root.iconSource
            width: Settings.us(16); height: Settings.us(16)
            sourceSize.width: 64; sourceSize.height: 64
            fillMode: Image.PreserveAspectFit
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            id: label
            text: root.text
            color: root.role === "danger" ? "#FF8888" : "white"
            font.pixelSize: root.fontSize
            font.bold: root.bold
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        onClicked: root.clicked()
    }
}
