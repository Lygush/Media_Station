// Tile.qml
// ─────────────────────────────────────────────────────────────────────────────
// Tile — визуальная плитка: ярлык, папка или кнопка добавления.
// Фаза 1: поддержка обложки игры (coverSource).
//   - Если coverSource задан и изображение загружается успешно — показываем
//     обложку на весь фон плитки с градиентным оверлеем снизу.
//   - Если обложки нет — поведение идентично предыдущей версии (иконка).
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0
import "../widgets"

Item {
    id: root

    // ── Размер плитки — задаётся снаружи, не масштабируется ─────────────────
    property int tileW: 380
    property int tileH: 270

    width:  tileW
    height: tileH

    property string title:       ""
    property string icon:        "+"
    property string iconSource:  ""
    property string coverSource: ""   // NEW: путь к обложке (cover.jpg)
    property string type:        ""
    property string tileId:      ""

    property bool isAddButton:       false
    property bool enableContextMenu: true
    property bool forceHover:        false

    property bool   installing:      false
    property real   installProgress: 0.0
    property string installPhase:    ""

    property bool launching: false

    // ── Android TV boot-анимация ──────────────────────────────────────────────
    property bool androidTvBooting: false
    property bool androidTvRunning: false

    readonly property bool _showBootAnim: type === "androidtv" && (androidTvBooting || androidTvRunning)

    // ── Внутреннее состояние обложки ─────────────────────────────────────────
    // coverReady = true только когда Image реально загрузилась без ошибки
    readonly property bool _hasCover: coverImage.status === Image.Ready

    signal clicked()
    signal editRequested()
    signal deleteRequested()
    signal cancelInstallRequested()
    signal folderChosen()
    signal shortcutChosen()
    signal detailsRequested()   // Фаза 2: открыть страницу игры

    property bool keepScale: false

    readonly property string _menuMode: {
        if (root.isAddButton) return "add"
        if (root.installing)  return "installing"
        return "normal"
    }

    readonly property string _phaseLabel: {
        if (!root.installing) return ""
        switch (root.installPhase) {
            case "packing":    return Settings.tr("phase_packing")
            case "receiving":  return Settings.tr("phase_receiving")
            case "extracting": return Settings.tr("phase_extracting")
            default:           return Settings.tr("phase_installing")
        }
    }

    // ══════════════════════════════════════════════════════════════════════════
    // Основной прямоугольник плитки
    // ══════════════════════════════════════════════════════════════════════════
    // Маска для обрезки обложки по скруглениям.
    // Qt6: clip:true не уважает radius — используем OpacityMask.
    // visible:false убирает из рендера, но layer/OpacityMask читают геометрию.
    Rectangle {
        id: coverMask
        anchors.fill: parent
        radius: 18
        color: "black"
        visible: false
    }

    Rectangle {
        id: tile
        anchors.fill: parent
        radius: 18
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#33FFFFFF" }
            GradientStop { position: 0.5; color: "#AA202020" }
            GradientStop { position: 1.0; color: "#33FFFFFF" }
        }

        // ── ОБЛОЖКА + ОВЕРЛЕЙ через OpacityMask ──────────────────────────────
        // coverMask — непрозрачный rect с тем же radius, даёт чистое обрезание.
        Item {
            id: coverLayer
            anchors.fill: parent
            visible: root._hasCover

            layer.enabled: true
            layer.effect: OpacityMask {
                cached: true
                maskSource: coverMask
            }

            Image {
                id: coverImage
                anchors.fill: parent

                source: {
                    if (root.coverSource === "") return ""
                    var s = root.coverSource
                    if (s.startsWith("qrc:") || s.startsWith("file:")) return s
                    return "file:///" + s.replace(/\\/g, "/")
                }

                fillMode:          Image.PreserveAspectCrop
                sourceSize.width:  root.tileW * 2
                sourceSize.height: root.tileH * 2
                cache: true
                asynchronous: true

                opacity: 0
                onStatusChanged: {
                    if (status === Image.Ready)       coverFadeIn.start()
                    else if (status === Image.Error)  opacity = 0
                }
                onSourceChanged: { opacity = 0; coverFadeIn.stop() }

                NumberAnimation {
                    id: coverFadeIn
                    target: coverImage; property: "opacity"
                    from: 0; to: 1; duration: 300; easing.type: Easing.OutCubic
                }
            }

            Rectangle {
                id: coverGradientOverlay
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0.0;  color: "#00000000" }
                    GradientStop { position: 0.55; color: "#00000000" }
                    GradientStop { position: 0.78; color: "#99000000" }
                    GradientStop { position: 1.0;  color: "#CC000000" }
                }
            }
        }

        // ── ИКОНКА (показывается только когда нет обложки) ───────────────────
        Item {
            id: iconContainer
            // По центру плитки, как раньше, но не ниже верхнего края названия:
            // на ужатых плитках (1280 px) и крупном шрифте двухстрочное название
            // заезжало на значок. При нехватке места значок уменьшается.
            readonly property int _gap: Settings.us(8)
            readonly property int _centerY: Math.round((parent.height - height) / 2)
                                            + ((root.installing || root.launching) ? Settings.us(-28) : 0)
            anchors.horizontalCenter: parent.horizontalCenter
            y: Math.max(_gap, Math.min(_centerY, titleText.y - height - _gap))
            Behavior on y { NumberAnimation { duration: 200 } }

            width: Math.max(Settings.us(24), Math.min(Math.round(root.tileW * 0.24),
                                                     titleText.y - 2 * _gap))
            height: width
            property bool imageError: false

            // Скрываем иконку когда показана обложка или когда идёт AndroidTV-запуск
            visible: !root._hasCover
            opacity: (root._showBootAnim ? 0.0 : (root.installing || root.launching) ? 0.5 : 1.0)
            Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

            Image {
                id: tileImage
                anchors.fill: parent
                source: {
                    if (root.iconSource === "") return ""
                    var src = root.iconSource
                    if (src.startsWith("qrc:") || src.startsWith("file:")) return src
                    return "file:///" + src.replace(/\\/g, "/")
                }
                sourceSize.width: 100; sourceSize.height: 100
                fillMode: Image.PreserveAspectFit
                visible: false
                onStatusChanged: {
                    if      (status === Image.Ready) { iconContainer.imageError = false; visible = true  }
                    else if (status === Image.Error) { iconContainer.imageError = true;  visible = false }
                }
                onSourceChanged: { iconContainer.imageError = false; visible = false }
            }

            Image {
                id: defaultIcon
                anchors.fill: parent
                visible: !tileImage.visible && !iconContainer.imageError && root.type !== ""
                source: root.type === "folder"   ? "qrc:/assets/icons/ic_folder.png"
                      : root.type === "shortcut" ? "qrc:/assets/icons/ic_label.png" : ""
                sourceSize.width: 100; sourceSize.height: 100
                fillMode: Image.PreserveAspectFit
            }

            Text {
                anchors.centerIn: parent
                text: root.icon
                font.pixelSize: 64; color: "white"
                visible: !tileImage.visible && !defaultIcon.visible
            }
        }

        // ── ЗАГОЛОВОК ПЛИТКИ ─────────────────────────────────────────────────
        Text {
            id: titleText
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: (root.installing || root.launching) ? statusArea.top : parent.bottom
            anchors.bottomMargin: (root.installing || root.launching) ? Settings.us(10) : Settings.us(14)
            Behavior on anchors.bottomMargin { NumberAnimation { duration: 200 } }

            width: parent.width - Settings.us(24)
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.WordWrap

            text: root.title
            color: "white"; font.pixelSize: Settings.fs(24); font.bold: true
            opacity: root._showBootAnim ? 0.0 : ((root.installing || root.launching) ? 0.7 : 1.0)
            Behavior on opacity { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }

            // Лёгкая тень на тексте — чтобы читался поверх любой обложки
            layer.enabled: root._hasCover
            layer.effect: DropShadow {
                cached: true
                horizontalOffset: 0; verticalOffset: 1
                radius: 6; samples: 13; color: "#CC000000"
            }
        }

        // ── НИЖНЯЯ ОБЛАСТЬ СТАТУСА ───────────────────────────────────────────
        Item {
            id: statusArea
            anchors.bottom: parent.bottom; anchors.bottomMargin: Settings.us(10)
            anchors.left:   parent.left;   anchors.leftMargin:   Settings.us(18)
            anchors.right:  parent.right;  anchors.rightMargin:  Settings.us(18)
            height: Settings.us(26)
            visible: root.installing || root.launching
            opacity: (root.installing || root.launching) ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 200 } }

            // ── Режим УСТАНОВКИ ───────────────────────────────────────────────
            Item {
                anchors.fill: parent
                visible: root.installing && !root.launching

                Text {
                    id: phaseLabel
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: installBar.top; anchors.bottomMargin: Settings.us(7)
                    text: root._phaseLabel
                    color: Settings.accent(1.0); font.pixelSize: Settings.fs(11); font.bold: true
                }

                Item {
                    id: installBar
                    anchors.bottom: parent.bottom
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    height: Settings.us(14)

                    Rectangle {
                        id: trackRect
                        anchors.left: parent.left
                        anchors.right: pctText.left; anchors.rightMargin: Settings.us(6)
                        anchors.verticalCenter: parent.verticalCenter
                        height: Settings.us(4); radius: 2; color: "#33FFFFFF"

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top; anchors.bottom: parent.bottom
                            radius: 2
                            width: Math.max(radius * 2, root.installProgress * parent.width)
                            color: Settings.accent(1.0)
                            Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                        }
                    }

                    Text {
                        id: pctText
                        anchors.right: cancelZone.left; anchors.rightMargin: Settings.us(8)
                        anchors.verticalCenter: parent.verticalCenter
                        text: Math.round(root.installProgress * 100) + "%"
                        color: "white"; font.pixelSize: Settings.fs(10); font.bold: true
                    }

                    Item {
                        id: cancelZone
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: Settings.us(20); height: Settings.us(20); z: 10

                        property bool _hovered: false

                        Rectangle {
                            id: cancelRect
                            anchors.fill: parent; radius: width / 2
                            color:        cancelZone._hovered ? "#99FF3333" : "#44FF3333"
                            border.color: cancelZone._hovered ? "#CCFF5555" : "#55FF4444"
                            border.width: 1.5
                            Behavior on color        { ColorAnimation { duration: 120 } }
                            Behavior on border.color { ColorAnimation { duration: 120 } }
                        }

                        Glow {
                            cached: true
                            visible: opacity > 0   // невидимое свечение не считать каждый кадр
                            anchors.fill: cancelRect; source: cancelRect
                            radius: 8; samples: 16; color: "#FF4444"
                            opacity: cancelZone._hovered ? 0.6 : 0.0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }

                        Text {
                            anchors.centerIn: parent
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: "✕"
                            color: cancelZone._hovered ? "white" : "#DDAAAA"
                            font.pixelSize: Settings.fs(9); font.bold: true
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                    }
                }
            }

            // ── Режим ЗАПУСКА ─────────────────────────────────────────────────
            Item {
                anchors.fill: parent
                visible: root.launching

                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(8)

                    Item {
                        id: spinnerItem
                        width: Settings.us(20); height: Settings.us(20)
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            anchors.fill: parent; radius: width / 2
                            color: "transparent"
                            border.color: "#33FFFFFF"; border.width: 2.5
                        }

                        Item {
                            id: spinnerArc
                            anchors.fill: parent
                            transformOrigin: Item.Center

                            RotationAnimator {
                                target: spinnerArc
                                from: 0; to: 360; duration: 900
                                loops: Animation.Infinite
                                running: root.launching
                                easing.type: Easing.Linear
                            }

                            Item {
                                width: parent.width; height: parent.height / 2
                                anchors.top: parent.top
                                clip: true
                                Rectangle {
                                    width: parent.width; height: parent.width
                                    anchors.top: parent.top; radius: width / 2
                                    color: "transparent"
                                    border.color: Settings.accent(1.0); border.width: 2.5
                                }
                            }
                        }
                    }

                    Text {
                        text: Settings.tr("phase_launching")
                        anchors.verticalCenter: parent.verticalCenter
                        color: Settings.accent(1.0); font.pixelSize: Settings.fs(12); font.bold: true
                    }
                }
            }
        }
    }

    Rectangle {
        id: borderRect
        anchors.fill: parent; radius: 18
        color: "transparent"; border.color: "#80FFFFFF"; border.width: 2
    }

    // ── Boot-анимация Android TV поверх плитки ────────────────────────────────
    Rectangle {
        anchors.fill: parent
        radius: 18
        color:  "transparent"
        clip:   true
        z:      5
        visible: root._showBootAnim

        Rectangle {
            anchors.fill: parent
            anchors.margins: borderRect.border.width
            radius: 16
            color: "black"
            opacity: root._showBootAnim ? 1.0 : 0.0
            z: 0
            Behavior on opacity { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }
        }

        BootAnimationPlayer {
            id: bootAnim
            anchors.fill: parent
            anchors.margins: borderRect.border.width
            opacity: root._showBootAnim ? 1.0 : 0.0
            visible: root._showBootAnim
            z: 1
            booting: root.androidTvBooting
            running: root.androidTvRunning
            Behavior on opacity { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
        }
    }

    Glow {
        cached: true
        visible: opacity > 0   // невидимое свечение не считать каждый кадр
        id: borderGlow
        anchors.fill: borderRect; source: borderRect
        radius: 16; samples: 24; color: Settings.accent(1.0)
        opacity: (hoverHandler.hovered || contextMenu.visible || root.forceHover) ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }

    Glow {
        cached: true
        id: launchGlow
        anchors.fill: borderRect; source: borderRect
        radius: 24; samples: 32; color: Settings.accent(1.0)
        opacity: 0; visible: root.launching

        SequentialAnimation {
            running: root.launching
            loops: Animation.Infinite
            NumberAnimation { target: launchGlow; property: "opacity"; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
            NumberAnimation { target: launchGlow; property: "opacity"; to: 0.1; duration: 700; easing.type: Easing.InOutSine }
        }
    }

    property bool holdProgressVisible: false
    property bool holdBlockedClick: false
    property real holdProgressWidth: 0

    Item {
        id: holdProgressContainer
        anchors.bottom: parent.bottom; anchors.bottomMargin: 6
        anchors.left: parent.left; anchors.leftMargin: Settings.us(18)
        anchors.right: parent.right; anchors.rightMargin: Settings.us(18)
        height: Settings.us(4)
        visible: !root.installing && !root.isAddButton && !root.launching
        opacity: holdProgressVisible ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Rectangle {
            id: holdProgressRect
            anchors.verticalCenter: parent.verticalCenter
            width: holdProgressWidth
            x: (parent.width - width) / 2
            height: parent.height
            radius: 2
            color: Settings.accent(1.0)
        }
    }

    NumberAnimation {
        id: holdAnim; target: root; property: "holdProgressWidth"
        from: 0
        to: root.width - Settings.us(36)
        duration: 1200
        easing.type: Easing.Linear
    }

    scale: (hoverHandler.hovered || keepScale || forceHover || launching) ? 1.05 : 1.0
    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    HoverHandler {
        id: hoverHandler
        onHoveredChanged: {
            if (hovered) {
                root.forceHover = false
                menuCloseTimer.stop()
            } else {
                if (contextMenu.visible && !contextMenu.containsMouse)
                    menuCloseTimer.start()
            }
        }
    }

    Timer {
        id: menuCloseTimer; interval: 1500
        onTriggered: if (contextMenu.visible && !contextMenu.containsMouse) contextMenu.close()
    }

    Timer {
        id: holdProgressDelay; interval: 300; repeat: false
        onTriggered: {
            holdProgressVisible = true
            holdBlockedClick = true
            holdAnim.start()
        }
    }

    Timer {
        id: holdTimer; interval: 1500; repeat: false
        onTriggered: if (!root.isAddButton && root.enableContextMenu) contextMenu.open()
    }

    function _stopHold() {
        holdTimer.stop()
        holdProgressDelay.stop()
        holdAnim.stop()
        holdProgressWidth = 0
        holdProgressVisible = false
        Qt.callLater(function() { holdBlockedClick = false })
    }

    function _overCancel(mx, my) {
        if (!root.installing || root.launching) return false
        var p = cancelZone.mapFromItem(root, mx, my)
        return p.x >= 0 && p.x <= cancelZone.width && p.y >= 0 && p.y <= cancelZone.height
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        propagateComposedEvents: true

        onEntered: menuCloseTimer.stop()
        onExited: {
            cancelZone._hovered = false
            if (contextMenu.visible && !contextMenu.containsMouse) menuCloseTimer.start()
        }

        onPositionChanged: function(mouse) {
            if (root.installing || root.launching) {
                cancelZone._hovered = root._overCancel(mouse.x, mouse.y)
                mouse.accepted = false
            }
        }

        onPressed: function(mouse) {
            if (root.installing || root.launching) {
                if (root._overCancel(mouse.x, mouse.y)) return
                mouse.accepted = false
                return
            }
            if (!root.isAddButton && root.enableContextMenu) {
                holdTimer.start()
                holdProgressDelay.start()
            }
        }

        onReleased: function(mouse) {
            if (root.installing || root.launching) { mouse.accepted = false; return }
            root._stopHold()
        }

        onCanceled: root._stopHold()

        onClicked: function(mouse) {
            if (root.installing || root.launching) {
                if (root._overCancel(mouse.x, mouse.y))
                    root.cancelInstallRequested()
                mouse.accepted = false
                return
            }
            if (root.isAddButton) {
                contextMenu.visible ? contextMenu.close() : contextMenu.open()
            } else {
                if (holdBlockedClick) return
                if (!contextMenu.visible) root.clicked()
            }
        }
    }

    TileContextMenu {
        id: contextMenu
        anchors.centerIn: parent
        tileMode: root._menuMode

        onVisibleChanged:       root.keepScale = visible
        onEditClicked:          root.editRequested()
        onDeleteClicked:        root.deleteRequested()
        onCancelInstallClicked: root.cancelInstallRequested()
        onFolderChosen:         root.folderChosen()
        onShortcutChosen:       root.shortcutChosen()
        onDetailsRequested:     root.detailsRequested()   // Фаза 2
        onMenuLeft:             { if (!hoverHandler.hovered) menuCloseTimer.start() }
    }
}
