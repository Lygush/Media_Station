// Заглушки C++-объектов для визуальной проверки интерфейса (ui_snapshots).
// Данные нарочно «неудобные»: длинные названия, много плиток, низкий заряд —
// чтобы наложения и обрезка текста были видны на снимках.
import QtQuick

Item {
    // ── SettingsManager (синглтон AppSettings) ───────────────────────────────
    property QtObject settingsManager: QtObject {
        property color  accentColor:     "#4f8ef7"
        property string backgroundImage: ""
        property bool   fullscreen:      false
        property string language:        "ru"
        property string installPath:     "C:/MediaStation/data/games"
        property string archivePath:     ""
        property double uiScale:         1.0
        property int    fontOffset:      0
        property bool   enableFogShader: false
    }

    // ── AppController (синглтон AppSettings) ─────────────────────────────────
    property QtObject appController: QtObject {
        signal gameWindowAppeared(string tileId, bool windowFound)
        signal gameClosed(string tileId)
        signal launchFailed(string tileId, string reason)
        signal gameRunningChanged()
        function hideBlackOverlay() {}
        function showBlackOverlay() {}
        function launch(path, tileId) {}
        function powerAction(action) {}
        function quitApplication() {}
        function parentPath(p) { var i = p.lastIndexOf("/"); return i > 0 ? p.substring(0, i) : "" }
    }

    readonly property var demoTiles: [
        { type: "shortcut", kind: "app", title: "Cyberpunk 2077", icon: "", path: "C:/g/1/launcher/launch.ps1", id: "t1", installFolder: "C:/g/1" },
        { type: "shortcut", kind: "app", title: "The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade", icon: "", path: "C:/g/2/launcher/launch.ps1", id: "t2", installFolder: "C:/g/2" },
        { type: "shortcut", kind: "app", title: "Ведьмак 3: Дикая Охота — Издание «Игра года»", icon: "", path: "C:/g/3/launcher/launch.ps1", id: "t3", installFolder: "C:/g/3" },
        { type: "folder", title: "Гонки и симуляторы с очень длинным названием папки", id: "f1", children: [] },
        { type: "shortcut", kind: "app", title: "Hollow Knight", icon: "", path: "C:/g/5/launcher/launch.ps1", id: "t5", installFolder: "C:/g/5" },
        { type: "shortcut", kind: "external", title: "Steam", icon: "", path: "C:/g/6/launcher/launch.ps1", id: "t6", installFolder: "C:/g/6" },
        { type: "shortcut", kind: "app", title: "Устанавливается…", icon: "", path: "", id: "net_x", installFolder: "", installing: true, installProgress: 0.42, installPhase: "receiving" },
        { type: "shortcut", kind: "app", title: "Baldur's Gate 3", icon: "", path: "C:/g/8/launcher/launch.ps1", id: "t8", installFolder: "C:/g/8" },
        { type: "shortcut", kind: "app", title: "Disco Elysium — The Final Cut", icon: "", path: "C:/g/9/launcher/launch.ps1", id: "t9", installFolder: "C:/g/9" }
    ]

    // ── TileManager ───────────────────────────────────────────────────────────
    property QtObject tileManager: QtObject {
        signal tileStructureChanged(string storageKey, bool isFolder)
        signal installationProgress(string tileId, real progress)
        signal installationFinished(string tileId, bool success, string ps1Path, string iconPath, string installFolder)
        signal installationCancelled(string tileId)
        signal exeScanReady(string requestId, var exeFiles)
        signal exeDescriptionReady(string requestId, string description)
        signal dependencyCheckReady(string requestId, var missingDeps)
        signal launchExeUpdated(string tileId, bool success, string newPs1Path, string newExeRelative)
        function loadScreen(s) { return demoTiles }
        function loadFolder(f) { return demoTiles.slice(0, 3) }
        function saveScreen(s, t) {}
        function saveFolder(f, t) {}
        function deleteFolder(f) {}
        function listScreens() { return [ { title: "Игры", storageKey: "games", isFolder: false },
                                          { title: "ТВ", storageKey: "tv", isFolder: false } ] }
        function listFolderTiles(k, f) { return [] }
        function listDrives() { return [ { name: "C:", path: "C:/" }, { name: "D: Игры и очень длинная метка тома", path: "D:/" } ] }
        function listDir(p, f) { return [] }
        signal dirListReady(string requestId, var entries)
        function listDirAsync(id, p, f) { dirListReady(id, p === "" ? listDrives() : listDir(p, f)) }
        function readGameInfo(folder) { return { name: "Cyberpunk 2077", description: "Длинное описание игры. ".repeat(20),
                                                 developer: "CD PROJEKT RED", genres: ["RPG", "Action"], releaseYear: 2020 } }
        function isInstalling(id) { return id === "net_x" }
        function getInstallProgress(id) { return 0.42 }
        function cancelInstallation(id) {}
        function startInstallation() {}
        function createExternalShortcut() {}
        function deleteInstalledGame(f) {}
        function findInstallFolderByTileId(id, b) { return "" }
        function installedGameExists(f, b) { return true }
        function getExeDescription(r, e) {}
        function scanExeFiles(r, d) {}
        function updateLaunchExe(t, f, e) {}
    }

    // ── NetworkListener ───────────────────────────────────────────────────────
    property QtObject networkListener: QtObject {
        signal showInstallDialog(string title, string kind, string exeRelativePath, string iconRelativePath, var socket)
        signal installProgress(string tileId, real progress, string phase)
        signal serverCancelledInstall(string tileId)
        property QtObject logSender: QtObject {
            property bool   busy: false
            property int    progress: 0
            property string status: "Готов"
            property var    foundServers: ["192.168.1.10", "192.168.1.25"]
            signal finished(bool success, string message)
            function discoverServers() {}
            function sendLogs(ip) {}
            function cancelSend() {}
        }
        function acceptInstall(s, t) {}
        function rejectInstall(s) {}
        function cancelInstallByTileId(t) {}
        function hasActiveInstall(t) { return t === "net_x" }
    }

    // ── GameLibraryModule ────────────────────────────────────────────────────
    property QtObject gameLibrary: QtObject {
        property bool scanning: false
        property bool compressAvailable: true
        signal compressProgress(string gameId, real progress)
        signal compressFinished(string gameId, bool success, bool compressed)
        signal scanFinished(var entries)
        signal libraryEntryChanged(string gameId, var entry)
        signal archiveProgress(string gameId, real progress, string currentFile)
        signal archiveFinished(string gameId, bool success)
        signal extractProgress(string gameId, real progress)
        signal extractFinished(string gameId, bool success)
        signal folderSizeReady(string gameId, var bytes)
        signal tileCreated(string gameId, bool success)
        function currentEntries() { return [
            { id: "a1", gameId: "a1", name: "Cyberpunk 2077", type: "installed", available: true, installPath: "C:/g/1", sizeCached: 72000000000, tileCount: 1, iconPath: "", compressed: true },
            { id: "a2", gameId: "a2", name: "The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade", type: "archive", available: true, installPath: "C:/g/2", archivePath: "C:/g/2/archive/x.7z", sizeCached: 18000000000, tileCount: 0, iconPath: "" },
            { id: "a3", gameId: "a3", name: "Ведьмак 3: Дикая Охота — Издание «Игра года»", type: "installed", available: true, installPath: "C:/g/3", sizeCached: 50000000000, tileCount: 2, iconPath: "", archiving: true, archiveProgress: 0.37, archiveCurrentFile: "game/content/patch0/bundles/very_long_file_name_texture.bundle" },
            { id: "a4", gameId: "a4", name: "Steam", type: "external", available: false, installPath: "C:/g/6", sizeCached: -1, tileCount: 1, iconPath: "" },
            { id: "a5", gameId: "a5", name: "Hollow Knight", type: "installed", available: true, installPath: "C:/g/5", sizeCached: 9000000000, tileCount: 1, iconPath: "", legacyNoMetadata: true },
            { id: "a6", gameId: "a6", name: "Baldur's Gate 3", type: "installed", available: true, installPath: "C:/g/7", sizeCached: 120000000000, tileCount: 1, iconPath: "", compressing: true, compressProgress: 0.62 }
        ] }
        function scanLibrary() { scanFinished(currentEntries()) }
        function archiveGame(id) {} function cancelArchive(id) {} function cancelExtract(id) {}
        function createTile(id, k, f) {} function deleteGame(id) {} function extractGame(id, k, s, f) {}
        function openFolder(p) {} function reassignPath(id, p) {} function regenerateMetadata(id) {}
        function requestFolderSize(id, p) {}
        function compressGame(id, c) {} function cancelCompress(id) {}
    }

    // ── JoystickModule ───────────────────────────────────────────────────────
    property QtObject joystick: QtObject {
        property var  controllers: [
            { slot: 1, name: "DualShock 4 — геймпад гостиной с длинным именем", macAddress: "AA:BB:CC:DD:EE:01", battery: 15, charging: false, connType: "BT" },
            { slot: 2, name: "DualSense", macAddress: "AA:BB:CC:DD:EE:02", battery: 80, charging: true, connType: "USB" }
        ]
        property bool ds4Running: true
        property bool anyConnected: true
        property int  controllerCount: 2
        function activate() {}
        function setControllerName(mac, name) {}
    }

    // ── G72Battery ───────────────────────────────────────────────────────────
    property QtObject g72Battery: QtObject {
        property int    battery: 12
        property bool   connected: true
        property int    zone: 2
    }

    // ── GameInfoModule / GameMediaModule ─────────────────────────────────────
    property QtObject gameInfo: QtObject {
        property bool   fetching: false
        property string stage: ""
        signal infoFetched(string tileId, var meta)
        signal fetchError(string tileId, string reason)
        signal tokenGenerated(string token)
        signal jsonTokenGenerated(string token)
        signal candidatesReady(string tileId, var candidates)
        signal fieldsReady(string tileId, var fields)
        function requestFetch() {} function selectCandidate(t, a) {}
    }
    property QtObject gameMedia: QtObject {
        property bool fetchingCovers: false
        property bool fetchingTrailer: false
        property real trailerProgress: 0
        signal mediaReady(string tileId, var paths)
        signal mediaFetched(string tileId, var paths)
        function requestFetch() {} function cancelTrailer() {}
    }

    // ── Karaoke / Overlay / Notification / Update / AndroidTV ────────────────
    property QtObject karaoke: QtObject {
        signal progress(int pct, string stage)
        signal ready(string lrc, string vocals, string instrumental)
        signal error(string message)
        signal metaProposed(string artist, string title, string source)
        signal analysisStatus(string message)
        signal analysisToken(string token)
        function openSong(p) {} function confirmSong(a, t) {} function cancel() {}
        function readTextFile(p) { return "" }
    }
    property QtObject g72Keys: QtObject {
        signal upPressed()
        signal downPressed()
        signal leftPressed()
        signal rightPressed()
        signal selectPressed()
        signal backPressed()
        signal menuPressed()
        signal homePressed()
        signal searchPressed()
    }
    property QtObject overlay: QtObject {
        property bool visible: false
        property string context: ""
        signal overlayKeyPressed(string key)
        function show(c) {} function hide() {} function exitAndroidTV() {}
    }
    property QtObject notification: QtObject {
        function post(message, level, autoDismiss, durationMs) {}
        property string message: "Установка завершена: The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade"
        property int    level: 0
        property bool   autoDismiss: false
        property int    duration: 3000
        property bool   visible: false
        signal showRequested()
        signal currentChanged()
        signal hideRequested()
        function dismiss() {} function onDismissed() {}
    }
    property QtObject updateManager: QtObject {
        property bool   updateReady: false
        property bool   downloading: false
        property bool   preparing: false
        property real   downloadProgress: 0
        property string newVersion: "1.2.0"
        property string releaseNotes: "• Исправления\n• Улучшения"
        function acceptUpdate() {} function declineUpdate() {}
    }
    property QtObject androidTV: QtObject {
        property bool   booting: false
        property bool   running: false
        property string status: ""
        property int    qcow2SizeMb: 3400
        property bool   adBlockEnabled: true
        property string adBlockState: "running"
        property bool   adBlockBusy: false
        property real   adBlockDownload: -1
        function downloadAdBlock() {}
        function refreshAdBlockState() {} function installAdBlock() {}
        function uninstallAdBlock() {} function openAdBlockPanel() {}
        signal emulatorReadyBehind()
        signal emulatorClosed()
        signal emulatorRestoringMediaStation()
        signal bootCompleted()
        property ListModel profileManager: ListModel {
            property string activeProfileId: "p1"
            property bool   locked: false
            property var    currentProfileData: ({ id: "p1", displayName: "Android TV 14 (Google APIs, x86_64) — system-images;android-34;android-tv;x86", name: "MediaStation TV (Android 14)", resolution: "2560x1440", gpu: "auto", ramMb: 8192, cores: 4, audioDriver: "dsound", audioTimerMs: 8, dsoundLatencyMs: 60, diagnosticLogcat: false, consoleInput: true })
            ListElement { name: "MediaStation TV (Android 14)"; profileId: "p1" }
            ListElement { name: "Детский профиль с длинным названием"; profileId: "p2" }
            signal profilesChanged()
            function getProfile(id) { return currentProfileData }
            function scanSystemImages() { return [] }
            function profileSizeMb(id) { return 2650 }
            function addProfile(n) {} function removeProfile(id) {} function save() {}
            function setActiveProfile(id) {} function updateProfileField(id, f, v) {}
            function clearProfileData(id) { return true }
        }
        function launch() {} function stop() {} function applyFullscreenAndSwitch() {}
        function refreshQcow2Size() {} function resetQcow2() { return true }
    }
}
