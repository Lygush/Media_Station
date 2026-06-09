// screens/TileLocationDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// Диалог выбора экрана/папки назначения при создании плитки или извлечении архива.
//
// Публичный API:
//   openForGame(gameId, gameName, tileCount)    — создать/управлять плиткой
//   openForExtract(gameId, gameName, rowIndex)  — извлечь архив
//   close()
//
// Сигналы:
//   tileCreateRequested(gameId, storageKey, isFolder)
//   extractRequested(gameId, rowIndex, storageKey, isFolder)
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import AppSettings 1.0
import "../base"

Item {
    id: root
    anchors.fill: parent
    z: 300
    visible: false

    property string targetGameId:     ""
    property string targetGameName:   ""
    property int    existingTiles:    0
    property string targetStorageKey: "games"
    property bool   targetIsFolder:   false
    property string targetTitle:      "Games"

    property bool _extractMode:  false
    property int  _extractIndex: -1

    signal tileCreateRequested(string gameId, string storageKey, bool isFolder)
    signal extractRequested(string gameId, int rowIndex, string storageKey, bool isFolder)

    function openForGame(gameId, gameName, tileCount) {
        _extractMode   = false
        _extractIndex  = -1
        targetGameId   = gameId
        targetGameName = gameName
        existingTiles  = tileCount
        _buildTree()
        visible = true
        tldBase.open()
    }

    function openForExtract(gameId, gameName, rowIndex) {
        _extractMode   = true
        _extractIndex  = rowIndex
        targetGameId   = gameId
        targetGameName = gameName
        existingTiles  = 0
        _buildTree()
        visible = true
        tldBase.open()
    }

    function close() {
        _extractMode  = false
        _extractIndex = -1
        tldBase.close()
    }

    ListModel { id: tldTreeModel }

    function _buildTree() {
        tldTreeModel.clear()
        var screens = TileManager.listScreens()
        for (var i = 0; i < screens.length; i++) {
            var s = screens[i]
            tldTreeModel.append({
                storageKey:  s.storageKey,
                isFolder:    s.isFolder,
                label:       s.title,
                depth:       0,
                hasChildren: TileManager.listFolderTiles(s.storageKey, s.isFolder).length > 0,
                expanded:    false
            })
        }
    }

    function _toggleExpand(idx) {
        var node = tldTreeModel.get(idx)
        if (!node || !node.hasChildren) return
        if (node.expanded) {
            tldTreeModel.setProperty(idx, "expanded", false)
            var depth = node.depth
            while (idx + 1 < tldTreeModel.count && tldTreeModel.get(idx + 1).depth > depth)
                tldTreeModel.remove(idx + 1)
        } else {
            tldTreeModel.setProperty(idx, "expanded", true)
            var folders = TileManager.listFolderTiles(node.storageKey, node.isFolder)
            for (var i = folders.length - 1; i >= 0; i--) {
                var f = folders[i]
                tldTreeModel.insert(idx + 1, {
                    storageKey:  f.id,
                    isFolder:    true,
                    label:       (f.title && f.title !== "") ? f.title : f.id,
                    depth:       node.depth + 1,
                    hasChildren: TileManager.listFolderTiles(f.id, true).length > 0,
                    expanded:    false
                })
            }
        }
    }

    BaseDialog {
        id: tldBase
        dialogWidth:  Settings.us(620)
        dialogHeight: Settings.us(500)
        closeOnClickOutside: false
        onVisibleChanged: if (!visible) root.visible = false

        Column {
            width: parent.width
            spacing: Settings.us(14)

            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: 10
                Image {
                    source: "qrc:/assets/icons/ic_label.png"
                    width: 28; height: 28; sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit; anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: root._extractMode
                          ? Settings.tr("lib_menu_extract")
                          : (root.existingTiles > 0
                             ? Settings.tr("lib_manage_tiles_title")
                             : Settings.tr("lib_create_tile_title"))
                    color: "white"; font.pixelSize: Settings.fs(22); font.bold: true
                }
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.267) }

            Text {
                width: parent.width
                text: Settings.tr("lib_tile_for", { name: root.targetGameName })
                color: "white"; font.pixelSize: Settings.fs(17)
                horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
            }

            Column {
                width: parent.width; spacing: 6

                Text {
                    text: Settings.tr("select_destination")
                    color: Settings.accent(1.0); font.pixelSize: Settings.fs(15); font.bold: true
                }

                NavTreeView {
                    width:  parent.width
                    height: Settings.us(200)
                    model:            tldTreeModel
                    selectedKey:      root.targetStorageKey
                    selectedIsFolder: root.targetIsFolder
                    onNodeSelected: function(storageKey, isFolder, label) {
                        root.targetStorageKey = storageKey
                        root.targetIsFolder   = isFolder
                        root.targetTitle      = label
                    }
                    onNodeToggled: function(index) { root._toggleExpand(index) }
                }

                Row {
                    spacing: 6
                    Text { text: Settings.tr("destination"); color: Settings.accent(0.7); font.pixelSize: Settings.fs(14); anchors.verticalCenter: parent.verticalCenter }
                    Text { text: root.targetTitle; color: "white"; font.pixelSize: Settings.fs(14); font.bold: true; anchors.verticalCenter: parent.verticalCenter }
                }
            }

            Item { height: Settings.us(4); width: 1 }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Settings.us(20)

                StyledButton {
                    text: Settings.tr("cancel")
                    width: Settings.us(160); height: Settings.us(52)
                    onClicked: root.close()
                }
                StyledButton {
                    text: root._extractMode
                          ? Settings.tr("lib_menu_extract")
                          : Settings.tr("lib_create_tile_btn")
                    role: "primary"
                    width: Settings.us(160); height: Settings.us(52)
                    onClicked: {
                        var gid    = root.targetGameId
                        var sk     = root.targetStorageKey
                        var isF    = root.targetIsFolder
                        var exMode = root._extractMode
                        var exIdx  = root._extractIndex
                        root.close()
                        if (exMode)
                            root.extractRequested(gid, exIdx, sk, isF)
                        else
                            root.tileCreateRequested(gid, sk, isF)
                    }
                }
            }
        }
    }
}
