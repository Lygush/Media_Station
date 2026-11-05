// GameLibraryScreen.qml
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import Qt5Compat.GraphicalEffects
import AppSettings 1.0
import "../base"

Item {
    id: libScreen
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    property string screenTitle: Settings.tr("library")
    property bool   _isScanning: false
    property string _reassignGameId: ""
    property bool   _menuOpen:   false

    function forceHoverCheck() {}
    function closeDialogs() {
        if (contextMenu.visible)        { contextMenu.close();        return true }
        if (tileLocationDialog.visible) { tileLocationDialog.close(); return true }
        if (root.promptDialog && root.promptDialog.visible) { root.promptDialog.close(); return true }
        return false
    }

    ListModel { id: libraryModel }

    readonly property var statusColors: ({
        "installed":   "#4CAF50", "external":    "#2196F3",
        "unavailable": "#FFC107", "archive":     "#9E9E9E",
        "no_tile":     "#9E9E9E", "processing":  "#64B5F6"
    })
    readonly property var statusIcons: ({
        "installed":   "qrc:/assets/icons/ic_label.png",
        "external":    "qrc:/assets/icons/ic_label.png",
        "unavailable": "qrc:/assets/icons/ic_error.png",
        "archive":     "qrc:/assets/icons/ic_folder.png",
        "no_tile":     "qrc:/assets/icons/ic_label.png",
        "processing":  "qrc:/assets/icons/ic_folder.png"
    })

    // Ширина колонок «Статус» и «Размер» — по самым длинным подписям при
    // текущем кегле (иначе «Архивирование…» наезжало на размер)
    TextMetrics {
        id: statusMetrics
        font.pixelSize: Settings.fs(13)
        text: {
            var l = [Settings.tr("lib_status_archiving"), Settings.tr("lib_status_extracting"),
                     Settings.tr("lib_status_archive"), Settings.tr("lib_status_unavailable"),
                     Settings.tr("lib_status_no_tile"), Settings.tr("lib_status_external"),
                     Settings.tr("lib_status_installed")]
            l.sort(function(a, b) { return b.length - a.length })
            return l[0]
        }
    }
    TextMetrics { id: sizeMetrics; font.pixelSize: Settings.fs(13); text: "999.99 GB" }
    readonly property int _colStatusW: Math.max(Settings.us(180), Math.ceil(statusMetrics.advanceWidth) + Settings.us(36))
    readonly property int _colSizeW:   Math.max(Settings.us(120), Math.ceil(sizeMetrics.advanceWidth) + Settings.us(16))

    function statusInfo(type, available, tileCount, archiving, extracting) {
        if (archiving)          return { key: "processing", label: Settings.tr("lib_status_archiving") }
        if (extracting)         return { key: "processing", label: Settings.tr("lib_status_extracting") }
        if (type === "archive") return { key: "archive",    label: Settings.tr("lib_status_archive") }
        if (!available)         return { key: "unavailable",label: Settings.tr("lib_status_unavailable") }
        if (tileCount === 0)    return { key: "no_tile",    label: Settings.tr("lib_status_no_tile") }
        if (type === "external")return { key: "external",   label: Settings.tr("lib_status_external") }
        return                         { key: "installed",  label: Settings.tr("lib_status_installed") }
    }

    function sizeTargetPath(entry) {
        if (entry.type === "archive")   return entry.archivePath   || ""
        if (entry.type === "installed") return (entry.installPath  || "") + "/game"
        return entry.installPath || ""
    }

    function formatSize(bytes) {
        if (bytes < 0)                   return "…"
        if (bytes === 0)                 return "0 B"
        if (bytes < 1024)                return bytes + " B"
        if (bytes < 1024 * 1024)         return (bytes / 1024).toFixed(1) + " KB"
        if (bytes < 1024 * 1024 * 1024)  return (bytes / 1024 / 1024).toFixed(1) + " MB"
        return (bytes / 1024 / 1024 / 1024).toFixed(2) + " GB"
    }

    Component.onCompleted: {
        var existing = GameLibraryModule.currentEntries()
        if (existing.length > 0) _fillModel(existing)
        if (!GameLibraryModule.scanning && Settings.installPath && Settings.installPath !== "")
            GameLibraryModule.scanLibrary()
    }

    function _fillModel(entries) {
        libraryModel.clear()
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i]
            libraryModel.append({
                gameId: e.id || "", name: e.name || "", type: e.type || "installed",
                available: e.available !== false, installPath: e.installPath || "",
                archivePath: e.archivePath || "", iconPath: e.iconPath || "",
                tileCount: -1,
                sizeCached: e.sizeCached !== undefined ? e.sizeCached : -1,
                sizeLoading: false, archiving: e.archiving || false,
                archiveProgress: e.archiveProgress || 0.0,
                archiveCurrentFile: e.archiveCurrentFile || "",
                extracting: e.extracting || false, extractProgress: e.extractProgress || 0.0,
                legacyNoMetadata: e.legacyNoMetadata || false
            })
            if ((e.sizeCached === undefined || e.sizeCached < 0) && !e.archiving && !e.extracting) {
                var targetPath = libScreen.sizeTargetPath(e)
                if (targetPath && targetPath !== "") {
                    libraryModel.setProperty(i, "sizeLoading", true)
                    GameLibraryModule.requestFolderSize(e.id, targetPath)
                }
            }
        }
    }

    function _mergeModel(entries) {
        var newMap = {}
        for (var i = 0; i < entries.length; i++) newMap[entries[i].id || ""] = entries[i]

        for (var r = libraryModel.count - 1; r >= 0; r--) {
            var row0 = libraryModel.get(r)
            if (!newMap[row0.gameId] && !row0.archiving && !row0.extracting)
                libraryModel.remove(r)
        }

        for (var j = 0; j < entries.length; j++) {
            var ne = entries[j]; var neId = ne.id || ""
            if (!neId) continue
            var found = -1
            for (var k = 0; k < libraryModel.count; k++)
                if (libraryModel.get(k).gameId === neId) { found = k; break }

            if (found >= 0) {
                var row = libraryModel.get(found)
                var isActive = row.archiving || row.extracting
                if (!isActive) {
                    libraryModel.setProperty(found, "name",           ne.name          || row.name)
                    libraryModel.setProperty(found, "type",           ne.type          || row.type)
                    libraryModel.setProperty(found, "available",      ne.available     !== false)
                    libraryModel.setProperty(found, "installPath",    ne.installPath   || "")
                    libraryModel.setProperty(found, "archivePath",    ne.archivePath   || "")
                    libraryModel.setProperty(found, "iconPath",       ne.iconPath      || "")
                    libraryModel.setProperty(found, "legacyNoMetadata", ne.legacyNoMetadata || false)
                    if (row.sizeCached <= 0 && !row.sizeLoading) {
                        var tp = libScreen.sizeTargetPath(ne)
                        if (tp && tp !== "") {
                            libraryModel.setProperty(found, "sizeLoading", true)
                            GameLibraryModule.requestFolderSize(neId, tp)
                        }
                    }
                }
                if (!isActive) libraryModel.setProperty(found, "tileCount", -1)
            } else {
                libraryModel.append({
                    gameId: neId, name: ne.name || "", type: ne.type || "installed",
                    available: ne.available !== false, installPath: ne.installPath || "",
                    archivePath: ne.archivePath || "", iconPath: ne.iconPath || "",
                    tileCount: -1, sizeCached: ne.sizeCached !== undefined ? ne.sizeCached : -1,
                    sizeLoading: false, archiving: false, archiveProgress: 0.0,
                    archiveCurrentFile: "", extracting: false, extractProgress: 0.0,
                    legacyNoMetadata: ne.legacyNoMetadata || false
                })
                var newIdx = libraryModel.count - 1
                var newPath = libScreen.sizeTargetPath(ne)
                if ((ne.sizeCached === undefined || ne.sizeCached < 0) && newPath && newPath !== "") {
                    libraryModel.setProperty(newIdx, "sizeLoading", true)
                    GameLibraryModule.requestFolderSize(neId, newPath)
                }
            }
        }
    }

    Connections {
        target: GameLibraryModule

        function onScanningChanged()         { libScreen._isScanning = GameLibraryModule.scanning }
        function onScanFinished(entries) {
            libScreen._isScanning = false
            if (libraryModel.count === 0) libScreen._fillModel(entries)
            else                          libScreen._mergeModel(entries)
        }
        function onFolderSizeReady(gameId, bytes) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "sizeCached",  bytes)
                    libraryModel.setProperty(i, "sizeLoading", false)
                    break
                }
            }
        }
        function onTileCreated(gameId, success) {}
        function onArchiveProgress(gameId, progress, currentFile) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "archiveProgress",    progress)
                    libraryModel.setProperty(i, "archiveCurrentFile", currentFile)
                    break
                }
            }
        }
        function onArchiveFinished(gameId, success) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "archiving",          false)
                    libraryModel.setProperty(i, "archiveProgress",    0.0)
                    libraryModel.setProperty(i, "archiveCurrentFile", "")
                    if (success) {
                        libraryModel.setProperty(i, "type",       "archive")
                        libraryModel.setProperty(i, "sizeCached", -1)
                        libraryModel.setProperty(i, "sizeLoading", false)
                    }
                    break
                }
            }
        }
        function onExtractProgress(gameId, progress) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "extractProgress", progress); break
                }
            }
        }
        function onExtractFinished(gameId, success) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "extracting",      false)
                    libraryModel.setProperty(i, "extractProgress", 0.0)
                    if (success) {
                        libraryModel.setProperty(i, "type",       "installed")
                        libraryModel.setProperty(i, "available",  true)
                        libraryModel.setProperty(i, "sizeCached", -1)
                        libraryModel.setProperty(i, "sizeLoading", true)
                    }
                    break
                }
            }
        }
        function onLibraryEntryChanged(gameId, entry) {
            for (var i = 0; i < libraryModel.count; i++) {
                if (libraryModel.get(i).gameId === gameId) {
                    libraryModel.setProperty(i, "available", entry.available !== false)
                    if (entry.type && entry.type !== "") libraryModel.setProperty(i, "type", entry.type)
                    var newArchivePath = entry.archivePath || ""
                    libraryModel.setProperty(i, "archivePath", newArchivePath)
                    if (entry.type === "archive" && newArchivePath !== "") {
                        libraryModel.setProperty(i, "sizeLoading", true)
                        GameLibraryModule.requestFolderSize(gameId, newArchivePath)
                    }
                    if (entry.installPath && entry.installPath !== "")
                        libraryModel.setProperty(i, "installPath", entry.installPath)
                    if (entry.tileCount !== undefined && entry.tileCount >= 0)
                        libraryModel.setProperty(i, "tileCount", entry.tileCount)
                    libraryModel.setProperty(i, "legacyNoMetadata", entry.legacyNoMetadata || false)
                    break
                }
            }
        }
    }

    readonly property int _topSafe: Settings.us(108)
    readonly property int _botSafe: Settings.us(32)

    Item {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top:    parent.top
        anchors.topMargin: libScreen._topSafe
        width:  Math.min(parent.width - Settings.us(80), Settings.us(1100))
        height: Math.min(parent.height - libScreen._topSafe - libScreen._botSafe, Settings.us(680))

        GlowingCard {
            anchors.fill: parent; radius: 20

            Column {
                anchors { fill: parent; margins: Settings.us(28) }
                spacing: Settings.us(16)

                Item {
                    width: parent.width; height: Settings.us(40)

                    Row {
                        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
                        spacing: Settings.us(10)
                        Image {
                            source: "qrc:/assets/icons/ic_lib.png"
                            width: Settings.us(26); height: Settings.us(26)
                            sourceSize.width: 100; sourceSize.height: 100
                            fillMode: Image.PreserveAspectFit; anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: Settings.tr("library"); color: "white"
                            font.pixelSize: Settings.fs(22); font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: libraryModel.count > 0
                                  ? Settings.tr("lib_found", { count: libraryModel.count }) : ""
                            color: Settings.accent(0.7); font.pixelSize: Settings.fs(14)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        spacing: Settings.us(8); visible: libScreen._isScanning
                        Rectangle {
                            width: Settings.us(16); height: Settings.us(16); radius: Settings.us(8)
                            color: "transparent"; border.color: "white"; border.width: 2
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                width: Settings.us(8); height: Settings.us(8)
                                color: "white"; radius: Settings.us(4)
                                anchors.top: parent.top; anchors.left: parent.left
                                RotationAnimation on rotation {
                                    running: libScreen._isScanning
                                    from: 0; to: 360; duration: 800; loops: Animation.Infinite
                                }
                            }
                        }
                        Text {
                            text: Settings.tr("lib_scanning"); color: Settings.accent(0.7)
                            font.pixelSize: Settings.fs(14); anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                Rectangle { width: parent.width; height: 1; color: Settings.accent(0.3) }

                Row {
                    width: parent.width; leftPadding: Settings.us(8); spacing: 0
                    Text { width: Settings.us(56); text: ""; color: Settings.accent(0.7); font.pixelSize: Settings.fs(12); font.bold: true }
                    Text {
                        width: parent.width - Settings.us(56) - libScreen._colStatusW - libScreen._colSizeW - Settings.us(16)
                        text: Settings.tr("lib_col_name"); color: Settings.accent(0.7); font.pixelSize: Settings.fs(12); font.bold: true
                    }
                    Text { width: libScreen._colStatusW; text: Settings.tr("lib_col_status"); color: Settings.accent(0.7); font.pixelSize: Settings.fs(12); font.bold: true }
                    Text { width: libScreen._colSizeW; text: Settings.tr("lib_col_size"); color: Settings.accent(0.7); font.pixelSize: Settings.fs(12); font.bold: true; horizontalAlignment: Text.AlignRight }
                }

                Rectangle { width: parent.width; height: 1; color: Settings.accent(0.15) }

                Item {
                    width: parent.width
                    height: parent.height - Settings.us(170)

                    Column {
                        anchors.centerIn: parent; spacing: Settings.us(14)
                        visible: libraryModel.count === 0 && !libScreen._isScanning
                        Image {
                            source: "qrc:/assets/icons/ic_folder.png"
                            width: Settings.us(48); height: Settings.us(48)
                            sourceSize.width: 100; sourceSize.height: 100
                            fillMode: Image.PreserveAspectFit
                            anchors.horizontalCenter: parent.horizontalCenter; opacity: 0.3
                        }
                        Text {
                            text: Settings.installPath && Settings.installPath !== ""
                                  ? Settings.tr("lib_empty") : Settings.tr("lib_no_path")
                            color: Settings.accent(0.4); font.pixelSize: Settings.fs(15)
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }

                    ListView {
                        id: gameListView
                        anchors { fill: parent; rightMargin: Settings.us(14) }
                        model: libraryModel; clip: true; spacing: Settings.us(2)

                        ScrollBar.vertical: ScrollBar {
                            id: listScrollBar
                            parent: gameListView.parent
                            anchors.right:  parent.right
                            anchors.top:    gameListView.top
                            anchors.bottom: gameListView.bottom
                            policy: ScrollBar.AsNeeded
                            opacity: active ? 1.0 : 0.0
                            Behavior on opacity { NumberAnimation { duration: 150 } }
                            contentItem: Rectangle { implicitWidth: Settings.us(4); radius: Settings.us(2); color: Settings.accent(0.6) }
                            background:  Rectangle { implicitWidth: Settings.us(4); color: Settings.accent(0.08); radius: Settings.us(2) }
                        }

                        delegate: Item {
                            id: rowDelegate
                            width: gameListView.width
                            height: (model.archiving || model.extracting) ? Settings.us(72) : Settings.us(56)
                            Behavior on height { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

                            property bool  _pressing:     false
                            property int   _pressX:       0
                            property int   _pressY:       0
                            property real  _holdProgress: 0.0

                            Rectangle {
                                anchors.fill: parent; radius: 8
                                color: rowMA.containsMouse
                                       ? Settings.accent(0.18)
                                       : (index % 2 === 0 ? Settings.accent(0.05) : "transparent")
                                Behavior on color { ColorAnimation { duration: 100 } }
                            }
                            Rectangle {
                                anchors.fill: parent; radius: 8; color: Settings.accent(1.0)
                                opacity: (rowDelegate._pressing && !model.archiving
                                          && !model.extracting && !libScreen._menuOpen)
                                         ? rowDelegate._holdProgress * 0.22 : 0.0
                            }
                            Rectangle {
                                visible: rowDelegate._pressing && !model.archiving && !model.extracting
                                anchors.bottom: parent.bottom; anchors.left: parent.left
                                height: Settings.us(3); radius: Settings.us(1)
                                width: parent.width * rowDelegate._holdProgress
                                color: Settings.accent(0.9)
                            }

                            Row {
                                id: mainRow
                                anchors { left: parent.left; right: parent.right; top: parent.top }
                                anchors.leftMargin: Settings.us(8); anchors.rightMargin: Settings.us(8)
                                height: Settings.us(56); spacing: 0

                                Item {
                                    width: Settings.us(56); height: Settings.us(56)
                                    Image {
                                        anchors.centerIn: parent
                                        width: Settings.us(36); height: Settings.us(36)
                                        source: model.iconPath && model.iconPath !== ""
                                                ? "file:///" + model.iconPath.replace(/\\/g, "/")
                                                : "qrc:/assets/icons/ic_label.png"
                                        sourceSize.width: 100; sourceSize.height: 100
                                        fillMode: Image.PreserveAspectFit; smooth: true
                                        onStatusChanged: { if (status === Image.Error) source = "qrc:/assets/icons/ic_label.png" }
                                    }
                                }

                                Item {
                                    width: parent.parent.width - Settings.us(56) - libScreen._colStatusW - libScreen._colSizeW - Settings.us(16)
                                    height: Settings.us(56)
                                    Column {
                                        anchors.verticalCenter: parent.verticalCenter; spacing: 2
                                        Text {
                                            text: model.name; color: "white"
                                            font.pixelSize: Settings.fs(15); font.bold: true
                                            elide: Text.ElideRight; width: parent.parent.width - Settings.us(8)
                                        }
                                        Row {
                                            visible: model.legacyNoMetadata; spacing: Settings.us(4)
                                            Image {
                                                source: "qrc:/assets/icons/ic_error.png"
                                                width: Settings.us(14); height: Settings.us(14)
                                                sourceSize.width: 100; sourceSize.height: 100
                                                fillMode: Image.PreserveAspectFit
                                                anchors.verticalCenter: parent.verticalCenter; opacity: 0.9
                                            }
                                            Text {
                                                text: Settings.tr("lib_legacy_hint"); color: "#FFC107"
                                                font.pixelSize: Settings.fs(11); anchors.verticalCenter: parent.verticalCenter
                                            }
                                        }
                                    }
                                }

                                Item {
                                    width: libScreen._colStatusW; height: Settings.us(56)
                                    StatusBadge {
                                        anchors.verticalCenter: parent.verticalCenter
                                        readonly property var si: libScreen.statusInfo(
                                            model.type, model.available, model.tileCount,
                                            model.archiving, model.extracting)
                                        dotColor: libScreen.statusColors[si.key] || "#888"
                                        label:    si.label
                                    }
                                }

                                Item {
                                    width: libScreen._colSizeW; height: Settings.us(56)
                                    Text {
                                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                        visible: model.type !== "external"
                                        text: model.sizeLoading ? "…" : libScreen.formatSize(model.sizeCached)
                                        color: model.sizeLoading ? Settings.accent(0.5) : "white"
                                        font.pixelSize: Settings.fs(13)
                                    }
                                }
                            }

                            Item {
                                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                                anchors.leftMargin: Settings.us(64); anchors.rightMargin: Settings.us(8)
                                anchors.bottomMargin: Settings.us(6)
                                height: Settings.us(28)
                                visible: model.archiving || model.extracting

                                Item {
                                    id: cancelBtn
                                    visible: model.archiving || model.extracting
                                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                    width: Settings.us(22); height: Settings.us(22)
                                    Rectangle {
                                        anchors.fill: parent; radius: Settings.us(11)
                                        color: cancelBtnMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.15)
                                        Behavior on color { ColorAnimation { duration: 100 } }
                                    }
                                    Text { anchors.centerIn: parent; text: "✕"; color: "white"; font.pixelSize: Settings.fs(10); font.bold: true }
                                    MouseArea {
                                        id: cancelBtnMA; anchors.fill: parent; hoverEnabled: true
                                        onClicked: {
                                            if (model.archiving) GameLibraryModule.cancelArchive(model.gameId)
                                            else                 GameLibraryModule.cancelExtract(model.gameId)
                                        }
                                    }
                                }

                                Column {
                                    anchors.left: parent.left
                                    anchors.right: cancelBtn.visible ? cancelBtn.left : parent.right
                                    anchors.rightMargin: cancelBtn.visible ? Settings.us(6) : 0
                                    anchors.verticalCenter: parent.verticalCenter; spacing: Settings.us(3)
                                    Text {
                                        width: parent.width
                                        text: model.archiving
                                              ? (model.archiveCurrentFile !== "" ? model.archiveCurrentFile : Settings.tr("lib_status_archiving"))
                                              : Settings.tr("lib_status_extracting")
                                        color: Settings.accent(0.7); font.pixelSize: Settings.fs(11); elide: Text.ElideLeft
                                    }
                                    Rectangle {
                                        width: parent.width; height: Settings.us(4); radius: Settings.us(2)
                                        color: Settings.accent(0.15)
                                        ProgressBar {
                                            anchors.fill: parent; radius: Settings.us(2)
                                            animated: true; trackColor: "transparent"; barColor: Settings.accent(0.9)
                                            value: model.archiving ? model.archiveProgress : model.extractProgress
                                        }
                                    }
                                }
                            }

                            MouseArea {
                                id: rowMA
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                height: Settings.us(56); hoverEnabled: true
                                acceptedButtons: Qt.LeftButton; preventStealing: true

                                Timer {
                                    id: holdTimer; interval: 500; repeat: false
                                    onTriggered: {
                                        if (rowDelegate._pressing && !model.archiving && !model.extracting) {
                                            libScreen._menuOpen = true
                                            contextMenu.openForRow(
                                                index,
                                                rowDelegate._pressX,
                                                rowDelegate._pressY,
                                                libraryModel.get(index)
                                            )
                                            rowDelegate._pressing     = false
                                            rowDelegate._holdProgress = 0.0
                                        }
                                    }
                                }
                                onPressed: function(mouse) {
                                    rowDelegate._pressing     = true
                                    rowDelegate._holdProgress = 0.0
                                    var pt = rowMA.mapToItem(libScreen, mouse.x, mouse.y)
                                    rowDelegate._pressX = pt.x; rowDelegate._pressY = pt.y
                                    holdTimer.start(); holdProgressAnim.start()
                                }
                                onReleased: {
                                    rowDelegate._pressing     = false
                                    rowDelegate._holdProgress = 0.0
                                    holdTimer.stop(); holdProgressAnim.stop()
                                }
                                NumberAnimation {
                                    id: holdProgressAnim; target: rowDelegate; property: "_holdProgress"
                                    from: 0.0; to: 1.0; duration: 500; easing.type: Easing.Linear
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── Контекстное меню ──────────────────────────────────────────────────────
    LibraryContextMenu {
        id: contextMenu
        onVisibleChanged: if (!visible) libScreen._menuOpen = false

        onTileActionRequested: function(gameId, gameName, tileCount) {
            tileLocationDialog.openForGame(gameId, gameName, tileCount)
        }
        onExtractRequested: function(gameId, gameName, rowIndex) {
            tileLocationDialog.openForExtract(gameId, gameName, rowIndex)
        }
        onArchiveRequested: function(gameId, rowIndex) {
            var entry     = libraryModel.get(rowIndex)
            var tileCount = entry.tileCount
            var pd = root.promptDialog
            if (!pd) return
            pd.show({
                title:   Settings.tr("lib_menu_archive"),
                message: tileCount > 0
                         ? Settings.tr("lib_archive_confirm_tiles", { name: entry.name, count: tileCount })
                         : Settings.tr("lib_archive_confirm", { name: entry.name }),
                icon:    "qrc:/assets/icons/ic_folder.png",
                buttons: [
                    { label: Settings.tr("cancel"), role: "cancel", action: function(){} },
                    { label: Settings.tr("lib_archive_btn"), role: "primary",
                      action: function() {
                          for (var i = 0; i < libraryModel.count; i++) {
                              if (libraryModel.get(i).gameId === gameId) {
                                  libraryModel.setProperty(i, "archiving",        true)
                                  libraryModel.setProperty(i, "archiveProgress",  0.0)
                                  break
                              }
                          }
                          GameLibraryModule.archiveGame(gameId)
                      }
                    }
                ]
            })
        }
        onOpenFolderRequested: function(gameId, type, installPath, archivePath) {
            // Ищем entry в модели по gameId
            for (var i = 0; i < libraryModel.count; i++) {
                var e = libraryModel.get(i)
                if (e.gameId === gameId) {
                    var path = e.type === "archive" ? e.archivePath : e.installPath
                    GameLibraryModule.openFolder(path)
                    break
                }
            }
        }
        onReassignRequested: function(gameId) {
            libScreen._reassignGameId = gameId
            reassignBrowser.open()
        }
        onDeleteRequested: function(gameId, rowIndex) {
            var entry = libraryModel.get(rowIndex)
            var msg = entry.type === "archive"   ? Settings.tr("lib_delete_archive",   { name: entry.name })
                    : entry.type === "external"  ? Settings.tr("lib_delete_external",  { name: entry.name })
                    :                              Settings.tr("lib_delete_installed",  { name: entry.name })
            var pd = root.promptDialog
            if (!pd) return
            pd.show({
                title: Settings.tr("lib_menu_delete"), message: msg,
                icon: "qrc:/assets/icons/ic_error.png",
                buttons: [
                    { label: Settings.tr("cancel"), role: "cancel", action: function(){} },
                    { label: Settings.tr("lib_delete_btn"), role: "danger",
                      action: function() {
                          GameLibraryModule.deleteGame(gameId)
                          if (rowIndex >= 0 && rowIndex < libraryModel.count)
                              libraryModel.remove(rowIndex)
                      }
                    }
                ]
            })
        }
        onRegenerateMetadataRequested: function(gameId) {
            GameLibraryModule.regenerateMetadata(gameId)
        }
    }

    // ── Диалог выбора места плитки ────────────────────────────────────────────
    TileLocationDialog {
        id: tileLocationDialog
        onTileCreateRequested: function(gameId, storageKey, isFolder) {
            GameLibraryModule.createTile(gameId, storageKey, isFolder)
        }
        onExtractRequested: function(gameId, rowIndex, storageKey, isFolder) {
            libraryModel.setProperty(rowIndex, "extracting",      true)
            libraryModel.setProperty(rowIndex, "extractProgress", 0.0)
            GameLibraryModule.extractGame(gameId, false, storageKey, isFolder)
        }
    }

    // ── Переназначение пути ───────────────────────────────────────────────────
    FileDialog {
        id: reassignBrowser
        title:       Settings.tr("lib_menu_reassign")
        nameFilters: ["Executable files (*.exe *.bat *.cmd)", "All files (*)"]
        onAccepted: {
            var path = selectedFile.toString().replace(/^file:\/\/\//, "")
            if (path.length > 0 && path[1] !== ":") path = "/" + path
            GameLibraryModule.reassignPath(libScreen._reassignGameId, path)
        }
    }
}
