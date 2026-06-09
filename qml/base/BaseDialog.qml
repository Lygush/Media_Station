// BaseDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// Базовый модальный диалог: затемнение + карточка с bg/shadow/glow + анимации.
//
// Использование:
//   BaseDialog {
//       dialogWidth:  Settings.us(580)
//       dialogHeight: Settings.us(400)
//       closeOnClickOutside: false   // опционально
//
//       Column { ... }   // content — default property
//   }
//
// Для диалогов с переменной высотой (EditTileDialog) — менять dialogHeight
// снаружи; Behavior on height анимирует изменение плавно после открытия.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import Qt5Compat.GraphicalEffects
import AppSettings 1.0

Item {
    id: root
    anchors.fill: parent
    z: 500
    visible: false

    // Размеры карточки — должны быть заданы явно.
    property int dialogWidth:  Settings.us(500)
    property int dialogHeight: Settings.us(400)

    property alias cardOpacity: dialogBox.opacity
    property alias cardScale:   dialogBox.scale
    default property alias content: contentArea.children

    property bool closeOnClickOutside: true

    // Анимировать изменения dialogHeight (включается после open, выключается во время close).
    property bool animateResize: false

    function open() {
        visible = true
        openAnim.start()
    }

    function close() {
        closeAnim.start()
    }

    // ── Затемнение ────────────────────────────────────────────────────────────
    Rectangle {
        id: backdrop
        anchors.fill: parent
        color: "#99000000"
        opacity: 0

        MouseArea {
            anchors.fill: parent
            enabled: root.closeOnClickOutside
            onClicked: root.close()
        }
    }

    // ── Карточка ──────────────────────────────────────────────────────────────
    Item {
        id: dialogBox
        anchors.centerIn: parent
        width:  root.dialogWidth
        height: root.dialogHeight

        Behavior on height {
            enabled: root.animateResize
            NumberAnimation { duration: 260; easing.type: Easing.OutCubic }
        }

        opacity: 0
        scale:   0.88
        transformOrigin: Item.Center

        DropShadow {
            anchors.fill: bg
            source: bg
            radius: 30; samples: 40
            color: "#AA000000"
            horizontalOffset: 0; verticalOffset: 8
        }

        Rectangle {
            id: bg
            anchors.fill: parent
            radius: 20
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#F0082040" }
                GradientStop { position: 0.5; color: "#F5051528" }
                GradientStop { position: 1.0; color: "#F0082040" }
            }
        }

        Rectangle {
            anchors.fill: bg
            radius: 20
            color: "transparent"
            border.color: Settings.accent(0.667)
            border.width: 1.5
        }

        Glow {
            anchors.fill: bg
            source: bg
            radius: 20; samples: 28
            color: Settings.accent(0.267)
        }

        Item {
            id: contentArea
            anchors.fill: parent
            anchors.margins: Settings.us(28)
        }
    }

    // ── Анимация открытия ─────────────────────────────────────────────────────
    SequentialAnimation {
        id: openAnim
        PropertyAction { target: root;      property: "animateResize"; value: false }
        PropertyAction { target: dialogBox; property: "opacity";       value: 0    }
        PropertyAction { target: dialogBox; property: "scale";         value: 0.88 }
        PropertyAction { target: backdrop;  property: "opacity";       value: 0    }
        ParallelAnimation {
            NumberAnimation { target: dialogBox; property: "opacity"; from: 0;    to: 1.0; duration: 280; easing.type: Easing.OutCubic }
            NumberAnimation { target: dialogBox; property: "scale";   from: 0.88; to: 1.0; duration: 320; easing.type: Easing.OutBack; easing.overshoot: 0.7 }
            NumberAnimation { target: backdrop;  property: "opacity"; from: 0;    to: 1.0; duration: 220; easing.type: Easing.OutCubic }
        }
        PropertyAction { target: root; property: "animateResize"; value: true }
    }

    // ── Анимация закрытия ─────────────────────────────────────────────────────
    SequentialAnimation {
        id: closeAnim
        PropertyAction { target: root; property: "animateResize"; value: false }
        ParallelAnimation {
            NumberAnimation { target: dialogBox; property: "opacity"; from: 1.0; to: 0;    duration: 200; easing.type: Easing.InCubic }
            NumberAnimation { target: dialogBox; property: "scale";   from: 1.0; to: 0.88; duration: 220; easing.type: Easing.InBack  }
            NumberAnimation { target: backdrop;  property: "opacity"; from: 1.0; to: 0;    duration: 180; easing.type: Easing.InCubic }
        }
        PropertyAction { target: root; property: "visible"; value: false }
    }
}
