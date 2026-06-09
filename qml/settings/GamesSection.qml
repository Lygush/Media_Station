// settings/GamesSection.qml
import QtQuick
import QtQuick.Controls.Basic 2.15
import QtQuick.Dialogs
import AppSettings 1.0
import "../base"

Column {
    spacing: 0

    FolderDialog {
        id: installFolderDialog
        title: Settings.safeTr("install_path")
        currentFolder: {
            var p = Settings.installPath
            return (p && p !== "") ? "file:///" + p.replace(/\\/g, "/") : "file:///C:/"
        }
        onAccepted: {
            var path = selectedFolder.toString().replace(/^file:\/\/\//, "")
            if (path.length > 0 && path[1] !== ":") path = "/" + path
            Settings.installPath = path
        }
    }

    SettingsRow {
        label: Settings.safeTr("install_path")
        Row {
            spacing: Settings.us(8)

            TextField {
                id: installPathField
                width: Settings.us(280); height: Settings.us(36)
                color: "white"; font.pixelSize: Settings.fs(13)
                text: Settings.installPath
                placeholderText:      "C:/Games"
                placeholderTextColor: Settings.accent(0.4)
                verticalAlignment:    Text.AlignVCenter
                leftPadding:  Settings.us(10); rightPadding: Settings.us(10)
                onEditingFinished: if (text.trim() !== "") Settings.installPath = text.trim()
                background: Rectangle {
                    radius: 9; clip: true
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Settings.accent(0.13) }
                        GradientStop { position: 1.0; color: Settings.accent(0.08) }
                    }
                    Rectangle {
                        anchors.fill: parent; radius: 9; color: "transparent"
                        border.color: installPathField.activeFocus
                                      ? Settings.accent(0.9) : Settings.accent(0.33)
                        border.width: 1.5
                        Behavior on border.color { ColorAnimation { duration: 150 } }
                    }
                }
            }

            Item {
                width: Settings.us(36); height: Settings.us(36)
                Rectangle {
                    anchors.fill: parent; radius: 9
                    color: browseMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
                    border.color: Settings.accent(0.4); border.width: 1
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Image {
                    anchors.centerIn: parent
                    source: "qrc:/assets/icons/ic_folder.png"
                    width: Settings.us(18); height: Settings.us(18)
                    sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit
                }
                MouseArea {
                    id: browseMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: installFolderDialog.open()
                }
            }
        }
    }
}
