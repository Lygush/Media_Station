import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root

    property bool checked: false
    signal toggled()

    width: Settings.us(56)
    height: Settings.us(30)

    Rectangle {
        anchors.fill: parent
        radius: Settings.us(15)
        color: root.checked ? Settings.accent(0.8) : "#44FFFFFF"
        border.color: root.checked ? Settings.accent(1.0) : "#60FFFFFF"
        border.width: 1.5
        Behavior on color { ColorAnimation { duration: 200 } }

        Rectangle {
            id: knob
            width: Settings.us(22)
            height: Settings.us(22)
            radius: Settings.us(11)
            color: "white"
            anchors.verticalCenter: parent.verticalCenter
            x: root.checked ? parent.width - width - 4 : 4
            Behavior on x { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

            layer.enabled: true
            layer.effect: DropShadow {
                cached: true
                radius: 6
                samples: 12
                color: "#40000000"
                horizontalOffset: 0
                verticalOffset: 2
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled()
    }
}