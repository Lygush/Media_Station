// SelectGroup.qml
// ─────────────────────────────────────────────────────────────────────────────
// Группа кнопок-пилюль с одним активным вариантом.
// Заменяет повторяющийся паттерн Repeater+isActive в SettingsScreen и
// AndroidTvSettings (язык, масштаб, RAM, аудиодрайвер, разрешение).
//
// Использование:
//   SelectGroup {
//       model: [
//           { value: "ru", label: "RU" },
//           { value: "en", label: "EN" }
//       ]
//       currentValue: Settings.language
//       onSelected: function(val) { Settings.language = val }
//   }
//
//   // Короткая форма — если label выводить через функцию:
//   SelectGroup {
//       model: [1024, 2048, 4096, 8192]
//       currentValue: profileData.ramMb
//       labelFor: function(v) { return v >= 1024 ? (v / 1024 + " GB") : (v + " MB") }
//       buttonWidth: Settings.us(70)
//       onSelected: function(val) { saveAndRefresh(id, "ramMb", val) }
//   }
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import AppSettings 1.0

Row {
    id: root
    spacing: Settings.us(8)

    // ── Публичные свойства ───────────────────────────────────────────────────

    // model: массив значений или объектов { value, label }
    property var    model:        []

    // Текущее активное значение (сравнивается с item.value или item напрямую)
    property var    currentValue: null

    // Необязательная функция форматирования метки для скалярных моделей
    property var    labelFor:     null

    // Размеры кнопок
    property int    buttonWidth:  Settings.us(80)
    property int    buttonHeight: Settings.us(34)

    // ── Сигналы ──────────────────────────────────────────────────────────────
    signal selected(var value)

    // ── Вспомогательные функции ──────────────────────────────────────────────
    function _itemValue(item) {
        return (item !== null && typeof item === "object" && item.hasOwnProperty("value"))
               ? item.value : item
    }

    function _itemLabel(item) {
        var val = _itemValue(item)
        if (item !== null && typeof item === "object" && item.hasOwnProperty("label"))
            return item.label
        if (root.labelFor)
            return root.labelFor(val)
        return String(val)
    }

    function _isActive(item) {
        var val = _itemValue(item)
        // Нестрогое сравнение намеренно — currentValue может быть числом или строкой
        // eslint-disable-next-line eqeqeq
        return root.currentValue == val
    }

    // ── Кнопки ───────────────────────────────────────────────────────────────
    Repeater {
        model: root.model

        delegate: Item {
            id: btn
            width:  root.buttonWidth
            height: root.buttonHeight

            readonly property bool active: root._isActive(modelData)

            scale: btnMA.containsMouse ? 1.05 : 1.0
            Behavior on scale { NumberAnimation { duration: 120 } }

            Rectangle {
                anchors.fill: parent
                radius: 9
                color: btn.active
                       ? Settings.accent(0.4)
                       : (btnMA.containsMouse ? Settings.accent(0.2) : Settings.accent(0.1))
                border.color: btn.active ? Settings.accent(1.0) : Settings.accent(0.3)
                border.width: 1.5
                Behavior on color { ColorAnimation { duration: 150 } }
                Behavior on border.color { ColorAnimation { duration: 150 } }
            }

            Text {
                anchors.centerIn: parent
                text: root._itemLabel(modelData)
                color: "white"
                font.pixelSize: Settings.fs(13)
                font.bold: btn.active
                elide: Text.ElideRight
                width: parent.width - Settings.us(8)
                horizontalAlignment: Text.AlignHCenter
            }

            MouseArea {
                id: btnMA
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.selected(root._itemValue(modelData))
            }
        }
    }
}
