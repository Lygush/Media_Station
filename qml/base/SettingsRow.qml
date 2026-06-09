import QtQuick
import AppSettings 1.0

Item {
    id: root

    property string label: ""
    default property alias content: rowContainer.children

    width: parent ? parent.width : 0
    height: Settings.us(44)

    Text {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        color: "white"
        font.pixelSize: Settings.fs(14)
    }

    Row {
        id: rowContainer
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
    }
}