import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root

    property string text: ""
    property string iconSource: ""
    property string role: "default"   // "primary", "danger", "default"
    property bool enabled: true
    property alias containsMouse: buttonMouse.containsMouse

    signal clicked()

    width: Math.max(Settings.us(120), buttonContent.implicitWidth + Settings.us(32))
    height: Settings.us(52)

    scale: buttonMouse.containsMouse ? 1.04 : 1.0
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    // Цвета по ролям
    function bgColor(hovered) {
        switch (role) {
            case "primary": return hovered ? Settings.accent(0.53) : Settings.accent(0.33);
            case "danger":  return hovered ? "#66FF3333" : "#33FF3333";
            default:        return hovered ? Settings.accent(0.267) : Settings.accent(0.133);
        }
    }
    function borderColor() {
        switch (role) {
            case "primary": return Settings.accent(0.667);
            case "danger":  return "#99FF5555";
            default:        return Settings.accent(0.4);
        }
    }
    function textColor() {
        return role === "danger" ? "#FF8888" : "white";
    }

    Rectangle {
        id: buttonBg
        anchors.fill: parent
        radius: 12
        color: root.bgColor(buttonMouse.containsMouse && root.enabled)
        border.color: root.borderColor()
        border.width: 1.5
        Behavior on color { ColorAnimation { duration: 120 } }

        // Глоу-эффект только для primary (можно расширить)
        Glow {
            anchors.fill: parent
            radius: 16
            samples: 24
            color: Settings.accent(0.8)
            opacity: (buttonMouse.containsMouse && root.role === "primary") ? 0.8 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
    }

    Row {
        id: buttonContent
        anchors.centerIn: parent
        spacing: Settings.us(8)

        Image {
            source: root.iconSource
            width: Settings.us(18)
            height: Settings.us(18)
            sourceSize.width: 100
            sourceSize.height: 100
            fillMode: Image.PreserveAspectFit
            visible: root.iconSource !== ""
            anchors.verticalCenter: parent.verticalCenter
        }

        Text {
            text: root.text
            color: root.textColor()
            font.pixelSize: Settings.fs(16)
            font.bold: root.role === "primary" || root.role === "danger"
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        id: buttonMouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: root.enabled
        onClicked: root.clicked()
    }
}