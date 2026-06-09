import QtQuick
import AppSettings 1.0

Item {
    id: root

    property bool running: false
    property real size: Settings.us(20)
    property color color: Settings.accent(1.0)

    width: size
    height: size

    // Серый трек
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "transparent"
        border.color: "#33FFFFFF"
        border.width: size / 8
    }

    // Вращающаяся дуга
    Item {
        id: arcContainer
        anchors.fill: parent
        transformOrigin: Item.Center

        RotationAnimator {
            target: arcContainer
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
            running: root.running
            easing.type: Easing.Linear
        }

        Item {
            width: parent.width
            height: parent.height / 2
            anchors.top: parent.top
            clip: true

            Rectangle {
                width: parent.width
                height: parent.width
                radius: width / 2
                anchors.top: parent.top
                color: "transparent"
                border.color: root.color
                border.width: root.size / 8
            }
        }
    }
}