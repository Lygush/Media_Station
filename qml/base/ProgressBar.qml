// ProgressBar.qml
// ─────────────────────────────────────────────────────────────────────────────
// Горизонтальная полоска прогресса с треком и заполнением.
// Заменяет повторяющийся паттерн в Tile, GameLibraryScreen, GameDetailScreen,
// Notification.
//
// Использование:
//   ProgressBar {
//       value: installProgress          // 0.0 … 1.0
//       height: Settings.us(4)
//       barColor: Settings.accent(1.0)
//   }
//
//   // С анимацией изменения ширины:
//   ProgressBar {
//       value: archiveProgress
//       animated: true
//   }
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import AppSettings 1.0

Item {
    id: root

    // ── Публичные свойства ───────────────────────────────────────────────────

    // Прогресс в диапазоне 0.0 … 1.0
    property real   value:     0.0

    // Цвет заполнения. По умолчанию — акцентный цвет темы.
    property color  barColor:  Settings.accent(1.0)

    // Цвет трека (фон за полоской)
    property color  trackColor: "#33FFFFFF"

    // Скругление углов
    property int    radius:    2

    // Анимировать ли изменение ширины (true для медленных операций)
    property bool   animated:  false

    // Длительность анимации в мс
    property int    duration:  300

    // ── Высота по умолчанию ──────────────────────────────────────────────────
    height: Settings.us(4)

    // ── Трек ─────────────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.trackColor
    }

    // ── Заполнение ────────────────────────────────────────────────────────────
    Rectangle {
        id: fill
        anchors.left:   parent.left
        anchors.top:    parent.top
        anchors.bottom: parent.bottom
        radius: root.radius
        color:  root.barColor

        width: Math.max(root.radius * 2,
                        Math.min(root.width, root.value * root.width))

        Behavior on width {
            enabled: root.animated
            NumberAnimation { duration: root.duration; easing.type: Easing.OutCubic }
        }

        Behavior on color { ColorAnimation { duration: 200 } }
    }
}
