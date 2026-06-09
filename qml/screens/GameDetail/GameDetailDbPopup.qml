// qml/screens/GameDetail/GameDetailDbPopup.qml
import QtQuick
import AppSettings 1.0

Item {
    id: dbPopup

    property var    candidates: []
    property bool   isVisible:  false
    property string tileId:     ""

    // Позиционирование — передаётся снаружи
    // x и y выставляются в GameDetailScreen

    signal candidateSelected(string appId)

    // Пагинация
    property int _page: 0
    readonly property int _perPage: 4
    readonly property var _pageItems: {
        var s = _page * _perPage
        return candidates.slice(s, s + _perPage)
    }
    readonly property bool _hasNext: (_page + 1) * _perPage < candidates.length

    onIsVisibleChanged: if (isVisible) _page = 0

    visible: isVisible
    opacity: isVisible ? 1.0 : 0.0
    Behavior on opacity { NumberAnimation { duration: 180 } }

    width:  Settings.us(300)
    height: dbBubble.height

    // Y-координата куда должен указывать хвостик (в системе координат попапа)
    property real tipY: Settings.us(26)

    // Хвостик — треугольник указывающий влево
    Canvas {
        id: dbTipArrow
        width:  Settings.us(10)
        height: Settings.us(16)
        anchors.left: parent.left
        y: dbPopup.tipY - height / 2
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            ctx.beginPath()
            ctx.moveTo(width, 0)
            ctx.lineTo(0, height / 2)
            ctx.lineTo(width, height)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(0.09, 0.09, 0.13, 0.97)
            ctx.fill()
        }
    }

    Rectangle {
        id: dbBubble
        anchors.left:  dbTipArrow.right
        anchors.right: parent.right
        anchors.top:   parent.top
        height:        dbBubbleCol.implicitHeight + Settings.us(16)
        radius:        Settings.us(10)
        color:         Qt.rgba(0.09, 0.09, 0.13, 0.97)
        border.color:  Qt.rgba(1, 1, 1, 0.12)
        border.width:  1

        Column {
            id: dbBubbleCol
            anchors {
                left: parent.left; right: parent.right; top: parent.top
                margins: Settings.us(10); topMargin: Settings.us(10)
            }
            spacing: Settings.us(2)

            Text {
                text: Settings.tr("gamedetail_select_app")
                color: Qt.rgba(1, 1, 1, 0.45)
                font.pixelSize: Settings.fs(11)
                font.letterSpacing: 1.2
                width: parent.width
                elide: Text.ElideRight
                bottomPadding: Settings.us(4)
            }

            Repeater {
                model: dbPopup._pageItems
                delegate: Rectangle {
                    required property var modelData
                    required property int index

                    width:  dbBubbleCol.width
                    height: Math.max(Settings.us(36), nameText.implicitHeight + Settings.us(14))
                    radius: Settings.us(6)
                    color:  cMA.containsMouse ? Qt.rgba(1, 1, 1, 0.10) : "transparent"
                    Behavior on color { ColorAnimation { duration: 120 } }
                    scale: cMA.pressed ? 0.97 : 1.0
                    Behavior on scale { NumberAnimation { duration: 80 } }

                    Text {
                        id: nameText
                        anchors {
                            left: parent.left;        leftMargin:  Settings.us(8)
                            right: dbScoreLabel.left; rightMargin: Settings.us(6)
                            verticalCenter: parent.verticalCenter
                        }
                        text: modelData.name || ""
                        color: cMA.containsMouse ? "white" : Qt.rgba(1, 1, 1, 0.75)
                        font.pixelSize: Settings.fs(13)
                        wrapMode: Text.WordWrap
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }

                    Text {
                        id: dbScoreLabel
                        anchors {
                            right: parent.right; rightMargin: Settings.us(8)
                            verticalCenter: parent.verticalCenter
                        }
                        text: Math.round((modelData.score || 0) * 100) + "%"
                        color: Settings.accent(0.7)
                        font.pixelSize: Settings.fs(11)
                        width: Settings.us(32)
                        horizontalAlignment: Text.AlignRight
                    }

                    MouseArea {
                        id: cMA
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            dbPopup.isVisible = false
                            dbPopup._page = 0
                            dbPopup.candidateSelected(modelData.appId || "")
                        }
                    }
                }
            }

            Item { width: parent.width; height: Settings.us(4) }
            Rectangle {
                width: parent.width; height: 1
                color: Qt.rgba(1, 1, 1, 0.08)
            }
            Item { width: parent.width; height: Settings.us(4) }

            Row {
                width: parent.width
                spacing: Settings.us(6)

                Rectangle {
                    visible: dbPopup._hasNext
                    width:  visible ? (parent.width - Settings.us(6)) / 2 : 0
                    height: Settings.us(30)
                    radius: Settings.us(6)
                    color:  nextMA.containsMouse ? Qt.rgba(1,1,1,0.12) : Qt.rgba(1,1,1,0.06)
                    border.color: Qt.rgba(1,1,1,0.15); border.width: 1
                    Behavior on color { ColorAnimation { duration: 120 } }
                    Text {
                        anchors.centerIn: parent
                        text: Settings.tr("gamedetail_next")
                        color: "white"; font.pixelSize: Settings.fs(12)
                    }
                    MouseArea {
                        id: nextMA; anchors.fill: parent; hoverEnabled: true
                        onClicked: dbPopup._page++
                    }
                }

                Rectangle {
                    width:  dbPopup._hasNext
                            ? (parent.width - Settings.us(6)) / 2
                            : parent.width
                    height: Settings.us(30)
                    radius: Settings.us(6)
                    color:  skipMA.containsMouse ? Qt.rgba(1,1,1,0.12) : Qt.rgba(1,1,1,0.06)
                    border.color: Qt.rgba(1,1,1,0.15); border.width: 1
                    Behavior on color { ColorAnimation { duration: 120 } }
                    Text {
                        anchors.centerIn: parent
                        text: Settings.tr("gamedetail_skip")
                        color: Qt.rgba(1, 1, 1, 0.6); font.pixelSize: Settings.fs(12)
                    }
                    MouseArea {
                        id: skipMA; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            dbPopup.isVisible = false
                            dbPopup._page = 0
                            dbPopup.candidateSelected("")
                        }
                    }
                }
            }
        }
    }
}
