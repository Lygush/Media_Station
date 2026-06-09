// UpdateAvailableDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// Диалог уведомления о доступном обновлении MediaStation.
//
// Показывается автоматически когда UpdateManager.updateReady становится true.
// Содержит: версию, release notes, прогресс загрузки, кнопки «Установить» / «Позже».
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: dialog
    anchors.fill: parent
    z: 700
    visible: UpdateManager != null && (UpdateManager.updateReady || UpdateManager.preparing)

    // ── Фон-затемнение ────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: "#BB000000"
        MouseArea { anchors.fill: parent } // блокируем клики сквозь диалог
    }

    // ─ Карточка диалога ──────────────────────────────────────────────────────
    Item {
        id: card
        anchors.centerIn: parent
        // Увеличена максимальная ширина, чтобы вместить длинные строки вроде предупреждения о перезапуске
        width:  Math.min(Settings.us(600), parent.width - Settings.us(80))
        height: cardCol.implicitHeight + Settings.us(56)

        // Анимация появления
        opacity: dialog.visible ? 1 : 0
        scale:   dialog.visible ? 1 : 0.88
        Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
        Behavior on scale   { NumberAnimation { duration: 320; easing.type: Easing.OutBack; easing.overshoot: 0.7 } }

        DropShadow {
            anchors.fill: bg; source: bg
            radius: 30; samples: 40; color: "#AA000000"; verticalOffset: 8
        }
        Rectangle {
            id: bg
            anchors.fill: parent; radius: Settings.us(20)
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#F0082040" }
                GradientStop { position: 0.5; color: "#F5051528" }
                GradientStop { position: 1.0; color: "#F0082040" }
            }
        }
        Rectangle {
            anchors.fill: bg; radius: Settings.us(20); color: "transparent"
            border.color: Settings.accent(0.667); border.width: 1.5
        }
        Glow {
            anchors.fill: bg; source: bg
            radius: 20; samples: 28; color: Settings.accent(0.267)
        }

        Column {
            id: cardCol
            anchors {
                top: parent.top; left: parent.left; right: parent.right
                margins: Settings.us(32); topMargin: Settings.us(28)
            }
            spacing: Settings.us(16)

            // ── Заголовок ─────────────────────────────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Settings.us(10)

                Image {
                    source: "qrc:/assets/icons/ic_download.png"
                    width: Settings.us(28); height: Settings.us(28)
                    sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: Settings.tr("update_available")
                    color: "white"
                    font.family: appFont.name
                    font.pixelSize: Settings.fs(22)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Rectangle { width: parent.width; height: 1; color: Settings.accent(0.267) }

            // ── Версия ────────────────────────────────────────────────────────
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: UpdateManager != null ? Settings.tr("version_label").replace("{version}", UpdateManager.newVersion) : ""
                color: Settings.accent(1.0)
                font.pixelSize: Settings.fs(17)
                font.bold: true
            }

            // ── Release notes ─────────────────────────────────────────────────
            Rectangle {
                width: parent.width
                height: Math.min(notesText.implicitHeight + Settings.us(24), Settings.us(180))
                radius: Settings.us(12)
                color: Settings.accent(0.082)
                border.color: Settings.accent(0.2); border.width: 1
                clip: true

                Flickable {
                    anchors { fill: parent; margins: Settings.us(12) }
                    contentHeight: notesText.implicitHeight
                    clip: true

                    Text {
                        id: notesText
                        width: parent.width
                        text: (UpdateManager != null && UpdateManager.releaseNotes !== "")
                              ? UpdateManager.releaseNotes
                              : Settings.tr("no_changelog")
                        color: "#CCFFFFFF"
                        font.pixelSize: Settings.fs(14)
                        wrapMode: Text.WordWrap
                        lineHeight: 1.4
                    }
                }
            }

            // ── Прогресс загрузки (пока downloading) ─────────────────────────
            Column {
                width: parent.width
                spacing: Settings.us(6)
                visible: UpdateManager != null && UpdateManager.downloading

                Text {
                    text: Settings.tr("downloading_update")
                    color: Settings.accent(0.8)
                    font.pixelSize: Settings.fs(13)
                }

                Item {
                    width: parent.width; height: Settings.us(6)

                    Rectangle {
                        anchors.fill: parent; radius: Settings.us(3); color: "#22FFFFFF"
                    }
                    Rectangle {
                        width: parent.width * (UpdateManager != null ? UpdateManager.downloadProgress : 0)
                        height: parent.height; radius: Settings.us(3)
                        color: Settings.accent(1.0)
                        Behavior on width { NumberAnimation { duration: 300 } }
                    }
                }
            }

            // ── Кнопки / Состояние подготовки ────────────────────────────────
            Item {
                width: parent.width
                height: Settings.us(52)

                // Кнопки — видны когда обновление готово и подготовка ещё не началась
                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(16)
                    visible: UpdateManager != null && UpdateManager.updateReady && !UpdateManager.preparing

                    // Позже
                    Item {
                        width: Math.max(Settings.us(160), laterText.implicitWidth + Settings.us(32))
                        height: Settings.us(52)

                        Rectangle {
                            anchors.fill: parent; radius: Settings.us(12)
                            color: laterMA.containsMouse ? Settings.accent(0.267) : Settings.accent(0.133)
                            border.color: Settings.accent(0.4); border.width: 1
                            Behavior on color { ColorAnimation { duration: 120 } }
                        }
                        Text {
                            id: laterText
                            anchors.centerIn: parent
                            text: Settings.tr("later_btn")
                            color: "white"
                            font.family: appFont.name
                            font.pixelSize: Settings.fs(16)
                            horizontalAlignment: Text.AlignHCenter
                        }
                        MouseArea {
                            id: laterMA; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (UpdateManager != null)
                                    UpdateManager.declineUpdate()
                            }
                        }
                        scale: laterMA.containsMouse ? 1.03 : 1.0
                        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    }

                    // Установить
                    Item {
                        width: Math.max(Settings.us(160), installText.implicitWidth + Settings.us(32))
                        height: Settings.us(52)
                        enabled: UpdateManager != null && !UpdateManager.downloading

                        Rectangle {
                            anchors.fill: parent; radius: Settings.us(12)
                            color: installMA.containsMouse && parent.enabled
                                   ? Settings.accent(0.533) : Settings.accent(0.333)
                            border.color: Settings.accent(0.667); border.width: 1
                            Behavior on color { ColorAnimation { duration: 120 } }
                            opacity: parent.enabled ? 1.0 : 0.5
                        }
                        Text {
                            id: installText
                            anchors.centerIn: parent
                            text: Settings.tr("install_now_btn")
                            color: "white"
                            font.family: appFont.name
                            font.pixelSize: Settings.fs(16)
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        MouseArea {
                            id: installMA; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (UpdateManager != null)
                                    UpdateManager.acceptUpdate()
                            }
                        }
                        scale: (installMA.containsMouse && parent.enabled) ? 1.03 : 1.0
                        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    }
                }

                // Состояние подготовки — спиннер + текст
                Row {
                    anchors.centerIn: parent
                    spacing: Settings.us(12)
                    visible: UpdateManager != null && UpdateManager.preparing

                    // Вращающийся индикатор
                    Rectangle {
                        id: spinner
                        width: Settings.us(24); height: Settings.us(24)
                        radius: Settings.us(12)
                        color: "transparent"
                        border.color: Settings.accent(1.0)
                        border.width: 2
                        anchors.verticalCenter: parent.verticalCenter

                        Rectangle {
                            width: Settings.us(6); height: Settings.us(6); radius: Settings.us(3)
                            color: Settings.accent(1.0)
                            anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
                            anchors.topMargin: -1
                        }

                        RotationAnimator {
                            target: spinner
                            from: 0; to: 360
                            duration: 900
                            loops: Animation.Infinite
                            running: UpdateManager != null && UpdateManager.preparing
                        }
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Settings.tr("preparing_update")
                        color: Settings.accent(0.9)
                        font.family: appFont.name
                        font.pixelSize: Settings.fs(16)
                    }
                }
            }

            // Предупреждение (с переносом текста и адаптацией к новой ширине)
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width
                text: Settings.tr("restart_warning")
                color: "#66FFFFFF"
                font.pixelSize: Settings.fs(12)
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
