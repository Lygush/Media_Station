// qml/screens/GameDetail/GameDetailLeftPanel.qml
import QtQuick
import QtQuick.Effects
import Qt5Compat.GraphicalEffects
import AppSettings 1.0
import "../../base"

// Используем компоненты из той же папки GameDetail/
import "."

Item {
    id: leftPanel

    // ── Данные ────────────────────────────────────────────────────────────────
    property string title:      ""
    property string cover:      ""
    property string genre:      ""
    property string year:       ""
    property string developer:  ""
    property string rating:     ""
    property bool   infoFetched: false
    property bool   fetching:   false
    property string path:       ""
    property string tileId:     ""
    property var    gameData:   ({})

    // Состояние попапов — управляется из GameDetailScreen через эти свойства
    property var    _dbCandidates:    []
    property bool   _dbPopupVisible:  false
    property bool   _jsonPopupVisible: false
    property string _jsonText:        ""

    signal launchRequested()
    signal fetchRequested()

    // ── Вспомогательная функция URL ───────────────────────────────────────────
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

    Column {
        id: mainCol
        anchors { left: parent.left; right: parent.right; top: parent.top }
        spacing: Settings.us(20)

        // ── Обложка ───────────────────────────────────────────────────────────
        // Размер карточки постоянный (от высоты панели, не от данных) — ничего
        // не прыгает. Вертикальная обложка Steam (600×900) заполняет карточку;
        // горизонтальная шапка (460×215 — у игр без вертикальной обложки)
        // показывается целиком по центру на тёмном фоне, без обрезки.
        Item {
            id: coverBox
            // Всё остальное в панели (оценка, не зависит от данных): название в
            // 2 строки, 4 строки характеристик, кнопки и место под стадии
            // загрузки с полосой трейлера и «Отменить» — чтобы при загрузке
            // ничего не уезжало за нижний край экрана
            readonly property real _stageRowH: Math.max(Settings.us(26), Math.ceil(Settings.fs(13) * 1.5)) + Settings.us(2)
            readonly property real _restBase: Math.ceil(Settings.fs(36) * 1.25) * 2
                + 4 * (Math.max(Settings.us(22), Math.ceil(Settings.fs(14) * 1.4)) + Settings.us(6))
                + Settings.us(56) + Settings.us(48)
                + 4 * mainCol.spacing
            // Низкое окно / крупный шрифт: все 4 стадии не помещаются — показываем
            // только текущую (см. stageIndicator)
            readonly property bool compactStages: leftPanel.height < Settings.us(130) + _restBase + 4 * _stageRowH
            readonly property real _rest: _restBase + (compactStages ? 1 : 4) * _stageRowH
            // Совсем низкое окно — обложку не показываем вовсе, а не жмём в марку
            readonly property real _avail: Math.min(Settings.us(330), leftPanel.height * 0.34,
                                                    leftPanel.height - _rest - mainCol.spacing)
            visible: _avail >= Settings.us(80)
            height: visible ? Math.round(_avail) : 0
            width:  Math.round(Math.min(mainCol.width, height * 2 / 3))

            // Пропорции узнаём по отдельной невидимой копии: если завязать
            // fillMode видимой картинки на её же размер — цикл привязок
            readonly property bool _landscape: coverProbe.status === Image.Ready
                                               && coverProbe.implicitWidth > coverProbe.implicitHeight * 0.9
            Image {
                id: coverProbe
                visible: false
                source: coverImage.source
                asynchronous: true
                cache: false
            }

            // Тень — отдельным элементом (DropShadow от слоя с маской рисовал
            // смещённые копии самой картинки)
            RectangularGlow {
                anchors.fill: parent
                anchors.topMargin: 6
                glowRadius: 14
                spread: 0.1
                cornerRadius: 12 + glowRadius
                color: "#88000000"
                opacity: coverImage.status === Image.Ready ? 1.0 : 0.0
            }

            Rectangle {
                id: coverMask2
                anchors.fill: parent
                radius: 12
                color: "black"
                visible: false
            }

            Item {
                id: coverContent
                anchors.fill: parent
                layer.enabled: true
                layer.effect: OpacityMask { cached: true;  maskSource: coverMask2 }

                // Заглушка до загрузки / фон для горизонтальной шапки
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: coverBox._landscape ? "#FF0B1622" : Settings.accent(0.22) }
                        GradientStop { position: 1.0; color: coverBox._landscape ? "#FF05090F" : "#CC06121F" }
                    }
                }
                Image {
                    anchors.centerIn: parent
                    source: "qrc:/assets/icons/ic_games.png"
                    width: Math.round(Math.min(parent.width, parent.height) * 0.42); height: width
                    sourceSize.width: 100; sourceSize.height: 100
                    smooth: true; mipmap: true
                    opacity: coverImage.status === Image.Ready ? 0.0 : 0.35
                    Behavior on opacity { NumberAnimation { duration: 400 } }
                }

                Image {
                    id: coverImage
                    anchors.fill: parent
                    source: leftPanel._fileUrl(leftPanel.cover)
                    fillMode: coverBox._landscape ? Image.PreserveAspectFit : Image.PreserveAspectCrop
                    horizontalAlignment: Image.AlignHCenter
                    verticalAlignment:   Image.AlignVCenter
                    smooth: true; mipmap: true
                    cache: false          // файл перезаписывается при новой загрузке медиа
                    asynchronous: true
                    opacity: status === Image.Ready ? 1.0 : 0.0
                    Behavior on opacity { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }
                }
            }

            // Рамка поверх (у заглушки и у картинки одинаковая)
            Rectangle {
                anchors.fill: parent
                radius: 12
                color: "transparent"
                border.color: Settings.accent(coverImage.status === Image.Ready ? 0.0 : 0.35)
                border.width: 1.5
            }
        }

        // ── Название ──────────────────────────────────────────────────────────
        Text {
            anchors { left: parent.left; right: parent.right }
            text: leftPanel.title
            color: "white"
            font.pixelSize: Settings.fs(36)
            font.bold: true
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            style: Text.Outline
            styleColor: "#33000000"
        }

        // ── Мета: жанр, год, разработчик, рейтинг ────────────────────────────
        // Поля приходят по одному — высота блока меняется плавно, кнопки ниже
        // съезжают анимацией, а не рывком.
        Item {
            id: metaBox
            width: parent.width
            readonly property bool hasMeta: leftPanel.genre !== "" || leftPanel.year !== ""
                                            || leftPanel.developer !== "" || leftPanel.rating !== ""
            visible: hasMeta || height > 0
            height: hasMeta ? metaCol.implicitHeight : 0
            clip: true
            Behavior on height { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

        Column {
            id: metaCol
            width: parent.width
            spacing: Settings.us(6)

            // Компонент одной мета-строки
            component MetaRow: Row {
                property string icon:  ""
                property string value: ""
                spacing: Settings.us(8)
                height:  Math.max(Settings.us(22), Math.ceil(Settings.fs(14) * 1.4))

                // Значки — картинки в стиле остальных иконок (эмодзи Windows
                // рисует цветными и разного размера)
                Image {
                    source: parent.icon
                    width: Math.round(Settings.fs(14) * 1.3); height: width
                    sourceSize.width: 100; sourceSize.height: 100
                    smooth: true; mipmap: true
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: parent.value
                    color: "#CCFFFFFF"
                    font.pixelSize: Settings.fs(14)
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    width: leftPanel.width - Math.round(Settings.fs(14) * 1.3) - Settings.us(8)
                }
            }

            // Поля появляются с задержкой и анимацией — по мере fieldsReady
            MetaRow {
                icon: "qrc:/assets/icons/ic_games.png"; value: leftPanel.genre
                visible: leftPanel.genre !== ""
                opacity: leftPanel.genre !== "" ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
            }
            MetaRow {
                icon: "qrc:/assets/icons/ic_calendar.png"; value: leftPanel.year
                visible: leftPanel.year !== ""
                opacity: leftPanel.year !== "" ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
            }
            MetaRow {
                icon: "qrc:/assets/icons/ic_building.png"; value: leftPanel.developer
                visible: leftPanel.developer !== ""
                opacity: leftPanel.developer !== "" ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
            }
            MetaRow {
                icon: "qrc:/assets/icons/ic_star.png"; value: leftPanel.rating
                visible: leftPanel.rating !== ""
                opacity: leftPanel.rating !== "" ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.OutCubic } }
            }
        }
        }

        // ── Кнопка ЗАПУСКА ────────────────────────────────────────────────────
        Item {
            width: parent.width
            height: Settings.us(56)

            Rectangle {
                id: launchBtn
                anchors.fill: parent
                radius: 12
                color: launchBtnMA.containsMouse ? Settings.accent(1.0)
                                                 : Settings.accent(0.85)
                Behavior on color { ColorAnimation { duration: 150 } }
                scale: launchBtnMA.pressed ? 0.96 : 1.0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(10)
                    Text {
                        text: "▶"
                        color: "white"
                        font.pixelSize: Settings.fs(20)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: Settings.tr("launch_game")
                        color: "white"
                        font.pixelSize: Settings.fs(20)
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: launchBtnMA
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: leftPanel.launchRequested()
                }
            }

            Glow {
                cached: true
                visible: opacity > 0   // невидимое свечение не считать каждый кадр
                anchors.fill: launchBtn; source: launchBtn
                radius: 12; samples: 20
                color: Settings.accent(1.0)
                opacity: launchBtnMA.containsMouse ? 0.7 : 0.0
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
        }

        // ── Кнопка «Получить информацию» ──────────────────────────────────────
        // Пока идёт загрузка трейлера, в этой же кнопке — полоса прогресса,
        // проценты и отмена (раньше отдельной строкой под стадиями, которая
        // уезжала за нижний край экрана)
        Item {
            id: statusBtn
            width: parent.width
            height: Settings.us(48)
            readonly property bool trailer: GameMediaModule.fetchingTrailer
            visible: !leftPanel.infoFetched || trailer

            Rectangle {
                anchors.fill: parent
                radius: 12
                clip: true
                color: (leftPanel.fetching || statusBtn.trailer)
                       ? "#44FFFFFF"
                       : (fetchBtnMA.containsMouse ? "#55FFFFFF" : "#33FFFFFF")

                // Заполнение — прогресс трейлера
                Rectangle {
                    visible: statusBtn.trailer
                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                    width: parent.width * GameMediaModule.trailerProgress
                    radius: parent.radius
                    color: Settings.accent(0.45)
                    Behavior on width { NumberAnimation { duration: 200 } }
                }
                Behavior on color { ColorAnimation { duration: 150 } }
                scale: fetchBtnMA.pressed ? 0.96 : 1.0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(8)

                    // Вращающаяся дуга (полный круг выглядел как буква «O»)
                    Spinner {
                        size: Settings.us(18)
                        color: "white"
                        running: leftPanel.fetching || statusBtn.trailer
                        visible: leftPanel.fetching || statusBtn.trailer
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        text: statusBtn.trailer
                              ? Settings.tr("gamedetail_trailer")
                              : leftPanel.fetching
                              ? Settings.tr("gamedetail_fetching")
                              : Settings.tr("gamedetail_fetch_info")
                        color: "white"
                        font.pixelSize: Settings.fs(16)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, leftPanel.width - Settings.us(32)
                                        - ((leftPanel.fetching || statusBtn.trailer) ? Settings.us(26) : 0)
                                        - (statusBtn.trailer ? cancelTrailerBtn.width * 2 + pctText.implicitWidth + Settings.us(8) : 0))
                        elide: Text.ElideRight
                    }
                    // Проценты не обрезаются вместе с подписью
                    Text {
                        id: pctText
                        visible: statusBtn.trailer
                        text: Math.round(GameMediaModule.trailerProgress * 100) + "%"
                        color: "white"
                        font.pixelSize: Settings.fs(16); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: fetchBtnMA
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !leftPanel.fetching && !statusBtn.trailer
                    onClicked: leftPanel.fetchRequested()
                }

                // Отмена загрузки трейлера
                Item {
                    id: cancelTrailerBtn
                    visible: statusBtn.trailer
                    anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                    width: height
                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.height * 0.62; height: width; radius: width / 2
                        color: cancelMA.containsMouse ? "#AAE53935" : "#55000000"
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Text {
                            anchors.centerIn: parent
                            text: "✕"; color: "white"
                            font.pixelSize: Math.round(parent.height * 0.5); font.bold: true
                        }
                    }
                    MouseArea {
                        id: cancelMA
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: GameMediaModule.cancelTrailer()
                    }
                }
            }
        }

        // ── Индикатор стадий ──────────────────────────────────────────────────
        Item {
            width: parent.width
            height: stageCol.implicitHeight
            visible: GameInfoModule.fetching
                     || GameMediaModule.fetchingCovers
                     || GameMediaModule.fetchingTrailer

            readonly property int _stageIndex: {
                if (GameInfoModule.stage === "db")         return 0
                if (GameInfoModule.stage === "db_select")  return 0
                if (GameInfoModule.stage === "fetching")   return 1
                if (GameInfoModule.stage === "generating") return 2
                if (GameMediaModule.fetchingCovers || GameMediaModule.fetchingTrailer) return 3
                return -1
            }

            property bool _seenDb:         false
            property bool _seenFetching:   false
            property bool _seenGenerating: false
            property bool _seenMedia:      false

            id: stageIndicator

            onVisibleChanged: {
                if (!visible) {
                    _seenDb = false; _seenFetching = false
                    _seenGenerating = false; _seenMedia = false
                }
            }

            Connections {
                target: GameInfoModule
                function onStageChanged() {
                    var s = GameInfoModule.stage
                    if (s === "fetching")   stageIndicator._seenDb        = true
                    if (s === "generating") stageIndicator._seenFetching  = true
                    if (s === "")           stageIndicator._seenGenerating = true
                }
            }
            Connections {
                target: GameMediaModule
                function onFetchingCoversChanged() {
                    if (!GameMediaModule.fetchingCovers && !GameMediaModule.fetchingTrailer)
                        stageIndicator._seenMedia = true
                }
                function onFetchingTrailerChanged() {
                    if (!GameMediaModule.fetchingCovers && !GameMediaModule.fetchingTrailer)
                        stageIndicator._seenMedia = true
                }
            }

            Column {
                id: stageCol
                anchors { left: parent.left; right: parent.right }
                spacing: Settings.us(2)

                GameDetailStageRow {
                    id: _stageRowDb
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_db")
                    active: stageIndicator._stageIndex === 0
                    visible: !coverBox.compactStages || active
                    done:   stageIndicator._seenDb
                }

                GameDetailStageRow {
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_fetching")
                    active: stageIndicator._stageIndex === 1
                    visible: !coverBox.compactStages || active
                    done:   stageIndicator._seenFetching
                }

                GameDetailStageRow {
                    id: _stageRowGenerating
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_generating")
                    active: stageIndicator._stageIndex === 2
                    visible: !coverBox.compactStages || active
                    done:   stageIndicator._seenGenerating
                }

                GameDetailStageRow {
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_media")
                    active: stageIndicator._stageIndex === 3
                    visible: !coverBox.compactStages || active
                    done:   stageIndicator._seenMedia
                }

            }
        }

        // Заглушка "нет данных"
        Text {
            visible: !leftPanel.infoFetched && !leftPanel.fetching
                     && leftPanel.genre === "" && leftPanel.developer === ""
            text: Settings.tr("gamedetail_no_info")
            color: "#88FFFFFF"
            font.pixelSize: Settings.fs(13)
            wrapMode: Text.WordWrap
            width: parent.width
        }
    }

    // Попап не выходит за правый край экрана
    readonly property real _popupMaxWidth: (leftPanel.parent ? leftPanel.parent.width : 1920)
                                           - leftPanel.x - leftPanel.width - Settings.us(12) - Settings.us(40)

    // Попап не выходит за верх/низ окна (координаты — в системе панели)
    function _popupY(anchorY, h) {
        var screenH = leftPanel.parent ? leftPanel.parent.height : leftPanel.height
        var maxY = screenH - leftPanel.y - h - Settings.us(24)
        var minY = -leftPanel.y + Settings.us(24)
        return Math.max(minY, Math.min(anchorY, maxY))
    }
    function _popupTip(tip, h) {
        return Math.max(Settings.us(14), Math.min(tip, h - Settings.us(14)))
    }

    // ── Попапы — фиксированы справа от индикатора, хвостик указывает на строку
    GameDetailDbPopup {
        id: _dbPopup
        z: 999
        candidates: leftPanel._dbCandidates
        isVisible:  leftPanel._dbPopupVisible
        maxWidth:   leftPanel._popupMaxWidth

        x: leftPanel.width + Settings.us(12)
        // Верх — у строки этапа, но не ниже, чем позволяет экран: раньше
        // попап уходил за нижний край. Хвостик всё равно указывает на строку.
        readonly property real _anchorY: stageIndicator.y + mainCol.y
        y: leftPanel._popupY(_anchorY, height)
        tipY: leftPanel._popupTip(_anchorY + _stageRowDb.y + _stageRowDb.height / 2 - y, height)

        onCandidateSelected: function(appId) {
            leftPanel._dbPopupVisible = false
            GameInfoModule.selectCandidate(leftPanel.tileId, appId)
        }
    }

    GameDetailJsonPopup {
        id: _jsonPopup
        z: 999
        isVisible: leftPanel._jsonPopupVisible
        jsonText:  leftPanel._jsonText
        maxWidth:  leftPanel._popupMaxWidth

        x: leftPanel.width + Settings.us(12)
        readonly property real _anchorY: stageIndicator.y + mainCol.y
        y: leftPanel._popupY(_anchorY, height)
        tipY: leftPanel._popupTip(_anchorY + _stageRowGenerating.y + _stageRowGenerating.height / 2 - y, height)
    }
}
