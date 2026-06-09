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

    // Когда нет никаких медиа — компонент полностью прозрачен, системный фон виден
    visible: _hasAnyMedia

    // ── Состояние: backdrop → trailer → backdrop ──────────────────────────────
    property bool showTrailer: false

    Timer {
        id: backdropTimer
        interval: 5000
        repeat:   false
        running:  bg._hasTrailer && bg._hasBackdrop && !bg.showTrailer
        onTriggered: fadeOverlay.startFadeIn()
    }

    onTrailerPathChanged: {
        if (_hasTrailer && !_hasBackdrop) {
            showTrailer = true
            mediaPlayer.play()
        }
    }

    // ── Backdrop / обложка ────────────────────────────────────────────────────
    Image {
        id: bgImage
        anchors.fill: parent
        source:   bg._hasBackdrop ? bg._fileUrl(bg.backdropPath)
                : bg._hasCover    ? bg._fileUrl(bg.coverPath)
                : ""
        fillMode: Image.PreserveAspectCrop
        cache:    true
        asynchronous: true
        opacity:  bg.showTrailer ? 0.0 : 1.0
        Behavior on opacity { NumberAnimation { duration: 600 } }
    }

    // ── Медиаплеер ────────────────────────────────────────────────────────────
    MediaPlayer {
        id: mediaPlayer
        source:      bg._hasTrailer ? bg._trailerSource : ""
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
                        mediaPlayer.stop()
                        backdropTimer.restart()
                    } else {
                        bg.showTrailer = true
                        mediaPlayer.play()
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
