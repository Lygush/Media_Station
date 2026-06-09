// HomeScreen.qml
// ─────────────────────────────────────────────────────────────────────────────
// HomeScreen — стартовый экран с двумя плитками: ТВ и Игры.
//
// Это единственный экран без tilesModel — плитки статические.
// closeDialogs() всегда возвращает false (диалогов нет).
//
// forceHoverCheck() вызывается из Main.qml после завершения
// анимации StackView, чтобы подсветить плитку под курсором.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import QtQuick.Layouts
import AppSettings 1.0
import "../tiles"

Item {
    id: homeScreen
    width:  parent ? parent.width  : 0
    height: parent ? parent.height : 0

    property string screenTitle: "Home"
    property bool   dialogOpen:  false   ///< всегда false — диалогов нет

    // ── Плитки ────────────────────────────────────────────────────────────────
    RowLayout {
        id: rowLayout
        anchors.centerIn: parent
        spacing: 80
        opacity: homeScreen.dialogOpen ? 0.15 : 1.0
        Behavior on opacity { NumberAnimation { duration: 200 } }

        Tile {
            tileW: 460; tileH: 280
            title: Settings.tr("TV")
            iconSource: "qrc:/assets/icons/ic_tv.png"
            enableContextMenu: false
            onClicked: root.openScreen(Settings.tr("TV"), "tv", false)
        }

        Tile {
            tileW: 460; tileH: 280
            title: Settings.tr("Games")
            iconSource: "qrc:/assets/icons/ic_games.png"
            enableContextMenu: false
            onClicked: root.openScreen(Settings.tr("Games"), "games", false)
        }

    }



    // ── Невидимая MouseArea для сброса forceHover ─────────────────────────────
    MouseArea {
        id: globalCursor
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        z: -1
        onPositionChanged: {
            for (var i = 0; i < rowLayout.children.length; i++) {
                var c = rowLayout.children[i]
                if (c.hasOwnProperty("forceHover"))
                    c.forceHover = false
            }
        }
    }

    // ── Публичные методы ──────────────────────────────────────────────────────

    // Определяет, какая плитка находится под курсором, и выставляет forceHover.
    function forceHoverCheck() {
        var mx = globalCursor.mouseX
        var my = globalCursor.mouseY

        // Сначала сбрасываем все
        for (var i = 0; i < rowLayout.children.length; i++) {
            var c = rowLayout.children[i]
            if (c.hasOwnProperty("forceHover"))
                c.forceHover = false
        }

        // Затем находим плитку под курсором
        for (var k = 0; k < rowLayout.children.length; k++) {
            var t  = rowLayout.children[k]
            if (!t.hasOwnProperty("forceHover")) continue
            var pt = t.mapFromItem(homeScreen, mx, my)
            if (pt.x >= 0 && pt.x <= t.width && pt.y >= 0 && pt.y <= t.height) {
                t.forceHover = true; break
            }
        }
    }

    function closeDialogs() { return false }

    Component.onCompleted: Qt.callLater(forceHoverCheck)
}
