// PowerMenu.qml
// ─────────────────────────────────────────────────────────────────────────────
// PowerMenu — меню питания в верхней панели.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: pm
    width: Settings.us(64); height: Settings.us(64)

    property bool menuVisible: false

    Timer { id: closeTimer; interval: 3000; repeat: false; onTriggered: pm.menuVisible = false }

    // ── Кнопка питания ────────────────────────────────────────────────────────
    Item {
        id: powerBtn
        anchors.fill: parent
        scale: 1.0
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

        Rectangle {
            anchors.fill: parent; radius: 10
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#33FFFFFF" }
                GradientStop { position: 0.5; color: "#AA202020" }
                GradientStop { position: 1.0; color: "#33FFFFFF" }
            }
        }
        Rectangle { id: powerBorder; anchors.fill: parent; radius: 10; color: "transparent"; border.color: "#80FFFFFF"; border.width: 2 }
        Glow {
            cached: true
            visible: opacity > 0   // невидимое свечение не считать каждый кадр
            anchors.fill: powerBorder; source: powerBorder
            radius: 16; samples: 24; color: Settings.accent(1.0)
            opacity: powerMA.containsMouse ? 1.0 : 0.0
            Behavior on opacity { NumberAnimation { duration: 150 } }
        }
        Image {
            anchors.centerIn: parent; width: Settings.us(40); height: Settings.us(40)
            source: "qrc:/assets/icons/ic_power.png"
            sourceSize.width: 100; sourceSize.height: 100
            smooth: true; fillMode: Image.PreserveAspectFit
        }
        MouseArea {
            id: powerMA; anchors.fill: parent; hoverEnabled: true
            onEntered: powerBtn.scale = 1.05
            onExited:  powerBtn.scale = 1.0
            onClicked: {
                pm.menuVisible = !pm.menuVisible
                if (pm.menuVisible) closeTimer.start()
            }
        }
    }

    // ── Выпадающее меню ───────────────────────────────────────────────────────
    Item {
        id: powerMenu
        anchors.top: powerBtn.bottom; anchors.topMargin: Settings.us(10); anchors.right: powerBtn.right
        // Ширина по подписям: при крупном шрифте «Выключение» обрезалось
        // до «Выключен…» в фиксированных us(250).
        width: Math.max(Settings.us(250),
                        Settings.us(64) + Settings.us(20)
                        + Math.ceil(Math.max(exitMetrics.advanceWidth, shutdownMetrics.advanceWidth)))
        height: menuColumn.height
        TextMetrics { id: exitMetrics;     font.family: appFont.name; font.pixelSize: Settings.fs(20); text: Settings.tr("exit") }
        TextMetrics { id: shutdownMetrics; font.family: appFont.name; font.pixelSize: Settings.fs(20); text: Settings.tr("shutdown") }
        transformOrigin: Item.TopRight
        visible: false

        HoverHandler {
            onHoveredChanged: { if (hovered) closeTimer.stop(); else closeTimer.start() }
        }

        states: [
            State { name: "open";   when:  pm.menuVisible; PropertyChanges { target: powerMenu; visible: true;  scale: 1.0; opacity: 1.0 } },
            State { name: "closed"; when: !pm.menuVisible; PropertyChanges { target: powerMenu; scale: 0.15; opacity: 0.0 } }
        ]

        transitions: [
            Transition {
                from: "closed"; to: "open"
                SequentialAnimation {
                    PropertyAction  { property: "visible"; value: true }
                    ParallelAnimation {
                        NumberAnimation { property: "scale";   from: 0.15; to: 1.0; duration: 350; easing.type: Easing.OutBack;  easing.overshoot: 1.2 }
                        NumberAnimation { property: "opacity"; from: 0.0;  to: 1.0; duration: 250; easing.type: Easing.OutCubic }
                    }
                }
            },
            Transition {
                from: "open"; to: "closed"
                SequentialAnimation {
                    ParallelAnimation {
                        NumberAnimation { property: "scale";   from: 1.0; to: 0.15; duration: 280; easing.type: Easing.InBack;  easing.overshoot: 1.2 }
                        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0;  duration: 200; easing.type: Easing.InCubic }
                    }
                    PropertyAction { property: "visible"; value: false }
                }
            }
        ]

        Column {
            id: menuColumn; width: parent.width; spacing: Settings.us(8)

            // ── Exit ──────────────────────────────────────────────────────────
            Item {
                width: parent.width; height: Settings.us(64)

                Text {
                    anchors.left: parent.left; anchors.leftMargin: 5
                    anchors.right: exitButton.left; anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: Settings.tr("exit")
                    font.family: appFont.name; font.pixelSize: Settings.fs(20)
                    color: "white"; horizontalAlignment: Text.AlignRight; elide: Text.ElideRight
                }
                Item {
                    id: exitButton; anchors.right: parent.right; width: Settings.us(64); height: Settings.us(64)
                    scale: 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Rectangle {
                        anchors.fill: parent; radius: 10
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#33FFFFFF" }
                            GradientStop { position: 0.5; color: "#AA202020" }
                            GradientStop { position: 1.0; color: "#33FFFFFF" }
                        }
                    }
                    Rectangle { id: exitBorder; anchors.fill: parent; radius: 10; color: "transparent"; border.color: "#80FFFFFF"; border.width: 2 }
                    Glow { cached: true; visible: opacity > 0;  anchors.fill: exitBorder; source: exitBorder; radius: 16; samples: 24; color: Settings.accent(1.0); opacity: exitMA.containsMouse ? 1.0 : 0.0; Behavior on opacity { NumberAnimation { duration: 150 } } }
                    Image { anchors.centerIn: parent; width: Settings.us(40); height: Settings.us(40); source: "qrc:/assets/icons/ic_exit.png"; sourceSize.width: 100; sourceSize.height: 100; smooth: true; fillMode: Image.PreserveAspectFit }
                    MouseArea {
                        id: exitMA; anchors.fill: parent; hoverEnabled: true
                        onEntered: exitButton.scale = 1.05
                        onExited:  exitButton.scale = 1.0
                        onClicked: { pm.menuVisible = false; Qt.quit() }
                    }
                }
            }

            // ── Shutdown ──────────────────────────────────────────────────────
            Item {
                width: parent.width; height: Settings.us(64)

                Text {
                    anchors.left: parent.left; anchors.leftMargin: 5
                    anchors.right: shutdownButton.left; anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    text: Settings.tr("shutdown")
                    font.family: appFont.name; font.pixelSize: Settings.fs(20)
                    color: "white"; horizontalAlignment: Text.AlignRight; elide: Text.ElideRight
                }
                Item {
                    id: shutdownButton; anchors.right: parent.right; width: Settings.us(64); height: Settings.us(64)
                    scale: 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Rectangle {
                        anchors.fill: parent; radius: 10
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#33FFFFFF" }
                            GradientStop { position: 0.5; color: "#AA202020" }
                            GradientStop { position: 1.0; color: "#33FFFFFF" }
                        }
                    }
                    Rectangle { id: shutdownBorder; anchors.fill: parent; radius: 10; color: "transparent"; border.color: "#80FFFFFF"; border.width: 2 }
                    Glow { cached: true; visible: opacity > 0;  anchors.fill: shutdownBorder; source: shutdownBorder; radius: 16; samples: 24; color: Settings.accent(1.0); opacity: shutdownMA.containsMouse ? 1.0 : 0.0; Behavior on opacity { NumberAnimation { duration: 150 } } }
                    Image { anchors.centerIn: parent; width: Settings.us(40); height: Settings.us(40); source: "qrc:/assets/icons/ic_power.png"; sourceSize.width: 100; sourceSize.height: 100; smooth: true; fillMode: Image.PreserveAspectFit }
                    MouseArea {
                        id: shutdownMA; anchors.fill: parent; hoverEnabled: true
                        onEntered: shutdownButton.scale = 1.05
                        onExited:  shutdownButton.scale = 1.0
                        onClicked: { pm.menuVisible = false; Qt.quit() }
                    }
                }
            }
        }
    }
}
