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
    property alias editDialog: editDialog
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
        var droppedDead = false
        for (var i = 0; i < saved.length; i++) {
            var item = saved[i]
            var tid  = item.id || ""

            // Сетевая установка, прерванная выходом/сбоем: плитка сохранилась без
            // пути и больше никогда не завершится. Раньше она оставалась навсегда
            // как нерабочая. Живую установку (идёт приём) не трогаем.
            if (tid.indexOf("net_") === 0 && !item.path
                    && !TileManager.isInstalling(tid)
                    && !NetworkListener.hasActiveInstall(tid)) {
                console.warn("Dropping unfinished network install tile:", tid, item.title)
                droppedDead = true
                continue
            }
            if (item.installFolder === undefined) item.installFolder = ""
            if (item.kind          === undefined) item.kind          = ""
            if (item.installPhase  === undefined) item.installPhase  = ""
            if (item.coverPath     === undefined) item.coverPath     = ""

            var tileId = tid
            if (tileId !== "" && TileManager.isInstalling(tileId)) {
                item.installing      = true
                item.installProgress = TileManager.getInstallProgress(tileId)
                item.installPhase    = ""
            } else if (tileId !== "" && NetworkListener.hasActiveInstall(tileId)) {
                // Идёт приём по сети: прогресс подтянется следующим сигналом
                item.installing      = true
                item.installProgress = item.installProgress || 0.0
                item.installPhase    = item.installPhase || "receiving"
            } else {
                item.installing      = false
                item.installProgress = 0.0
                item.installPhase    = ""
            }
            tilesModel.append(item)
        }
        if (droppedDead)
            saveAll()
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

    // ── Раскладка сетки ──────────────────────────────────────────────────────
    // Плитки исходного размера 420×270 (в пикселях, без масштаба интерфейса —
    // как было изначально), до 3 столбцов. Уменьшаются только если ряд не
    // помещается по ширине (например, окно 1280 px). Контент начинается под
    // шапкой, при переполнении — вертикальная прокрутка (колесо, перетаскивание,
    // стрелки пульта).
    readonly property int  _baseTileW: 420
    readonly property int  _baseTileH: 270
    readonly property int  _tileCount: tilesModel.count + 1
                                       + ((storageKey === "tv" && !isFolder) ? 2 : 0)
    readonly property int  _cols:      Math.max(1, Math.min(3, _tileCount))
    readonly property int  _spacing:   24
    readonly property int  _sideMargin: 90     // не заходить под стрелку «назад»
    readonly property real _fit: {
        var avail = tilesContainer.width - 2 * _sideMargin - (_cols - 1) * _spacing
        return Math.max(0.4, Math.min(1.0, avail / (_cols * _baseTileW)))
    }
    readonly property int  gridTileW: Math.floor(_baseTileW * _fit)
    readonly property int  gridTileH: Math.floor(_baseTileH * _fit)

    function scrollBy(rows) {
        var step = (gridTileH + _spacing) * rows
        var maxY = Math.max(0, tilesFlick.contentHeight - tilesFlick.height)
        tilesFlick.contentY = Math.max(0, Math.min(maxY, tilesFlick.contentY + step))
    }

    Item {
        id: tilesContainer
        anchors.fill: parent
        anchors.topMargin: (typeof root !== "undefined" && root.headerHeight) ? root.headerHeight : 160
        visible: !dynScreen.dialogOpen

        Flickable {
            id: tilesFlick
            anchors.fill: parent
            clip: true
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            // Запас сверху/снизу — под увеличение плитки при наведении
            readonly property int pad: 16
            contentWidth: width
            contentHeight: Math.max(height, flow.height + 2 * pad)
            Behavior on contentY { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        Flow {
            id: flow
            x: Math.round((tilesFlick.width - width) / 2)
            y: Math.max(tilesFlick.pad, Math.round((tilesFlick.height - height) / 2))
            spacing: dynScreen._spacing
            width: dynScreen._cols * dynScreen.gridTileW + (dynScreen._cols - 1) * dynScreen._spacing

            Tile {
                id: androidTvTile
                tileW: dynScreen.gridTileW; tileH: dynScreen.gridTileH
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
                tileW: dynScreen.gridTileW; tileH: dynScreen.gridTileH
                visible:           dynScreen.storageKey === "tv" && !dynScreen.isFolder
                title:             Settings.tr("karaoke")
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
                    tileW: dynScreen.gridTileW; tileH: dynScreen.gridTileH
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
                tileW: dynScreen.gridTileW; tileH: dynScreen.gridTileH
                title: Settings.tr("add"); iconSource: "qrc:/assets/icons/ic_add.png"; isAddButton: true
                onFolderChosen:   { dynScreen._creatingNew = true; editDialog.open({ type:"folder",   kind:"", title:"", icon:"", path:"", id:"" }) }
                onShortcutChosen: { dynScreen._creatingNew = true; editDialog.open({ type:"shortcut", kind:"", title:"", icon:"", path:"", id:"" }) }
            }
        }
        }   // Flickable

        // Индикатор прокрутки — видно, что плиток больше, чем помещается
        Rectangle {
            visible: tilesFlick.contentHeight > tilesFlick.height + 1
            anchors.right: parent.right; anchors.rightMargin: Settings.us(12)
            width: Settings.us(4); radius: width / 2
            color: "#80FFFFFF"
            y: tilesFlick.height * tilesFlick.visibleArea.yPosition
            height: Math.max(Settings.us(24), tilesFlick.height * tilesFlick.visibleArea.heightRatio)
        }
    }
}
