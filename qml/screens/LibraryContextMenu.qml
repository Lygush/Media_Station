// screens/LibraryContextMenu.qml
// ─────────────────────────────────────────────────────────────────────────────
// Контекстное меню строки в GameLibraryScreen.
//
// Свойства (out):
//   activeGameId, activeName, activeType, activeAvail, tileCount, activeLegacy
//
// Сигналы:
//   tileActionRequested(gameId, gameName, tileCount)
//   extractRequested(gameId, gameName, rowIndex)
//   archiveRequested(gameId, index)
//   openFolderRequested(path)
//   reassignRequested(gameId)
//   deleteRequested(gameId, index)
//   regenerateMetadataRequested(gameId)
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 200

    property int    activeIndex:  -1
    property string activeGameId: ""
    property string activeName:   ""
    property string activeType:   ""
    property bool   activeAvail:  true
    property int    tileCount:    0
    property bool   activeLegacy: false

    signal tileActionRequested(string gameId, string gameName, int tileCount)
    signal extractRequested(string gameId, string gameName, int rowIndex)
    signal archiveRequested(string gameId, int rowIndex)
    signal openFolderRequested(string path, string type, string installPath, string archivePath)
    signal reassignRequested(string gameId)
    signal deleteRequested(string gameId, int rowIndex)
    signal regenerateMetadataRequested(string gameId)

    function openForRow(idx, gx, gy, entry) {
        activeIndex  = idx
        activeGameId = entry.gameId
        activeName   = entry.name
        activeType   = entry.type
        activeAvail  = entry.available
        tileCount    = entry.tileCount
        activeLegacy = entry.legacyNoMetadata

        // Размер меню зависит от видимых пунктов — считаем после их обновления
        var mw = menuBox.width
        var mh = menuBox.height
        var m  = Settings.us(8)
        menuBox.x = Math.max(m, Math.min(gx, root.width  - mw - m))
        menuBox.y = Math.max(m, Math.min(gy, root.height - mh - m))

        visible = true
        openMenuAnim.start()
    }

    function close() { closeMenuAnim.start() }

    MouseArea { anchors.fill: parent; z: -1; onClicked: root.close() }

    SequentialAnimation {
        id: openMenuAnim
        PropertyAction { target: menuBox; property: "opacity"; value: 0 }
        PropertyAction { target: menuBox; property: "scale";   value: 0.88 }
        ParallelAnimation {
            NumberAnimation { target: menuBox; property: "opacity"; from: 0;    to: 1.0; duration: 180; easing.type: Easing.OutCubic }
            NumberAnimation { target: menuBox; property: "scale";   from: 0.88; to: 1.0; duration: 200; easing.type: Easing.OutBack; easing.overshoot: 0.8 }
        }
    }
    SequentialAnimation {
        id: closeMenuAnim
        ParallelAnimation {
            NumberAnimation { target: menuBox; property: "opacity"; from: 1.0; to: 0;    duration: 150; easing.type: Easing.InCubic }
            NumberAnimation { target: menuBox; property: "scale";   from: 1.0; to: 0.88; duration: 160; easing.type: Easing.InBack }
        }
        PropertyAction { target: root; property: "visible"; value: false }
    }

    Rectangle {
        id: menuBox
        // Ширина — по самому длинному видимому пункту (раньше 260 при любом
        // кегле, «Управление ярлыками» упиралось в рамку)
        width: {
            var w = Settings.us(220)
            for (var i = 0; i < menuCol.children.length; ++i) {
                var c = menuCol.children[i]
                if (c.visible && c.neededWidth !== undefined)
                    w = Math.max(w, c.neededWidth)
            }
            return Math.min(Math.ceil(w), root.width - Settings.us(16))
        }
        height: menuCol.implicitHeight + Settings.us(16)
        radius: 12; transformOrigin: Item.TopLeft
        layer.enabled: true
        layer.effect: DropShadow { cached: true;  radius: 20; samples: 28; color: "#AA000000"; verticalOffset: 6 }
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#FF0D3060" }
            GradientStop { position: 1.0; color: "#FF0A2248" }
        }
        border.color: Settings.accent(0.667); border.width: 1.5

        Column {
            id: menuCol
            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Settings.us(8) }
            spacing: 0

            Text {
                readonly property real neededWidth: Math.min(implicitWidth, Settings.us(420))
                width: parent.width; text: root.activeName
                color: Settings.accent(1.0); font.pixelSize: Settings.fs(13); font.bold: true
                horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                topPadding: Settings.us(4); bottomPadding: Settings.us(8)
                leftPadding: Settings.us(12); rightPadding: Settings.us(12)
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.2) }

            ContextMenuItem {
                visible: root.activeType !== "archive"
                iconSource: "qrc:/assets/icons/ic_label.png"
                label: root.tileCount > 0
                       ? Settings.tr("lib_menu_manage_tiles")
                       : Settings.tr("lib_menu_create_tile")
                onClicked: {
                    root.close()
                    root.tileActionRequested(root.activeGameId, root.activeName, root.tileCount)
                }
            }

            ContextMenuItem {
                visible: root.activeLegacy
                iconSource: "qrc:/assets/icons/ic_label.png"
                label: Settings.tr("lib_menu_regenerate_metadata")
                onClicked: {
                    root.close()
                    root.regenerateMetadataRequested(root.activeGameId)
                }
            }

            Rectangle {
                visible: root.activeType !== "archive" || root.activeLegacy
                width: parent.width; height: 1; color: Settings.accent(0.1)
            }

            ContextMenuItem {
                visible: root.activeType === "installed"
                iconSource: "qrc:/assets/icons/ic_folder.png"
                label: Settings.tr("lib_menu_archive")
                onClicked: { root.close(); root.archiveRequested(root.activeGameId, root.activeIndex) }
            }

            ContextMenuItem {
                visible: root.activeType === "archive"
                iconSource: "qrc:/assets/icons/ic_folder.png"
                label: Settings.tr("lib_menu_extract")
                onClicked: {
                    root.close()
                    root.extractRequested(root.activeGameId, root.activeName, root.activeIndex)
                }
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.1) }

            ContextMenuItem {
                iconSource: "qrc:/assets/icons/ic_folder.png"
                label:    Settings.tr("lib_menu_open_folder")
                enabled:  root.activeAvail
                onClicked: {
                    root.close()
                    root.openFolderRequested(root.activeGameId, root.activeType, "", "")
                }
            }

            ContextMenuItem {
                visible: !root.activeAvail && root.activeType !== "archive"
                iconSource: "qrc:/assets/icons/ic_folder.png"
                label: Settings.tr("lib_menu_reassign")
                onClicked: { root.close(); root.reassignRequested(root.activeGameId) }
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.1) }

            ContextMenuItem {
                iconSource: "qrc:/assets/icons/ic_error.png"
                label:      Settings.tr("lib_menu_delete")
                labelColor: "#EF5350"
                onClicked:  { root.close(); root.deleteRequested(root.activeGameId, root.activeIndex) }
            }

            Item { height: Settings.us(8); width: 1 }
        }
    }

    // Inline-компонент пункта меню
    component ContextMenuItem: Item {
        property string iconSource: ""
        property string label:      ""
        property color  labelColor: "white"
        property bool   enabled:    true
        signal clicked()

        width: parent ? parent.width : 0
        height: Math.max(Settings.us(44), Math.ceil(miRow.implicitHeight) + Settings.us(16))
        opacity: enabled ? 1.0 : 0.45
        // Отступ слева us(14) + содержимое + отступ справа us(20)
        readonly property real neededWidth: Settings.us(14) + miRow.implicitWidth + Settings.us(20)

        Rectangle {
            anchors.fill: parent
            color: miMA.containsMouse && parent.enabled
                   ? Settings.accent(0.2) : Settings.accent(0.0)
            Behavior on color { ColorAnimation { duration: 100 } }
        }
        Row {
            id: miRow
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: Settings.us(14)
            spacing: Settings.us(10)
            Image {
                source: iconSource; width: Settings.us(18); height: Settings.us(18)
                sourceSize.width: 60; sourceSize.height: 60
                fillMode: Image.PreserveAspectFit; anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: label; color: labelColor; font.pixelSize: Settings.fs(14)
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            id: miMA; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled
            onClicked: parent.clicked()
        }
    }
}
