// qml/screens/GameDetail/GameDetailBackground.qml
import QtQuick
import QtMultimedia
import AppSettings 1.0

Item {
    id: bg
    anchors.fill: parent

    property string backdropPath: ""
    property string coverPath:    ""
    property string trailerPath:  ""

    readonly property bool _hasBackdrop: backdropPath !== ""
    readonly property bool _hasCover:    coverPath    !== ""
    readonly property bool _hasTrailer:  trailerPath  !== ""
    readonly property bool _hasAnyMedia: _hasBackdrop || _hasCover || _hasTrailer

    readonly property string _trailerSource: {
        if (trailerPath === "") return ""
        if (trailerPath.startsWith("http")) return trailerPath
        return _fileUrl(trailerPath)
    }

    function _fileUrl(p) {
        if (p === "") return ""
        if (p.startsWith("qrc:") || p.startsWith("file:")) return p
        var fixed = p.replace(/\\/g, "/")
        var parts = fixed.split("/")
        for (var i = 0; i < parts.length; i++) {
            if (i === 0 && parts[i].match(/^[a-zA-Z]:$/)) continue
            parts[i] = encodeURIComponent(parts[i])
        }
        return "file:///" + parts.join("/")
    }

    // Пока медиа нет — компонент прозрачен, виден системный фон. Появление фона
    // и смена обложки на backdrop — плавные (раньше картинка возникала рывком).
    readonly property bool _imageShown: frontImage.status === Image.Ready
    opacity: (_imageShown || showTrailer) ? 1.0 : 0.0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 700; easing.type: Easing.InOutQuad } }

    // ── Состояние: backdrop → trailer → backdrop ──────────────────────────────
    property bool showTrailer: false

    // Плеер трейлера создаётся не сразу, а после анимации открытия экрана:
    // создание MediaPlayer (на Windows — загрузка FFmpeg) и открытие файла
    // шли в момент перехода и давали заметный фриз.
    property bool _playerArmed: false
    Timer {
        interval: 900; running: bg._hasTrailer && !bg._playerArmed
        onTriggered: bg._playerArmed = true
    }
    readonly property var mediaPlayer: playerLoader.item ? playerLoader.item.player : null

    Timer {
        id: backdropTimer
        interval: 5000
        repeat:   false
        running:  bg._hasTrailer && bg._hasBackdrop && !bg.showTrailer && bg.mediaPlayer !== null
        onTriggered: fadeOverlay.startFadeIn()
    }

    function _startTrailerIfOnly() {
        if (_hasTrailer && !_hasBackdrop && mediaPlayer && !showTrailer) {
            showTrailer = true
            mediaPlayer.play()
        }
    }
    onTrailerPathChanged: _startTrailerIfOnly()
    onMediaPlayerChanged: _startTrailerIfOnly()

    // ── Backdrop / обложка: две картинки с перекрёстным переходом ───────────
    readonly property string _imageSource: _hasBackdrop ? _fileUrl(backdropPath)
                                         : _hasCover    ? _fileUrl(coverPath)
                                         : ""
    property bool _aFront: true
    readonly property Image frontImage: _aFront ? imgA : imgB
    readonly property Image backImage:  _aFront ? imgB : imgA

    on_ImageSourceChanged: {
        if (frontImage.source == "" || frontImage.status !== Image.Ready) {
            frontImage.source = _imageSource       // первое появление — общий fade
        } else if (frontImage.source != _imageSource) {
            backImage.source = _imageSource        // переход, когда загрузится
        }
    }
    Component.onCompleted: imgA.source = _imageSource

    function _onBackReady(img) {
        if (img === backImage && img.status === Image.Ready && img.source == _imageSource)
            _aFront = !_aFront
    }

    Item {
        anchors.fill: parent
        opacity: bg.showTrailer ? 0.0 : 1.0
        Behavior on opacity { NumberAnimation { duration: 600 } }

        Image {
            id: imgA
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            cache: true; asynchronous: true
            opacity: bg._aFront ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 700; easing.type: Easing.InOutQuad } }
            onStatusChanged: bg._onBackReady(imgA)
        }
        Image {
            id: imgB
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            cache: true; asynchronous: true
            opacity: bg._aFront ? 0.0 : 1.0
            Behavior on opacity { NumberAnimation { duration: 700; easing.type: Easing.InOutQuad } }
            onStatusChanged: bg._onBackReady(imgB)
        }
    }

    // ── Медиаплеер (создаётся отложенно, см. _playerArmed) ────────────────────
    Loader {
        id: playerLoader
        anchors.fill: parent
        active: bg._hasTrailer && bg._playerArmed
        sourceComponent: Item {
            property alias player: mediaPlayer
            MediaPlayer {
                id: mediaPlayer
                source:      bg._trailerSource
                videoOutput: videoOutput
                audioOutput: AudioOutput { volume: 0.0 }

                onPlaybackStateChanged: {
                    if (playbackState === MediaPlayer.StoppedState && bg.showTrailer && bg._hasTrailer)
                        fadeOverlay.startFadeIn()
                }
                onErrorOccurred: console.warn("Video error:", errorString)
            }
            VideoOutput {
                id: videoOutput
                anchors.fill: parent
                fillMode:     VideoOutput.PreserveAspectCrop
                opacity:      bg.showTrailer ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 600 } }
            }
        }
    }

    // ── Затемняющий оверлей для переходов ────────────────────────────────────
    Rectangle {
        id: fadeOverlay
        anchors.fill: parent
        color:   "#000000"
        opacity: 0.0

        function startFadeIn() { fadeInAnim.start() }

        SequentialAnimation {
            id: fadeInAnim
            NumberAnimation {
                target: fadeOverlay; property: "opacity"
                to: 1.0; duration: 600; easing.type: Easing.InOutQuad
            }
            ScriptAction {
                script: {
                    if (bg.showTrailer) {
                        bg.showTrailer = false
                        if (bg.mediaPlayer) bg.mediaPlayer.stop()
                        backdropTimer.restart()
                    } else if (bg.mediaPlayer) {
                        bg.showTrailer = true
                        bg.mediaPlayer.play()
                    }
                }
            }
            NumberAnimation {
                target: fadeOverlay; property: "opacity"
                to: 0.0; duration: 600; easing.type: Easing.InOutQuad
            }
        }
    }

    // ── Постоянные оверлеи — только когда есть медиа ─────────────────────────
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.35
    }

    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: parent.width * 0.55
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#EE000000" }
            GradientStop { position: 0.7; color: "#88000000" }
            GradientStop { position: 1.0; color: "#00000000" }
        }
    }
}
