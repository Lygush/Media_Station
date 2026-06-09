// InstallRequestDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// InstallRequestDialog — диалог подтверждения сетевой установки игры.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import QtQuick.Controls
import AppSettings 1.0
import "../base"

Item {
    id: dialog

    anchors.fill: parent
    z: 500; visible: false

    property string tileTitle:  ""
    property string tileKind:   ""
    property string exePath:    ""
    property string iconPath:   ""
    property var    socket:     null

    property string targetStorageKey: "games"
    property bool   targetIsFolder:   false
    property string targetTitle:      "Games"

    signal accepted()
    signal rejected()

    ListModel { id: treeModel }

    function open(params) {
        tileTitle = params.title    || ""
        tileKind  = params.kind     || ""
        exePath   = params.exePath  || ""
        iconPath  = params.iconPath || ""
        socket    = params.socket

        _buildTree()

        if (treeModel.count > 0) {
            var first        = treeModel.get(0)
            targetStorageKey = first.storageKey
            targetIsFolder   = first.isFolder
            targetTitle      = first.label
        }

        visible = true
        base.open()
    }

    function close() { base.close() }

    function _buildTree() {
        treeModel.clear()
        var screens = TileManager.listScreens()
        for (var i = 0; i < screens.length; i++) {
            var s = screens[i]
            _addNode(s.storageKey, s.isFolder, s.title, 0)
        }
    }

    function _addNode(storageKey, isFolder, label, depth) {
        var children = TileManager.listFolderTiles(storageKey, isFolder)
        treeModel.append({
            storageKey:  storageKey,
            isFolder:    isFolder,
            label:       label,
            depth:       depth,
            hasChildren: children.length > 0,
            expanded:    false
        })
    }

    function _toggleExpand(idx) {
        var node = treeModel.get(idx)
        if (!node || !node.hasChildren) return

        if (node.expanded) {
            treeModel.setProperty(idx, "expanded", false)
            var depth = node.depth
            while (idx + 1 < treeModel.count && treeModel.get(idx + 1).depth > depth)
                treeModel.remove(idx + 1)
        } else {
            treeModel.setProperty(idx, "expanded", true)
            var folders = TileManager.listFolderTiles(node.storageKey, node.isFolder)
            for (var i = folders.length - 1; i >= 0; i--) {
                var f         = folders[i]
                var fChildren = TileManager.listFolderTiles(f.id, true)
                treeModel.insert(idx + 1, {
                    storageKey:  f.id,
                    isFolder:    true,
                    label:       (f.title !== undefined && f.title !== "") ? f.title : f.id,
                    depth:       node.depth + 1,
                    hasChildren: fChildren.length > 0,
                    expanded:    false
                })
            }
        }
    }

    BaseDialog {
        id: base
        dialogWidth:  Settings.us(620)
        dialogHeight: Settings.us(560)
        closeOnClickOutside: false
        onVisibleChanged: if (!visible) dialog.visible = false

        Column {
            width: parent.width; spacing: Settings.us(14)

            // ── Заголовок ──────────────────────────────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: 10
                Image {
                    source: "qrc:/assets/icons/ic_download.png"
                    width: 28; height: 28
                    sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: Settings.tr("install_request")
                    color: "white"; font.pixelSize: Settings.fs(22); font.bold: true
                }
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.267) }

            // ── Описание ──────────────────────────────────────────────────────
            Text {
                width: parent.width
                text: Settings.tr("server_wants_install", { title: dialog.tileTitle })
                color: "white"; font.pixelSize: Settings.fs(19)
                horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
            }

            // ── Браузер папок ─────────────────────────────────────────────────
            Column {
                width: parent.width; spacing: 6

                Text {
                    text: Settings.tr("select_destination")
                    color: Settings.accent(1.0); font.pixelSize: Settings.fs(15); font.bold: true
                }

                NavTreeView {
                    width:  parent.width
                    height: Settings.us(210)
                    model:            treeModel
                    selectedKey:      dialog.targetStorageKey
                    selectedIsFolder: dialog.targetIsFolder
                    onNodeSelected: function(storageKey, isFolder, label) {
                        dialog.targetStorageKey = storageKey
                        dialog.targetIsFolder   = isFolder
                        dialog.targetTitle      = label
                    }
                    onNodeToggled: function(index) { dialog._toggleExpand(index) }
                }

                // Текущее место назначения
                Row {
                    spacing: 6
                    Text { text: Settings.tr("destination"); color: Settings.accent(0.7); font.pixelSize: Settings.fs(14); anchors.verticalCenter: parent.verticalCenter }
                    Text { text: dialog.targetTitle; color: "white"; font.pixelSize: Settings.fs(14); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
            }

            Item { height: Settings.us(4); width: 1 }

            // ── Кнопки (используем StyledButton) ───────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: Settings.us(20)

                StyledButton {
                    text: Settings.tr("cancel")
                    width: Settings.us(160); height: Settings.us(52)
                    onClicked: {
                        if (dialog.socket) NetworkListener.rejectInstall(dialog.socket)
                        dialog.rejected()
                        dialog.close()
                    }
                }
                StyledButton {
                    text: Settings.tr("install")
                    role: "primary"
                    width: Settings.us(160); height: Settings.us(52)
                    onClicked: { dialog.accepted(); dialog.close() }
                }
            }
        }
    }
}
