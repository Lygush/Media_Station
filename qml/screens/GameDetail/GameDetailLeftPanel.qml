// qml/screens/GameDetail/GameDetailLeftPanel.qml
import QtQuick
import QtQuick.Effects
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

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
        Item {
            width:  Settings.us(220)
            height: Settings.us(300)
            visible: leftPanel.cover !== ""

            Rectangle {
                id: coverMask2
                anchors.fill: parent
                radius: 12
                color: "black"
                visible: false
            }

            Item {
                anchors.fill: parent
                layer.enabled: true
                layer.effect: OpacityMask { maskSource: coverMask2 }

                Image {
                    anchors.fill: parent
                    source: leftPanel._fileUrl(leftPanel.cover)
                    fillMode: Image.PreserveAspectCrop
                    cache: true
                    asynchronous: true
                }
            }

            DropShadow {
                anchors.fill: parent
                source: parent
                radius: 20; samples: 32
                color: "#AA000000"
                verticalOffset: 6
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
        Column {
            width: parent.width
            spacing: Settings.us(6)
            visible: leftPanel.genre !== "" || leftPanel.year !== "" || leftPanel.developer !== ""

            // Компонент одной мета-строки
            component MetaRow: Row {
                property string icon:  ""
                property string value: ""
                spacing: Settings.us(8)
                height:  Settings.us(22)

                Text {
                    text: parent.icon
                    font.pixelSize: Settings.fs(14)
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: parent.value
                    color: "#CCFFFFFF"
                    font.pixelSize: Settings.fs(14)
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    width: leftPanel.width - Settings.us(30)
                }
            }

            // Поля появляются с задержкой и анимацией — по мере fieldsReady
            MetaRow {
                icon: "🎮"; value: leftPanel.genre
                visible: leftPanel.genre !== ""
                opacity: 0.0
                onVisibleChanged: if (visible) showAnim.start()
                NumberAnimation on opacity { id: showAnim; to: 1.0; duration: 350; easing.type: Easing.OutCubic }
            }
            MetaRow {
                icon: "📅"; value: leftPanel.year
                visible: leftPanel.year !== ""
                opacity: 0.0
                onVisibleChanged: if (visible) showAnim2.start()
                NumberAnimation on opacity { id: showAnim2; to: 1.0; duration: 350; easing.type: Easing.OutCubic; running: false }
            }
            MetaRow {
                icon: "🏢"; value: leftPanel.developer
                visible: leftPanel.developer !== ""
                opacity: 0.0
                onVisibleChanged: if (visible) showAnim3.start()
                NumberAnimation on opacity { id: showAnim3; to: 1.0; duration: 350; easing.type: Easing.OutCubic; running: false }
            }
            MetaRow {
                icon: "⭐"; value: leftPanel.rating
                visible: leftPanel.rating !== ""
                opacity: 0.0
                onVisibleChanged: if (visible) showAnim4.start()
                NumberAnimation on opacity { id: showAnim4; to: 1.0; duration: 350; easing.type: Easing.OutCubic; running: false }
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
                anchors.fill: launchBtn; source: launchBtn
                radius: 12; samples: 20
                color: Settings.accent(1.0)
                opacity: launchBtnMA.containsMouse ? 0.7 : 0.0
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
        }

        // ── Кнопка «Получить информацию» ──────────────────────────────────────
        Item {
            width: parent.width
            height: Settings.us(48)
            visible: !leftPanel.infoFetched

            Rectangle {
                anchors.fill: parent
                radius: 12
                color: leftPanel.fetching
                       ? "#44FFFFFF"
                       : (fetchBtnMA.containsMouse ? "#55FFFFFF" : "#33FFFFFF")
                Behavior on color { ColorAnimation { duration: 150 } }
                scale: fetchBtnMA.pressed ? 0.96 : 1.0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(8)

                    Rectangle {
                        width: Settings.us(16); height: Settings.us(16)
                        radius: width / 2
                        color: "transparent"
                        border.color: "white"; border.width: 2
                        visible: leftPanel.fetching
                        anchors.verticalCenter: parent.verticalCenter

                        RotationAnimator on rotation {
                            from: 0; to: 360; duration: 900
                            loops: Animation.Infinite
                            running: leftPanel.fetching
                        }
                    }

                    Text {
                        text: leftPanel.fetching
                              ? Settings.tr("gamedetail_fetching")
                              : Settings.tr("gamedetail_fetch_info")
                        color: "white"
                        font.pixelSize: Settings.fs(16)
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                MouseArea {
                    id: fetchBtnMA
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !leftPanel.fetching
                    onClicked: leftPanel.fetchRequested()
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
                    done:   stageIndicator._seenDb
                }

                GameDetailStageRow {
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_fetching")
                    active: stageIndicator._stageIndex === 1
                    done:   stageIndicator._seenFetching
                }

                GameDetailStageRow {
                    id: _stageRowGenerating
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_generating")
                    active: stageIndicator._stageIndex === 2
                    done:   stageIndicator._seenGenerating
                }

                GameDetailStageRow {
                    width: parent.width
                    label:  Settings.tr("gamedetail_stage_media")
                    active: stageIndicator._stageIndex === 3
                    done:   stageIndicator._seenMedia
                }

                // Прогресс-бар трейлера
                Item {
                    width: parent.width
                    height: progressCol.implicitHeight
                    visible: GameMediaModule.fetchingTrailer

                    Column {
                        id: progressCol
                        width: parent.width
                        spacing: Settings.us(4)

                        // Строка: прогресс-бар + процент
                        Row {
                            width: parent.width
                            spacing: Settings.us(8)

                            Item {
                                width: parent.width - percentText.width - Settings.us(8)
                                height: Settings.us(6)
                                anchors.verticalCenter: parent.verticalCenter

                                Rectangle {
                                    anchors.fill: parent
                                    radius: 3
                                    color: "#33FFFFFF"
                                }
                                Rectangle {
                                    width: parent.width * GameMediaModule.trailerProgress
                                    height: parent.height
                                    radius: 3
                                    color: Settings.accent(1.0)
                                    Behavior on width { NumberAnimation { duration: 200 } }
                                }
                            }

                            Text {
                                id: percentText
                                text: Math.round(GameMediaModule.trailerProgress * 100) + "%"
                                color: "white"
                                font.pixelSize: Settings.fs(11)
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        // Кнопка отмены
                        Text {
                            text: Settings.tr("gamedetail_cancel_trailer")
                            color: "#AA4444"
                            font.pixelSize: Settings.fs(11)
                            leftPadding: Settings.us(2)

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: GameMediaModule.cancelTrailer()
                            }
                        }
                    }
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

    // ── Попапы — фиксированы справа от индикатора, хвостик указывает на строку
    GameDetailDbPopup {
        id: _dbPopup
        z: 999
        candidates: leftPanel._dbCandidates
        isVisible:  leftPanel._dbPopupVisible

        x: leftPanel.width + Settings.us(12)
        y: stageIndicator.y + mainCol.y

        // tipY = расстояние от верха попапа до центра строки _stageRowDb
        tipY: _stageRowDb.y + _stageRowDb.height / 2

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

        x: leftPanel.width + Settings.us(12)
        y: stageIndicator.y + mainCol.y

        // tipY = расстояние от верха попапа до центра строки _stageRowGenerating
        tipY: _stageRowGenerating.y + _stageRowGenerating.height / 2
    }
}
