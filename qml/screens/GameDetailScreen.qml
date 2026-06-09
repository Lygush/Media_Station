// qml/screens/GameDetailScreen.qml
import QtQuick
import QtQuick.Effects
import AppSettings 1.0
import "GameDetail"

Item {
    id: root

    // ── Интерфейс экрана ──────────────────────────────────────────────────────
    property string screenTitle: gameData.title || ""
    property var    launchGame:  null
    property var    gameData:    ({})

    property bool   metadataLoaded: false
    property string streamingText:  ""
    property bool   streaming:      false

    // JSON стриминг для попапа (первый запрос)
    property string _jsonStreamText: ""

    // ── Геттеры ───────────────────────────────────────────────────────────────
    readonly property string _title:       gameData.title        || ""
    readonly property string _tileId:      gameData.tileId       || ""
    readonly property string _path:        gameData.path         || ""
    readonly property string _cover:       gameData.coverPath    || ""
    readonly property string _backdrop:    gameData.backdropPath || ""
    readonly property string _trailer:     gameData.trailerPath  || ""
    readonly property string _description: gameData.description  || ""
    readonly property string _genre:       gameData.genre        || ""
    readonly property string _year:        gameData.releaseYear  || ""
    readonly property string _developer:   gameData.developer    || ""
    readonly property string _publisher:   gameData.publisher    || ""
    readonly property string _rating:      gameData.rating       || ""
    readonly property string _sysreqMin:   gameData.sysreqMin    || ""
    readonly property string _sysreqRec:   gameData.sysreqRec    || ""
    readonly property bool   _infoFetched: gameData.infoFetched  || false
    readonly property string _steamAppId:  gameData.steamAppId   || ""

    // ── Попап кандидатов ──────────────────────────────────────────────────────
    // Управляется через leftPanel._dbPopupVisible / leftPanel._dbCandidates

    // ── Загрузка локальных метаданных ─────────────────────────────────────────
    function loadLocalMetadata() {
        if (metadataLoaded) return
        var folder = gameData.installFolder || ""
        if (folder === "") return
        var info = TileManager.readGameInfo(folder)
        if (info && Object.keys(info).length > 0) {
            var merged = {}
            for (var k in gameData) merged[k] = gameData[k]
            for (var k in info) merged[k] = info[k]
            if (gameData.coverPath    && !merged.coverPath)    merged.coverPath    = gameData.coverPath
            if (gameData.backdropPath && !merged.backdropPath) merged.backdropPath = gameData.backdropPath
            gameData = merged
            metadataLoaded = true
        }
    }

    Component.onCompleted: loadLocalMetadata()

    // ── Connections: GameInfoModule ───────────────────────────────────────────
    Connections {
        target: GameInfoModule

        function onInfoFetched(tileId, metadata) {
            if (tileId !== root._tileId) return
            var merged = {}
            for (var k in root.gameData) merged[k] = root.gameData[k]
            if (metadata)
                for (var k in metadata)
                    if (metadata[k] !== undefined && metadata[k] !== "")
                        merged[k] = metadata[k]
            merged.infoFetched = true
            root.gameData = merged
            root.metadataLoaded = true
            root.streamingText = ""
            root.streaming = false
            root._jsonStreamText = ""
            leftPanel._jsonPopupVisible = false
            leftPanel._jsonText = ""

            var sid = merged.steamAppId || ""
            if (sid !== "" && !GameMediaModule.fetchingCovers && !GameMediaModule.fetchingTrailer) {
                GameMediaModule.requestFetch(
                    root._tileId, root._title,
                    merged.installFolder || root.gameData.installFolder || "", sid
                )
            }
        }

        function onFieldsReady(tileId, fields) {
            if (tileId !== root._tileId) return
            var merged = {}
            for (var k in root.gameData) merged[k] = root.gameData[k]
            for (var k in fields)
                if (fields[k] !== undefined && fields[k] !== "")
                    merged[k] = fields[k]
            root.gameData = merged
            // Первый запрос завершён — убираем JSON попап
            leftPanel._jsonPopupVisible = false
            root._jsonStreamText = ""
            leftPanel._jsonText = ""
        }

        function onTokenGenerated(token) {
            root.streaming = true
            root.streamingText += token
        }

        function onCandidatesReady(tileId, candidates) {
            if (tileId !== root._tileId) return
            leftPanel._dbCandidates   = candidates
            leftPanel._dbPopupVisible = true
        }

        function onJsonTokenGenerated(token) {
            if (!leftPanel._jsonPopupVisible) leftPanel._jsonPopupVisible = true
            root._jsonStreamText += token
            leftPanel._jsonText = root._jsonStreamText
        }

        function onFetchingChanged() {
            if (!GameInfoModule.fetching) {
                leftPanel._jsonPopupVisible = false
                root._jsonStreamText = ""
                leftPanel._jsonText = ""
            }
        }
    }

    // ── Connections: GameMediaModule ──────────────────────────────────────────
    Connections {
        target: GameMediaModule
        function onMediaFetched(tileId, media) {
            if (tileId !== root._tileId) return
            var merged = {}
            for (var k in root.gameData) merged[k] = root.gameData[k]
            if (media)
                for (var k in media)
                    if (media[k] !== undefined && media[k] !== "")
                        merged[k] = media[k]
            root.gameData = merged
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    // UI
    // ══════════════════════════════════════════════════════════════════════════

    // ── Фон с плавным переходом backdrop → трейлер → backdrop ────────────────
    GameDetailBackground {
        id: background
        anchors.fill: parent
        backdropPath: root._backdrop
        coverPath:    root._cover
        trailerPath:  root._trailer
    }

    // ── Тень под левой панелью — от края экрана, градиент вправо ────────────
    Rectangle {
        x:      0
        y:      0
        width:  leftPanel.x + leftPanel.width + Settings.us(80)
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0;  color: "#AA000000" }
            GradientStop { position: 0.75; color: "#66000000" }
            GradientStop { position: 1.0;  color: "#00000000" }
        }
    }

    // ── Тень под блоком описания — размытые края через layer ─────────────────
    Item {
        x:       rightPanel.x - Settings.us(40)
        y:       rightPanel.y - Settings.us(30)
        width:   rightPanel.width + Settings.us(80)
        height:  rightPanel.contentHeight + Settings.us(60)
        visible: rightPanel.contentHeight > 0

        layer.enabled: true
        layer.effect: MultiEffect {
            blurEnabled: true
            blur: 1.0
            blurMax: 32
        }

        Rectangle {
            anchors {
                fill: parent
                leftMargin:   Settings.us(40)
                rightMargin:  Settings.us(40)
                topMargin:    Settings.us(30)
                bottomMargin: Settings.us(30)
            }
            color: "#77000000"
            radius: Settings.us(4)
        }
    }

    // ── Левая панель ──────────────────────────────────────────────────────────
    GameDetailLeftPanel {
        id: leftPanel
        anchors {
            left:   parent.left; leftMargin:   Settings.us(80)
            top:    parent.top;  topMargin:    Settings.us(140)
            bottom: parent.bottom; bottomMargin: Settings.us(40)
        }
        width: Settings.us(320)

        title:       root._title
        cover:       root._cover
        genre:       root._genre
        year:        root._year
        developer:   root._developer
        rating:      root._rating
        infoFetched: root._infoFetched
        fetching:    GameInfoModule.fetching
        path:        root._path
        tileId:      root._tileId
        gameData:    root.gameData
        onLaunchRequested: {
            if (root._path !== "" && root.launchGame)
                root.launchGame(root._path, root._tileId)
        }
        onFetchRequested: {
            GameInfoModule.requestFetch(root._tileId, root._title, root.gameData.installFolder || "")
        }
    }

    // ── Правая панель — описание ──────────────────────────────────────────────
    GameDetailRightPanel {
        id: rightPanel
        anchors {
            left:   leftPanel.right; leftMargin:  Settings.us(40)
            right:  parent.right;    rightMargin: Settings.us(60)
            bottom: parent.bottom;   bottomMargin: Settings.us(60)
        }
        height: contentHeight

        description:   root._description
        streamingText: root.streamingText
        streaming:     root.streaming
    }

}

