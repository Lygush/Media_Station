// qml/screens/GameDetail/GameDetailJsonPopup.qml
import QtQuick
import AppSettings 1.0

Item {
    id: jsonPopup

    property string jsonText:  ""
    property bool   isVisible: false

    visible: isVisible
    opacity: isVisible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    property real maxWidth: 100000
    width:  Math.min(maxWidth, Math.max(Settings.us(320), Settings.us(140) + Settings.fs(11) * 18))
    height: jsonBubble.height

    // Y-координата куда должен указывать хвостик (в системе координат попапа)
    property real tipY: Settings.us(26)

    // Хвостик — треугольник указывающий влево
    Canvas {
        id: jsonTipArrow
        width:  Settings.us(10)
        height: Settings.us(16)
        anchors.left: parent.left
        y: jsonPopup.tipY - height / 2
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.beginPath()
            ctx.moveTo(width, 0)
            ctx.lineTo(0, height / 2)
            ctx.lineTo(width, height)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(0.05, 0.10, 0.07, 1.0)
            ctx.fill()
        }
    }

    Rectangle {
        id: jsonBubble
        anchors.left:  jsonTipArrow.right
        anchors.right: parent.right
        anchors.top:   parent.top
        height:        jsonCol.implicitHeight + Settings.us(16)
        radius:        Settings.us(10)
        color:         Qt.rgba(0.05, 0.10, 0.07, 1.0)
        border.color:  Settings.accent(0.30)
        border.width:  1

        Column {
            id: jsonCol
            anchors {
                left: parent.left; right: parent.right; top: parent.top
                margins: Settings.us(12); topMargin: Settings.us(10)
            }
            spacing: Settings.us(8)

            Row {
                spacing: Settings.us(8)

                // Мигающий индикатор генерации
                Rectangle {
                    width:  Settings.us(7); height: Settings.us(7)
                    radius: width / 2
                    color:  Settings.accent(1.0)
                    anchors.verticalCenter: parent.verticalCenter

                    SequentialAnimation on opacity {
                        loops:   Animation.Infinite
                        running: jsonPopup.isVisible
                        NumberAnimation { to: 0.2; duration: 450 }
                        NumberAnimation { to: 1.0; duration: 450 }
                    }
                }

                Text {
                    text: Settings.tr("gamedetail_generating_json")
                    color: Settings.accent(0.85)
                    font.pixelSize: Settings.fs(11)
                    font.letterSpacing: 1.1
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            // JSON текст — видны последние строки (генерация идёт потоком),
            // высота ограничена, чтобы попап не уходил за край экрана
            Item {
                width: parent.width
                height: Math.min(jsonTextItem.implicitHeight, Settings.us(320))
                clip: true

                Text {
                    id: jsonTextItem
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    text:  jsonPopup.jsonText
                    color: "#AAFFCC"
                    font.pixelSize: Settings.fs(11)
                    font.family:    "Consolas, monospace"
                    wrapMode:       Text.WrapAnywhere
                }
                // Верхняя строка срезана — плавно растворяем её, а не рубим пополам
                Rectangle {
                    visible: jsonTextItem.implicitHeight > parent.height
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    height: Math.round(Settings.fs(11) * 2.4)
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.rgba(0.05, 0.10, 0.07, 1.0) }
                        GradientStop { position: 1.0; color: Qt.rgba(0.05, 0.10, 0.07, 0.0) }
                    }
                }
            }
        }
    }
}
