// settings/AppearanceSection.qml
import QtQuick
import QtQuick.Window
import AppSettings 1.0
import "../base"

Column {
    spacing: 0

    // Ссылка на корневое окно нужна для переключения fullscreen.
    // SettingsScreen передаёт её через root Window.
    property var appWindow: null

    SettingsRow {
        label: Settings.safeTr("fullscreen")
        ToggleSwitch {
            checked: Settings.fullscreen
            onToggled: {
                Settings.fullscreen = !Settings.fullscreen
                if (appWindow)
                    appWindow.visibility = Settings.fullscreen
                                           ? Window.FullScreen : Window.Windowed
            }
        }
    }
    SectionDivider {}

    SettingsRow {
        label: Settings.safeTr("ui_scale")
        SelectGroup {
            model: [
                { value: 1.0,  label: "100%" },
                { value: 1.25, label: "125%" },
                { value: 1.5,  label: "150%" }
            ]
            currentValue: {
                var s = SettingsManager.uiScale
                if (Math.abs(s - 1.0)  < 0.01) return 1.0
                if (Math.abs(s - 1.25) < 0.01) return 1.25
                return 1.5
            }
            buttonWidth: Settings.us(68)
            onSelected: function(val) { SettingsManager.uiScale = val }
        }
    }
    SectionDivider {}

    SettingsRow {
        label: Settings.safeTr("font_size")
        NumStepper {
            value:    16 + SettingsManager.fontOffset
            minValue: 16 + (-4)
            maxValue: 16 + 12
            step:     2
            displaySuffix: " px"
            valueWidth: Settings.us(58)
            onDecremented: { if (SettingsManager.fontOffset > -4) SettingsManager.fontOffset -= 2 }
            onIncremented: { if (SettingsManager.fontOffset < 12)  SettingsManager.fontOffset += 2 }
        }
    }
    SectionDivider {}

    SettingsRow {
        label: Settings.safeTr("enable_fog_shader")
        ToggleSwitch {
            checked: Settings.enableFogShader
            onToggled: Settings.enableFogShader = !Settings.enableFogShader
        }
    }
}
