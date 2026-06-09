// DynamicScreen.qml
import QtQuick
import AppSettings 1.0
import "../dialogs"
import "../tiles"

Item {
    id: dynScreen
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    property string storageKey:  ""
    property bool   isFolder:    false
    property string screenTitle: ""

    property var    launchGame:      null
    property var    openScreen:      null
    property var    openGameDetail:  null
    property string launchingTileId: ""
    property bool   gameLaunching:   false

    property alias tilesModel: tilesModel
    property int   editIndex:   -1
    property bool  _creatingNew: false

    property bool dialogOpen: editDialog.visible || fileBrowser.visible || (root.promptDialog && root.promptDialog.visible)

    ListModel { id: tilesModel }

    Component.onCompleted: {
        loadData();
        forceHoverCheck();
    }

    function loadData() {
        tilesModel.clear()
        var saved = isFolder ? TileManager.loadFolder(storageKey) : TileManager.loadScreen(storageKey)
        for (var i = 0; i < saved.length; i++) {
            var item = saved[i]
            if (item.installFolder === undefined) item.installFolder = ""
            if (item.kind          === undefined) item.kind          = ""
            if (item.installPhase  === undefined) item.installPhase  = ""
            if (item.coverPath     === undefined) item.coverPath     = ""

            var tileId = item.id || ""
            if (tileId !== "" && TileManager.isInstalling(tileId)) {
                item.installing      = true
                item.installProgress = TileManager.getInstallProgress(tileId)
                item.installPhase    = ""
            } else {
                item.installing      = false
                item.installProgress = 0.0
                item.installPhase    = ""
            }
            tilesModel.append(item)
        }
    }

    function saveAll() {
        var arr = []
        for (var i = 0; i < tilesModel.count; i++) {
            var m = tilesModel.get(i)
            arr.push({
                type:          m.type          || "",
                kind:          m.kind          || "",
                title:         m.title         || "",
                icon:          m.icon          || "",
                path:          m.path          || "",
                id:            m.id            || "",
                installFolder: m.installFolder || ""
            })
        }
        if (isFolder) TileManager.saveFolder(storageKey, arr)
        else          TileManager.saveScreen(storageKey, arr)
    }

    function forceHoverCheck() {
        var mx = cursorMA.mouseX
        var my = cursorMA.mouseY
        for (var i = 0; i < flow.children.length; i++)
            if (flow.children[i].hasOwnProperty("forceHover"))
                flow.children[i].forceHover = false
        for (var j = 0; j < flow.children.length; j++) {
            var tile = flow.children[j]
            if (!tile.hasOwnProperty("forceHover")) continue
            var r = tile.mapToItem(dynScreen, 0, 0, tile.width, tile.height)
            if (mx >= r.x && mx <= r.x + r.width && my >= r.y && my <= r.y + r.height) {
                tile.forceHover = true; break
            }
        }
    }

    function closeDialogs() {
        if (fileBrowser.visible) { fileBrowser.close();  return true }
        if (editDialog.visible)  { editDialog.cancel();  return true }
        if (root.promptDialog && root.promptDialog.visible) { root.promptDialog.close(); return true }
        return false
    }

    function requestDelete(idx, tileType, tileTitle, tileId, tileInstallFolder, tileKind) {
        var message = ""
        var buttons = []

        if (tileType === "folder") {
            var contents = TileManager.loadFolder(tileId)
            var nonEmpty = contents.length > 0
            if (nonEmpty) {
                message = Settings.tr("del_body_folder_nonempty", { title: tileTitle, count: contents.length })
            } else {
                message = Settings.tr("del_body_folder_empty", { title: tileTitle })
            }
            buttons = [
                { label: Settings.tr("cancel"), role: "cancel", action: function() {} },
                { label: nonEmpty ? Settings.tr("del_all") : Settings.tr("delete"), role: "danger",
                  action: function() { dynScreen._doDelete(idx, tileId, tileType, tileInstallFolder, false, tileKind) } }
            ]
        } else {
            var installFolder = tileInstallFolder
            if (!installFolder || installFolder === "") {
                var found = TileManager.findInstallFolderByTileId(tileId, SettingsManager.installPath)
                if (found && found !== "") installFolder = found
            }
            if (installFolder && installFolder !== "" && installFolder !== tileInstallFolder) {
                tilesModel.setProperty(idx, "installFolder", installFolder)
                saveAll()
            }

            var isExternal   = (tileKind === "external")
            var hasInstalled = !isExternal && TileManager.installedGameExists(installFolder, SettingsManager.installPath)

            if (hasInstalled) {
                message = Settings.tr("del_body_shortcut_installed", { title: tileTitle })
                buttons = [
                    { label: Settings.tr("cancel"), role: "cancel", action: function() {} },
                    { label: Settings.tr("del_shortcut_only"), role: "primary",
                      action: function() { dynScreen._doDelete(idx, tileId, tileType, installFolder, false, tileKind) } },
                    { label: Settings.tr("del_shortcut_with_files"), role: "danger",
                      action: function() { dynScreen._doDelete(idx, tileId, tileType, installFolder, true, tileKind) } }
                ]
            } else {
                message = Settings.tr("del_body_shortcut_external", { title: tileTitle })
                buttons = [
                    { label: Settings.tr("cancel"), role: "cancel", action: function() {} },
                    { label: Settings.tr("delete"), role: "danger",
                      action: function() { dynScreen._doDelete(idx, tileId, tileType, installFolder, false, tileKind) } }
                ]
            }
        }

        if (root.promptDialog) {
            root.promptDialog.show({
                title: Settings.tr("delete_title"),
                message: message,
                icon: "qrc:/assets/icons/ic_delete.png",
                buttons: buttons
            })
        }
    }

    function _doDelete(idx, tileId, tileType, installFolder, withFiles, tileKind) {
        var folder = installFolder
        if ((!folder || folder === "") && tileId && tileId !== "")
            folder = TileManager.findInstallFolderByTileId(tileId, SettingsManager.installPath)

        if (withFiles && folder && folder !== "") {
            TileManager.deleteInstalledGame(folder)
        } else if (!withFiles && folder && folder !== "" && tileType !== "folder") {
            if (tileKind === "external")
                TileManager.deleteInstalledGame(folder)
        }

        if (tileType === "folder" && tileId && tileId !== "")
            TileManager.deleteFolder(tileId)

        if (idx >= 0 && idx < tilesModel.count) {
            tilesModel.remove(idx)
            saveAll()
        }
    }

    MouseArea {
        id: cursorMA
        anchors.fill: parent
        hoverEnabled: true; acceptedButtons: Qt.NoButton; z: -1
        onPositionChanged: {
            for (var i = 0; i < flow.children.length; i++)
                if (flow.children[i].hasOwnProperty("forceHover"))
                    flow.children[i].forceHover = false
        }
    }

    FileBrowserDialog { id: fileBrowser }

    EditTileDialog {
        id: editDialog
        fileBrowser: fileBrowser

        onSaved: function(data) {
            if (dynScreen._creatingNew) {
                if (!data.id || data.id === "") data.id = Date.now().toString()

                var suffix      = data.path ? data.path.split('.').pop().toLowerCase() : ""
                var isExternalExe = (data.type === "shortcut" && !data.installing &&
                                     (suffix === "exe" || suffix === "bat" || suffix === "cmd"))

                if (isExternalExe) {
                    data.kind = "external"
                    var pendingPath = data.path
                    var pendingIcon = data.icon
                    data.path = ""
                    tilesModel.append(data)
                    dynScreen.saveAll()
                    TileManager.createExternalShortcut(
                        data.id, data.title, pendingPath, pendingIcon,
                        SettingsManager.installPath
                    )
                } else {
                    tilesModel.append(data)
                    dynScreen.saveAll()
                }
                dynScreen._creatingNew = false

            } else if (dynScreen.editIndex >= 0 && dynScreen.editIndex < tilesModel.count) {
                tilesModel.set(dynScreen.editIndex, data)
                dynScreen.saveAll()
            }
            dynScreen.editIndex = -1
        }

        onCancelled: {
            dynScreen._creatingNew = false
            dynScreen.editIndex    = -1
        }
    }

    Connections {
        target: TileManager

        function onTileStructureChanged(storageKey, isFolder) {
            var mine = storageKey === ""
                    || (storageKey === dynScreen.storageKey && isFolder === dynScreen.isFolder)
            if (mine) {
                loadData()
                forceHoverCheck()
            }
        }

        function onInstallationProgress(tileId, progress) {
            for (var i = 0; i < tilesModel.count; i++) {
                if (tilesModel.get(i).id === tileId) {
                    tilesModel.setProperty(i, "installProgress", progress)
                    tilesModel.setProperty(i, "installing",      true)
                    tilesModel.setProperty(i, "installPhase",    "")
                    break
                }
            }
        }

        function onInstallationFinished(tileId, success, ps1Path, iconPath, installFolder) {
            for (var i = 0; i < tilesModel.count; i++) {
                if (tilesModel.get(i).id === tileId) {
                    if (success) {
                        tilesModel.setProperty(i, "path",            ps1Path)
                        if (iconPath      && iconPath      !== "") tilesModel.setProperty(i, "icon",          iconPath)
                        if (installFolder && installFolder !== "") tilesModel.setProperty(i, "installFolder", installFolder)
                        tilesModel.setProperty(i, "installing",      false)
                        tilesModel.setProperty(i, "installProgress", 0.0)
                        tilesModel.setProperty(i, "installPhase",    "")
                        dynScreen.saveAll()
                    } else {
                        tilesModel.remove(i)
                        dynScreen.saveAll()
                    }
                    break
                }
            }
        }

        function onInstallationCancelled(tileId) {
            for (var i = 0; i < tilesModel.count; i++) {
                if (tilesModel.get(i).id === tileId) {
                    tilesModel.remove(i)
                    dynScreen.saveAll()
                    break
                }
            }
        }

        function onLaunchExeUpdated(tileId, success, newPs1Path, newExeRelative) {
            if (!success) return
            for (var i = 0; i < tilesModel.count; i++) {
                if (tilesModel.get(i).id === tileId) {
                    tilesModel.setProperty(i, "path", newPs1Path)
                    dynScreen.saveAll()
                    break
                }
            }
        }
    }

    Item {
        id: tilesContainer
        anchors.fill: parent; anchors.topMargin: 80
        visible: !dynScreen.dialogOpen

        Flow {
            id: flow
            anchors.centerIn: parent
            spacing: 24
            width: {
                var n = tilesModel.count + 1
                if (dynScreen.storageKey === "tv" && !dynScreen.isFolder) n += 2
                return Math.min(n, 3) * 444
            }

            Tile {
                id: androidTvTile
                width: 420; height: 270
                visible:           dynScreen.storageKey === "tv" && !dynScreen.isFolder
                title:             Settings.tr("android_tv")
                iconSource:        "qrc:/assets/icons/ic_android_tv.png"
                type:              "androidtv"
                enableContextMenu: false

                // ── Биндинг к состоянию AndroidTV ────────────────────────────
                androidTvBooting: AndroidTV?.booting ?? false
                androidTvRunning: AndroidTV?.running ?? false

                onClicked: {
                    if (!AndroidTV) return
                    if (AndroidTV.running) {
                        AndroidTV.launch()
                    } else if (!AndroidTV.booting) {
                        AndroidTV.launch()
                    }
                }
            }

            Tile {
                id: karaokeTile
                width: 420; height: 270
                visible:           dynScreen.storageKey === "tv" && !dynScreen.isFolder
                title:             "Karaoke"
                iconSource:        "qrc:/assets/icons/ic_karaoke.png"
                type:              "karaoke"
                enableContextMenu: false

                onClicked: {
                    if (typeof root !== "undefined" && root.openKaraokeScreen)
                        root.openKaraokeScreen()
                }
            }

            Repeater {
                model: tilesModel
                delegate: Tile {
                    width: 420; height: 270
                    type:            model.type
                    title:           model.title
                    iconSource:      model.icon            || ""
                    coverSource:     model.coverPath       || ""
                    installing:      model.installing      || false
                    installProgress: model.installProgress || 0.0
                    installPhase:    model.installPhase    || ""
                    tileId:          model.id              || ""
                    launching:       dynScreen.launchingTileId === (model.id || "") && dynScreen.gameLaunching

                    onClicked: {
                        if (model.type === "folder")
                            dynScreen.openScreen(model.title, model.id, true)
                        else if (model.type === "shortcut" && model.path !== "")
                            dynScreen.launchGame(model.path, model.id || model.path)
                    }

                    onEditRequested: {
                        dynScreen.editIndex = index
                        var item = dynScreen.tilesModel.get(index)
                        editDialog.open({
                            type:          item.type          || "",
                            kind:          item.kind          || "",
                            title:         item.title         || "",
                            icon:          item.icon          || "",
                            path:          item.path          || "",
                            id:            item.id            || "",
                            installFolder: item.installFolder || ""
                        })
                    }

                    onDeleteRequested: {
                        dynScreen.requestDelete(
                            index,
                            model.type          || "",
                            model.title         || "",
                            model.id            || "",
                            model.installFolder || "",
                            model.kind          || ""
                        )
                    }

                    onCancelInstallRequested: {
                        TileManager.cancelInstallation(model.id)
                        NetworkListener.cancelInstallByTileId(model.id)
                    }

                    // Фаза 2: открыть страницу игры
                    onDetailsRequested: {
                        if (dynScreen.openGameDetail) {
                            var item = dynScreen.tilesModel.get(index)
                            dynScreen.openGameDetail({
                                title:         item.title         || "",
                                tileId:        item.id            || "",
                                path:          item.path          || "",
                                installFolder: item.installFolder || "",
                                coverPath:     item.coverPath     || "",
                                backdropPath:  item.backdropPath  || "",
                                trailerPath:   item.trailerPath   || "",
                                description:   item.description   || "",
                                genre:         item.genre         || "",
                                releaseYear:   item.releaseYear   || "",
                                developer:     item.developer     || "",
                                publisher:     item.publisher     || "",
                                rating:        item.rating        || "",
                                sysreqMin:     item.sysreqMin     || "",
                                sysreqRec:     item.sysreqRec     || ""
                            })
                        }
                    }
                }
            }

            Tile {
                width: 420; height: 270
                title: Settings.tr("add"); iconSource: "qrc:/assets/icons/ic_add.png"; isAddButton: true
                onFolderChosen:   { dynScreen._creatingNew = true; editDialog.open({ type:"folder",   kind:"", title:"", icon:"", path:"", id:"" }) }
                onShortcutChosen: { dynScreen._creatingNew = true; editDialog.open({ type:"shortcut", kind:"", title:"", icon:"", path:"", id:"" }) }
            }
        }
    }
}
