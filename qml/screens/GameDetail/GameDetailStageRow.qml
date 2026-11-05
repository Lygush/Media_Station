// qml/screens/GameDetail/GameDetailStageRow.qml
import QtQuick
import AppSettings 1.0
import "../../base"

Item {
    id: stageRow

    property string label:  ""
    property bool   active: false
    property bool   done:   false

    width:  parent ? parent.width : 0
    height: Settings.us(26)

    Rectangle {
        id: dot
        width: Settings.us(8); height: Settings.us(8)
        radius: width / 2
        anchors.verticalCenter: stageRow.verticalCenter
        color:   stageRow.done   ? Settings.accent(1.0)
               : stageRow.active ? "white"
               :                   "#44FFFFFF"
        opacity: stageRow.active || stageRow.done ? 1.0 : 0.35
        Behavior on color   { ColorAnimation { duration: 200 } }
        Behavior on opacity { NumberAnimation { duration: 200 } }

        SequentialAnimation on opacity {
            loops:   Animation.Infinite
            running: stageRow.active
            NumberAnimation { to: 0.3; duration: 500 }
            NumberAnimation { to: 1.0; duration: 500 }
        }
    }

    Text {
        anchors.left:            dot.right
        anchors.leftMargin:      Settings.us(10)
        anchors.verticalCenter:  stageRow.verticalCenter
        text:    stageRow.label
        color:   stageRow.done   ? Settings.accent(1.0)
               : stageRow.active ? "white"
               :                   "#66FFFFFF"
        font.pixelSize: Settings.fs(13)
        Behavior on color { ColorAnimation { duration: 200 } }
    }

    // Галочка когда done
    Text {
        anchors.right:          stageRow.right
        anchors.verticalCenter: stageRow.verticalCenter
        text:    "✓"
        color:   Settings.accent(1.0)
        font.pixelSize: Settings.fs(13)
        visible: stageRow.done && !stageRow.active
    }

    // Спиннер когда active
    Spinner {
        anchors.right:          stageRow.right
        anchors.verticalCenter: stageRow.verticalCenter
        size: Settings.us(14)
        color: "white"
        visible: stageRow.active
        running: stageRow.active
    }
}
