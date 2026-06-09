// NavTreeView.qml
// ─────────────────────────────────────────────────────────────────────────────
// Дерево экранов и папок для выбора места назначения плитки.
// Устраняет дублирование между InstallRequestDialog, GameLibraryScreen
// (TileLocationDialog) и любыми будущими диалогами выбора локации.
//
// Использование:
//   NavTreeView {
//       id: treeView
//       model: treeModel          // ListModel с полями ниже
//       selectedKey: dialog.targetStorageKey
//       selectedIsFolder: dialog.targetIsFolder
//       onNodeSelected: function(storageKey, isFolder, label) {
//           dialog.targetStorageKey = storageKey
//           dialog.targetIsFolder   = isFolder
//           dialog.targetTitle      = label
//       }
//       onNodeToggled: function(index) { _toggleExpand(index) }
//   }
//
// Ожидаемые поля ListModel:
//   storageKey  : string  — уникальный ключ экрана/папки
//   isFolder    : bool    — это папка или корневой экран
//   label       : string  — отображаемое название
//   depth       : int     — уровень вложенности (0 = корень)
//   hasChildren : bool    — есть ли дочерние элементы
//   expanded    : bool    — развёрнут ли узел
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import AppSettings 1.0

Rectangle {
    id: root

    // ── Публичные свойства ───────────────────────────────────────────────────
    property alias  model:           listView.model
    property string selectedKey:     ""
    property bool   selectedIsFolder: false

    // ── Сигналы ──────────────────────────────────────────────────────────────
    signal nodeSelected(string storageKey, bool isFolder, string label)
    signal nodeToggled(int index)

    // ── Внешний вид ──────────────────────────────────────────────────────────
    radius:       10
    color:        Settings.accent(0.08)
    border.color: Settings.accent(0.25)
    border.width: 1
    clip:         true

    // ── Список ───────────────────────────────────────────────────────────────
    ListView {
        id: listView
        anchors { fill: parent; margins: 4 }
        spacing: 2
        clip: true

        delegate: Item {
            id: row
            width:  listView.width
            height: Settings.us(40)

            readonly property bool isSelected:
                model.storageKey === root.selectedKey &&
                model.isFolder   === root.selectedIsFolder

            // Фон строки
            Rectangle {
                anchors.fill: parent
                radius: 7
                color: row.isSelected
                       ? Settings.accent(0.45)
                       : rowMA.containsMouse ? Settings.accent(0.2) : "transparent"
                Behavior on color { ColorAnimation { duration: 100 } }
            }

            // Клик по строке — выбор
            MouseArea {
                id: rowMA
                anchors.fill: parent
                hoverEnabled: true
                z: 0
                onClicked: root.nodeSelected(model.storageKey, model.isFolder, model.label)
            }

            // Содержимое строки
            Row {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left:  parent.left
                anchors.leftMargin: 8 + model.depth * 20
                spacing: 6

                // Кнопка раскрытия (только если есть дети)
                Item {
                    width: 20; height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    visible: model.hasChildren
                    z: 1

                    Text {
                        anchors.centerIn: parent
                        text: model.expanded ? "▾" : "▸"
                        color: Settings.accent(0.8)
                        font.pixelSize: Settings.fs(13)
                    }

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
                        z: 1
                        onClicked: function(mouse) {
                            mouse.accepted = true
                            root.nodeToggled(index)
                        }
                    }
                }

                // Заглушка по ширине если нет кнопки раскрытия
                Item {
                    width: 20; height: 20
                    visible: !model.hasChildren
                }

                // Иконка
                Image {
                    source: model.isFolder
                            ? "qrc:/assets/icons/ic_folder.png"
                            : "qrc:/assets/icons/ic_label.png"
                    width: 20; height: 20
                    sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit
                    anchors.verticalCenter: parent.verticalCenter
                }

                // Метка
                Text {
                    text: model.label
                    color: "white"
                    font.pixelSize: Settings.fs(15)
                    font.bold: row.isSelected
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    width: listView.width
                            - 8 - model.depth * 20
                            - 20 - 6   // toggle btn
                            - 20 - 6   // icon
                            - 16       // right dot + margin
                }
            }

            // Точка выбора справа
            Rectangle {
                anchors.right:         parent.right
                anchors.rightMargin:   8
                anchors.verticalCenter: parent.verticalCenter
                width: 8; height: 8; radius: 4
                color: Settings.accent(1.0)
                visible: row.isSelected
            }
        }

        // Пустой список
        Text {
            anchors.centerIn: parent
            text: Settings.tr("no_screens")
            color: Settings.accent(0.4)
            font.pixelSize: Settings.fs(14)
            visible: listView.count === 0
        }
    }
}
