// Main.qml
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import Qt5Compat.GraphicalEffects
import AppSettings 1.0
import QtQuick.Effects
import "base"
import "dialogs"
import "screens"
import "settings"
import "tiles"
import "widgets"


Window {
    id: root
    width: 1280; height: 720
    visible: true
    color: "black"
    title: "Media Center"

    visibility: Settings.fullscreen ? Window.FullScreen : Window.Windowed

    Shortcut { sequence: "F11";         onActivated: Settings.fullscreen = !Settings.fullscreen }
    Shortcut { sequence: "Back";        onActivated: root.goBack() }
    Shortcut { sequence: "BrowserBack"; onActivated: root.goBack() }

    // Escape — открыть/закрыть оверлей когда AndroidTV запущен,
    // иначе стандартная навигация назад.
    Shortcut {
        sequence: "Escape"
        onActivated: {
            if (!OverlayModule) { root.goBack(); return }
            if (OverlayModule.visible) {
                OverlayModule.hide()
            } else if (AndroidTV?.running) {
                OverlayModule.show("androidtv")
            } else {
                root.goBack()
            }
        }
    }

    FontLoader { id: appFont; source: "qrc:/fonts/Montserrat-SemiBold.ttf" }

    Component { id: dynamicScreenComponent;  DynamicScreen  {} }
    Component { id: karaokeScreenComponent;   KaraokeScreen  {} }
    Component { id: settingsScreenComponent; SettingsScreen {} }
    Component { id: libraryScreenComponent; GameLibraryScreen {} }
    Component { id: gameDetailComponent;     GameDetailScreen {} }

    property bool   settingsOpen:    false
    property bool libraryOpen: false
    property string launchingTileId: ""
    property bool   gameLaunching:   false
    property bool   gameRunning:     false

    // Флаг для предотвращения повторного запуска анимации затемнения эмулятора
    property bool emulatorFadeInProgress: false

    readonly property bool onGamesScreen: stack.currentItem !== null
                                          && stack.currentItem.storageKey === "games"
                                          && !stack.currentItem.isFolder

    Component.onCompleted: {
        if (typeof JoystickModule !== "undefined" && JoystickModule)
            JoystickModule.activate()
    }

    Image {
        id: bgImage
        anchors.fill: parent
        source: Settings.backgroundImage
        fillMode: Image.PreserveAspectCrop
    }

    FastBlur {
        anchors.fill: parent
        source: bgImage
        radius: 20
    }

    // ── Анимированная дымка (ShaderEffect) ─────────────────────────────────
    ShaderEffect {
        id: mistEffect
        anchors.fill: parent   // растягиваем на весь экран
        z: 0
        blending: true         // алфа-наложение поверх фона, под всем UI
        visible: Settings.enableFogShader

        // Uniform-переменные, пробрасываемые в шейдер
        property real time:  0.0
        property real scale: 2.5
        property real speed: 0.35

        // ~30 fps, медленное накопление времени
        Timer {
            interval: 33
            running:  true
            repeat:   true
            onTriggered: mistEffect.time += 0.033
        }

        vertexShader:   "qrc:/assets/shaders/mist/mist.vert.qsb"
        fragmentShader: "qrc:/assets/shaders/mist/mist.frag.qsb"

        Component.onCompleted: {
            if (mistEffect.status === ShaderEffect.Error)
                console.error("ShaderEffect failed to load shaders!")
            else
                console.log("ShaderEffect initialized, status:", mistEffect.status)
        }

        onStatusChanged: {
            console.log("ShaderEffect status:", mistEffect.status)
            if (mistEffect.status === ShaderEffect.Error)
                console.error("ShaderEffect error!")
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.25
    }

    property string currentTime: new Date().toLocaleTimeString(Qt.locale(), "hh:mm")
    Timer {
        interval: 1000; running: true; repeat: true
        onTriggered: currentTime = new Date().toLocaleTimeString(Qt.locale(), "hh:mm")
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: Settings.us(20)
        text: currentTime
        font.family: appFont.name; font.pixelSize: Settings.us(64)
        color: "white"
        z: 10
    }

    Item {
        anchors.top: parent.top; anchors.right: parent.right
        anchors.topMargin: Settings.us(20); anchors.rightMargin: Settings.us(20)
        width: topRow.width; height: topRow.height; z: 10

        Row {
            id: topRow; spacing: Settings.us(20)

            JoystickBar { id: joystickBar }
            G72BatteryWidget {}
            IconButton {
                source: "qrc:/assets/icons/ic_settings.png"
                active: root.settingsOpen
                onClicked: root.settingsOpen ? root.goBack() : root.openSettingsScreen()
            }
            IconButton {
                source: "qrc:/assets/icons/ic_lib.png"
                active: root.libraryOpen
                onClicked: root.libraryOpen ? root.goBack() : root.openLibraryScreen()
            }
            PowerMenu { id: powerMenu }
        }
    }

    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 200
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Settings.accent(0.5) }
            GradientStop { position: 1.0; color: Settings.accent(0.0) }
        }
        visible: stack.depth > 1
        opacity: (stack.depth > 1 && backMA.containsMouse) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    Item {
        id: leftPanel
        anchors.top: parent.top; anchors.bottom: parent.bottom
        width: 120; z: 2
        x: stack.depth > 1 ? 0 : -width
        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Image {
            source: "qrc:/assets/icons/ic_back.png"
            sourceSize.width: 100; sourceSize.height: 100
            width: Settings.us(64); height: Settings.us(64); smooth: true
            fillMode: Image.PreserveAspectFit
            anchors.left: parent.left; anchors.leftMargin: Settings.us(20)
            anchors.verticalCenter: parent.verticalCenter
        }
        MouseArea { id: backMA; anchors.fill: parent; hoverEnabled: true; onClicked: root.goBack() }
    }

    property string currentScreenTitle: stack.currentItem ? stack.currentItem.screenTitle : qsTr("Home")
    onCurrentScreenTitleChanged: updateTitleWithAnimation(currentScreenTitle)

    Text {
        id: screenTitle
        font.family: appFont.name; font.pixelSize: Settings.fs(36); color: "white"
        anchors.horizontalCenter: parent.horizontalCenter
        y: Settings.us(20) + Settings.us(64) - 100; opacity: 0
        z: 10

        SequentialAnimation { id: hideAnim
            ParallelAnimation {
                NumberAnimation { target: screenTitle; property: "y";       to: Settings.us(20) + Settings.us(64) - 60; duration: 200; easing.type: Easing.InCubic }
                NumberAnimation { target: screenTitle; property: "opacity"; to: 0;   duration: 150; easing.type: Easing.InCubic }
            }
        }

        SequentialAnimation { id: showAnim; property string newTitle
            ScriptAction { script: { screenTitle.text = showAnim.newTitle; screenTitle.y = Settings.us(20) + Settings.us(64) - 60; screenTitle.opacity = 0 } }
            ParallelAnimation {
                NumberAnimation { target: screenTitle; property: "y";       to: Settings.us(20) + Settings.us(64) + Settings.us(16); duration: 250; easing.type: Easing.OutCubic }
                NumberAnimation { target: screenTitle; property: "opacity"; to: 1;  duration: 200; easing.type: Easing.OutCubic }
            }
        }

        SequentialAnimation { id: replaceAnim; property string newTitle
            ParallelAnimation {
                NumberAnimation { target: screenTitle; property: "y";       to: Settings.us(20) + Settings.us(64) - 60; duration: 200; easing.type: Easing.InCubic }
                NumberAnimation { target: screenTitle; property: "opacity"; to: 0;   duration: 150; easing.type: Easing.InCubic }
            }
            ScriptAction { script: { screenTitle.text = replaceAnim.newTitle; screenTitle.y = Settings.us(20) + Settings.us(64) - 60; screenTitle.opacity = 0 } }
            ParallelAnimation {
                NumberAnimation { target: screenTitle; property: "y";       to: Settings.us(20) + Settings.us(64) + Settings.us(16); duration: 250; easing.type: Easing.OutCubic }
                NumberAnimation { target: screenTitle; property: "opacity"; to: 1;  duration: 200; easing.type: Easing.OutCubic }
            }
        }
    }

    function updateTitleWithAnimation(newTitle) {
        var hiddenTitles = [
            qsTr("Home"), Settings.tr("settings"), Settings.tr("library")
        ]
        if (hiddenTitles.indexOf(newTitle) !== -1) {
            if (screenTitle.opacity > 0) hideAnim.start()
        } else {
            if (screenTitle.opacity > 0) { replaceAnim.newTitle = newTitle; replaceAnim.start() }
            else                         { showAnim.newTitle    = newTitle; showAnim.start()    }
        }
    }

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: HomeScreen {}
        background: Item {}

        pushEnter:  Transition { ParallelAnimation {
            NumberAnimation { property: "y";       from: root.height/3;     to: 0; duration: 500 }
            NumberAnimation { property: "opacity"; from: 0; to: 1;               duration: 400 }
        }}
        pushExit:   Transition { ParallelAnimation {
            NumberAnimation { property: "y";       from: 0; to: -root.height*3/4; duration: 500 }
            NumberAnimation { property: "opacity"; from: 1; to: 0;               duration: 250 }
        }}

        popEnter:   Transition { ParallelAnimation {
            NumberAnimation { property: "y";       from: -root.height/3;    to: 0; duration: 500 }
            NumberAnimation { property: "opacity"; from: 0; to: 1;               duration: 400 }
        }}
        popExit:    Transition { ParallelAnimation {
            NumberAnimation { property: "y";       from: 0; to: root.height*3/4;  duration: 500 }
            NumberAnimation { property: "opacity"; from: 1; to: 0;               duration: 250 }
        }}

        onBusyChanged:        if (!busy) Qt.callLater(forceHoverCheckOnCurrent)
        onCurrentItemChanged: {
            if (currentItem) {
                root.settingsOpen = (currentItem.screenTitle === Settings.tr("settings"))
                root.libraryOpen  = (currentItem.screenTitle === Settings.tr("library"))
            }
        }
    }

    Rectangle {
        id: transitionOverlay
        anchors.fill: parent
        color: "black"; opacity: 0; z: 500
        visible: opacity > 0
    }

    NumberAnimation {
        id: fadeToBlack
        target: transitionOverlay; property: "opacity"
        from: 0; to: 1; duration: 600; easing.type: Easing.InQuad
    }

    SequentialAnimation {
        id: fadeInMediaCenter
        ScriptAction {
            script: {
                AppController.hideBlackOverlay()
                root.raise()
                root.requestActivate()
                transitionOverlay.opacity = 1
            }
        }
        PauseAnimation { duration: 80 }
        NumberAnimation {
            target: transitionOverlay; property: "opacity"
            from: 1; to: 0; duration: 700; easing.type: Easing.OutCubic
        }
        onFinished: {
            root.gameLaunching   = false
            root.launchingTileId = ""
            root.emulatorFadeInProgress = false
        }
    }

    NumberAnimation {
        id: fadeToBlackEmulator
        target: transitionOverlay
        property: "opacity"
        from: 0; to: 1
        duration: 600
        easing.type: Easing.InQuad
        onStarted: {
            root.emulatorFadeInProgress = true
        }
        onFinished: {
            if (AndroidTV?.running && !AndroidTV?.switching) {
                AndroidTV.applyFullscreenAndSwitch()
            }
            root.emulatorFadeInProgress = false
        }
    }

    Connections {
        target: AppController

        function onGameWindowAppeared(tileId, windowFound) {
            root.gameLaunching = false
            root.gameRunning   = true
            if (windowFound) {
                fadeToBlack.start()
            }
        }

        function onGameClosed(tileId) {
            root.gameRunning = false
            if (transitionOverlay.opacity > 0 || fadeToBlack.running) {
                fadeToBlack.stop()
                fadeInMediaCenter.start()
            } else {
                root.gameLaunching   = false
                root.launchingTileId = ""
                AppController.hideBlackOverlay()
            }
        }

        function onLaunchFailed(tileId, reason) {
            root.gameLaunching   = false
            root.launchingTileId = ""
            root.gameRunning     = false
            AppController.hideBlackOverlay()
            fadeToBlack.stop()
            fadeInMediaCenter.stop()
            transitionOverlay.opacity = 0
            console.warn("Launch failed for", tileId, ":", reason)
        }
    }

    Connections {
        target: AndroidTV

        function onEmulatorReadyBehind() {
            if (root.gameRunning) return
            if (root.emulatorFadeInProgress) {
                console.log("Emulator fade already in progress, ignoring")
                return
            }
            fadeToBlackEmulator.start()
        }

        function onEmulatorRestoringMediaStation() {
            if (root.gameRunning) return
            if (transitionOverlay.opacity > 0 || fadeToBlackEmulator.running) {
                fadeToBlackEmulator.stop()
                fadeInMediaCenter.start()
            } else {
                AppController.hideBlackOverlay()
            }
        }
    }

    Connections {
        target: G72Keys

        function onHomePressed() {
            if (stack.depth > 1) {
                stack.pop(null)
            }
        }

        function onBackPressed() {
            root.goBack()
        }

        function onSearchPressed() {
            console.log("Search pressed — feature not implemented yet")
        }

        function onMenuPressed() {
            if (powerMenu && typeof powerMenu.open === "function") {
                powerMenu.open()
            } else {
                if (powerMenu) powerMenu.visible = true
            }
        }

        function onUpPressed()    {}
        function onDownPressed()  {}
        function onLeftPressed()  {}
        function onRightPressed() {}
        function onSelectPressed(){}
    }

    function launchGame(path, tileId) {
        root.launchingTileId = tileId
        root.gameLaunching   = true
        AppController.showBlackOverlay()
        AppController.launch(path, tileId)
    }

    function forceHoverCheckOnCurrent() {
        var cur = stack.currentItem
        if (cur && typeof cur.forceHoverCheck === "function") cur.forceHoverCheck()
    }

    function openScreen(title, storageKey, isFolder) {
        var screen = dynamicScreenComponent.createObject(stack, {
            storageKey:      storageKey,
            isFolder:        isFolder,
            screenTitle:     title,
            launchGame:      root.launchGame,
            openScreen:      root.openScreen,
            openGameDetail:  root.openGameDetail,
            launchingTileId: Qt.binding(function() { return root.launchingTileId }),
            gameLaunching:   Qt.binding(function() { return root.gameLaunching })
        })
        stack.push(screen)
    }

    // Фаза 2: открыть страницу игры
    function openGameDetail(gameData) {
        var screen = gameDetailComponent.createObject(stack, {
            gameData:   gameData,
            launchGame: root.launchGame
        })
        stack.push(screen)
    }

    function openSettingsScreen() {
        if (root.settingsOpen) return
        root.settingsOpen = true
        stack.push(settingsScreenComponent.createObject(stack, {
            appWindow: root
        }))
    }

    function openLibraryScreen() {
        if (root.libraryOpen) return
        root.libraryOpen = true
        stack.push(libraryScreenComponent.createObject(stack, {}))
    }

    function goBack() {
        var cur = stack.currentItem
        if (cur && typeof cur.closeDialogs === "function" && cur.closeDialogs()) return
        if (stack.depth > 1) {
            if (stack.currentItem && stack.currentItem.screenTitle === Settings.tr("settings"))
                root.settingsOpen = false
            if (stack.currentItem && stack.currentItem.screenTitle === Settings.tr("library"))
                root.libraryOpen = false
            stack.pop()
        }
    }

    function _updateTileInAllScreens(tileId, props) {
        for (var d = 0; d < stack.depth; d++) {
            var item = stack.get(d)
            if (!item || !item.hasOwnProperty("tilesModel")) continue
            for (var i = 0; i < item.tilesModel.count; i++) {
                if (item.tilesModel.get(i).id === tileId) {
                    for (var key in props)
                        item.tilesModel.setProperty(i, key, props[key])
                    break
                }
            }
        }
    }

    function _removeTileFromAllScreens(tileId) {
        for (var d = 0; d < stack.depth; d++) {
            var item = stack.get(d)
            if (!item || !item.hasOwnProperty("tilesModel")) continue
            for (var i = 0; i < item.tilesModel.count; i++) {
                if (item.tilesModel.get(i).id === tileId) {
                    item.tilesModel.remove(i)
                    if (typeof item.saveAll === "function") item.saveAll()
                    break
                }
            }
        }
    }

    InstallRequestDialog {
        id: installRequestDialog

        onAccepted: {
            var targetStorageKey = installRequestDialog.targetStorageKey
            var targetIsFolder   = installRequestDialog.targetIsFolder
            var targetTitle      = installRequestDialog.targetTitle
            var title            = installRequestDialog.tileTitle
            var kind             = installRequestDialog.tileKind
            var exePath          = installRequestDialog.exePath
            var iconPath         = installRequestDialog.iconPath
            var socket           = installRequestDialog.socket

            var tileId = "net_" + Date.now() + "_" + Math.random().toString(36).substring(2, 8)

            var duplicate = false
            for (var d = 0; d < stack.depth; d++) {
                var item = stack.get(d)
                if (!item || !item.hasOwnProperty("tilesModel")) continue
                for (var i = 0; i < item.tilesModel.count; i++) {
                    var t = item.tilesModel.get(i)
                    if (t.title === title && t.type === "shortcut") {
                        duplicate = true; break
                    }
                }
                if (duplicate) break
            }

            if (duplicate) {
                root.promptDialog.show({
                    title: "",
                    message: Settings.tr("already_installed_msg", { title: title }),
                    icon: "qrc:/assets/icons/ic_error.png",
                    buttons: [
                        { label: Settings.tr("cancel"), role: "cancel",
                          action: function() { if (socket) NetworkListener.rejectInstall(socket) } },
                        { label: Settings.tr("install"), role: "primary",
                          action: function() {
                              _doAddTileAndAccept(tileId, title, kind, exePath, iconPath, socket,
                                                  targetStorageKey, targetIsFolder, targetTitle)
                          }}
                    ]
                })
                return
            }

            _doAddTileAndAccept(tileId, title, kind, exePath, iconPath, socket,
                                targetStorageKey, targetIsFolder, targetTitle)
        }
    }

    // Глобальный PromptDialog — доступен дочерним экранам через root.promptDialog
    property alias promptDialog: promptDialog

    PromptDialog {
        id: promptDialog
    }

    function _doAddTileAndAccept(tileId, title, kind, exePath, iconPath,
                                  socket, targetStorageKey, targetIsFolder, targetTitle)
    {
        function addTile(screenItem) {
            if (!screenItem || !screenItem.hasOwnProperty("tilesModel")) {
                console.warn("Target screen not available for tileId:", tileId)
                return
            }
            screenItem.tilesModel.append({
                type:            "shortcut",
                kind:            kind,
                title:           title,
                icon:            iconPath,
                path:            "",
                id:              tileId,
                installFolder:   "",
                installing:      true,
                installProgress: 0.0,
                installPhase:    "packing"
            })
            screenItem.saveAll()
        }

        var cur = stack.currentItem
        if (cur && cur.storageKey === targetStorageKey && cur.isFolder === targetIsFolder) {
            addTile(cur)
            NetworkListener.acceptInstall(socket, tileId)
        } else {
            root.openScreen(targetTitle, targetStorageKey, targetIsFolder)
            var wait = function() {
                var c = stack.currentItem
                if (c && c.storageKey === targetStorageKey && c.isFolder === targetIsFolder) {
                    stack.currentItemChanged.disconnect(wait)
                    addTile(c)
                    NetworkListener.acceptInstall(socket, tileId)
                }
            }
            stack.currentItemChanged.connect(wait)
            Qt.callLater(wait)
        }
    }

    function openKaraokeScreen() {
        if (stack.currentItem && stack.currentItem.screenTitle === "Karaoke")
            return
        stack.push(karaokeScreenComponent.createObject(stack))
    }

    function openAndroidTVScreen() {
        if (stack.currentItem && stack.currentItem.screenTitle === Settings.tr("android_tv"))
            return
        stack.push(androidTVScreenComponent)
    }

    Connections {
        target: NetworkListener

        function onShowInstallDialog(title, kind, exeRelativePath, iconRelativePath, socket) {
            installRequestDialog.open({
                title:    title,
                kind:     kind,
                exePath:  exeRelativePath,
                iconPath: iconRelativePath,
                socket:   socket
            })
        }

        function onInstallProgress(tileId, progress, phase) {
            root._updateTileInAllScreens(tileId, {
                installProgress: progress,
                installPhase:    phase,
                installing:      true
            })
        }

        function onServerCancelledInstall(tileId) {
            root._removeTileFromAllScreens(tileId)
        }
    }

    Notification { id: notification }

    OverlayScreen {}

    component IconButton: Item {
        width: Settings.us(64); height: Settings.us(64)
        required property string source
        property bool active: false
        signal clicked()

        Rectangle {
            anchors.fill: parent; radius: 10
            gradient: Gradient {
                GradientStop { position: 0.0; color: parent.active ? Settings.accent(0.4) : "#33FFFFFF" }
                GradientStop { position: 0.5; color: parent.active ? Settings.accent(0.2) : "#AA202020" }
                GradientStop { position: 1.0; color: parent.active ? Settings.accent(0.4) : "#33FFFFFF" }
            }
        }
        Rectangle {
            id: ibBorder
            anchors.fill: parent; radius: 10; color: "transparent"
            border.color: parent.active ? Settings.accent(1.0) : "#80FFFFFF"; border.width: 2
        }
        Glow {
            anchors.fill: ibBorder; source: ibBorder
            radius: 16; samples: 24; color: Settings.accent(1.0)
            opacity: (ibMA.containsMouse || parent.active) ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
        Image {
            anchors.centerIn: parent
            width: Settings.us(40); height: Settings.us(40)
            source: parent.source
            sourceSize.width: 100; sourceSize.height: 100
            smooth: true; fillMode: Image.PreserveAspectFit
        }
        MouseArea { id: ibMA; anchors.fill: parent; hoverEnabled: true; onClicked: parent.clicked() }
    }

    UpdateAvailableDialog {}
}
