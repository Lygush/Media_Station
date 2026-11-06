// dialogs/EditTileDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// EditTileDialog — диалог создания / редактирования плитки.
// Принимает tileData через open(data), возвращает результат через saved().
//
// Режимы:
//   type="folder"   — только поле «Название»
//   type="shortcut" — выбор kind (web/app/playstation1), путь/URL, иконка
//
// Подформы вынесены в:
//   AppShortcutForm   — папка игры, список exe, имя, чекбокс установки
//   WebShortcutForm   — URL/путь для web и playstation1
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import Qt5Compat.GraphicalEffects
import QtQuick.Dialogs
import AppSettings 1.0
import "../base"

Item {
    id: dialog

    anchors.fill: parent
    z: 300
    visible: false

    // ── Свойства ──────────────────────────────────────────────────────────────
    property var    tileData:         null
    property var    fileBrowser:      null
    property string _browseTarget:    ""
    property bool   installChecked:   true
    property string shortcutKind:     ""   // "web" | "app" | "playstation1"

    property string gameDirPath:      ""
    property var    exeList:          []
    property string selectedExe:      ""
    property string _descRequestId:   ""
    property string _scanRequestId:   ""
    property string _depRequestId:    ""
    property string _folderBaseName:  ""
    property bool   exeScanning:      false
    property bool   depChecking:      false
    property bool   _editingInstalled:false
    property bool   _scanResultReady: false
    property var    _scanPendingList: null
    property bool   _depResultReady:  false
    property var    _depPendingSaved: null

    readonly property int  _spinnerMinMs: 300
    readonly property bool isNew: tileData ? (!tileData.id || tileData.id === "") : false

    // ── Сигналы ───────────────────────────────────────────────────────────────
    signal saved(var updatedData)
    signal cancelled()

    // ── Таймеры минимального отображения спиннеров ────────────────────────────
    Timer {
        id: scanMinTimer; interval: dialog._spinnerMinMs; repeat: false
        onTriggered: {
            if (dialog._scanResultReady) {
                dialog.exeScanning      = false
                dialog.exeList          = dialog._scanPendingList || []
                dialog._scanPendingList = null
            }
        }
    }
    Timer {
        id: depMinTimer; interval: dialog._spinnerMinMs; repeat: false
        onTriggered: {
            if (dialog._depResultReady) {
                dialog.depChecking      = false
                var s                   = dialog._depPendingSaved
                dialog._depPendingSaved = null
                dialog._finalizeSave(s)
            }
        }
    }

    // ── Connections с TileManager ─────────────────────────────────────────────
    Connections {
        target: TileManager
        function onExeDescriptionReady(requestId, description) {
            if (requestId !== dialog._descRequestId) return
            var isNewApp     = tileData && tileData.type === "shortcut" &&
                               dialog.shortcutKind === "app" && isNew
            var candidate    = description !== "" ? description : dialog._folderBaseName
            if (candidate !== "") {
                if (isNewApp) appForm.nameText = candidate
                else          nameField.text   = candidate
            }
        }
        function onExeScanReady(requestId, exeFiles) {
            if (requestId !== dialog._scanRequestId) return
            if (scanMinTimer.running) {
                dialog._scanResultReady = true
                dialog._scanPendingList = exeFiles
            } else {
                dialog.exeScanning = false
                dialog.exeList     = exeFiles
            }
        }
        function onDependencyCheckReady(requestId, missingDeps) {
            if (requestId !== dialog._depRequestId) return
            if (missingDeps.length > 0)
                console.warn("InstallManager [deps]: missing:", missingDeps.join(", "))
            if (depMinTimer.running) {
                dialog._depResultReady = true
            } else {
                dialog.depChecking      = false
                var s                   = dialog._depPendingSaved
                dialog._depPendingSaved = null
                dialog._finalizeSave(s)
            }
        }
        function onLaunchExeUpdated(tileId, success, newPs1Path, newExeRelative) {
            if (!tileData || tileData.id !== tileId) return
            dialog.depChecking = false
            if (success) {
                var updated     = dialog._depPendingSaved || {}
                updated.path    = newExeRelative
                updated.ps1Path = newPs1Path
                dialog.saved(updated)
            }
            dialog._depPendingSaved = null
            dialog.close()
        }
    }

    // ── FolderDialog ──────────────────────────────────────────────────────────
    FolderDialog {
        id: gameFolderDialog
        title: Settings.safeTr("select_game_folder")
        currentFolder: "file:///C:/"
        onAccepted: {
            var path = selectedFolder.toString().replace(/^file:\/\/\//, "")
            if (path.length > 0 && path[1] !== ":") path = "/" + path
            path = path.replace(/\\/g, "/")
            dialog.gameDirPath      = path
            dialog.exeList          = []
            dialog.selectedExe      = ""
            dialog._descRequestId   = ""
            dialog.exeScanning      = true
            dialog._scanResultReady = false
            dialog._scanPendingList = null
            var parts = path.split("/")
            dialog._folderBaseName = parts[parts.length - 1] || parts[parts.length - 2] || ""
            dialog._scanRequestId  = Date.now().toString()
            scanMinTimer.restart()
            TileManager.scanExeFiles(dialog._scanRequestId, path)
        }
    }

    // ── Публичный API ─────────────────────────────────────────────────────────
    function open(data) {
        tileData       = data
        nameField.text = data.title || ""

        if (data.type === "shortcut") {
            shortcutKind   = data.kind || "app"
            installChecked = (data.kind === "app") && isNew
        } else {
            shortcutKind   = ""
            installChecked = false
        }

        iconField.text = data.icon || ""

        if (data.type === "shortcut" && data.kind === "app" && !isNew) {
            _editingInstalled = data.installFolder ? true : false
            gameDirPath  = data.installFolder
                           ? (data.installFolder.replace(/\\/g, "/") + "/game") : ""
            exeList      = []
            selectedExe  = data.path ? data.path.replace(/\\/g, "/") : ""
            if (gameDirPath !== "") {
                exeScanning      = true
                _scanResultReady = false
                _scanPendingList = null
                _scanRequestId   = Date.now().toString()
                scanMinTimer.restart()
                TileManager.scanExeFiles(_scanRequestId, gameDirPath)
            }
        } else {
            _editingInstalled = false
            gameDirPath       = ""
            exeList           = []
            selectedExe       = ""
            exeScanning       = false
            _folderBaseName   = ""
            _descRequestId    = ""
            _scanRequestId    = ""
            appForm.nameText  = ""
        }

        webForm.text   = data.path || ""
        dialog.visible = true
        base.open()
    }

    function close()   { base.close() }
    function cancel()  { cancelled(); close() }
    function onFilePicked(filePath) { iconField.text = filePath }

    // ── BaseDialog ────────────────────────────────────────────────────────────
    BaseDialog {
        id: base
        // Три кнопки типа ярлыка должны помещаться с полями при любом кегле
        dialogWidth:  Math.max(Settings.us(580),
                               3 * (Math.ceil(kindMetrics.advanceWidth) + Settings.us(20) + 6 + Settings.us(36))
                               + 2 * 8 + 2 * Settings.us(28))
        dialogHeight: mainCol.implicitHeight + Settings.us(60)
        closeOnClickOutside: false
        onVisibleChanged: if (!visible) dialog.visible = false

        Column {
            id: mainCol
            width: parent.width
            spacing: Settings.us(14)

            // Заголовок
            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: 8
                Image {
                    source: "qrc:/assets/icons/ic_edit.png"
                    sourceSize.width: 24; sourceSize.height: 24
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: isNew
                          ? (tileData && tileData.type === "folder"
                             ? Settings.safeTr("new_folder") : Settings.safeTr("new_shortcut"))
                          : Settings.safeTr("edit_tile")
                    color: "white"; font.pixelSize: Settings.fs(22); font.bold: true
                }
            }

            // ── Выбор типа ярлыка (только при создании shortcut) ──────────────
            Column {
                width: parent.width
                visible: tileData && tileData.type === "shortcut" && isNew

                Text { width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14); text: Settings.safeTr("shortcut_type") }

                TextMetrics {
                    id: kindMetrics
                    font.pixelSize: Settings.fs(16); font.bold: true
                    text: {
                        var l = [Settings.safeTr("internet"), Settings.safeTr("application"),
                                 Settings.safeTr("playstation1")]
                        l.sort(function(a, b) { return b.length - a.length })
                        return l[0]
                    }
                }

                Row {
                    id: kindRow
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: [
                            { key: "web",          label: Settings.safeTr("internet"),    icon: "qrc:/assets/icons/ic_ethernet.png" },
                            { key: "app",          label: Settings.safeTr("application"), icon: "qrc:/assets/icons/ic_windows.png"  },
                            { key: "playstation1", label: Settings.safeTr("playstation1"),icon: "qrc:/assets/icons/ic_label.png"    }
                        ]
                        delegate: Item {
                            // Кнопки делят ширину поровну, подпись — с полями
                            width: Math.floor((kindRow.width - 2 * kindRow.spacing) / 3)
                            height: Settings.us(48)
                            scale: kindMA.containsMouse ? 1.04 : 1.0
                            Behavior on scale { NumberAnimation { duration: 130 } }

                            Rectangle {
                                anchors.fill: parent; radius: 10
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: dialog.shortcutKind === modelData.key ? Settings.accent(0.4) : Settings.accent(0.133) }
                                    GradientStop { position: 0.5; color: "#1A0A2D4A" }
                                    GradientStop { position: 1.0; color: dialog.shortcutKind === modelData.key ? Settings.accent(0.4) : Settings.accent(0.133) }
                                }
                            }
                            Rectangle {
                                id: kindBorder; anchors.fill: parent; radius: 10; color: "transparent"
                                border.color: dialog.shortcutKind === modelData.key ? Settings.accent(1.0) : Settings.accent(0.333)
                                border.width: 1.5
                            }
                            Glow {
                                cached: true
                                visible: opacity > 0   // невидимое свечение не считать каждый кадр
                                anchors.fill: kindBorder; source: kindBorder
                                radius: 14; samples: 20; color: Settings.accent(0.4)
                                opacity: (dialog.shortcutKind === modelData.key || kindMA.containsMouse) ? 1.0 : 0.0
                                Behavior on opacity { NumberAnimation { duration: 150 } }
                            }
                            Row {
                                anchors.centerIn: parent; spacing: 6
                                Image {
                                    source: modelData.icon
                                    width: Settings.us(20); height: Settings.us(20)
                                    sourceSize.width: 64; sourceSize.height: 64
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text  {
                                    text: modelData.label; color: "white"; font.pixelSize: Settings.fs(16); font.bold: true
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, kindRow.width / 3 - Settings.us(20) - 6 - Settings.us(24))
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                id: kindMA; anchors.fill: parent; hoverEnabled: true
                                onClicked: dialog.shortcutKind = modelData.key
                            }
                        }
                    }
                }
            }

            // ── Название (папка / web / редактирование app) ───────────────────
            Text {
                visible: !(tileData && tileData.type === "shortcut" && shortcutKind === "app" && isNew)
                width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14)
                text: Settings.safeTr("name")
            }
            TextField {
                id: nameField
                visible: !(tileData && tileData.type === "shortcut" && shortcutKind === "app" && isNew)
                width: parent.width; height: Settings.us(46)
                verticalAlignment: TextInput.AlignVCenter
                color: "white"; font.pixelSize: Settings.fs(16)
                placeholderText: tileData && tileData.type === "folder"
                                 ? Settings.safeTr("folder_name_ph") : Settings.safeTr("shortcut_name_ph")
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
                        border.color: nameField.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                        border.width: 1.5
                        Behavior on border.color { ColorAnimation { duration: 150 } }
                    }
                }
            }

            // ── Web / PS1 форма ───────────────────────────────────────────────
            WebShortcutForm {
                id: webForm
                width: parent.width
                visible: tileData && tileData.type === "shortcut" &&
                         (shortcutKind === "web" || shortcutKind === "playstation1")
                shortcutKind: dialog.shortcutKind
                fileBrowser:  dialog.fileBrowser
                ownerDialog:  dialog
            }

            // ── App форма ─────────────────────────────────────────────────────
            AppShortcutForm {
                id: appForm
                width: parent.width
                visible: tileData && tileData.type === "shortcut" && shortcutKind === "app"
                isNew:          dialog.isNew
                gameDirPath:    dialog.gameDirPath
                exeList:        dialog.exeList
                selectedExe:    dialog.selectedExe
                exeScanning:    dialog.exeScanning
                installChecked: dialog.installChecked

                onGameFolderRequested: gameFolderDialog.open()
                onExeSelected: function(relative) {
                    dialog.selectedExe = relative
                    if (dialog.isNew) {
                        var absPath = dialog.gameDirPath + "/" + relative
                        dialog._descRequestId = Date.now().toString()
                        TileManager.getExeDescription(dialog._descRequestId, absPath)
                    }
                }
                onInstallCheckToggled: function(newVal) { dialog.installChecked = newVal }
            }

            // ── Иконка ────────────────────────────────────────────────────────
            Text { width: parent.width; color: Settings.accent(1.0); font.pixelSize: Settings.fs(14); text: Settings.safeTr("icon_path") }
            Item {
                width: parent.width; height: Settings.us(46)
                TextField {
                    id: iconField
                    anchors.fill: parent
                    verticalAlignment: TextInput.AlignVCenter
                    color: "white"; font.pixelSize: Settings.fs(15)
                    placeholderText:      Settings.safeTr("icon_ph")
                    placeholderTextColor: Settings.accent(0.333)
                    leftPadding: 12; rightPadding: 50
                    background: Rectangle {
                        anchors.fill: parent; radius: 10; clip: true
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: Settings.accent(0.133) }
                            GradientStop { position: 0.5; color: Settings.accent(0.082) }
                            GradientStop { position: 1.0; color: Settings.accent(0.133) }
                        }
                        Rectangle {
                            anchors.fill: parent; radius: 10; color: "transparent"
                            border.color: iconField.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                            border.width: 1.5
                            Behavior on border.color { ColorAnimation { duration: 150 } }
                        }
                    }
                    Item {
                        width: 34; height: 34
                        anchors.right: parent.right; anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle {
                            anchors.fill: parent; radius: 7
                            color: browseIconMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.2)
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                        Image { anchors.centerIn: parent; source: "qrc:/assets/icons/ic_folder.png"; sourceSize.width: 18; sourceSize.height: 18 }
                        MouseArea {
                            id: browseIconMA; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (dialog.fileBrowser) {
                                    dialog._browseTarget = "icon"
                                    dialog.fileBrowser.openFor(dialog, "C:/", "png,jpg,jpeg,ico,bmp,svg")
                                }
                            }
                        }
                    }
                }
            }

            // Превью иконки
            Row {
                spacing: 12; height: 60
                visible: iconField.text !== ""
                Text { text: Settings.safeTr("preview"); color: Settings.accent(1.0); font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Rectangle {
                    width: 52; height: 52; radius: 8
                    color: Settings.accent(0.133); border.color: Settings.accent(0.333); border.width: 1
                    Image { anchors.fill: parent; anchors.margins: 4; source: iconField.text; fillMode: Image.PreserveAspectFit; sourceSize.width: 48; sourceSize.height: 48 }
                }
            }

            // ── Кнопки «Отмена» / «Сохранить» ────────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 16; bottomPadding: 4

                Item {
                    width: Math.max(Settings.us(160), btnLbl0.implicitWidth + Settings.us(32)); height: Settings.us(52)
                    Rectangle {
                        anchors.fill: parent; radius: 12
                        color: cancelMA.containsMouse ? Settings.accent(0.267) : Settings.accent(0.133)
                        border.color: Settings.accent(0.4); border.width: 1
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text { id: btnLbl0; anchors.centerIn: parent; text: Settings.safeTr("cancel"); color: "white"; font.pixelSize: Settings.fs(16) }
                    MouseArea { id: cancelMA; anchors.fill: parent; hoverEnabled: true; onClicked: dialog.cancel() }
                }

                Item {
                    width: Math.max(Settings.us(160), btnLbl1.implicitWidth + Settings.us(32)); height: Settings.us(52)
                    Rectangle {
                        anchors.fill: parent; radius: 12
                        color: saveMA.containsMouse ? Settings.accent(0.533) : Settings.accent(0.333)
                        border.color: Settings.accent(0.667); border.width: 1
                        Behavior on color { ColorAnimation { duration: 120 } }
                    }
                    Text { id: btnLbl1; anchors.centerIn: parent; text: Settings.safeTr("save"); color: "white"; font.pixelSize: Settings.fs(16); font.bold: true }
                    MouseArea {
                        id: saveMA; anchors.fill: parent; hoverEnabled: true
                        onClicked: {
                            var isNewApp       = tileData && tileData.type === "shortcut" && shortcutKind === "app" && isNew
                            var isEditInstalled= tileData && tileData.type === "shortcut" && shortcutKind === "app" && !isNew && dialog._editingInstalled

                            var titleText = isNewApp ? appForm.nameText.trim() : nameField.text.trim()
                            if (titleText === "") return
                            if (isNewApp && (gameDirPath === "" || selectedExe === "")) return
                            if (isEditInstalled && selectedExe === "") return

                            var updated = {
                                type:  tileData ? (tileData.type || "") : "",
                                kind:  (tileData && tileData.type === "shortcut") ? dialog.shortcutKind : "",
                                id:    tileData ? (tileData.id   || "") : "",
                                title: titleText,
                                icon:  iconField.text.trim(),
                                path:  shortcutKind === "app"
                                       ? selectedExe
                                       : webForm.text.trim()
                            }

                            if (isEditInstalled) {
                                var installFolder = tileData.installFolder
                                                    ? tileData.installFolder.replace(/\\/g, "/") : ""
                                if (installFolder === "" || selectedExe === "") {
                                    dialog._finalizeSave(updated); return
                                }
                                dialog._depPendingSaved = updated
                                dialog.depChecking      = true
                                TileManager.updateLaunchExe(updated.id, installFolder, selectedExe)
                                return
                            }

                            if (isNewApp && installChecked) {
                                var newId = Date.now().toString() + Math.random().toString().substring(2)
                                updated.id              = newId
                                updated.installing      = true
                                updated.installProgress = 0.0
                                updated._pendingInstall = true
                                dialog._finalizeSave(updated)
                                return
                            }

                            if (isNewApp && !installChecked) {
                                var newIdExt = Date.now().toString() + Math.random().toString().substring(2)
                                updated.id   = newIdExt
                                updated.kind = "external"
                                updated.path = ""
                                var absExe   = gameDirPath + "/" + selectedExe
                                dialog.saved(updated)
                                dialog.close()
                                TileManager.createExternalShortcut(
                                    updated.id, updated.title, absExe, updated.icon,
                                    SettingsManager.installPath
                                )
                                return
                            }

                            dialog._finalizeSave(updated)
                        }
                    }
                }
            }
        }
    }

    // ── Оверлей проверки зависимостей ─────────────────────────────────────────
    Rectangle {
        id: depOverlay
        anchors.fill: base; radius: 20; color: "#CC000000"
        visible: depChecking
        opacity: depChecking ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        Column {
            anchors.centerIn: parent; spacing: 16
            Spinner { running: depChecking; size: 32; anchors.horizontalCenter: parent.horizontalCenter }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Settings.safeTr("checking_deps"); color: "white"; font.pixelSize: Settings.fs(15)
            }
        }
    }

    // ── Финальный шаг сохранения ──────────────────────────────────────────────
    function _finalizeSave(updatedData) {
        if (updatedData && updatedData._pendingInstall) {
            TileManager.startInstallation(
                updatedData.id, updatedData.title,
                gameDirPath, selectedExe,
                updatedData.icon, SettingsManager.installPath
            )
            delete updatedData._pendingInstall
        }
        dialog.saved(updatedData)
        dialog.close()
    }
}
