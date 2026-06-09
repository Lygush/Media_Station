// dialogs/WebShortcutForm.qml
// ─────────────────────────────────────────────────────────────────────────────
// Форма URL/пути для web и playstation1 ярлыков.
//
// Свойства:
//   shortcutKind  — "web" | "playstation1"
//   text          — текущее значение поля (двустороннее через alias)
//   fileBrowser   — ссылка на FileBrowserDialog (для browse-кнопки ps1)
//   ownerDialog   — ссылка на EditTileDialog (для onFilePicked)
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import AppSettings 1.0

Column {
    id: root
    spacing: 4

    property string shortcutKind: "web"
    property alias  text:         pathField.text
    property var    fileBrowser:  null
    property var    ownerDialog:  null

    // EditLabel — цветная метка (inline-компонент, определён в EditTileDialog и переиспользован здесь)
    Text {
        width: parent.width
        color: Settings.accent(1.0)
        font.pixelSize: Settings.fs(14)
        text: shortcutKind === "web"
              ? Settings.safeTr("url")
              : Settings.safeTr("exe_path")
    }

    Item {
        width: parent.width; height: Settings.us(46)

        TextField {
            id: pathField
            anchors.fill: parent
            verticalAlignment: TextInput.AlignVCenter
            color: "white"; font.pixelSize: Settings.fs(15)
            placeholderText: shortcutKind === "web"
                             ? Settings.safeTr("url_ph")
                             : Settings.safeTr("exe_ph")
            placeholderTextColor: Settings.accent(0.333)
            leftPadding:  12
            rightPadding: shortcutKind === "playstation1" ? 50 : 12

            background: Rectangle {
                anchors.fill: parent; radius: 10; clip: true
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Settings.accent(0.133) }
                    GradientStop { position: 0.5; color: Settings.accent(0.082) }
                    GradientStop { position: 1.0; color: Settings.accent(0.133) }
                }
                Rectangle {
                    anchors.fill: parent; radius: 10; color: "transparent"
                    border.color: pathField.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                    border.width: 1.5
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                }
            }

            // Кнопка «Обзор» — только для playstation1
            Item {
                width: 34; height: 34
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: root.shortcutKind === "playstation1"

                Rectangle {
                    anchors.fill: parent; radius: 7
                    color: browsePathMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                Image {
                    anchors.centerIn: parent
                    source: "qrc:/assets/icons/ic_folder.png"
                    sourceSize.width: 18; sourceSize.height: 18
                }
                MouseArea {
                    id: browsePathMA; anchors.fill: parent; hoverEnabled: true
                    onClicked: {
                        if (root.fileBrowser)
                            root.fileBrowser.openFor(root.ownerDialog, "C:/", "")
                    }
                }
            }
        }
    }
}
