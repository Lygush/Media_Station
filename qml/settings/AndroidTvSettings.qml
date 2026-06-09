// settings/AndroidTvSettings.qml
import QtQuick
import QtQuick.Dialogs
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
                ramMb:          (typeof raw.ramMb   === "number") ? raw.ramMb   : 4096,
                resolution:     (typeof raw.resolution   === "string") ? raw.resolution   : "auto",
                audioDriver:    (typeof raw.audioDriver  === "string") ? raw.audioDriver  : "dsound",
                audioTimerMs:   (typeof raw.audioTimerMs === "string" || typeof raw.audioTimerMs === "number")
                                ? raw.audioTimerMs : "auto",
                dsoundLatencyMs:(typeof raw.dsoundLatencyMs === "string" || typeof raw.dsoundLatencyMs === "number")
                                ? raw.dsoundLatencyMs : "auto"
            }
        } else {
            profileData = {
                displayName: "", cores: 4, ramMb: 4096, resolution: "auto",
                audioDriver: "dsound", audioTimerMs: "auto", dsoundLatencyMs: "auto"
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

        Rectangle {
            anchors.fill: parent; color: "transparent"
            visible: root.subView === "common"
            Text { anchors.centerIn: parent; text: "Common settings placeholder"; color: "white" }
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

            // Детали профиля
            ProfileDetails {
                width: parent.width
                height: parent.height - profileTabBar.height
                currentProfileId: root.currentProfileId
                profileData:      root.profileData
                onSaveAndRefresh: function(profileId, field, value) {
                    root._doSaveAndRefresh(profileId, field, value)
                }
            }
        }
    }

    // ── FileDialog (обзор образа) ─────────────────────────────────────────────
    FileDialog {
        id: imageFileDialog
        title: Settings.safeTr("select_system_image")
        nameFilters: ["Image files (*.img *.iso *.qcow2)", "All files (*)"]
        onAccepted: {
            var path = selectedFile.toString().replace(/^file:\/\/\//, "")
            if (path.length > 0 && path[1] !== ":") path = "/" + path
            if (AndroidTV && AndroidTV.profileManager) {
                AndroidTV.profileManager.addProfile(path, "")
                root.currentProfileId = AndroidTV.profileManager.activeProfileId
            }
        }
    }

    // ── Попап выбора образа ───────────────────────────────────────────────────
    ImagePickerPopup {
        id: imagePickerPopup
        onProfileAdded: function(imagePath, profileName) {
            if (AndroidTV && AndroidTV.profileManager) {
                AndroidTV.profileManager.addProfile(imagePath, profileName)
                root.currentProfileId = AndroidTV.profileManager.activeProfileId
            }
        }
        onBrowseRequested: imageFileDialog.open()
    }
}
