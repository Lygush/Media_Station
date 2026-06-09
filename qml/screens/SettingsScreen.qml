// SettingsScreen.qml
import QtQuick
import QtQuick.Controls.Basic 2.15
import QtQuick.Window
import Qt5Compat.GraphicalEffects
import AppSettings 1.0
import "../base"
import "../dialogs"
import "../settings"

Item {
    id: settingsScreen

    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    // Ссылка на корневое окно — нужна AppearanceSection для fullscreen.
    property var appWindow: null

    property string screenTitle:    Settings.safeTr("settings")
    property string activeCategory: ""

    function forceHoverCheck() {}
    function closeDialogs() {
        if (activeCategory !== "") { activeCategory = ""; return true }
        if (settingsFileBrowser.visible) { settingsFileBrowser.close(); return true }
        if (root.promptDialog && root.promptDialog.visible) { root.promptDialog.close(); return true }
        return false
    }

    FileBrowserDialog { id: settingsFileBrowser }

    readonly property int _topOffset: Settings.us(108)
    readonly property int _cardW:     Settings.us(820)
    readonly property int _cardH:     settingsScreen.height - _topOffset - Settings.us(110)

    Item {
        id: cardRoot
        x: Math.round((settingsScreen.width - _cardW) / 2)
        y: _topOffset
        width:  _cardW
        height: _cardH

        GlowingCard {
            anchors.fill: parent
            radius: 22

            // ── Сетка плиток ──────────────────────────────────────────────────
            Item {
                id: tilesView
                anchors.fill: parent
                anchors.margins: Settings.us(28)
                opacity: settingsScreen.activeCategory === "" ? 1.0 : 0.0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 180 } }

                Row {
                    id: tilesHeader
                    anchors.top: parent.top
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: Settings.us(10)

                    Image {
                        source: "qrc:/assets/icons/ic_settings.png"
                        width: Settings.us(20); height: Settings.us(20)
                        sourceSize.width: 100; sourceSize.height: 100
                        fillMode: Image.PreserveAspectFit
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: Settings.safeTr("settings")
                        color: "white"; font.pixelSize: Settings.fs(18); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Rectangle {
                    id: tilesDiv
                    anchors.top: tilesHeader.bottom; anchors.topMargin: Settings.us(14)
                    width: parent.width; height: 1
                    color: Settings.accent(0.3)
                }

                Item {
                    id: tilesGrid
                    anchors.top: tilesDiv.bottom; anchors.topMargin: Settings.us(16)
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: parent.bottom; anchors.bottomMargin: Settings.us(12)

                    readonly property int gap:   Settings.us(12)
                    readonly property int tileW: Math.round((width  - gap * 2) / 3)
                    readonly property int tileH: Math.min(tileW, Math.round((height - gap) / 2))

                    SettingsTile {
                        x: 0; y: 0
                        tileWidth: tilesGrid.tileW; tileHeight: tilesGrid.tileH
                        titleKey: "general"; iconSrc: "qrc:/assets/icons/ic_settings.png"; iconFallback: "⚙"
                        onClicked: settingsScreen.activeCategory = "general"
                    }
                    SettingsTile {
                        x: tilesGrid.tileW + tilesGrid.gap; y: 0
                        tileWidth: tilesGrid.tileW; tileHeight: tilesGrid.tileH
                        titleKey: "appearance"; iconSrc: "qrc:/assets/icons/ic_palette.png"; iconFallback: "🎨"
                        onClicked: settingsScreen.activeCategory = "appearance"
                    }
                    SettingsTile {
                        x: (tilesGrid.tileW + tilesGrid.gap) * 2; y: 0
                        tileWidth: tilesGrid.tileW; tileHeight: tilesGrid.tileH
                        titleKey: "games"; iconSrc: "qrc:/assets/icons/ic_games.png"; iconFallback: "🎮"
                        onClicked: settingsScreen.activeCategory = "games"
                    }

                    readonly property int row2x: Math.round((width - tileW * 2 - gap) / 2)
                    readonly property int row2y: tileH + gap

                    SettingsTile {
                        x: tilesGrid.row2x; y: tilesGrid.row2y
                        tileWidth: tilesGrid.tileW; tileHeight: tilesGrid.tileH
                        titleKey: "android_tv"; iconSrc: "qrc:/assets/icons/ic_tv.png"; iconFallback: "📺"
                        onClicked: settingsScreen.activeCategory = "androidtv"
                    }
                    SettingsTile {
                        x: tilesGrid.row2x + tilesGrid.tileW + tilesGrid.gap; y: tilesGrid.row2y
                        tileWidth: tilesGrid.tileW; tileHeight: tilesGrid.tileH
                        titleKey: "log_upload"; iconSrc: "qrc:/assets/icons/ic_logs.png"; iconFallback: "📋"
                        onClicked: settingsScreen.activeCategory = "logs"
                    }
                }
            }

            // ── Детальный вид ─────────────────────────────────────────────────
            Item {
                id: detailView
                anchors.fill: parent
                anchors.margins: Settings.us(28)
                opacity: settingsScreen.activeCategory !== "" ? 1.0 : 0.0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 180 } }

                Row {
                    id: detailHeader
                    anchors.top: parent.top
                    width: parent.width
                    spacing: Settings.us(10)

                    Item {
                        width: Settings.us(30); height: Settings.us(30)
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.fill: parent; radius: 8
                            color: backBtnMA.containsMouse ? Settings.accent(0.4) : Settings.accent(0.18)
                            border.color: Settings.accent(0.5); border.width: 1.5
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/assets/icons/ic_up.png"
                            width: Settings.us(16); height: Settings.us(16)
                            sourceSize.width: 100; sourceSize.height: 100
                            fillMode: Image.PreserveAspectFit
                        }
                        MouseArea {
                            id: backBtnMA; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (settingsScreen.activeCategory === "androidtv"
                                        && detailLoader.item
                                        && detailLoader.item.hasOwnProperty("subView")
                                        && detailLoader.item.subView !== "tiles") {
                                    detailLoader.item.subView = "tiles"
                                } else {
                                    if (settingsScreen.activeCategory === "androidtv"
                                            && AndroidTV && AndroidTV.profileManager)
                                        AndroidTV.profileManager.save()
                                    settingsScreen.activeCategory = ""
                                }
                            }
                        }
                    }

                    Image {
                        source: {
                            switch (settingsScreen.activeCategory) {
                                case "appearance": return "qrc:/assets/icons/ic_palette.png"
                                case "games":      return "qrc:/assets/icons/ic_games.png"
                                case "androidtv":  return "qrc:/assets/icons/ic_tv.png"
                                case "logs":       return "qrc:/assets/icons/ic_logs.png"
                                default:           return "qrc:/assets/icons/ic_settings.png"
                            }
                        }
                        width: Settings.us(20); height: Settings.us(20)
                        sourceSize.width: 100; sourceSize.height: 100
                        fillMode: Image.PreserveAspectFit
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        text: {
                            switch (settingsScreen.activeCategory) {
                                case "general":    return Settings.safeTr("general")
                                case "appearance": return Settings.safeTr("appearance")
                                case "games":      return Settings.safeTr("games")
                                case "androidtv":  return Settings.safeTr("android_tv")
                                case "logs":       return Settings.safeTr("log_upload")
                                default:           return ""
                            }
                        }
                        color: "white"; font.pixelSize: Settings.fs(18); font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Rectangle {
                    id: detailDiv
                    anchors.top: detailHeader.bottom; anchors.topMargin: Settings.us(14)
                    width: parent.width; height: 1
                    color: Settings.accent(0.3)
                }

                Loader {
                    id: detailLoader
                    anchors.top: detailDiv.bottom; anchors.topMargin: Settings.us(8)
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: parent.bottom

                    sourceComponent: {
                        switch (settingsScreen.activeCategory) {
                            case "general":    return generalSectionComp
                            case "appearance": return appearanceSectionComp
                            case "games":      return gamesSectionComp
                            case "androidtv":  return androidTvSectionComp
                            case "logs":       return logsSectionComp
                            default:           return null
                        }
                    }
                }
            }
        }
    }

    // ── Компоненты секций ─────────────────────────────────────────────────────

    Component {
        id: generalSectionComp
        GeneralSection {}
    }

    Component {
        id: appearanceSectionComp
        AppearanceSection {
            appWindow: settingsScreen.appWindow
        }
    }

    Component {
        id: gamesSectionComp
        GamesSection {}
    }

    Component {
        id: androidTvSectionComp
        AndroidTvSettings {}
    }

    Component {
        id: logsSectionComp
        LogsSection {}
    }
}
