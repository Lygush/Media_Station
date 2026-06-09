// settings/GeneralSection.qml
import QtQuick
import AppSettings 1.0
import "../base"

Column {
    spacing: 0

    SettingsRow {
        label: Settings.safeTr("language")
        SelectGroup {
            model: [
                { value: "ru", label: "RU" },
                { value: "en", label: "EN" }
            ]
            currentValue: Settings.language
            buttonWidth:  Settings.us(60)
            onSelected: function(val) { Settings.language = val }
        }
    }
}
