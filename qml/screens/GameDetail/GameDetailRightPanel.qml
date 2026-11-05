// qml/screens/GameDetail/GameDetailRightPanel.qml
import QtQuick
import AppSettings 1.0

Item {
    id: rightPanel

    property string description: ""
    property string streamingText: ""
    property bool   streaming:     false

    readonly property bool _hasContent: description !== "" || streaming
    readonly property real contentHeight: _hasContent ? descCol.implicitHeight : 0

    Behavior on height { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

    visible: _hasContent
    opacity: _hasContent ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 300 } }

    // Тёмная подложка: описание лежало прямо на фоне, и на светлых
    // участках картинки (небо, вода, светлые обложки) текст не читался.
    Rectangle {
        anchors.fill: descCol
        anchors.margins: -Settings.us(16)
        radius: Settings.us(12)
        color: Qt.rgba(0, 0, 0, 0.45)
        visible: rightPanel._hasContent
    }

    Column {
        id: descCol
        anchors {
            left: parent.left;   leftMargin:  Settings.us(20)
            right: parent.right; rightMargin: Settings.us(20)
            top: parent.top
        }
        spacing: Settings.us(20)

        // ── Об игре ───────────────────────────────────────────────────────────
        Column {
            width: parent.width
            spacing: Settings.us(10)
            visible: rightPanel.description !== "" || rightPanel.streaming

            // Заголовок секции
            Text {
                text: Settings.tr("gamedetail_about")
                color: "white"
                font.pixelSize: Settings.fs(13)
                font.bold: true
                font.letterSpacing: 1.5
                opacity: 0.55
            }

            // Текст описания — во время стриминга зелёный с курсором
            Text {
                id: descText
                width: parent.width
                text:  rightPanel.streaming ? rightPanel.streamingText : rightPanel.description
                color: rightPanel.streaming ? "#AADDAA" : "#DDDDDD"
                font.pixelSize: Settings.fs(14)
                wrapMode: Text.WordWrap
                lineHeight: 1.45
                Behavior on color { ColorAnimation { duration: 400 } }

                // Мигающий курсор во время стриминга
                Rectangle {
                    visible: rightPanel.streaming
                    width:   Settings.us(2)
                    height:  Settings.fs(14)
                    color:   "#88AADDAA"
                    anchors.left:           descText.left
                    anchors.leftMargin:     descText.contentWidth + Settings.us(2)
                    anchors.verticalCenter: descText.verticalCenter

                    SequentialAnimation on opacity {
                        running: rightPanel.streaming
                        loops:   Animation.Infinite
                        NumberAnimation { to: 0.0; duration: 500 }
                        NumberAnimation { to: 1.0; duration: 500 }
                    }
                }
            }
        }
    }
}
