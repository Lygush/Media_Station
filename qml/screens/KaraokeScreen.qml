// qml/screens/KaraokeScreen.qml
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtMultimedia
import AppSettings 1.0
import Qt5Compat.GraphicalEffects
import "../base"

Item {
    id: root
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    property string screenTitle: "Karaoke"

    // ── Состояние ─────────────────────────────────────────────────────────────
    property string currentFilePath:  ""
    property bool   processing:       false
    property int    progressPct:      0
    property string progressStage:    ""
    property bool   playerReady:      false
    property string lrcPath:          ""
    property string vocalsPath:       ""
    property string instrumentalPath: ""

    // ── LRC данные (парсятся в JS) ────────────────────────────────────────────
    property var lrcLines:  []   // [{startMs, text, words:[{startMs,text}]}]
    property int activeLine: -1
    property int activeWord: -1

    // ── JS-парсер LRC ─────────────────────────────────────────────────────────
    function parseTimestamp(s) {
        // [mm:ss.xx] или [mm:ss.xxx]
        var m = s.match(/\[(\d+):(\d+)\.(\d+)\]/)
        if (!m) return -1
        var cs = parseInt(m[3])
        if (m[3].length === 2) cs *= 10
        else if (m[3].length === 1) cs *= 100
        return parseInt(m[1]) * 60000 + parseInt(m[2]) * 1000 + cs
    }

    function parseInlineTs(s) {
        // <mm:ss.xx>
        var m = s.match(/<(\d+):(\d+)\.(\d+)>/)
        if (!m) return -1
        var cs = parseInt(m[3])
        if (m[3].length === 2) cs *= 10
        else if (m[3].length === 1) cs *= 100
        return parseInt(m[1]) * 60000 + parseInt(m[2]) * 1000 + cs
    }

    function loadLrc(path) {
        // Читаем файл через XMLHttpRequest
        var xhr = new XMLHttpRequest()
        xhr.open("GET", "file:///" + path.replace(/\\/g, "/"), false)
        xhr.send()
        if (xhr.status !== 200 && xhr.responseText === "") return

        var rawLines = xhr.responseText.split("\n")
        var metaRe   = /^\[(ti|ar|al|by|offset|length|re|ve):/
        var result   = []

        for (var i = 0; i < rawLines.length; i++) {
            var line = rawLines[i].trim()
            if (line === "" || metaRe.test(line)) continue

            var ts = parseTimestamp(line)
            if (ts < 0) continue

            var rest = line.replace(/^\[\d+:\d+\.\d+\]/, "").trim()

            // Inline тайм-коды
            var words = []
            var inlineRe = /<(\d+:\d+\.\d+)>([^<]*)/g
            var match
            while ((match = inlineRe.exec(rest)) !== null) {
                var wTs  = parseInlineTs("<" + match[1] + ">")
                var wTxt = match[2].trim()
                if (wTs >= 0 && wTxt !== "")
                    words.push({ startMs: wTs, text: wTxt })
            }

            var plainText = words.length > 0
                ? words.map(function(w){ return w.text }).join(" ")
                : rest

            result.push({ startMs: ts, text: plainText, words: words })
        }

        result.sort(function(a, b){ return a.startMs - b.startMs })
        root.lrcLines = result
    }

    function updateLyrics(posMs) {
        if (root.lrcLines.length === 0) return

        // Бинарный поиск активной строки
        var lo = 0, hi = root.lrcLines.length - 1, line = -1
        while (lo <= hi) {
            var mid = Math.floor((lo + hi) / 2)
            if (root.lrcLines[mid].startMs <= posMs) { line = mid; lo = mid + 1 }
            else hi = mid - 1
        }
        root.activeLine = line

        // Активное слово
        if (line >= 0 && root.lrcLines[line].words.length > 0) {
            var words = root.lrcLines[line].words
            var word  = 0
            for (var i = 0; i < words.length; i++) {
                if (words[i].startMs <= posMs) word = i
                else break
            }
            root.activeWord = word
        } else {
            root.activeWord = -1
        }
    }

    // ── Connections: KaraokeModule ────────────────────────────────────────────
    Connections {
        target: KaraokeModule

        function onProgress(percent, stage) {
            root.progressPct   = percent
            root.progressStage = stage
        }

        function onReady(lrc, vocals, instrumental) {
            root.processing       = false
            root.lrcPath          = lrc
            root.vocalsPath       = vocals
            root.instrumentalPath = instrumental
            root.lrcLines         = []
            root.activeLine       = -1
            root.activeWord       = -1

            if (lrc !== "") root.loadLrc(lrc)

            root.playerReady       = true
            vocalsPlayer.source    = "file:///" + vocals.replace(/\\/g, "/")
            instrPlayer.source     = "file:///" + instrumental.replace(/\\/g, "/")
        }

        function onError(message) {
            root.processing    = false
            root.progressStage = "Ошибка: " + message
        }
    }

    // ── Медиаплееры ───────────────────────────────────────────────────────────
    MediaPlayer {
        id: vocalsPlayer
        audioOutput: AudioOutput { volume: 0.8 }
        onPositionChanged: root.updateLyrics(position)
    }

    MediaPlayer {
        id: instrPlayer
        audioOutput: AudioOutput { volume: 1.0 }
    }

    function playBoth()  { vocalsPlayer.play();  instrPlayer.play()  }
    function pauseBoth() { vocalsPlayer.pause(); instrPlayer.pause() }
    function stopBoth()  { vocalsPlayer.stop();  instrPlayer.stop()  }
    function seekBoth(ms){ vocalsPlayer.position = ms; instrPlayer.position = ms }

    // ── File picker ───────────────────────────────────────────────────────────
    FileDialog {
        id: filePicker
        title:       "Выберите аудиофайл"
        nameFilters: ["Аудио (*.mp3 *.flac *.wav *.ogg *.m4a *.aac)", "Все файлы (*)"]
        onAccepted: {
            var path = selectedFile.toString()
            path = path.replace(/^file:\/\/\//, "").replace(/\//g, "\\")
            root.currentFilePath = path
            root.playerReady     = false
            root.processing      = true
            root.progressPct     = 0
            root.progressStage   = "Запуск..."
            root.lrcLines        = []
            root.activeLine      = -1
            stopBoth()
            KaraokeModule.openSong(path)
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // UI
    // ═══════════════════════════════════════════════════════════════════════════

    Rectangle {
        anchors.fill: parent
        color: "#0a0a12"; opacity: 0.85
    }

    // ── Начальный экран ────────────────────────────────────────────────────────
    Column {
        anchors.centerIn: parent
        spacing: Settings.us(24)
        visible: !root.processing && !root.playerReady

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "🎤"
            font.pixelSize: Settings.us(96)
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Выберите трек для караоке"
            font.family: appFont.name
            font.pixelSize: Settings.fs(28)
            color: "white"; opacity: 0.8
        }
        StyledButton {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Выбрать файл"
            width: Settings.us(300)
            onClicked: filePicker.open()
        }
    }

    // ── Обработка ─────────────────────────────────────────────────────────────
    Column {
        anchors.centerIn: parent
        spacing: Settings.us(20)
        visible: root.processing

        Spinner {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Settings.us(64); height: Settings.us(64)
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.progressStage
            font.family: appFont.name; font.pixelSize: Settings.fs(22)
            color: "white"
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Settings.us(400); height: Settings.us(8)
            radius: 4; color: "#33ffffff"
            Rectangle {
                width: parent.width * root.progressPct / 100
                height: parent.height; radius: 4
                color: Settings.accent(1.0)
                Behavior on width { NumberAnimation { duration: 300 } }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.progressPct + "%"
            font.family: appFont.name; font.pixelSize: Settings.fs(18)
            color: Settings.accent(1.0)
        }
        StyledButton {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Отмена"; width: Settings.us(200)
            onClicked: KaraokeModule.cancel()
        }
    }

    // ── Плеер ─────────────────────────────────────────────────────────────────
    Item {
        anchors.fill: parent
        visible: root.playerReady

        // Текст песни
        Column {
            id: lyricsView
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -Settings.us(60)
            width: parent.width * 0.8
            spacing: Settings.us(14)

            Repeater {
                model: {
                    if (root.lrcLines.length === 0) return []
                    var al    = root.activeLine < 0 ? 0 : root.activeLine
                    var start = Math.max(0, al - 2)
                    var end   = Math.min(root.lrcLines.length, start + 6)
                    return root.lrcLines.slice(start, end)
                }

                delegate: Item {
                    width: lyricsView.width
                    height: lineRow.implicitHeight + Settings.us(4)

                    property int absIdx: {
                        var al    = root.activeLine < 0 ? 0 : root.activeLine
                        var start = Math.max(0, al - 2)
                        return start + index
                    }
                    property bool isActive: absIdx === root.activeLine

                    // Enhanced LRC — слово за словом
                    Row {
                        id: lineRow
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: Settings.us(6)
                        visible: modelData.words && modelData.words.length > 0

                        Repeater {
                            model: modelData.words || []
                            Text {
                                property bool isActiveWord: parent.parent.parent.isActive
                                                         && index === root.activeWord
                                text: modelData.text
                                font.family:    appFont.name
                                font.pixelSize: parent.parent.parent.isActive
                                                ? Settings.fs(34) : Settings.fs(22)
                                font.bold: isActiveWord
                                color: isActiveWord                      ? Settings.accent(1.0)
                                     : parent.parent.parent.isActive     ? "white"
                                     :                                     "#80ffffff"
                                Behavior on color          { ColorAnimation  { duration: 120 } }
                                Behavior on font.pixelSize { NumberAnimation { duration: 150 } }
                                layer.enabled: isActiveWord
                                layer.effect: Glow {
                                    radius: 12; samples: 20
                                    color: Settings.accent(0.8)
                                }
                            }
                        }
                    }

                    // Simple LRC — строка целиком
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: !modelData.words || modelData.words.length === 0
                        text:    modelData.text || ""
                        font.family:    appFont.name
                        font.pixelSize: parent.isActive ? Settings.fs(34) : Settings.fs(22)
                        color: parent.isActive ? "white" : "#80ffffff"
                        Behavior on color          { ColorAnimation  { duration: 120 } }
                        Behavior on font.pixelSize { NumberAnimation { duration: 150 } }
                    }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.lrcLines.length === 0
                text: "Текст не найден"
                font.family: appFont.name; font.pixelSize: Settings.fs(22)
                color: "#60ffffff"
            }
        }

        // Панель управления
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: Settings.us(32)
            width: parent.width * 0.7; height: Settings.us(120)
            color: "#CC111120"; radius: 16
            border.color: "#33ffffff"; border.width: 1

            Column {
                anchors.centerIn: parent
                spacing: Settings.us(12)
                width: parent.width - Settings.us(40)

                // Прогресс трека
                Item {
                    width: parent.width; height: Settings.us(16)
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width; height: Settings.us(4)
                        radius: 2; color: "#44ffffff"
                        Rectangle {
                            width: vocalsPlayer.duration > 0
                                   ? parent.width * vocalsPlayer.position / vocalsPlayer.duration : 0
                            height: parent.height; radius: 2
                            color: Settings.accent(1.0)
                            Behavior on width { NumberAnimation { duration: 100 } }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (vocalsPlayer.duration > 0)
                                seekBoth(mouseX / width * vocalsPlayer.duration)
                        }
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(24)

                    StyledButton {
                        text: "Другой трек"; width: Settings.us(180)
                        onClicked: { stopBoth(); root.playerReady = false; filePicker.open() }
                    }
                    StyledButton {
                        text: vocalsPlayer.playbackState === MediaPlayer.PlayingState
                               ? "⏸ Пауза" : "▶ Играть"
                        width: Settings.us(160)
                        onClicked: {
                            if (vocalsPlayer.playbackState === MediaPlayer.PlayingState)
                                pauseBoth()
                            else
                                playBoth()
                        }
                    }
                    StyledButton {
                        text: "⏹ Стоп"; width: Settings.us(140)
                        onClicked: stopBoth()
                    }
                }
            }
        }
    }
}
