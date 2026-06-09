import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root
    property alias radius: bg.radius
    default property alias content: contentArea.children

    DropShadow {
        anchors.fill: bg
        source: bg
        radius: 30
        samples: 40
        color: "#AA000000"
        horizontalOffset: 0
        verticalOffset: 8
    }

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: root.radius
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#F0082040" }
            GradientStop { position: 0.5; color: "#F5051528" }
            GradientStop { position: 1.0; color: "#F0082040" }
        }
    }

    Rectangle {
        anchors.fill: bg
        radius: root.radius
        color: "transparent"
        border.color: Settings.accent(0.667)
        border.width: 1.5
    }

    Glow {
        anchors.fill: bg
        source: bg
        radius: 20
        samples: 28
        color: Settings.accent(0.267)
    }

    Item {
        id: contentArea
        anchors.fill: parent
    }
}