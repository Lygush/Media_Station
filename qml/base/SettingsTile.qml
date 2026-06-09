// SettingsTile.qml
// Базовый компонент плитки для экранов настроек.
// Поддерживает автоматический перевод через titleKey.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root

    // ── Публичные свойства ──────────────────────────────────────────────────
    property string title:      ""          // Текст плитки (если titleKey не задан)
    property string titleKey:   ""          // Ключ для перевода (приоритет выше title)
    property string iconSrc:    ""          // Путь к иконке (qrc или файл)
    property string iconFallback: ""        // Текст-заглушка, если иконка не загрузилась
    property int    tileWidth:  Settings.us(130)
    property int    tileHeight: Settings.us(108)

    // ── Внешний вид и поведение ─────────────────────────────────────────────
    width:  tileWidth
    height: tileHeight

    signal clicked()

    // Масштабирование при наведении
    scale: hoverHandler.hovered ? 1.05 : 1.0
    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    // Тень под карточкой
    DropShadow {
        anchors.fill: bgRect
        source: bgRect
        radius: 32
        samples: 48
        color: "#AA000000"
        horizontalOffset: 0
        verticalOffset: 10
    }

    // Фон с градиентом
    Rectangle {
        id: bgRect
        anchors.fill: parent
        radius: 18
        gradient: Gradient {
            GradientStop { position: 0.0; color: Settings.accent(0.28) }
            GradientStop { position: 0.5; color: Qt.rgba(0.03, 0.09, 0.22, 0.82) }
            GradientStop { position: 1.0; color: Settings.accent(0.18) }
        }
    }

    // Рамка
    Rectangle {
        id: borderRect
        anchors.fill: parent
        radius: 18
        color: "transparent"
        border.color: Settings.accent(0.55)
        border.width: 1.5
    }

    // Свечение при наведении
    Glow {
        anchors.fill: borderRect
        source: borderRect
        radius: 16
        samples: 24
        color: Settings.accent(1.0)
        opacity: hoverHandler.hovered ? 1.0 : 0.0
        Behavior on opacity { NumberAnimation { duration: 150 } }
    }

    // ── Иконка / изображение ─────────────────────────────────────────────────
    Item {
        id: iconArea
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -Math.round(label.implicitHeight / 2 + Settings.us(8))
        width:  Math.round(root.tileHeight * 0.32)
        height: Math.round(root.tileHeight * 0.32)

        property bool imgOk: false

        Image {
            id: tileImage
            anchors.fill: parent
            source: root.iconSrc
            sourceSize.width: 128
            sourceSize.height: 128
            fillMode: Image.PreserveAspectFit
            visible: false
            onStatusChanged: {
                if (status === Image.Ready) {
                    iconArea.imgOk = true
                    visible = true
                }
                if (status === Image.Error) {
                    iconArea.imgOk = false
                    visible = false
                }
            }
        }

        Text {
            anchors.centerIn: parent
            text: root.iconFallback
            font.pixelSize: Math.round(root.tileHeight * 0.24)
            visible: !iconArea.imgOk
        }
    }

    // ── Подпись ──────────────────────────────────────────────────────────────
    Text {
        id: label
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Settings.us(16)
        anchors.horizontalCenter: parent.horizontalCenter
        text: root.titleKey !== "" ? Settings.tr(root.titleKey) : root.title
        color: "white"
        font.pixelSize: Settings.fs(13)
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        width: parent.width - Settings.us(12)
        wrapMode: Text.WordWrap
    }

    // ── Обработка взаимодействия ─────────────────────────────────────────────
    HoverHandler { id: hoverHandler }
    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
