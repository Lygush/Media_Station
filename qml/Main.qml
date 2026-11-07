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
        // Удержание Esc не должно открывать и закрывать оверлей по кругу
        autoRepeat: false
        onActivated: {
            if (!OverlayModule) { root.goBack(); return }
            if (OverlayModule.visible) {
                OverlayModule.hide()
            } else if (AndroidTV?.active) {
                // Оверлей Android TV — только когда эмулятор показан на экране
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
        // Обои бывают 6000×4000: без sourceSize это ~96 МБ текстуры и синхронное
        // декодирование на старте. Фон всё равно размыт — хватает размера экрана.
        sourceSize: Qt.size(Screen.width, Screen.height)
        asynchronous: true
    }

    FastBlur {
        cached: true
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

        // Дымка размытая — считаем её в текстуре вдвое меньшей по каждой
        // стороне и растягиваем: работы шейдера в 4 раза меньше, на глаз
        // не отличить (тонкие струйки ещё шире 1 пикселя текстуры).
        layer.enabled:     visible
        layer.smooth:      true
        layer.textureSize: Qt.size(Math.max(1, Math.round(width / 2)),
                                   Math.max(1, Math.round(height / 2)))

        // ~30 кадров/с. Время — по часам, а не «+0.033 за срабатывание»:
        // при подтормаживании дымка не замирает и не дёргается. Заворот раз в
        // 3 часа (шум в шейдере устойчив к большим значениям, но float
        // uniform — нет). FrameAnimation здесь не используется сознательно:
        // она перерисовывала бы всё окно с частотой экрана (60–144 Гц).
        // Дымка анимируется только когда её видно: под игрой / Android TV и
        // при выключенной дымке окно медиацентра не перерисовывается.
        property double _startMs: Date.now()
        Timer {
            interval: 33
            running:  mistEffect.visible && !root.gameRunning
                      && Qt.application.state === Qt.ApplicationActive
                      && !(typeof AndroidTV !== "undefined" && AndroidTV && AndroidTV.active)
            repeat:   true
            onTriggered: mistEffect.time = ((Date.now() - mistEffect._startMs) / 1000.0) % 10800.0
        }

        vertexShader:   "qrc:/assets/shaders/mist/mist.vert.qsb"
        fragmentShader: "qrc:/assets/shaders/mist/mist.frag.qsb"

        // В журнал — только ошибка шейдера (раньше при каждом запуске три
        // отладочные строки «status: 0»)
        onStatusChanged: {
            if (mistEffect.status === ShaderEffect.Error)
                console.error("ShaderEffect error:", mistEffect.log)
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

    // Часы по центру; если правая панель (геймпады, кнопки) до них дотягивается
    // (узкое окно, крупный масштаб) — сдвигаются влево, не наезжая на неё
    Text {
        id: clockText
        x: Math.max(Settings.us(20),
                    Math.min(Math.round((parent.width - width) / 2),
                             topRightPanel.x - width - Settings.us(24)))
        anchors.top: parent.top
        anchors.topMargin: Settings.us(20)
        text: currentTime
        font.family: appFont.name; font.pixelSize: Settings.us(64)
        color: "white"
        z: 10
    }

    Item {
        id: topRightPanel
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

    // Нижняя граница шапки (часы + заголовок экрана). Экраны начинают контент
    // ниже неё — раньше сетка плиток заезжала под часы и заголовок.
    readonly property int headerHeight: Settings.us(100) + Math.ceil(Settings.fs(36) * 1.4) + Settings.us(16)

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
        // Экран может сам показывать своё название (карточка игры) — тогда в
        // шапке его не дублируем: длинное название налезало на значки справа.
        var selfTitled = stack.currentItem && stack.currentItem.hideHeaderTitle === true
        if (selfTitled || hiddenTitles.indexOf(newTitle) !== -1) {
            if (screenTitle.opacity > 0) hideAnim.start()
        } else {
            if (screenTitle.opacity > 0) { replaceAnim.newTitle = newTitle; replaceAnim.start() }
            else                         { showAnim.newTitle    = newTitle; showAnim.start()    }
        }
    }

    StackView {
        id: stack
        objectName: "mainStack"   // для сторожа отзывчивости в main.cpp
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

        onBusyChanged: {
            if (!busy) {
                Qt.callLater(forceHoverCheckOnCurrent)
                Qt.callLater(root._destroyRemovedScreens)
            }
        }
        onDepthChanged: if (!busy) Qt.callLater(root._destroyRemovedScreens)
        onCurrentItemChanged: {
            if (currentItem) {
                console.debug("[UI] screen:", currentItem.screenTitle || "home", "depth", depth)
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
                // При неудаче C++ пошлёт emulatorRestoringMediaStation → затемнение уйдёт
                AndroidTV.applyFullscreenAndSwitch()
            } else if (!AndroidTV?.switching) {
                // Эмулятор успел закрыться, пока шло затемнение — не оставляем чёрный экран
                fadeInMediaCenter.start()
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
            NotificationModule.post(Settings.tr("launch_failed") + ": "
                                    + root._launchFailReason(reason), "error", true, 7000)
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
            if (fadeInMediaCenter.running) return   // сигнал может прийти дважды (stop + exit)
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
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            if (stack.depth > 1) {
                stack.pop(null)
            }
        }

        function onBackPressed() {
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            root.goBack()
        }

        function onSearchPressed() {
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            // Кнопка «Поиск» на пульте — быстрый переход в библиотеку игр
            if (!root.settingsOpen && !root.libraryOpen) root.openLibraryScreen()
        }

        function onMenuPressed() {
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            // Раньше здесь ставилось powerMenu.visible = true (а меню и так
            // видимо — открывается по menuVisible), кнопка ничего не делала.
            powerMenu.toggle()
        }

        // Стрелки пульта переводят рамку фокуса (RemoteNavigator, C++).
        // Если в этом направлении ничего нет — прокрутка экрана, как раньше:
        // так открываются плитки, которые были за краем.
        function onUpPressed() {
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            if (RemoteNav.move("up")) return
            if (stack.currentItem && typeof stack.currentItem.scrollBy === "function")
                stack.currentItem.scrollBy(-1)
        }
        function onDownPressed() {
            if (root.gameRunning) return   // пульт управляет игрой, а не скрытым меню
            if (RemoteNav.move("down")) return
            if (stack.currentItem && typeof stack.currentItem.scrollBy === "function")
                stack.currentItem.scrollBy(1)
        }
        // Влево/вправо в поле ввода двигают курсор в тексте
        function onLeftPressed() {
            if (root.gameRunning || RemoteNav.textInputFocused()) return
            RemoteNav.move("left")
        }
        function onRightPressed() {
            if (root.gameRunning || RemoteNav.textInputFocused()) return
            RemoteNav.move("right")
        }
        // OK/Enter — щелчок по элементу под рамкой. Без рамки ничего не делаем:
        // кнопка ОК аэромыши — это обычный щелчок мыши.
        function onSelectPressed() {
            if (root.gameRunning) return
            RemoteNav.activate()
        }
    }

    // ── Рамка фокуса для навигации стрелками ───────────────────────────────
    // Выше диалогов (z 500) и всплывающих окон Qt Quick Controls.
    Rectangle {
        id: remoteFocusRing
        z: 2000000
        readonly property real pad: Settings.us(4)
        visible: RemoteNav.active && !root.gameRunning
        x: RemoteNav.targetRect.x - pad
        y: RemoteNav.targetRect.y - pad
        width:  RemoteNav.targetRect.width  + 2 * pad
        height: RemoteNav.targetRect.height + 2 * pad
        radius: Settings.us(12)
        color: "transparent"
        border.width: Math.max(2, Settings.us(3))
        border.color: Settings.accent(1.0)
        Behavior on x      { enabled: remoteFocusRing.visible; NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        Behavior on y      { enabled: remoteFocusRing.visible; NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        Behavior on width  { enabled: remoteFocusRing.visible; NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        Behavior on height { enabled: remoteFocusRing.visible; NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }

    // Причины из GameWatcher (C++) — техническим английским; пользователю — коротко
    function _launchFailReason(reason) {
        var r = String(reason || "")
        if (r.indexOf("did not finish") >= 0)  return Settings.tr("launch_fail_previous")
        if (r.indexOf("PowerShell") >= 0)      return Settings.tr("launch_fail_script")
        if (r.indexOf("no PID") >= 0)          return Settings.tr("launch_fail_no_process")
        if (r.indexOf("exited") >= 0)          return Settings.tr("launch_fail_exited")
        return r
    }

    function launchGame(path, tileId) {
        // Отслеживаются (затемнение, скрытие панели задач, ожидание окна) только
        // игры, запускаемые через launch.ps1. Ссылки, .exe и прочие ярлыки
        // AppController просто открывает — раньше для них экран оставался
        // затемнённым до срабатывания watchdog (20 с).
        if (!String(path || "").toLowerCase().endsWith(".ps1")) {
            AppController.launch(path, tileId)
            return
        }
        root.launchingTileId = tileId
        root.gameLaunching   = true
        AppController.showBlackOverlay()
        AppController.launch(path, tileId)
    }

    function forceHoverCheckOnCurrent() {
        var cur = stack.currentItem
        if (cur && typeof cur.forceHoverCheck === "function") cur.forceHoverCheck()
    }

    // ── Владение экранами ────────────────────────────────────────────────────
    // Экраны создаются createObject() и кладутся в StackView готовыми.
    // StackView уничтожает только то, что создал сам, поэтому раньше каждый
    // закрытый экран оставался жить скрытым: со своими MediaPlayer (трейлер на
    // странице игры продолжал крутиться по таймеру), таймерами и подписками на
    // модули. С каждым открытием караоке/страницы игры интерфейс тормозил всё
    // сильнее. Теперь экран уничтожается после того, как ушёл со стека.
    property var _ownedScreens: []
    function _own(item) {
        if (item) _ownedScreens.push(item)
        return item
    }
    // Создание экрана с замером: долгие (>80 мс) пишутся в журнал — вместе со
    // строками «interface was unresponsive» это показывает, что тормозит
    function _createScreen(component, props) {
        var t0 = Date.now()
        var item = props ? component.createObject(stack, props) : component.createObject(stack)
        var ms = Date.now() - t0
        if (ms > 80)
            console.info("[UI] screen created in", ms, "ms:", item ? (item.screenTitle || "?") : "failed")
        return _own(item)
    }
    function _destroyRemovedScreens() {
        if (stack.busy) return
        var inStack = []
        for (var d = 0; d < stack.depth; d++) inStack.push(stack.get(d, StackView.DontLoad))
        var keep = []
        for (var i = 0; i < _ownedScreens.length; i++) {
            var it = _ownedScreens[i]
            if (!it) continue
            if (inStack.indexOf(it) >= 0) keep.push(it)
            else it.destroy()
        }
        _ownedScreens = keep
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
        stack.push(_own(screen))
    }

    // Фаза 2: открыть страницу игры
    function openGameDetail(gameData) {
        var screen = gameDetailComponent.createObject(stack, {
            gameData:   gameData,
            launchGame: root.launchGame
        })
        stack.push(_own(screen))
    }

    function openSettingsScreen() {
        if (root.settingsOpen) return
        root.settingsOpen = true
        stack.push(_own(settingsScreenComponent.createObject(stack, {
            appWindow: root
        })))
    }

    function openLibraryScreen() {
        if (root.libraryOpen) return
        root.libraryOpen = true
        stack.push(_createScreen(libraryScreenComponent, {}))
    }

    function goBack() {
        // «Назад» сначала закрывает то, что открыто поверх экрана
        if (powerMenu.menuVisible) { powerMenu.menuVisible = false; return }
        if (root.promptDialog && root.promptDialog.visible
            && typeof root.promptDialog.close === "function") { root.promptDialog.close(); return }
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

        // Раньше принятие установки открывало целевой экран — пользователя
        // «выкидывало» из настроек или со страницы игры. Теперь плитка
        // добавляется туда, где она должна быть, а экран не переключается.
        // Путь после установки C++ сохранит сам (TileManager::persistInstallResult),
        // прогресс подхватит экран, когда его откроют (DynamicScreen.loadData).
        for (var d = stack.depth - 1; d >= 0; d--) {
            var live = stack.get(d)
            if (live && live.hasOwnProperty("tilesModel")
                    && live.storageKey === targetStorageKey && live.isFolder === targetIsFolder) {
                addTile(live)
                NetworkListener.acceptInstall(socket, tileId)
                return
            }
        }

        // Экран не открыт — пишем плитку прямо в хранилище
        var tiles = targetIsFolder ? TileManager.loadFolder(targetStorageKey)
                                   : TileManager.loadScreen(targetStorageKey)
        tiles.push({
            type: "shortcut", kind: kind, title: title, icon: iconPath, path: "",
            id: tileId, installFolder: "", installing: true,
            installProgress: 0.0, installPhase: "packing"
        })
        if (targetIsFolder) TileManager.saveFolder(targetStorageKey, tiles)
        else                TileManager.saveScreen(targetStorageKey, tiles)
        NetworkListener.acceptInstall(socket, tileId)
        NotificationModule.post(Settings.tr("install_started_bg", { title: title, screen: targetTitle }),
                                "info", true, 6000)
    }

    function openKaraokeScreen() {
        if (stack.currentItem && stack.currentItem.screenTitle === Settings.tr("karaoke"))
            return
        stack.push(_createScreen(karaokeScreenComponent))
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
            cached: true
            visible: opacity > 0   // невидимое свечение не считать каждый кадр
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

    // Не во время игры и Android TV: диалог появлялся в окне MediaStation
    // (под игрой или в прозрачном окне поверх ТВ) и перехватывал ввод.
    // Покажется сам после возврата в меню.
    UpdateAvailableDialog {
        deferred: root.gameRunning || root.gameLaunching
                  || (typeof AndroidTV !== "undefined" && !!AndroidTV
                      && !!(AndroidTV.active || AndroidTV.booting || AndroidTV.switching))
    }
}
