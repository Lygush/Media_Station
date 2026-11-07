// settings/AndroidTvSettings.qml
import QtQuick
import QtQuick.Dialogs
import QtQuick.Window
import AppSettings 1.0
import "../base"

Item {
    id: root

    property string subView:          "tiles"
    property string currentProfileId: ""
    property var    profileData:      ({})

    readonly property bool isCurrentProfileActive:
        root.currentProfileId !== "" &&
        AndroidTV != null && AndroidTV.profileManager != null &&
        root.currentProfileId === AndroidTV.profileManager.activeProfileId

    function refreshProfileData() {
        if (AndroidTV && AndroidTV.profileManager) {
            var raw = AndroidTV.profileManager.getProfile(root.currentProfileId) || {}
            if (!raw.id) raw = AndroidTV.profileManager.currentProfileData || {}
            profileData = {
                displayName:    raw.displayName    || "",
                cores:          (typeof raw.cores   === "number") ? raw.cores   : 4,
                ramMb:          (typeof raw.ramMb   === "number") ? raw.ramMb   : 2048,
                resolution:     (typeof raw.resolution   === "string") ? raw.resolution   : "auto",
                audioDriver:    (typeof raw.audioDriver  === "string") ? raw.audioDriver  : "dsound",
                audioTimerMs:   (typeof raw.audioTimerMs === "string" || typeof raw.audioTimerMs === "number")
                                ? raw.audioTimerMs : "auto",
                dsoundLatencyMs:(typeof raw.dsoundLatencyMs === "string" || typeof raw.dsoundLatencyMs === "number")
                                ? raw.dsoundLatencyMs : "auto",
                diagnosticLogcat: raw.diagnosticLogcat === true,
                consoleInput:   raw.consoleInput !== false
            }
        } else {
            profileData = {
                displayName: "", cores: 4, ramMb: 2048, resolution: "auto",
                audioDriver: "dsound", audioTimerMs: "auto", dsoundLatencyMs: "auto",
                diagnosticLogcat: false, consoleInput: true
            }
        }
    }

    function _doSaveAndRefresh(profileId, field, value) {
        if (!AndroidTV || !AndroidTV.profileManager) return
        AndroidTV.profileManager.updateProfileField(profileId, field, value)
        AndroidTV.profileManager.save()
        root.refreshProfileData()
    }

    Component.onCompleted: {
        if (AndroidTV) AndroidTV.refreshQcow2Size()
        if (AndroidTV) AndroidTV.refreshAdBlockState()
        if (AndroidTV && AndroidTV.profileManager)
            currentProfileId = AndroidTV.profileManager.activeProfileId
        refreshProfileData()
    }

    onCurrentProfileIdChanged: refreshProfileData()

    Connections {
        target: AndroidTV && AndroidTV.profileManager ? AndroidTV.profileManager : null
        function onCurrentProfileDataChanged() { root.refreshProfileData() }
        function onProfilesChanged() {
            root.refreshProfileData()
            if (!AndroidTV.profileManager.getProfile(root.currentProfileId).id)
                root.currentProfileId = AndroidTV.profileManager.activeProfileId
        }
        function onActiveProfileIdChanged() { root.refreshProfileData() }
    }

    // ── Плитки-категории ──────────────────────────────────────────────────────
    Item {
        anchors.fill: parent
        visible: root.subView === "tiles"

        Row {
            anchors.centerIn: parent
            spacing: Settings.us(20)

            SettingsTile {
                tileWidth: Settings.us(200); tileHeight: Settings.us(200)
                titleKey: "general"
                iconSrc: "qrc:/assets/icons/ic_settings.png"
                iconFallback: "⚙"
                onClicked: root.subView = "common"
            }
            SettingsTile {
                tileWidth: Settings.us(200); tileHeight: Settings.us(200)
                titleKey: "profiles"
                iconSrc: "qrc:/assets/icons/ic_tv.png"
                iconFallback: "📺"
                onClicked: root.subView = "profiles"
            }
        }
    }

    // ── Детальный вид ─────────────────────────────────────────────────────────
    Item {
        anchors.fill: parent
        visible: root.subView !== "tiles"

        // ── Общие: состояние эмулятора, запуск/остановка, сброс данных ────────
        Column {
            visible: root.subView === "common"
            anchors.fill: parent
            spacing: 0

            SettingsRow {
                label: Settings.tr("atv_status")
                Text {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: {
                        if (!AndroidTV) return ""
                        if (AndroidTV.running) return Settings.tr("atv_state_running")
                        if (AndroidTV.booting) return Settings.tr("atv_state_booting")
                        // Ошибка последнего запуска (если была)
                        if (AndroidTV.status && AndroidTV.status.length > 0) return AndroidTV.status
                        return Settings.tr("atv_state_stopped")
                    }
                    color: AndroidTV && AndroidTV.running ? Settings.accent(1.0) : "#CCFFFFFF"
                    font.pixelSize: Settings.fs(13)
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, Settings.us(420))
                }
            }
            SectionDivider {}

            SettingsRow {
                label: Settings.tr("atv_emulator")
                Row {
                    spacing: Settings.us(10)
                    StyledButton {
                        height: Settings.us(36)
                        text: Settings.tr("atv_start")
                        role: "primary"
                        enabled: AndroidTV && !AndroidTV.running && !AndroidTV.booting
                        opacity: enabled ? 1.0 : 0.4
                        onClicked: if (enabled) AndroidTV.launch()
                    }
                    StyledButton {
                        height: Settings.us(36)
                        text: Settings.tr("atv_stop")
                        role: "danger"
                        enabled: AndroidTV && (AndroidTV.running || AndroidTV.booting)
                        opacity: enabled ? 1.0 : 0.4
                        onClicked: if (enabled) AndroidTV.stop()
                    }
                }
            }
            SectionDivider {}

            // ── Блокировка рекламы (AdGuard Home, CHANGES_2026-11-07_7) ─────
            SettingsRow {
                label: Settings.tr("atv_adblock")
                Row {
                    spacing: Settings.us(12)
                    ToggleSwitch {
                        anchors.verticalCenter: parent.verticalCenter
                        checked: AndroidTV ? AndroidTV.adBlockEnabled : false
                        onToggled: if (AndroidTV) AndroidTV.adBlockEnabled = !AndroidTV.adBlockEnabled
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: {
                            if (!AndroidTV) return ""
                            if (AndroidTV.adBlockBusy) return Settings.tr("atv_adblock_busy")
                            return Settings.tr("atv_adblock_" + (AndroidTV.adBlockState || "unknown"))
                        }
                        color: AndroidTV && AndroidTV.adBlockState === "running" ? Settings.accent(1.0) : "#CCFFFFFF"
                        font.pixelSize: Settings.fs(13)
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, Settings.us(300))
                    }
                }
            }
            SectionDivider {}

            SettingsRow {
                label: Settings.tr("atv_adblock_service")
                visible: !!AndroidTV
                Row {
                    spacing: Settings.us(10)
                    StyledButton {
                        height: Settings.us(36)
                        visible: AndroidTV && AndroidTV.adBlockState === "missing"
                        text: AndroidTV && AndroidTV.adBlockDownload >= 0
                              ? Settings.tr("atv_adblock_downloading") + " " + Math.round(AndroidTV.adBlockDownload * 100) + " %"
                              : Settings.tr("atv_adblock_download")
                        role: "primary"
                        enabled: AndroidTV && AndroidTV.adBlockDownload < 0
                        opacity: enabled ? 1.0 : 0.6
                        onClicked: if (enabled) AndroidTV.downloadAdBlock()
                    }
                    StyledButton {
                        height: Settings.us(36)
                        visible: AndroidTV && (AndroidTV.adBlockState === "notInstalled"
                                               || AndroidTV.adBlockState === "unknown")
                        text: Settings.tr("atv_adblock_install")
                        role: "primary"
                        enabled: AndroidTV && !AndroidTV.adBlockBusy
                        opacity: enabled ? 1.0 : 0.4
                        onClicked: if (enabled) AndroidTV.installAdBlock()
                    }
                    StyledButton {
                        height: Settings.us(36)
                        visible: AndroidTV && AndroidTV.adBlockState === "running"
                        text: Settings.tr("atv_adblock_panel")
                        onClicked: AndroidTV.openAdBlockPanel()
                    }
                    StyledButton {
                        height: Settings.us(36)
                        visible: AndroidTV && (AndroidTV.adBlockState === "running"
                                               || AndroidTV.adBlockState === "stopped"
                                               || AndroidTV.adBlockState === "starting")
                        text: Settings.tr("atv_adblock_uninstall")
                        role: "danger"
                        enabled: AndroidTV && !AndroidTV.adBlockBusy
                        opacity: enabled ? 1.0 : 0.4
                        onClicked: if (enabled) AndroidTV.uninstallAdBlock()
                    }
                }
            }
            SectionDivider {}

            SettingsRow {
                label: Settings.tr("atv_user_data")
                Row {
                    spacing: Settings.us(12)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: {
                            var mb = AndroidTV ? AndroidTV.qcow2SizeMb : -1
                            if (mb < 0) return Settings.safeTr("qcow2_not_found")
                            return mb < 1024 ? (mb + " MB") : ((mb / 1024).toFixed(1) + " GB")
                        }
                        color: "white"; font.pixelSize: Settings.fs(13)
                    }
                    StyledButton {
                        height: Settings.us(36)
                        text: Settings.tr("atv_reset_data")
                        role: "danger"
                        enabled: AndroidTV && !AndroidTV.running && !AndroidTV.booting
                        opacity: enabled ? 1.0 : 0.4
                        onClicked: {
                            if (!enabled) return
                            var pd = Window.window ? Window.window.promptDialog : null
                            if (!pd) { AndroidTV.resetQcow2(); return }
                            pd.show({
                                title:   Settings.tr("atv_reset_data"),
                                message: Settings.tr("atv_reset_confirm"),
                                buttons: [
                                    { label: Settings.safeTr("delete"), role: "danger",
                                      action: function() { AndroidTV.resetQcow2() } },
                                    { label: Settings.safeTr("cancel"), role: "primary",
                                      action: function() {} }
                                ]
                            })
                        }
                    }
                }
            }
        }

        Column {
            visible: root.subView === "profiles"
            anchors.fill: parent
            spacing: 0

            // Вкладки профилей
            ProfileTab {
                id: profileTabBar
                currentProfileId: root.currentProfileId
                onCurrentProfileIdChanged: root.currentProfileId = currentProfileId
                onAddClicked: {
                    imagePickerPopup.open()
                }
            }

            // Детали профиля — прокручиваются, если не влезают (крупный масштаб)
            Flickable {
                id: profileFlick
                width: parent.width
                height: parent.height - profileTabBar.height
                contentHeight: profileDetails.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height

                ProfileDetails {
                    id: profileDetails
                    width: profileFlick.width
                    currentProfileId: root.currentProfileId
                    profileData:      root.profileData
                    onSaveAndRefresh: function(profileId, field, value) {
                        root._doSaveAndRefresh(profileId, field, value)
                    }
                }
            }
        }
    }

    // ── FileDialog (обзор образа) ─────────────────────────────────────────────
    FileDialog {
        id: imageFileDialog
        title: Settings.safeTr("select_system_image")
        nameFilters: ["System image (system.img)", "Image files (*.img)", "All files (*)"]
        onAccepted: {
            // C++ сам разбирает file:-URL и берёт папку выбранного system.img
            if (AndroidTV && AndroidTV.profileManager) {
                var newId = AndroidTV.profileManager.addProfile(selectedFile.toString(), "")
                if (newId) root.currentProfileId = newId
            }
        }
    }

    // ── Попап выбора образа ───────────────────────────────────────────────────
    ImagePickerPopup {
        id: imagePickerPopup
        onProfileAdded: function(imagePath, profileName) {
            if (AndroidTV && AndroidTV.profileManager) {
                // Открываем только что созданный профиль, а не активный
                var newId = AndroidTV.profileManager.addProfile(imagePath, profileName)
                if (newId) root.currentProfileId = newId
            }
        }
        onBrowseRequested: imageFileDialog.open()
    }
}
