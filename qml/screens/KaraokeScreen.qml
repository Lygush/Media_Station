// qml/screens/KaraokeScreen.qml
import QtQuick
import QtQuick.Controls.Basic   // свой фон полей ввода; нативный стиль Windows его игнорирует
import QtQuick.Dialogs
import QtMultimedia
import AppSettings 1.0
import Qt5Compat.GraphicalEffects
import "../base"

Item {
    id: root
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    property string screenTitle: Settings.tr("karaoke")

    // ── Состояние ─────────────────────────────────────────────────────────────
    property string currentFilePath:  ""
    property bool   processing:       false
    property int    progressPct:      0
    property string progressStage:    ""

    // ── Анализ имени / подтверждение ─────────────────────────────────────────
    property bool   analyzing:        false   // быстрый анализ имени (теги + нейросеть)
    property bool   confirming:       false   // карточка «Проверьте название»
    property string analysisMsg:      ""
    property string analysisStream:   ""      // потоковый вывод нейросети
    property string proposedSource:   ""      // tags | ai | filename | manual
    property string proposedFile:     ""
    property string songArtist:       ""      // подтверждённые значения (для заголовка прогресса)
    property string songTitle:        ""
    property bool   usedWhisper:      false   // текст не найден онлайн → распознаём нейросетью
    property int    elapsedSec:       0
    property string errorText:        ""
    property bool   playerReady:      false
    property string lrcPath:          ""
    property string vocalsPath:       ""
    property string instrumentalPath: ""

    // ── LRC данные (парсятся в JS) ────────────────────────────────────────────
    property var lrcLines:  []   // [{startMs, text, words:[{startMs,text}]}]
    property int activeLine: -1
    property int activeWord: -1

    // Вокал в плеере: по умолчанию выключен — играет «минус»
    property bool vocalsOn: false

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
        // Читаем файл через C++ (XMLHttpRequest для file:// отключён в Qt)
        var text = KaraokeModule.readTextFile(path)
        if (text === "") return

        var rawLines = text.split("\n")
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

        // lrclib отдаёт тайм-коды только для строк целиком. Чтобы подсветка шла
        // слово за словом, как у распознанного Whisper, раскладываем слова строки
        // по времени пропорционально их длине.
        for (var k = 0; k < result.length; k++) {
            var ln = result[k]
            if (ln.words.length > 0) continue
            if (ln.text === "") ln.text = "♪"                       // инструментальная пауза
            var endMs = (k + 1 < result.length) ? result[k + 1].startMs : ln.startMs + 5000
            ln.words = root.synthWords(ln.text, ln.startMs, endMs)
        }
        root.lrcLines = result
    }

    // Равномерная раскладка слов строки по времени (для LRC без пословных меток)
    function synthWords(text, startMs, endMs) {
        var parts = text.split(/\s+/).filter(function(w) { return w.length > 0 })
        if (parts.length === 0) return []
        var total = 0
        for (var i = 0; i < parts.length; i++) total += parts[i].length + 1
        var dur  = Math.max(500, endMs - startMs)
        // Поют обычно быстрее, чем длится промежуток до следующей строки
        var sing = Math.min(dur * 0.92, Math.max(1500, total * 85))
        var out = [], acc = 0
        for (var j = 0; j < parts.length; j++) {
            out.push({ startMs: Math.round(startMs + sing * acc / total), text: parts[j] })
            acc += parts[j].length + 1
        }
        return out
    }

    // ── Текст строки для отображения (StyledText: перенос + подсветка слова) ──
    function escHtml(t) {
        return String(t).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }
    function hexColor(c) {
        function h(x) { var v = Math.round(x * 255).toString(16); return v.length < 2 ? "0" + v : v }
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }
    // activeIdx < 0 — строка неактивна (просто текст)
    function lineHtml(line, activeIdx) {
        if (!line.words || line.words.length === 0)
            return escHtml(line.text || "")
        var accent = hexColor(Settings.accent(1.0))
        var out = []
        for (var i = 0; i < line.words.length; i++) {
            var w = escHtml(line.words[i].text)
            if (activeIdx < 0)           out.push(w)
            else if (i < activeIdx)      out.push('<font color="' + accent + '">' + w + '</font>')
            else if (i === activeIdx)    out.push('<b><font color="' + accent + '">' + w + '</font></b>')
            else                         out.push(w)
        }
        return out.join(" ")
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

        function onAnalysisStatus(message) { root.analysisMsg = message }

        function onAnalysisToken(token) {
            var t = root.analysisStream + token
            root.analysisStream = t.length > 240 ? t.slice(t.length - 240) : t
        }

        function onMetaProposed(artist, title, source, fileName) {
            root.analyzing      = false
            root.confirming     = true
            root.proposedSource = source
            root.proposedFile   = fileName
            artistField.text    = artist
            titleField.text     = title
            titleField.forceActiveFocus()
        }

        function onProgress(percent, stage) {
            root.progressPct   = percent
            root.progressStage = stage
            if (stage.indexOf("аспознаван") >= 0) root.usedWhisper = true
        }

        function onReady(lrc, vocals, instrumental) {
            root.processing       = false
            root.analyzing        = false
            root.confirming       = false
            root.lrcPath          = lrc
            root.vocalsPath       = vocals
            root.instrumentalPath = instrumental
            root.lrcLines         = []
            root.activeLine       = -1
            root.activeWord       = -1

            // Сначала плееры — чтобы ошибка разбора LRC не блокировала воспроизведение
            root.playerReady       = true
            vocalsPlayer.source    = "file:///" + vocals.replace(/\\/g, "/")
            instrPlayer.source     = "file:///" + instrumental.replace(/\\/g, "/")

            if (lrc !== "") {
                try { root.loadLrc(lrc) }
                catch (e) { console.warn("[Karaoke] loadLrc failed:", e) }
            }
        }

        function onError(message) {
            root.processing  = false
            root.analyzing   = false
            root.confirming  = false
            root.errorText   = message
        }
    }

    // ── Медиаплееры ───────────────────────────────────────────────────────────
    // Создаются, только когда трек готов: раньше два MediaPlayer с аудиовыходами
    // создавались при каждом входе на экран (на Windows это инициализация
    // FFmpeg/аудио — фриз при открытии).
    Loader {
        id: playersLoader
        active: root.playerReady
        sourceComponent: QtObject {
            property MediaPlayer vocals: MediaPlayer {
                audioOutput: AudioOutput { volume: 0.6; muted: !root.vocalsOn }
                onPositionChanged: root.updateLyrics(position)
            }
            property MediaPlayer instr: MediaPlayer {
                audioOutput: AudioOutput { volume: 1.0 }
            }
        }
    }
    readonly property var vocalsPlayer: playersLoader.item ? playersLoader.item.vocals : null
    readonly property var instrPlayer:  playersLoader.item ? playersLoader.item.instr  : null
    readonly property int  vDuration: vocalsPlayer ? vocalsPlayer.duration : 0
    readonly property int  vPosition: vocalsPlayer ? vocalsPlayer.position : 0
    readonly property bool vPlaying:  vocalsPlayer ? vocalsPlayer.playbackState === MediaPlayer.PlayingState : false

    function playBoth()  { if (vocalsPlayer) { vocalsPlayer.play();  instrPlayer.play()  } }
    function pauseBoth() { if (vocalsPlayer) { vocalsPlayer.pause(); instrPlayer.pause() } }
    function stopBoth()  { if (vocalsPlayer) { vocalsPlayer.stop();  instrPlayer.stop()  } }
    function seekBoth(ms){ if (vocalsPlayer) { vocalsPlayer.position = ms; instrPlayer.position = ms } }

    Timer {
        interval: 1000; repeat: true
        running: root.processing || root.analyzing
        onTriggered: root.elapsedSec++
    }
    function fmtTime(sec) {
        var m = Math.floor(sec / 60), s = sec % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    // Шаги обработки (по диапазонам процентов из KaraokeWorker)
    readonly property var steps: [
        { name: "Подготовка аудио",                from: 0,  to: 10 },
        { name: "Разделение вокала и музыки",      from: 10, to: 66 },
        { name: "Поиск текста онлайн",             from: 66, to: 68 },
        { name: "Распознавание слов нейросетью",   from: 68, to: 97, optional: true },
        { name: "Сборка субтитров",                from: 97, to: 100 }
    ]
    // "done" | "active" | "pending" | "skipped"
    function stepState(i) {
        var st = steps[i], p = root.progressPct
        if (st.optional && !root.usedWhisper)
            return p >= st.to ? "skipped" : (p >= st.from ? "skipped" : "pending")
        if (p >= st.to) return "done"
        if (p >= st.from) return "active"
        return "pending"
    }
    function stepProgress(i) {
        var st = steps[i]
        return Math.max(0, Math.min(1, (root.progressPct - st.from) / (st.to - st.from)))
    }

    // Поле ввода в стиле приложения
    component NameField: TextField {
        id: nf
        width: parent ? parent.width : implicitWidth
        height: Settings.us(46)
        verticalAlignment: TextInput.AlignVCenter
        color: "white"; font.pixelSize: Settings.fs(18); font.family: appFont.name
        placeholderTextColor: Settings.accent(0.333)
        selectByMouse: true
        leftPadding: 14; rightPadding: 14
        background: Rectangle {
            radius: 10; clip: true
            gradient: Gradient {
                GradientStop { position: 0.0; color: Settings.accent(0.133) }
                GradientStop { position: 0.5; color: Settings.accent(0.082) }
                GradientStop { position: 1.0; color: Settings.accent(0.133) }
            }
            Rectangle {
                anchors.fill: parent; radius: 10; color: "transparent"
                border.color: nf.activeFocus ? Settings.accent(0.9) : Settings.accent(0.333)
                border.width: 1.5
                Behavior on border.color { ColorAnimation { duration: 150 } }
            }
        }
    }

    function confirmCurrent() {
        if (titleField.text.trim() === "") return
        root.songArtist   = artistField.text.trim()
        root.songTitle    = titleField.text.trim()
        root.confirming   = false
        root.processing   = true
        root.progressPct  = 0
        root.progressStage = "Запуск..."
        root.usedWhisper  = false
        KaraokeModule.confirmSong(root.songArtist, root.songTitle)
    }

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
            root.processing      = false
            root.confirming      = false
            root.analyzing       = true
            root.errorText       = ""
            root.analysisMsg     = "Читаю теги и имя файла…"
            root.analysisStream  = ""
            root.usedWhisper     = false
            root.elapsedSec      = 0
            root.progressPct     = 0
            root.progressStage   = ""
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
        visible: !root.processing && !root.playerReady && !root.analyzing && !root.confirming

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/assets/icons/ic_karaoke.png"
            width: Settings.us(110); height: width
            sourceSize.width: 256; sourceSize.height: 256
            smooth: true; mipmap: true
            opacity: 0.85
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Выберите трек для караоке"
            font.family: appFont.name
            font.pixelSize: Settings.fs(28)
            color: "white"; opacity: 0.8
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.errorText !== ""
            text: "⚠ " + root.errorText
            font.family: appFont.name; font.pixelSize: Settings.fs(18)
            color: "#ffb86b"
            width: Math.min(implicitWidth, root.width * 0.7)
            wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
        }
        StyledButton {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Выбрать файл"
            width: Math.max(Settings.us(300), naturalWidth)
            onClicked: filePicker.open()
        }
    }

    // ── Анализ имени (теги → нейросеть) ──────────────────────────────────────
    Rectangle {
        id: analyzeCard
        anchors.centerIn: parent
        visible: root.analyzing
        width: Math.min(root.width * 0.7, Settings.us(760))
        height: analyzeCol.implicitHeight + Settings.us(56)
        radius: 16; color: "#CC111120"
        border.color: "#33ffffff"; border.width: 1

        Column {
            id: analyzeCol
            anchors.centerIn: parent
            width: parent.width - Settings.us(56)
            spacing: Settings.us(16)

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Settings.us(14)
                Spinner { running: root.analyzing; size: Settings.us(40) }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.analysisMsg
                    font.family: appFont.name; font.pixelSize: Settings.fs(22)
                    color: "white"
                }
            }

            // «Мысли» нейросети в реальном времени
            Rectangle {
                visible: root.analysisStream !== ""
                width: parent.width
                height: streamText.implicitHeight + Settings.us(24)
                radius: 10; color: "#33000000"
                border.color: Settings.accent(0.333); border.width: 1
                Text {
                    id: streamText
                    anchors.fill: parent; anchors.margins: Settings.us(12)
                    text: root.analysisStream + "▍"
                    font.family: "Consolas"; font.pixelSize: Settings.fs(15)
                    color: Settings.accent(1.0)
                    wrapMode: Text.WrapAnywhere
                }
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.fmtTime(root.elapsedSec)
                font.family: appFont.name; font.pixelSize: Settings.fs(16)
                color: "#80ffffff"
            }
            StyledButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Отмена"; width: Math.max(Settings.us(200), naturalWidth)
                onClicked: KaraokeModule.cancel()
            }
        }
    }

    // ── Подтверждение названия ────────────────────────────────────────────────
    Rectangle {
        id: confirmCard
        anchors.centerIn: parent
        visible: root.confirming
        width: Math.min(root.width * 0.7, Settings.us(760))
        height: confirmCol.implicitHeight + Settings.us(56)
        radius: 16; color: "#CC111120"
        border.color: "#33ffffff"; border.width: 1

        Column {
            id: confirmCol
            anchors.centerIn: parent
            width: parent.width - Settings.us(56)
            spacing: Settings.us(14)

            Text {
                text: "Проверьте название песни"
                font.family: appFont.name; font.pixelSize: Settings.fs(28)
                color: "white"
            }

            // Откуда взято предложение
            Rectangle {
                height: srcText.implicitHeight + Settings.us(12)
                width:  srcText.implicitWidth  + Settings.us(28)
                radius: height / 2
                color: root.proposedSource === "manual" ? "#33ffb86b"
                     : root.proposedSource === "ai"     ? Settings.accent(0.2)
                     : root.proposedSource === "tags"   ? "#3344cc88" : "#22ffffff"
                Text {
                    id: srcText
                    anchors.centerIn: parent
                    text: root.proposedSource === "tags"   ? "Из тегов файла"
                        : root.proposedSource === "ai"     ? "Определено нейросетью — проверьте"
                        : root.proposedSource === "manual" ? "Не удалось определить — введите вручную"
                        :                                    "Из имени файла"
                    font.family: appFont.name; font.pixelSize: Settings.fs(15)
                    color: root.proposedSource === "manual" ? "#ffb86b" : "white"
                }
            }

            Text {
                width: parent.width
                text: "Файл: " + root.proposedFile
                font.family: appFont.name; font.pixelSize: Settings.fs(14)
                color: "#80ffffff"; elide: Text.ElideMiddle
            }

            Text {
                text: "Исполнитель"
                font.family: appFont.name; font.pixelSize: Settings.fs(15); color: "#b0ffffff"
            }
            NameField {
                id: artistField
                placeholderText: "Можно оставить пустым"
                KeyNavigation.tab: titleField
            }

            Text {
                text: "Название"
                font.family: appFont.name; font.pixelSize: Settings.fs(15); color: "#b0ffffff"
            }
            NameField {
                id: titleField
                placeholderText: "Название песни"
                onAccepted: root.confirmCurrent()
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "По этим данным ищется текст песни онлайн, а они же станут именем папки в кэше."
                font.family: appFont.name; font.pixelSize: Settings.fs(13)
                color: "#60ffffff"
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Settings.us(24)
                StyledButton {
                    text: "Другой файл"; width: Math.max(Settings.us(200), naturalWidth)
                    onClicked: { root.confirming = false; filePicker.open() }
                }
                StyledButton {
                    text: "Продолжить"; width: Math.max(Settings.us(240), naturalWidth)
                    enabled: titleField.text.trim() !== ""
                    opacity: enabled ? 1.0 : 0.4
                    onClicked: root.confirmCurrent()
                }
            }
        }
    }

    // ── Обработка: пошаговый прогресс ────────────────────────────────────────
    Rectangle {
        id: processCard
        anchors.centerIn: parent
        visible: root.processing
        width: Math.min(root.width * 0.7, Settings.us(760))
        height: processCol.implicitHeight + Settings.us(56)
        radius: 16; color: "#CC111120"
        border.color: "#33ffffff"; border.width: 1

        Column {
            id: processCol
            anchors.centerIn: parent
            width: parent.width - Settings.us(56)
            spacing: Settings.us(14)

            Text {
                width: parent.width
                text: (root.songArtist !== "" ? root.songArtist + " — " : "") + root.songTitle
                font.family: appFont.name; font.pixelSize: Settings.fs(26)
                color: "white"; elide: Text.ElideRight
            }
            Text {
                text: root.progressStage + "   ·   " + root.fmtTime(root.elapsedSec)
                font.family: appFont.name; font.pixelSize: Settings.fs(15)
                color: Settings.accent(1.0)
            }

            Repeater {
                model: root.steps.length
                delegate: Item {
                    id: stepRow
                    width: processCol.width
                    height: stepCol.implicitHeight
                    // биндинг сам пересчитывается при смене progressPct / usedWhisper
                    property string stepStatus: root.stepState(index)

                    Row {
                        spacing: Settings.us(14)
                        Item {
                            width: Settings.us(28); height: Settings.us(28)
                            Spinner {
                                anchors.centerIn: parent
                                size: Settings.us(26)
                                running: stepRow.stepStatus === "active"
                                visible: running
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: stepRow.stepStatus !== "active"
                                text: stepRow.stepStatus === "done" ? "✓"
                                    : stepRow.stepStatus === "skipped" ? "↷" : "○"
                                font.pixelSize: Settings.fs(22)
                                color: stepRow.stepStatus === "done" ? Settings.accent(1.0) : "#60ffffff"
                            }
                        }
                        Column {
                            id: stepCol
                            spacing: Settings.us(6)
                            Text {
                                text: root.steps[index].name
                                      + (stepRow.stepStatus === "skipped" ? " — пропущено, текст найден онлайн" : "")
                                font.family: appFont.name; font.pixelSize: Settings.fs(19)
                                color: stepRow.stepStatus === "active" ? "white"
                                     : stepRow.stepStatus === "done"   ? "#d0ffffff" : "#60ffffff"
                                font.bold: stepRow.stepStatus === "active"
                            }
                            Rectangle {
                                visible: stepRow.stepStatus === "active"
                                width: processCol.width - Settings.us(44); height: Settings.us(6)
                                radius: 3; color: "#33ffffff"
                                Rectangle {
                                    width: parent.width * root.stepProgress(index)
                                    height: parent.height; radius: 3
                                    color: Settings.accent(1.0)
                                    Behavior on width { NumberAnimation { duration: 300 } }
                                }
                            }
                        }
                    }
                }
            }

            // Общий прогресс
            Rectangle {
                width: parent.width; height: Settings.us(8)
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
                font.family: appFont.name; font.pixelSize: Settings.fs(16)
                color: Settings.accent(1.0)
            }
            StyledButton {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Отмена"; width: Math.max(Settings.us(200), naturalWidth)
                onClicked: KaraokeModule.cancel()
            }
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
                    id: lineItem
                    width: lyricsView.width
                    height: lineText.implicitHeight + Settings.us(4)

                    property int absIdx: {
                        var al    = root.activeLine < 0 ? 0 : root.activeLine
                        var start = Math.max(0, al - 2)
                        return start + index
                    }
                    property bool isActive: absIdx === root.activeLine

                    // Строка целиком: перенос по ширине, подсветка слов через StyledText
                    Text {
                        id: lineText
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode:   Text.WordWrap
                        textFormat: Text.StyledText
                        text: root.lineHtml(modelData, lineItem.isActive ? root.activeWord : -1)
                        font.family:    appFont.name
                        font.pixelSize: lineItem.isActive ? Settings.fs(34) : Settings.fs(22)
                        color: lineItem.isActive ? "white" : "#80ffffff"
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
                            width: root.vDuration > 0
                                   ? parent.width * root.vPosition / root.vDuration : 0
                            height: parent.height; radius: 2
                            color: Settings.accent(1.0)
                            Behavior on width { NumberAnimation { duration: 100 } }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (root.vDuration > 0)
                                seekBoth(mouseX / width * root.vDuration)
                        }
                    }
                }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(24)

                    StyledButton {
                        text: "Другой трек"; width: Math.max(Settings.us(180), naturalWidth)
                        onClicked: { stopBoth(); root.playerReady = false; filePicker.open() }
                    }
                    StyledButton {
                        text: root.vPlaying
                               ? "Пауза" : "Играть"
                        width: Math.max(Settings.us(160), naturalWidth)
                        onClicked: {
                            if (root.vPlaying)
                                pauseBoth()
                            else
                                playBoth()
                        }
                    }
                    StyledButton {
                        text: "Стоп"; width: Math.max(Settings.us(140), naturalWidth)
                        onClicked: stopBoth()
                    }
                    StyledButton {
                        text: root.vocalsOn ? "Вокал: вкл" : "Вокал: выкл"
                        width: Math.max(Settings.us(200), naturalWidth)
                        onClicked: root.vocalsOn = !root.vocalsOn
                    }
                }
            }
        }
    }
}
