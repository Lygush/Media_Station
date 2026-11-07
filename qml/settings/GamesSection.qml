// settings/GamesSection.qml
// Папка установки игр и папка архивов — отдельно (можно на разных дисках).
import QtQuick
import QtQuick.Controls.Basic 2.15
import QtQuick.Dialogs
import AppSettings 1.0
import "../base"

Column {
    spacing: 0

    // Квадратная кнопка с иконкой (обзор, сброс)
    component IconButton: Item {
        id: btn
        property string icon: ""
        signal clicked()
        width: Settings.us(36); height: Settings.us(36)
        Rectangle {
            anchors.fill: parent; radius: 9
            color: btnMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
            border.color: Settings.accent(0.4); border.width: 1
            Behavior on color { ColorAnimation { duration: 120 } }
        }
        Image {
            anchors.centerIn: parent
            source: btn.icon
            width: Settings.us(18); height: Settings.us(18)
            sourceSize.width: 100; sourceSize.height: 100
            fillMode: Image.PreserveAspectFit
        }
        MouseArea {
            id: btnMA; anchors.fill: parent; hoverEnabled: true
            onClicked: btn.clicked()
        }
    }

    // Строка «путь + обзор (+ сброс)». value — текущий путь, placeholder —
    // подсказка для пустого значения, chosen(path) — выбран новый путь.
    component FolderRow: SettingsRow {
        id: row
        property string value: ""
        property string placeholder: ""
        property bool   allowReset: false     // пустое значение допустимо (кнопка сброса)
        signal chosen(string path)

        // Свойством, а не дочерним элементом: содержимое SettingsRow уходит в
        // Row.children, куда невизуальный FolderDialog не положить
        property FolderDialog folderDialog: FolderDialog {
            title: row.label
            currentFolder: (row.value && row.value !== "")
                           ? "file:///" + row.value.replace(/\\/g, "/") : "file:///C:/"
            onAccepted: {
                var path = selectedFolder.toString().replace(/^file:\/\/\//, "")
                if (path.length > 0 && path[1] !== ":") path = "/" + path
                row.chosen(path)
            }
        }

        Row {
            spacing: Settings.us(8)

            TextField {
                id: pathField
                width: Settings.us(280); height: Settings.us(36)
                color: "white"; font.pixelSize: Settings.fs(13)
                text: row.value
                placeholderText:      row.placeholder
                placeholderTextColor: Settings.accent(0.4)
                verticalAlignment:    Text.AlignVCenter
                leftPadding:  Settings.us(10); rightPadding: Settings.us(10)
                onEditingFinished: {
                    var t = text.trim()
                    if (t !== "" || row.allowReset) row.chosen(t)
                }
                background: Rectangle {
                    radius: 9; clip: true
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Settings.accent(0.13) }
                        GradientStop { position: 1.0; color: Settings.accent(0.08) }
                    }
                    Rectangle {
                        anchors.fill: parent; radius: 9; color: "transparent"
                        border.color: pathField.activeFocus
                                      ? Settings.accent(0.9) : Settings.accent(0.33)
                        border.width: 1.5
                        Behavior on border.color { ColorAnimation { duration: 150 } }
                    }
                }
            }

            IconButton {
                icon: "qrc:/assets/icons/ic_folder.png"
                onClicked: row.folderDialog.open()
            }
            IconButton {
                visible: row.allowReset && row.value !== ""
                icon: "qrc:/assets/icons/ic_delete.png"
                onClicked: row.chosen("")
            }
        }
    }

    FolderRow {
        label: Settings.safeTr("install_path")
        value: Settings.installPath
        placeholder: "C:/Games"
        onChosen: function(path) { if (path !== "") Settings.installPath = path }
    }

    // Архивы — отдельно: можно держать на другом диске, тогда архивации не
    // нужно двойное место на диске с игрой. Пусто — как раньше, внутри папки
    // игры (<игра>/archive/).
    FolderRow {
        label: Settings.safeTr("archive_path")
        value: Settings.archivePath
        placeholder: Settings.safeTr("archive_path_default")
        allowReset: true
        onChosen: function(path) { Settings.archivePath = path }
    }
}
