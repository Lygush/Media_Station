// qml/OverlayScreen.qml
import QtQuick
import QtQuick.Window
import AppSettings 1.0

Item {
    id: root
    anchors.fill: parent
    z: 800

    property Window mainWindow: Window.window
    visible: OverlayModule?.visible ?? false

    property int focusIndex: 0
    readonly property int buttonCount: {
        if (!visible) return 0
        return (OverlayModule?.context === "androidtv") ? 2 : 0
    }

    Connections {
        target: OverlayModule ?? null

        function onOverlayKeyPressed(key) {
            if (!root.visible) return

            if (key === "left" || key === "up") {
                focusIndex = Math.max(0, focusIndex - 1)
                return
            }
            if (key === "right" || key === "down") {
                focusIndex = Math.min(buttonCount - 1, focusIndex + 1)
                return
            }

            if (key === "select") {
                if (focusIndex === 0)      _dismissAction()
                else if (focusIndex === 1) _exitAction()
            }
        }
    }

    property bool _actionPending: false

    function _dismissAction() {
        if (_actionPending) return
        if (OverlayModule?.visible) {
            _actionPending = true
            OverlayModule.hide()
        }
    }

    function _exitAction() {
        if (_actionPending) return
        if (OverlayModule?.visible) {
            _actionPending = true
            OverlayModule.exitAndroidTV()
        }
    }

    onVisibleChanged: {
        if (visible) {
            focusIndex = 0
            panelWindow.visible = true
        } else {
            panelWindow.visible = false
            Qt.callLater(function() { _actionPending = false })
        }
    }

    // Прозрачная область для кликов (закрытие оверлея)
    MouseArea {
        anchors.fill: parent
        onClicked: _dismissAction()
    }

    // Отдельное окно с панелью кнопок
    Window {
        id: panelWindow
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        color: "transparent"
        visible: false
        width: panel.width
        height: panel.height

        readonly property int bottomMargin: Settings.us(120)

        function _updatePos() {
            if (!mainWindow) return
            x = mainWindow.x + (mainWindow.width  - width)  / 2
            y = mainWindow.y +  mainWindow.height  - height - bottomMargin
        }

        Component.onCompleted: _updatePos()

        Connections {
            target: mainWindow
            function onXChanged()      { panelWindow._updatePos() }
            function onYChanged()      { panelWindow._updatePos() }
            function onWidthChanged()  { panelWindow._updatePos() }
            function onHeightChanged() { panelWindow._updatePos() }
        }

        // Панель с кнопками
        Rectangle {
            id: panel
            width: Settings.us(420)
            height: panelColumn.implicitHeight + Settings.us(48)
            radius: Settings.us(20)
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#F2061830" }
                GradientStop { position: 1.0; color: "#F5040E22" }
            }
            border.color: Settings.accent(0.45)
            border.width: 1.5

            Rectangle {
                anchors.fill: parent
                anchors.margins: -1
                radius: parent.radius + 1
                color: "transparent"
                border.color: Settings.accent(0.18)
                border.width: 3
            }

            // Анимация выезда снизу
            y: root.visible ? 0 : height + 20
            Behavior on y {
                NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
            }

            Column {
                id: panelColumn
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: Settings.us(24)
                spacing: 0
                width: parent.width - Settings.us(48)

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(10)
                    Image {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Settings.us(28); height: Settings.us(28)
                        source: OverlayModule?.context === "androidtv"
                                ? "qrc:/assets/icons/ic_android_tv.png"
                                : "qrc:/assets/icons/ic_power.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        opacity: 0.85
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: {
                            return OverlayModule?.context === "androidtv"
                                   ? Settings.tr("atv_overlay_title")
                                   : Settings.tr("game_overlay_title")
                        }
                        color: "white"
                        font.family: appFont.name
                        font.pixelSize: Settings.fs(18)
                        font.bold: true
                        opacity: 0.9
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: Settings.accent(0.25)
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: Settings.us(16)
                }

                Item { width: 1; height: Settings.us(20) }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(16)
                    visible: OverlayModule?.context === "androidtv"

                    OverlayButton {
                        buttonIndex: 0
                        label: Settings.tr("atv_overlay_back_to_tv")
                        primary: false
                        iconSource: "qrc:/assets/icons/ic_back.png"
                        onClicked: root._dismissAction()
                    }
                    OverlayButton {
                        buttonIndex: 1
                        label: Settings.tr("atv_overlay_exit")
                        primary: true
                        iconSource: "qrc:/assets/icons/ic_exit.png"
                        onClicked: root._exitAction()
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(16)
                    visible: OverlayModule?.context === "game"

                    OverlayButton {
                        buttonIndex: 0
                        label: Settings.tr("game_overlay_resume")
                        primary: false
                        iconSource: "qrc:/assets/icons/ic_back.png"
                        onClicked: root._dismissAction()
                    }
                    OverlayButton {
                        buttonIndex: 1
                        label: Settings.tr("game_overlay_exit")
                        primary: true
                        iconSource: "qrc:/assets/icons/ic_exit.png"
                        onClicked: {
                            OverlayModule?.hide()
                        }
                    }
                }

                Item { width: 1; height: Settings.us(24) }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Settings.tr("overlay_hint")
                    color: "#55FFFFFF"
                    font.pixelSize: Settings.fs(11)
                }
                Item { width: 1; height: Settings.us(4) }
            }
        }
    }

    component OverlayButton: Rectangle {
        id: btn
        required property int buttonIndex
        required property string label
        property bool primary: false
        property string iconSource: ""
        signal clicked()
        readonly property bool hasFocus: root.focusIndex === buttonIndex

        width: Settings.us(186); height: Settings.us(52); radius: Settings.us(12)
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: btn.primary
                       ? (btn.hasFocus ? Settings.accent(0.55) : Settings.accent(0.30))
                       : (btn.hasFocus ? "#44FFFFFF"           : "#1FFFFFFF")
            }
            GradientStop {
                position: 1.0
                color: btn.primary
                       ? (btn.hasFocus ? Settings.accent(0.35) : Settings.accent(0.18))
                       : (btn.hasFocus ? "#22FFFFFF"           : "#0DFFFFFF")
            }
        }
        border.color: btn.hasFocus ? (btn.primary ? Settings.accent(1.0) : "#CCFFFFFF")
                                   : (btn.primary ? Settings.accent(0.45) : "#33FFFFFF")
        border.width: btn.hasFocus ? 2 : 1.5
        Behavior on border.color { ColorAnimation { duration: 150 } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.color: Settings.accent(0.9)
            border.width: 2
            opacity: btn.hasFocus ? 0.6 : 0.0
            Behavior on opacity { NumberAnimation { duration: 200 } }
        }

        Row {
            anchors.centerIn: parent
            spacing: Settings.us(8)
            Image {
                anchors.verticalCenter: parent.verticalCenter
                visible: btn.iconSource !== ""
                width: Settings.us(18); height: Settings.us(18)
                source: btn.iconSource
                fillMode: Image.PreserveAspectFit
                smooth: true
                opacity: 0.85
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: btn.label
                color: "white"
                font.family: appFont.name
                font.pixelSize: Settings.fs(14)
                font.bold: btn.primary
            }
        }

        scale: btn.hasFocus ? 1.04 : 1.0
        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: false
            onClicked: btn.clicked()
        }
    }
}
