// settings/ImagePickerPopup.qml
// ─────────────────────────────────────────────────────────────────────────────
// Попап «Добавить профиль»: поле имени + список найденных образов + Browse.
//
// Сигналы:
//   profileAdded(path, name)  — пользователь выбрал образ и нажал «Добавить»
//   browseRequested()         — нужен нативный FileDialog (открывает вызывающий)
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Popup {
    id: root

    property string selectedImage: ""

    signal profileAdded(string imagePath, string profileName)
    signal browseRequested()

    width:  Settings.us(480)
    height: Settings.us(380)
    anchors.centerIn: parent
    modal: true
    padding: Settings.us(18)

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 200; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale";   from: 0.93; to: 1.0; duration: 200; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 150; easing.type: Easing.InCubic }
        NumberAnimation { property: "scale";   from: 1.0; to: 0.93; duration: 150; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: "#F0051528"; radius: 16
        border.color: Settings.accent(0.6); border.width: 1.5
        layer.enabled: true
        layer.effect: DropShadow {
            cached: true
            radius: 28; samples: 40; color: "#CC000000"; verticalOffset: 8
        }
    }

    contentItem: Item {

        Text {
            id: popupTitle
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            text:  Settings.safeTr("add_profile")
            color: "white"; font.pixelSize: Settings.fs(16); font.bold: true
        }

        Rectangle {
            id: popupDivider
            anchors.top: popupTitle.bottom; anchors.topMargin: Settings.us(10)
            width: parent.width; height: 1
            color: Settings.accent(0.25)
        }

        Item {
            id: nameInputArea
            anchors.top: popupDivider.bottom; anchors.topMargin: Settings.us(10)
            anchors.left: parent.left; anchors.right: parent.right
            height: Settings.us(40)

            Text {
                id: nameLabel
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                text:  Settings.safeTr("profile_name") + ":"
                color: "white"; font.pixelSize: Settings.fs(13)
            }

            TextField {
                id: profileNameField
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: nameLabel.right; anchors.leftMargin: Settings.us(10)
                anchors.right: parent.right
                height: Settings.us(34)
                placeholderText:      Settings.safeTr("new_profile")
                placeholderTextColor: Settings.accent(0.4)
                color: "white"; font.pixelSize: Settings.fs(13)
                leftPadding: Settings.us(8); rightPadding: Settings.us(8)
                verticalAlignment: Text.AlignVCenter
                background: Rectangle {
                    radius: 8; color: Settings.accent(0.1)
                    border.color: profileNameField.activeFocus
                                  ? Settings.accent(0.8) : Settings.accent(0.35)
                    border.width: 1.5
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                }
            }
        }

        Rectangle {
            id: nameDivider
            anchors.top: nameInputArea.bottom; anchors.topMargin: Settings.us(8)
            width: parent.width; height: 1
            color: Settings.accent(0.15)
        }

        Text {
            id: imageListTitle
            anchors.top: nameDivider.bottom; anchors.topMargin: Settings.us(8)
            text:  Settings.safeTr("select_system_image")
            color: Settings.accent(0.7); font.pixelSize: Settings.fs(12)
        }

        property var scannedImages: AndroidTV && AndroidTV.profileManager
                                    ? AndroidTV.profileManager.scanSystemImages() : []

        Item {
            anchors.top:          imageListTitle.bottom; anchors.topMargin: Settings.us(6)
            anchors.left:         parent.left; anchors.right: parent.right
            anchors.bottom:       popupButtons.top; anchors.bottomMargin: Settings.us(8)

            Column {
                visible: parent.parent.scannedImages.length === 0
                anchors.centerIn: parent
                spacing: Settings.us(10)
                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/assets/icons/ic_folder.png"
                    width: Settings.us(40); height: width
                    sourceSize.width: 128; sourceSize.height: 128
                    opacity: 0.7
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Settings.safeTr("no_images_found"); color: Settings.accent(0.7)
                    font.pixelSize: Settings.fs(13); horizontalAlignment: Text.AlignHCenter
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: Settings.safeTr("select_manually"); color: Settings.accent(0.45)
                    font.pixelSize: Settings.fs(11); horizontalAlignment: Text.AlignHCenter
                }
            }

            ListView {
                anchors.fill: parent
                visible: parent.parent.scannedImages.length > 0
                clip: true
                model: parent.parent.scannedImages
                spacing: Settings.us(4)
                delegate: Item {
                    width: ListView.view.width; height: Settings.us(36)
                    Rectangle {
                        anchors.fill: parent; radius: 8
                        color: parent.isSelected ? Settings.accent(0.45)
                               : (imgMouse.containsMouse ? Settings.accent(0.3) : Settings.accent(0.08))
                        border.color: parent.isSelected ? Settings.accent(1.0)
                                      : (imgMouse.containsMouse ? Settings.accent(0.6) : Settings.accent(0.2))
                        border.width: parent.isSelected ? 2 : 1
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text {
                        text: {
                            var parts = modelData.split("/")
                            return parts.slice(Math.max(0, parts.length - 3)).join(" / ")
                        }
                        color: "white"; font.pixelSize: Settings.fs(12)
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left; anchors.leftMargin: Settings.us(10)
                        elide: Text.ElideLeft; width: parent.width - Settings.us(20)
                    }
                    ToolTip { visible: imgMouse.containsMouse; text: modelData; delay: 400 }
                    property bool isSelected: root.selectedImage === modelData
                    MouseArea {
                        id: imgMouse; anchors.fill: parent; hoverEnabled: true
                        onClicked: root.selectedImage = modelData
                    }
                }
            }
        }

        Row {
            id: popupButtons
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Settings.us(10)

            // Добавить
            Item {
                width: Settings.us(130); height: Settings.us(34)
                enabled: root.selectedImage !== ""
                opacity: enabled ? 1.0 : 0.4
                Behavior on opacity { NumberAnimation { duration: 150 } }
                Rectangle {
                    anchors.fill: parent; radius: 8
                    color: addImgMA.containsMouse && parent.enabled
                           ? Settings.accent(0.7) : Settings.accent(0.5)
                    border.color: Settings.accent(0.9); border.width: 1.5
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Text {
                    anchors.centerIn: parent
                    text: Settings.safeTr("add"); color: "white"
                    font.pixelSize: Settings.fs(13); font.bold: true
                }
                MouseArea {
                    id: addImgMA; anchors.fill: parent; hoverEnabled: true
                    enabled: parent.enabled
                    onClicked: {
                        root.profileAdded(root.selectedImage, profileNameField.text.trim())
                        root.selectedImage = ""
                        root.close()
                    }
                }
            }

            // Обзор
            Item {
                width: Settings.us(130); height: Settings.us(34)
                Rectangle {
                    anchors.fill: parent; radius: 8
                    color: browseImgMA.containsMouse ? Settings.accent(0.45) : Settings.accent(0.22)
                    border.color: Settings.accent(0.6); border.width: 1.5
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Row {
                    anchors.centerIn: parent; spacing: Settings.us(6)
                    Image {
                        source: "qrc:/assets/icons/ic_folder.png"
                        width: Settings.us(16); height: width
                        sourceSize.width: 64; sourceSize.height: 64
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text { text: Settings.safeTr("browse"); color: "white"; font.pixelSize: Settings.fs(13) }
                }
                MouseArea {
                    id: browseImgMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: { root.close(); root.browseRequested() }
                }
            }

            // Отмена
            Item {
                width: Settings.us(110); height: Settings.us(34)
                Rectangle {
                    anchors.fill: parent; radius: 8
                    color: cancelImgMA.containsMouse ? Settings.accent(0.3) : Settings.accent(0.12)
                    border.color: Settings.accent(0.4); border.width: 1.5
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Text {
                    anchors.centerIn: parent; text: Settings.safeTr("cancel")
                    color: "white"; font.pixelSize: Settings.fs(13)
                }
                MouseArea {
                    id: cancelImgMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: root.close()
                }
            }
        }
    }
}
