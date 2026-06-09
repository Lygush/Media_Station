// dialogs/AppShortcutForm.qml
// ─────────────────────────────────────────────────────────────────────────────
// Форма app-ярлыка: папка игры → сканирование exe → выбор → имя → чекбокс.
//
// Свойства (in):
//   isNew             — создание нового ярлыка
//   gameDirPath       — корневая папка (двустороннее через сигналы)
//   exeList           — [{name, relative}]
//   selectedExe       — выбранный относительный путь (двустороннее)
//   exeScanning       — идёт сканирование
//   installChecked    — чекбокс «установить» (двустороннее)
//   nameText          — название ярлыка (двустороннее через alias)
//
// Сигналы:
//   gameFolderRequested()            — нажата кнопка «Обзор» папки
//   exeSelected(relative)            — пользователь выбрал exe
//   installCheckToggled(newVal)      — переключён чекбокс
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import AppSettings 1.0
import "../base"

Column {
    id: root
    spacing: Settings.us(14)

    property bool   isNew:          true
    property string gameDirPath:    ""
    property var    exeList:        []
    property string selectedExe:    ""
    property bool   exeScanning:    false
    property bool   installChecked: true
    property alias  nameText:       nameFieldApp.text

    signal gameFolderRequested()
    signal exeSelected(string relative)
    signal installCheckToggled(bool newVal)

    // ── Корневая папка (только при создании) ──────────────────────────────────
    Column {
        width: parent.width; spacing: 4
        visible: root.isNew

        Text { width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14); text: Settings.safeTr("game_folder") }

        Item {
            width: parent.width; height: Settings.us(46)

            Rectangle {
                anchors.fill: parent; radius: 10; clip: true
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Settings.accent(0.133) }
                    GradientStop { position: 0.5; color: Settings.accent(0.082) }
                    GradientStop { position: 1.0; color: Settings.accent(0.133) }
                }
                border.color: Settings.accent(0.333); border.width: 1.5
            }
            Text {
                anchors { fill: parent; leftMargin: 12; rightMargin: 50 }
                verticalAlignment: Text.AlignVCenter
                text:  root.gameDirPath !== "" ? root.gameDirPath : Settings.safeTr("game_folder_ph")
                color: root.gameDirPath !== "" ? "white" : Settings.accent(0.333)
                font.pixelSize: Settings.fs(15); elide: Text.ElideLeft
            }
            Item {
                width: 34; height: 34
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    anchors.fill: parent; radius: 7
                    color: browseDirMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Image { anchors.centerIn: parent; source: "qrc:/assets/icons/ic_folder.png"; sourceSize.width: 18; sourceSize.height: 18 }
                MouseArea {
                    id: browseDirMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: root.gameFolderRequested()
                }
            }
        }
    }

    // ── Выбор exe ─────────────────────────────────────────────────────────────
    Column {
        width: parent.width; spacing: 4

        Text { width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14); text: Settings.safeTr("exe_file") }

        Item {
            width: parent.width; height: Settings.us(46)

            Rectangle {
                anchors.fill: parent; radius: 10; clip: true
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Settings.accent(0.133) }
                    GradientStop { position: 0.5; color: Settings.accent(0.082) }
                    GradientStop { position: 1.0; color: Settings.accent(0.133) }
                }
                border.color: exeCombo.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                border.width: 1.5
                Behavior on border.color { ColorAnimation { duration: 150 } }
            }

            // Спиннер сканирования
            Row {
                anchors.centerIn: parent; spacing: 10; visible: root.exeScanning
                Spinner { running: root.exeScanning; size: 20; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: Settings.safeTr("exe_scanning"); color: Settings.accent(0.7)
                    font.pixelSize: Settings.fs(14)
                }
            }

            ComboBox {
                id: exeCombo
                anchors.fill: parent
                visible: !root.exeScanning
                enabled: root.exeList.length > 0
                opacity: root.exeList.length > 0 ? 1.0 : 0.35
                Behavior on opacity { NumberAnimation { duration: 200 } }

                model: root.exeList.map(function(e) {
                    return (e.relative.indexOf("/") !== -1 || e.relative.indexOf("\\") !== -1)
                           ? e.name + "  ·  " + e.relative
                           : e.name
                })

                onModelChanged: {
                    if (root.exeList.length === 0) { currentIndex = -1; return }
                    var target = root.selectedExe.replace(/\\/g, "/")
                    for (var i = 0; i < root.exeList.length; i++) {
                        if (root.exeList[i].relative.replace(/\\/g, "/") === target) {
                            currentIndex = i; return
                        }
                    }
                    currentIndex = root.isNew ? 0 : -1
                }

                onCurrentIndexChanged: {
                    if (currentIndex < 0 || currentIndex >= root.exeList.length) return
                    root.exeSelected(root.exeList[currentIndex].relative.replace(/\\/g, "/"))
                }

                contentItem: Text {
                    leftPadding: 12; rightPadding: 28
                    text:  exeCombo.currentIndex >= 0 ? exeCombo.displayText : Settings.safeTr("exe_select_ph")
                    color: exeCombo.currentIndex >= 0 ? "white" : Settings.accent(0.333)
                    font.pixelSize: Settings.fs(15); verticalAlignment: Text.AlignVCenter; elide: Text.ElideLeft
                }
                indicator: Image {
                    x: exeCombo.width - width - 10; anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/assets/icons/ic_up.png"; sourceSize.width: 16; sourceSize.height: 16
                    rotation: exeCombo.popup.visible ? 0 : 180
                    opacity: root.exeList.length > 0 ? 0.7 : 0.3
                    Behavior on rotation { NumberAnimation { duration: 150 } }
                }
                background: Item {}
                popup: Popup {
                    y: exeCombo.height + 2; width: exeCombo.width; padding: 4
                    background: Rectangle { radius: 10; color: "#F2051528"; border.color: Settings.accent(0.5); border.width: 1.5 }
                    contentItem: ListView {
                        implicitHeight: Math.min(contentHeight, 220)
                        model: exeCombo.popup.visible ? exeCombo.delegateModel : null
                        clip: true
                        ScrollIndicator.vertical: ScrollIndicator {}
                    }
                }
                delegate: ItemDelegate {
                    width: exeCombo.width
                    contentItem: Text {
                        text: modelData; color: "white"; font.pixelSize: Settings.fs(14)
                        elide: Text.ElideLeft; verticalAlignment: Text.AlignVCenter; leftPadding: 10
                    }
                    background: Rectangle { radius: 7; color: highlighted ? Settings.accent(0.3) : "transparent" }
                    highlighted: exeCombo.highlightedIndex === index
                }
            }
        }

        Text {
            visible: !root.exeScanning && root.gameDirPath !== "" && root.exeList.length === 0
            text: Settings.safeTr("no_exe_found"); color: Settings.accent(0.5); font.pixelSize: Settings.fs(13)
        }
    }

    // ── Название ──────────────────────────────────────────────────────────────
    Column {
        width: parent.width; spacing: 4
        visible: root.isNew

        Text { width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14); text: Settings.safeTr("name") }

        TextField {
            id: nameFieldApp
            width: parent.width; height: Settings.us(46)
            verticalAlignment: TextInput.AlignVCenter
            color: "white"; font.pixelSize: Settings.fs(16)
            placeholderText:      Settings.safeTr("shortcut_name_ph")
            placeholderTextColor: Settings.accent(0.333)
            leftPadding: 12; rightPadding: 12
            background: Rectangle {
                anchors.fill: parent; radius: 10; clip: true
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Settings.accent(0.133) }
                    GradientStop { position: 0.5; color: Settings.accent(0.082) }
                    GradientStop { position: 1.0; color: Settings.accent(0.133) }
                }
                Rectangle {
                    anchors.fill: parent; radius: 10; color: "transparent"
                    border.color: nameFieldApp.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                    border.width: 1.5
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                }
            }
        }
    }

    // ── Чекбокс «Установить игру» ─────────────────────────────────────────────
    Item {
        width: parent.width; height: 30
        visible: root.isNew

        Row {
            spacing: 8; anchors.verticalCenter: parent.verticalCenter
            Rectangle {
                width: 20; height: 20; radius: 4
                anchors.verticalCenter: parent.verticalCenter
                border.color: installCheckMA.containsMouse ? Settings.accent(0.8) : Settings.accent(0.4)
                color: root.installChecked ? Settings.accent(0.8) : "transparent"
                Image {
                    anchors.centerIn: parent
                    source: "qrc:/assets/icons/ic_check.png"
                    sourceSize.width: 100; sourceSize.height: 100
                    width: 14; height: 14; visible: root.installChecked
                    fillMode: Image.PreserveAspectFit
                }
                MouseArea {
                    id: installCheckMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: root.installCheckToggled(!root.installChecked)
                }
            }
            Text {
                text: Settings.safeTr("install_game_copy_files")
                anchors.verticalCenter: parent.verticalCenter
                color: "white"; font.pixelSize: Settings.fs(15)
            }
        }
    }
}
