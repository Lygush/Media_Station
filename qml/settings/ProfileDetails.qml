// settings/ProfileDetails.qml
// ─────────────────────────────────────────────────────────────────────────────
// Детальная панель настроек выбранного профиля Android TV.
//
// Свойства (in):
//   currentProfileId — id отображаемого профиля
//   profileData      — объект с полями displayName/cores/ramMb/resolution/…
//
// Доступ к PromptDialog — через Window.window.promptDialog (этап 4)
//
// Сигналы:
//   saveAndRefresh(profileId, field, value) — запрос на сохранение поля
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import QtQuick.Window
import AppSettings 1.0
import "../base"

Column {
    id: root

    property string currentProfileId: ""
    property var    profileData:      ({})

    signal saveAndRefresh(string profileId, string field, var value)

    // profileSizeMb() — обычный вызов, QML не знает, когда пересчитать.
    // Счётчик ревизий обновляется при смене профиля/данных и после очистки.
    property int _sizeRev: 0
    onCurrentProfileIdChanged: _sizeRev++
    onProfileDataChanged: _sizeRev++
    function _sizeMb() {
        var _dep = root._sizeRev
        return AndroidTV && AndroidTV.profileManager
               ? AndroidTV.profileManager.profileSizeMb(root.currentProfileId) : -1
    }

    spacing: 0

    // ── Активный профиль ──────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("active_profile")
        Row {
            spacing: Settings.us(10)
            ToggleSwitch {
                id: activeProfileSwitch
                checked: root.currentProfileId !== ""
                         && AndroidTV != null && AndroidTV.profileManager != null
                         && root.currentProfileId === AndroidTV.profileManager.activeProfileId
                // Пока эмулятор запущен, сменить активный профиль нельзя
                opacity: (AndroidTV && AndroidTV.profileManager && AndroidTV.profileManager.locked) ? 0.4 : 1.0
                onToggled: {
                    if (!checked && AndroidTV && AndroidTV.profileManager
                            && !AndroidTV.profileManager.locked)
                        AndroidTV.profileManager.setActiveProfile(root.currentProfileId)
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: activeProfileSwitch.checked
                      ? Settings.safeTr("active") : Settings.safeTr("inactive")
                color: activeProfileSwitch.checked
                       ? Settings.accent(1.0) : "#80FFFFFF"
                font.pixelSize: Settings.fs(12)
                Behavior on color { ColorAnimation { duration: 200 } }
            }
        }
    }
    SectionDivider {}

    // ── Системный образ ────────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("system_image")
        Text {
            text:  root.profileData.displayName || ""
            color: "white"; font.pixelSize: Settings.fs(13)
            elide: Text.ElideRight; width: Settings.us(260)
        }
    }
    SectionDivider {}

    // ── Количество ядер ────────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("cores")
        NumStepper {
            value:    root.profileData.cores !== undefined ? root.profileData.cores : 4
            minValue: 1; maxValue: 16
            onDecremented: {
                var cur = root.profileData.cores !== undefined ? root.profileData.cores : 4
                root.saveAndRefresh(root.currentProfileId, "cores", Math.max(1, cur - 1))
            }
            onIncremented: {
                var cur = root.profileData.cores !== undefined ? root.profileData.cores : 4
                root.saveAndRefresh(root.currentProfileId, "cores", Math.min(16, cur + 1))
            }
        }
    }
    SectionDivider {}

    // ── Оперативная память ─────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("ram")
        SelectGroup {
            model: [1024, 2048, 4096, 8192]
            currentValue: root.profileData.ramMb || 2048
            buttonWidth:  Settings.us(70)
            labelFor: function(v) {
                return v >= 1024 ? (v / 1024 + " GB") : (v + " MB")
            }
            onSelected: function(val) {
                root.saveAndRefresh(root.currentProfileId, "ramMb", val)
            }
        }
    }
    SectionDivider {}

    // ── Разрешение экрана ──────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("resolution")
        Row {
            spacing: Settings.us(8)

            Item {
                width: Settings.us(70); height: Settings.us(34)
                readonly property bool autoRes: (root.profileData.resolution || "auto") === "auto"
                Rectangle {
                    anchors.fill: parent; radius: 9
                    color: parent.autoRes ? Settings.accent(0.4) : Settings.accent(0.1)
                    border.color: parent.autoRes ? Settings.accent(1.0) : Settings.accent(0.3)
                    border.width: 1.5
                    Behavior on color { ColorAnimation { duration: 150 } }
                }
                Text {
                    anchors.centerIn: parent; text: "Auto"
                    color: "white"; font.pixelSize: Settings.fs(12)
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.saveAndRefresh(root.currentProfileId, "resolution", "auto")
                }
            }

            TextField {
                id: resField
                width: Settings.us(120); height: Settings.us(34)
                text: {
                    var res = root.profileData.resolution
                    return (typeof res === "string" && res !== "auto") ? res : ""
                }
                color: "white"; font.pixelSize: Settings.fs(13)
                placeholderText: "1920x1080"
                placeholderTextColor: Settings.accent(0.4)
                verticalAlignment: Text.AlignVCenter
                leftPadding:  Settings.us(8); rightPadding: Settings.us(8)
                onEditingFinished: {
                    if (text.trim() !== "")
                        root.saveAndRefresh(root.currentProfileId, "resolution", text.trim())
                }
                background: Rectangle {
                    radius: 9; color: Settings.accent(0.1)
                    border.color: Settings.accent(0.3); border.width: 1.5
                }
            }
        }
    }
    SectionDivider {}

    // ── Аудиодрайвер ──────────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("audio_driver")
        SelectGroup {
            model: [
                { value: "dsound",   label: "DirectSound" },
                { value: "winaudio", label: "WinAudio"    }
            ]
            currentValue: root.profileData.audioDriver || "dsound"
            buttonWidth:  Settings.us(110)
            onSelected: function(val) {
                root.saveAndRefresh(root.currentProfileId, "audioDriver", val)
            }
        }
    }
    SectionDivider {}

    // ── Аудио таймер ──────────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("audio_timer_period")
        Row {
            spacing: Settings.us(8)
            NumStepper {
                visible: audioTimerSwitch.checked
                opacity: audioTimerSwitch.checked ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                value:    typeof root.profileData.audioTimerMs === "number" ? root.profileData.audioTimerMs : 100
                minValue: 10; maxValue: 500; step: 10
                displaySuffix: " ms"
                onDecremented: {
                    var cur = root.profileData.audioTimerMs
                    if (typeof cur !== "number") return
                    root.saveAndRefresh(root.currentProfileId, "audioTimerMs", Math.max(10, cur - 10))
                }
                onIncremented: {
                    var cur = root.profileData.audioTimerMs
                    if (typeof cur !== "number") return
                    root.saveAndRefresh(root.currentProfileId, "audioTimerMs", Math.min(500, cur + 10))
                }
            }
            ToggleSwitch {
                id: audioTimerSwitch
                checked: {
                    var val = root.profileData.audioTimerMs
                    return val !== undefined && val !== "auto"
                }
                onToggled: {
                    // Не присваиваем checked напрямую — это рвёт биндинг, и при
                    // переключении профиля переключатель показывал чужое значение
                    var enable = !audioTimerSwitch.checked
                    root.saveAndRefresh(root.currentProfileId, "audioTimerMs", enable ? 100 : "auto")
                }
            }
        }
    }
    SectionDivider {}

    // ── DSound Latency ─────────────────────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("dsound_latency")
        Row {
            spacing: Settings.us(8)
            NumStepper {
                visible: dsoundLatencySwitch.checked
                opacity: dsoundLatencySwitch.checked ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                value:    typeof root.profileData.dsoundLatencyMs === "number" ? root.profileData.dsoundLatencyMs : 200
                minValue: 50; maxValue: 1000; step: 25
                displaySuffix: " ms"
                onDecremented: {
                    var cur = root.profileData.dsoundLatencyMs
                    if (typeof cur !== "number") return
                    root.saveAndRefresh(root.currentProfileId, "dsoundLatencyMs", Math.max(50, cur - 25))
                }
                onIncremented: {
                    var cur = root.profileData.dsoundLatencyMs
                    if (typeof cur !== "number") return
                    root.saveAndRefresh(root.currentProfileId, "dsoundLatencyMs", Math.min(1000, cur + 25))
                }
            }
            ToggleSwitch {
                id: dsoundLatencySwitch
                checked: {
                    var val = root.profileData.dsoundLatencyMs
                    return val !== undefined && val !== "auto"
                }
                onToggled: {
                    var enable = !dsoundLatencySwitch.checked
                    root.saveAndRefresh(root.currentProfileId, "dsoundLatencyMs", enable ? 200 : "auto")
                }
            }
        }
    }
    SectionDivider {}

    // ── Используемое место + кнопки ───────────────────────────────────────────
    SettingsRow {
        label: Settings.safeTr("used_space")
        Row {
            spacing: Settings.us(10)
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined

            Column {
                spacing: Settings.us(2)
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    text: {
                        var mb = root._sizeMb()
                        if (mb < 0) return Settings.safeTr("qcow2_not_found")
                        if (mb < 1024) return mb + " MB"
                        return (mb / 1024).toFixed(1) + " GB"
                    }
                    color: {
                        var mb = root._sizeMb()
                        var ratio = mb / 3072
                        if (ratio > 0.85) return "#FF6060"
                        if (ratio > 0.6)  return "#FFB347"
                        return Settings.accent(1.0)
                    }
                    font.pixelSize: Settings.fs(13); font.bold: true
                }

                ProgressBar {
                    width: Settings.us(140); height: Settings.us(6)
                    radius: Settings.us(3); animated: true; duration: 400
                    trackColor: Settings.accent(0.12)
                    value: {
                        var mb = root._sizeMb()
                        return mb <= 0 ? 0 : Math.min(1.0, mb / 3072)
                    }
                    barColor: {
                        var mb = root._sizeMb()
                        var ratio = mb / 3072
                        if (ratio > 0.85) return "#FF6060"
                        if (ratio > 0.6)  return "#FFB347"
                        return Settings.accent(1.0)
                    }
                }
            }

            // Кнопка «Очистить данные»
            Item {
                width: Settings.us(145); height: Settings.us(32)
                anchors.verticalCenter: parent.verticalCenter
                enabled: AndroidTV && !(AndroidTV.profileManager && AndroidTV.profileManager.locked
                                        && root.currentProfileId === AndroidTV.profileManager.activeProfileId)
                Rectangle {
                    anchors.fill: parent; radius: 9
                    color: clearMA.containsMouse && parent.enabled ? "#55FF4444" : "#33FF4444"
                    border.color: "#88FF4444"; border.width: 1.5
                    opacity: parent.enabled ? 1.0 : 0.35
                }
                Text {
                    anchors.centerIn: parent
                    text: Settings.safeTr("clear_data"); color: "#FF8888"
                    font.pixelSize: Settings.fs(12); opacity: parent.enabled ? 1.0 : 0.35
                }
                MouseArea {
                    id: clearMA; anchors.fill: parent; hoverEnabled: true
                    enabled: parent.enabled
                    onClicked: {
                        var pd = Window.window ? Window.window.promptDialog : null
                        if (pd) {
                            pd.show({
                                title:   Settings.safeTr("clear_data"),
                                message: Settings.safeTr("confirm_clear_data"),
                                buttons: [
                                    { label: Settings.safeTr("delete"), role: "danger",
                                      action: function() {
                                          AndroidTV.profileManager.clearProfileData(root.currentProfileId)
                                          root._sizeRev++
                                      }
                                    },
                                    { label: Settings.safeTr("cancel"), role: "primary",
                                      action: function() {}
                                    }
                                ]
                            })
                        }
                    }
                }
            }

            // Кнопка «Удалить профиль»
            Item {
                width: Settings.us(145); height: Settings.us(32)
                anchors.verticalCenter: parent.verticalCenter
                enabled: AndroidTV && AndroidTV.profileManager
                         && AndroidTV.profileManager.count > 1
                         && root.currentProfileId !== AndroidTV.profileManager.activeProfileId
                Rectangle {
                    anchors.fill: parent; radius: 9
                    color: delMA.containsMouse && parent.enabled ? "#55FF4444" : "#33FF4444"
                    border.color: "#88FF4444"; border.width: 1.5
                    opacity: parent.enabled ? 1.0 : 0.4
                }
                Text {
                    anchors.centerIn: parent
                    text: Settings.safeTr("delete_profile"); color: "#FF8888"
                    font.pixelSize: Settings.fs(12); opacity: parent.enabled ? 1.0 : 0.4
                }
                MouseArea {
                    id: delMA; anchors.fill: parent; hoverEnabled: true
                    enabled: parent.enabled
                    onClicked: {
                        if (AndroidTV.profileManager.removeProfile(root.currentProfileId))
                            root.currentProfileId = AndroidTV.profileManager.activeProfileId
                    }
                }
            }
        }
    }
}
