// Notification.qml

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root

    anchors.bottom: parent.bottom
    anchors.bottomMargin: Settings.us(32)
    anchors.horizontalCenter: parent.horizontalCenter

    width:  Settings.us(420)
    height: notifBox.height
    z:      650

    visible: opacity > 0
    opacity: 0

    property string _level:       "info"
    property string _message:     ""
    property bool   _autoDismiss: false
    property int    _duration:    4000

    readonly property color _accentColor: {
        switch (_level) {
            case "error":   return "#E85555"
            case "warning": return "#E8B840"
            default:        return Settings.accentColor
        }
    }

    readonly property color _bgTop: {
        switch (_level) {
            case "error":   return "#E8150505"
            case "warning": return "#E8150A00"
            default:        return "#E8062030"
        }
    }

    readonly property color _bgBot: {
        switch (_level) {
            case "error":   return "#F00A0303"
            case "warning": return "#F00D0700"
            default:        return "#F0040F1E"
        }
    }

    readonly property string _iconSource: {
        return _level === "info"
            ? "qrc:/assets/icons/ic_info.png"
            : "qrc:/assets/icons/ic_error.png"
    }

    Connections {
        target: NotificationModule

        function onCurrentChanged() {
            root._level       = NotificationModule.level
            root._message     = NotificationModule.message
            root._autoDismiss = NotificationModule.autoDismiss
            root._duration    = NotificationModule.duration
        }

        function onShowRequested() {
            root._level       = NotificationModule.level
            root._message     = NotificationModule.message
            root._autoDismiss = NotificationModule.autoDismiss
            root._duration    = NotificationModule.duration

            hideAnim.stop()
            autoDismissBar.width = 0
            showAnim.start()
            if (root._autoDismiss)
                progressAnim.restart()
        }

        function onHideRequested() {
            showAnim.stop()
            progressAnim.stop()
            hideAnim.start()
        }
    }

    ParallelAnimation {
        id: showAnim
        NumberAnimation {
            target: root; property: "opacity"
            from: 0; to: 1; duration: 280; easing.type: Easing.OutCubic
        }
        NumberAnimation {
            target: root; property: "anchors.bottomMargin"
            from: 0; to: Settings.us(32); duration: 300; easing.type: Easing.OutCubic
        }
    }

    SequentialAnimation {
        id: hideAnim
        ParallelAnimation {
            NumberAnimation {
                target: root; property: "opacity"
                from: 1; to: 0; duration: 220; easing.type: Easing.InCubic
            }
            NumberAnimation {
                target: root; property: "anchors.bottomMargin"
                from: Settings.us(32); to: 0; duration: 240; easing.type: Easing.InCubic
            }
        }
        ScriptAction {
            script: NotificationModule.onDismissed()
        }
    }

    DropShadow {
        cached: true
        anchors.fill: notifBox; source: notifBox
        radius: 24; samples: 32; color: "#CC000000"
        horizontalOffset: 0; verticalOffset: 6
    }

    Glow {
        cached: true
        visible: opacity > 0   // невидимое свечение не считать каждый кадр
        anchors.fill: notifBox; source: notifBox
        radius: 14; samples: 24
        color: root._accentColor
        opacity: 0.45
    }

    Rectangle {
        id: notifBox
        width:  root.width
        // Высота по тексту: раньше фиксированные us(56), и сообщение в 3–4
        // строки (длинное название игры) вылезало за плашку и за край экрана.
        height: Math.max(Settings.us(56), msgText.contentHeight + Settings.us(28))
                + (root._autoDismiss ? Settings.us(16) : 0)
        radius: Settings.us(16)

        gradient: Gradient {
            GradientStop { position: 0.0; color: root._bgTop }
            GradientStop { position: 1.0; color: root._bgBot }
        }

        Rectangle {
            id: borderRect
            anchors.fill: parent; radius: parent.radius
            color: "transparent"
            border.color: Qt.rgba(
                root._accentColor.r,
                root._accentColor.g,
                root._accentColor.b,
                0.6)
            border.width: 1.5
        }

        Row {
            id: contentRow
            anchors {
                verticalCenter: parent.verticalCenter
                verticalCenterOffset: root._autoDismiss ? -Settings.us(8) : 0
                left:   parent.left;  leftMargin:  Settings.us(16)
                right:  parent.right; rightMargin: Settings.us(16)
            }
            spacing: Settings.us(12)

            Behavior on anchors.verticalCenterOffset { NumberAnimation { duration: 150 } }

            Image {
                source: root._iconSource
                width:  Settings.us(26); height: Settings.us(26)
                sourceSize.width: 52; sourceSize.height: 52
                fillMode: Image.PreserveAspectFit
                smooth: true
                anchors.verticalCenter: parent.verticalCenter

                ColorOverlay {
                    anchors.fill: parent
                    source: parent
                    color: root._accentColor
                }
            }

            Text {
                id: msgText
                width: parent.width - Settings.us(26) - Settings.us(28) - Settings.us(24)
                maximumLineCount: 5
                elide: Text.ElideRight
                text: root._message
                color: "white"
                font.family: appFont.name
                font.pixelSize: Settings.fs(15)
                wrapMode: Text.WordWrap
                anchors.verticalCenter: parent.verticalCenter
            }

            Item {
                width: Settings.us(28); height: Settings.us(28)
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.fill: parent; radius: Settings.us(14)
                    color: closeMA.containsMouse ? "#44FFFFFF" : "transparent"
                    Behavior on color { ColorAnimation { duration: 100 } }
                }

                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    color: closeMA.containsMouse ? "white" : "#AAAAAA"
                    font.pixelSize: Settings.fs(13)
                    Behavior on color { ColorAnimation { duration: 100 } }
                }

                MouseArea {
                    id: closeMA
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: NotificationModule.dismiss()
                }
            }
        }

        Item {
            visible: root._autoDismiss
            anchors {
                bottom: parent.bottom; bottomMargin: Settings.us(8)
                left:   parent.left;   leftMargin:   Settings.us(16)
                right:  parent.right;  rightMargin:  Settings.us(16)
            }
            height: Settings.us(4)

            Rectangle {
                anchors.fill: parent
                radius: 2
                color: "#22FFFFFF"
            }

            Rectangle {
                id: autoDismissBar
                anchors.left:   parent.left
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                radius: 2
                width: parent.width
                color: root._accentColor
                opacity: 0.8
            }

            NumberAnimation {
                id: progressAnim
                target: autoDismissBar; property: "width"
                from: autoDismissBar.parent.width
                to:   0
                duration: root._duration
                easing.type: Easing.Linear
            }
        }
    }
}
