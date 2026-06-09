// TileContextMenu.qml
// ─────────────────────────────────────────────────────────────────────────────
// TileContextMenu — универсальное контекстное меню для всех типов плиток.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: menu

    // ── Вспомогательные функции масштабирования теперь через Settings ──────
    // Удалены локальные fs / us; используются Settings.fs / Settings.us

    property string tileMode: "normal"

    readonly property bool isAdd:        tileMode === "add"
    readonly property bool isInstalling: tileMode === "installing"
    readonly property bool isNormal:     tileMode === "normal"

    property int autoCloseDelay: 5000

    readonly property bool containsMouse: menuHoverHandler.hovered

    signal editClicked()
    signal deleteClicked()
    signal cancelInstallClicked()
    signal folderChosen()
    signal shortcutChosen()
    signal menuLeft()
    signal detailsRequested()   // Фаза 2: открыть страницу игры


    width:   Settings.us(200)
    height:  menuCol.implicitHeight + Settings.us(18)
    visible: false
    z:       300

    function open() {
        visible = true
        openAnim.start()
        if (!isAdd) autoCloseTimer.restart()
    }

    function close() {
        if (!visible) return
        autoCloseTimer.stop()
        closeAnim.start()
    }

    Timer {
        id: autoCloseTimer
        interval: menu.autoCloseDelay
        onTriggered: menu.close()
    }

    Rectangle {
        id: backdrop
        anchors.fill: parent.window
        color:   menu.isAdd ? "#99000000" : "transparent"
        opacity: 0
        visible: menu.visible
        z: -1000
        MouseArea {
            anchors.fill: parent
            onClicked: menu.close()
        }
    }

    SequentialAnimation {
        id: openAnim
        PropertyAction  { target: menuBox;  property: "opacity"; value: 0   }
        PropertyAction  { target: menuBox;  property: "scale";   value: 0.8 }
        PropertyAction  { target: backdrop; property: "opacity"; value: 0   }
        ParallelAnimation {
            NumberAnimation { target: menuBox;  property: "opacity"; from: 0;   to: 1.0; duration: 200; easing.type: Easing.OutCubic }
            NumberAnimation { target: menuBox;  property: "scale";   from: 0.8; to: 1.0; duration: 240; easing.type: Easing.OutBack; easing.overshoot: 0.9 }
            NumberAnimation { target: backdrop; property: "opacity"; from: 0;   to: 1.0; duration: 220; easing.type: Easing.OutCubic }
        }
    }

    SequentialAnimation {
        id: closeAnim
        ParallelAnimation {
            NumberAnimation { target: menuBox;  property: "opacity"; from: 1.0; to: 0;   duration: 180; easing.type: Easing.InCubic }
            NumberAnimation { target: menuBox;  property: "scale";   from: 1.0; to: 0.8; duration: 180; easing.type: Easing.InBack  }
            NumberAnimation { target: backdrop; property: "opacity"; from: 1.0; to: 0;   duration: 180; easing.type: Easing.InCubic }
        }
        PropertyAction { target: menu; property: "visible"; value: false }
    }

    Item {
        id: menuBox
        anchors.fill: parent
        transformOrigin: Item.Center

        DropShadow {
            anchors.fill: menuBg; source: menuBg
            radius: 20; samples: 32; color: "#99000000"
            horizontalOffset: 0; verticalOffset: 4
        }

        Rectangle {
            id: menuBg
            anchors.fill: parent; radius: 16
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#E8134A6E" }
                GradientStop { position: 0.5; color: "#F00A2D4A" }
                GradientStop { position: 1.0; color: "#E8134A6E" }
            }
        }

        Rectangle {
            anchors.fill: menuBg; radius: 16; color: "transparent"
            border.color: Settings.accent(0.6); border.width: 1.5
        }

        Column {
            id: menuCol
            anchors { top: parent.top; left: parent.left; right: parent.right; margins: Settings.us(8) }
            topPadding: Settings.us(8)
            spacing: Settings.us(5)

            MenuButton {
                visible:   menu.isAdd
                iconSrc:   "qrc:/assets/icons/ic_folder.png"
                label:     Settings.tr("folder")
                textColor: "white"
                bgNormal:  Settings.accent(0.133)
                bgHover:   Settings.accent(0.267)
                onActivated: { menu.close(); menu.folderChosen() }
            }

            MenuSeparator { visible: menu.isAdd }

            MenuButton {
                visible:   menu.isAdd
                iconSrc:   "qrc:/assets/icons/ic_label.png"
                label:     Settings.tr("shortcut")
                textColor: "white"
                bgNormal:  Settings.accent(0.133)
                bgHover:   Settings.accent(0.267)
                onActivated: { menu.close(); menu.shortcutChosen() }
            }

            MenuButton {
                visible:   menu.isInstalling
                iconText:  "✕"
                label:     Settings.tr("cancel_install")
                textColor: "#FFB366"
                bgNormal:  "#22FF8844"
                bgHover:   "#55FF8844"
                onActivated: { menu.close(); menu.cancelInstallClicked() }
            }

            MenuSeparator { visible: menu.isInstalling }

            MenuButton {
                visible:   menu.isNormal
                iconSrc:   "qrc:/assets/icons/ic_edit.png"
                label:     Settings.tr("edit")
                textColor: "white"
                bgNormal:  Settings.accent(0.133)
                bgHover:   Settings.accent(0.267)
                onActivated: { menu.close(); menu.editClicked() }
            }

            MenuSeparator { visible: menu.isNormal }

            // ── Фаза 2: кнопка "Подробнее" ────────────────────────────────────
            MenuButton {
                visible:   menu.isNormal
                iconSrc:   "qrc:/assets/icons/ic_lib.png"
                label:     Settings.tr("game_details")
                textColor: "white"
                bgNormal:  Settings.accent(0.133)
                bgHover:   Settings.accent(0.267)
                onActivated: { menu.close(); menu.detailsRequested() }
            }

            MenuSeparator { visible: menu.isNormal }

            MenuButton {
                visible:   menu.isNormal || menu.isInstalling
                iconSrc:   "qrc:/assets/icons/ic_delete.png"
                label:     Settings.tr("delete")
                textColor: "#FF8888"
                bgNormal:  "#22FF4444"
                bgHover:   "#55FF4444"
                onActivated: { menu.close(); menu.deleteClicked() }
            }
        }

        HoverHandler {
            id: menuHoverHandler
            onHoveredChanged: if (!hovered) menu.menuLeft()
        }
    }

    component MenuButton: Item {
        id: menuBtn
        width:  parent ? parent.width : 0
        height: Settings.us(40)

        property string iconSrc:   ""
        property string iconText:  ""
        property string label:     ""
        property string textColor: "white"
        property string bgNormal:  Settings.accent(0.133)
        property string bgHover:   Settings.accent(0.267)

        signal activated()

        Rectangle {
            anchors.fill: parent; radius: 8
            color: btnMA.containsMouse ? menuBtn.bgHover : menuBtn.bgNormal
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        Row {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.leftMargin: Settings.us(10)
            spacing: Settings.us(8)

            Image {
                source: menuBtn.iconSrc
                width: 18; height: 18
                sourceSize.width: 100; sourceSize.height: 100
                fillMode: Image.PreserveAspectFit
                anchors.verticalCenter: parent.verticalCenter
                visible: menuBtn.iconSrc !== ""
            }
            Text {
                text: menuBtn.iconText
                color: menuBtn.textColor
                font.pixelSize: Settings.fs(14); font.bold: true
                anchors.verticalCenter: parent.verticalCenter
                visible: menuBtn.iconSrc === ""
            }
            Text {
                text: menuBtn.label
                color: menuBtn.textColor
                font.pixelSize: Settings.fs(14); font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        MouseArea { id: btnMA; anchors.fill: parent; hoverEnabled: true; onClicked: menuBtn.activated() }
    }

    component MenuSeparator: Rectangle {
        width:  parent ? parent.width : 0
        height: 1
        color:  Settings.accent(0.267)
    }
}
