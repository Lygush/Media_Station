// settings/LogsSection.qml
import QtQuick
import QtQuick.Controls.Basic 2.15
import AppSettings 1.0
import "../base"

Item {
    id: logRoot

    property string currentServerText: ""

    Column {
        id: logContentColumn
        anchors.left: parent.left; anchors.right: parent.right
        spacing: 0

        SettingsRow {
            label: Settings.safeTr("log_server")
            Row {
                spacing: Settings.us(8)

                ComboBox {
                    id: serverCombo
                    width: Settings.us(180); height: Settings.us(34)
                    model: {
                        var s = NetworkListener && NetworkListener.logSender ? NetworkListener.logSender : null
                        return (s && s.foundServers && s.foundServers.length > 0) ? s.foundServers : ["—"]
                    }
                    enabled: !(NetworkListener && NetworkListener.logSender && NetworkListener.logSender.busy)
                    opacity: enabled ? 1.0 : 0.5
                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    background: Rectangle {
                        radius: 7; color: Settings.accent(0.1)
                        border.color: Settings.accent(0.3); border.width: 1.5
                    }
                    contentItem: Text {
                        text: serverCombo.currentText; color: "white"
                        font.pixelSize: Settings.fs(13); verticalAlignment: Text.AlignVCenter
                        leftPadding: Settings.us(10)
                    }
                    indicator: Canvas {
                        id: comboCanvas
                        x: serverCombo.width - width - Settings.us(8)
                        y: (serverCombo.availableHeight - height) / 2
                        width: 12; height: 8; contextType: "2d"
                        Connections {
                            target: serverCombo
                            function onPressedChanged() { comboCanvas.requestPaint() }
                        }
                        onPaint: {
                            context.reset()
                            context.moveTo(0, 0); context.lineTo(width, 0)
                            context.lineTo(width / 2, height); context.closePath()
                            context.fillStyle = "white"; context.fill()
                        }
                    }
                    popup: Popup {
                        y: serverCombo.height - 1; width: serverCombo.width
                        implicitHeight: contentItem.implicitHeight; padding: 1
                        background: Rectangle { color: "#F0051528"; border.color: Settings.accent(0.6); radius: 8 }
                        contentItem: ListView {
                            clip: true; implicitHeight: contentHeight
                            model: serverCombo.popup.visible ? serverCombo.delegateModel : null
                            currentIndex: serverCombo.highlightedIndex
                            ScrollIndicator.vertical: ScrollIndicator {}
                        }
                    }
                    delegate: ItemDelegate {
                        width: serverCombo.width
                        contentItem: Text {
                            text: modelData; color: "white"; font: serverCombo.font
                            elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: highlighted ? Settings.accent(0.4) : "transparent"; radius: 6
                        }
                        highlighted: serverCombo.highlightedIndex === index
                    }
                    onCurrentTextChanged: logRoot.currentServerText = currentText
                    Component.onCompleted: logRoot.currentServerText = currentText
                }

                Item {
                    width: Settings.us(165); height: Settings.us(34)
                    enabled: !(NetworkListener && NetworkListener.logSender && NetworkListener.logSender.busy)
                    Rectangle {
                        anchors.fill: parent; radius: 8
                        color: discoverMA.containsMouse && parent.enabled
                               ? Settings.accent(0.4) : Settings.accent(0.18)
                        border.color: Settings.accent(0.5); border.width: 1.5
                        Behavior on color { ColorAnimation { duration: 120 } }
                        opacity: parent.enabled ? 1.0 : 0.4
                    }
                    Row {
                        anchors.centerIn: parent; spacing: Settings.us(6)
                        Image {
                            source: "qrc:/assets/icons/ic_search.png"
                            width: Settings.us(16); height: Settings.us(16)
                            sourceSize.width: 64; sourceSize.height: 64
                            fillMode: Image.PreserveAspectFit
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: Settings.safeTr("search_servers"); color: "white"
                            font.pixelSize: Settings.fs(12); anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: discoverMA; anchors.fill: parent; hoverEnabled: true
                        enabled: parent.enabled
                        onClicked: {
                            var sender = NetworkListener && NetworkListener.logSender
                                         ? NetworkListener.logSender : null
                            if (sender) sender.discoverServers()
                        }
                    }
                }
            }
        }

        SectionDivider {}

        // Прогресс-бар отправки
        Item {
            width: parent.width; height: Settings.us(36)
            visible: NetworkListener && NetworkListener.logSender && NetworkListener.logSender.busy
            opacity: visible ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 150 } }

            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: Settings.safeTr("log_progress"); color: "white"
                font.pixelSize: Settings.fs(13); opacity: 0.65
            }
            ProgressBar {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: Settings.us(200); height: Settings.us(8); radius: Settings.us(4)
                animated: true
                value: (NetworkListener && NetworkListener.logSender
                        ? NetworkListener.logSender.progress : 0) / 100
                trackColor: Settings.accent(0.12); barColor: Settings.accent(1.0)
            }
        }

        SectionDivider {
            visible: NetworkListener && NetworkListener.logSender && NetworkListener.logSender.busy
        }

        // Кнопки «Отправить» / «Отмена»
        Item {
            width: parent.width; height: Settings.us(52)
            Row {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                spacing: Settings.us(10)

                Item {
                    width: Settings.us(165); height: Settings.us(38)
                    enabled: {
                        var s = NetworkListener && NetworkListener.logSender ? NetworkListener.logSender : null
                        return s && !s.busy && s.foundServers && s.foundServers.length > 0
                    }
                    Rectangle {
                        anchors.fill: parent; radius: 10
                        color: sendMA.containsMouse && parent.enabled
                               ? Settings.accent(0.55) : Settings.accent(0.28)
                        border.color: Settings.accent(0.7); border.width: 1.5
                        Behavior on color { ColorAnimation { duration: 130 } }
                        opacity: parent.enabled ? 1.0 : 0.35
                    }
                    Row {
                        anchors.centerIn: parent; spacing: Settings.us(8)
                        Image {
                            source: "qrc:/assets/icons/ic_upload.png"
                            width: Settings.us(16); height: Settings.us(16)
                            sourceSize.width: 64; sourceSize.height: 64
                            fillMode: Image.PreserveAspectFit
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: Settings.safeTr("send_logs"); color: "white"
                            font.pixelSize: Settings.fs(13); font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    MouseArea {
                        id: sendMA; anchors.fill: parent; hoverEnabled: true
                        enabled: parent.enabled
                        onClicked: {
                            var ip = logRoot.currentServerText
                            if (ip === "—" || ip === "") return
                            var sender = NetworkListener && NetworkListener.logSender
                                         ? NetworkListener.logSender : null
                            if (sender) sender.sendLogs(ip)
                        }
                    }
                }

                Item {
                    width: Settings.us(80); height: Settings.us(38)
                    visible: NetworkListener && NetworkListener.logSender && NetworkListener.logSender.busy
                    Rectangle {
                        anchors.fill: parent; radius: 10
                        color: cancelMA.containsMouse ? "#55FF4444" : "#33FF4444"
                        border.color: "#88FF4444"; border.width: 1.5
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        anchors.centerIn: parent; text: Settings.safeTr("cancel")
                        color: "#FF8888"; font.pixelSize: Settings.fs(12); font.bold: true
                    }
                    MouseArea {
                        id: cancelMA; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            var sender = NetworkListener && NetworkListener.logSender
                                         ? NetworkListener.logSender : null
                            if (sender) sender.cancelSend()
                        }
                    }
                }
            }
        }

        SectionDivider {}

        // Статус
        Item {
            width: parent.width; height: Settings.us(40)
            Text {
                anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                text: Settings.safeTr("log_status") + ":"
                color: "white"; font.pixelSize: Settings.fs(13); opacity: 0.65
            }
            Text {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: NetworkListener && NetworkListener.logSender
                      ? NetworkListener.logSender.status : ""
                color: Settings.accent(1.0); font.pixelSize: Settings.fs(13)
                elide: Text.ElideLeft; width: parent.width * 0.65
                horizontalAlignment: Text.AlignRight
            }
        }
    }

    // Результирующий попап (успех/ошибка)
    Item {
        id: resultPopup
        property bool   success: true
        property string message: ""
        visible: false
        anchors.fill: parent; z: 100

        Rectangle {
            anchors.fill: parent; color: "#AA000000"
            Rectangle {
                anchors.centerIn: parent
                width: Settings.us(300); height: Settings.us(150); radius: 16
                color: "#F0051528"
                border.color: resultPopup.success ? Settings.accent(0.7) : "#88FF4444"
                border.width: 1.5
                Column {
                    anchors.centerIn: parent; spacing: Settings.us(14)
                    width: parent.width - Settings.us(32)
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: resultPopup.success
                              ? Settings.safeTr("log_send_success")
                              : Settings.safeTr("log_send_error")
                        color: resultPopup.success ? Settings.accent(1.0) : "#FF8888"
                        font.pixelSize: Settings.fs(16); font.bold: true
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: resultPopup.message; color: "white"
                        font.pixelSize: Settings.fs(12); wrapMode: Text.WordWrap
                        width: parent.width; horizontalAlignment: Text.AlignHCenter
                    }
                    Item {
                        width: parent.width; height: Settings.us(32)
                        Rectangle {
                            anchors.centerIn: parent
                            width: Settings.us(70); height: Settings.us(28); radius: 8
                            color: okMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
                            border.color: Settings.accent(0.6); border.width: 1.5
                            Behavior on color { ColorAnimation { duration: 120 } }
                            Text {
                                anchors.centerIn: parent; text: Settings.safeTr("ok")
                                color: "white"; font.pixelSize: Settings.fs(13); font.bold: true
                            }
                            MouseArea {
                                id: okMA; anchors.fill: parent; hoverEnabled: true
                                onClicked: resultPopup.visible = false
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: NetworkListener && NetworkListener.logSender ? NetworkListener.logSender : null
        function onFinished(success, message) {
            resultPopup.success = success
            resultPopup.message = message
            resultPopup.visible = true
        }
    }
}
