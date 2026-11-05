import QtQuick
import AppSettings 1.0

Item {
    id: root

    property string label: ""
    default property alias content: rowContainer.children

    width: parent ? parent.width : 0
    height: Math.max(Settings.us(44), rowContainer.height + Settings.us(10),
                     Math.ceil(labelText.implicitHeight) + Settings.us(14))

    // Подпись не заходит под элементы справа — обрезается многоточием
    Text {
        id: labelText
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, root.width - rowContainer.width - Settings.us(16))
        text: root.label
        color: "white"
        font.pixelSize: Settings.fs(14)
        elide: Text.ElideRight
    }

    Row {
        id: rowContainer
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
    }
}