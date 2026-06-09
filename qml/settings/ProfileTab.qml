// settings/ProfileTab.qml
// ─────────────────────────────────────────────────────────────────────────────
// Горизонтальная полоса вкладок профилей.
//
// Свойства:
//   currentProfileId  — id выбранного профиля (двустороннее)
//   onAddClicked()    — пользователь нажал «+» (кнопка доступна если < 3 профилей)
// ─────────────────────────────────────────────────────────────────────────────
import QtQuick
import QtQuick.Controls.Basic 2.15
import AppSettings 1.0

Item {
    id: root

    property string currentProfileId: ""

    signal addClicked()

    width:  parent ? parent.width : 0
    height: Settings.us(38)

    ListView {
        id: profileTabsList
        anchors.left:        parent.left
        anchors.top:         parent.top
        anchors.bottom:      parent.bottom
        anchors.right:       addProfileBtn.left
        anchors.rightMargin: Settings.us(6)
        orientation:         ListView.Horizontal
        clip:                true
        model: AndroidTV && AndroidTV.profileManager
               ? AndroidTV.profileManager : null
        spacing: Settings.us(6)

        delegate: Item {
            id: tabDelegate
            property string tabLabel:    model.name || ""
            property int    desiredWidth: tabLabelMetrics.width + Settings.us(32)
            width:  Math.max(Settings.us(60), Math.min(Settings.us(240), desiredWidth))
            height: parent ? parent.height - Settings.us(4) : 0
            y:      Settings.us(2)
            property bool isActive: model.profileId === root.currentProfileId

            Rectangle {
                anchors.fill: parent; radius: 8
                color: isActive ? Settings.accent(0.45)
                                : (tabMA.containsMouse ? Settings.accent(0.22)
                                                       : Settings.accent(0.1))
                border.color: isActive ? Settings.accent(1.0) : Settings.accent(0.3)
                border.width: 1.5
                Behavior on color { ColorAnimation { duration: 150 } }
            }
            Text {
                id: profileTabText
                anchors.centerIn: parent
                text: tabDelegate.tabLabel
                color: "white"
                font.pixelSize: {
                    var maxWidth = parent.width - Settings.us(10)
                    if (tabLabelMetrics.width > maxWidth)
                        return Math.max(Settings.us(10), Settings.fs(13) * maxWidth / tabLabelMetrics.width)
                    return Settings.fs(13)
                }
                font.bold: isActive
                width: parent.width - Settings.us(10)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
            }
            TextMetrics {
                id: tabLabelMetrics
                font.pixelSize: Settings.fs(13)
                font.family:    profileTabText.font.family
                text:           tabDelegate.tabLabel
            }
            MouseArea {
                id: tabMA; anchors.fill: parent; hoverEnabled: true
                onClicked: root.currentProfileId = model.profileId
            }
            ToolTip {
                visible: tabMA.containsMouse
                         && tabLabelMetrics.width > parent.width - Settings.us(10)
                text:  tabDelegate.tabLabel
                delay: 500
            }
        }
    }

    Item {
        id: addProfileBtn
        anchors.right:        parent.right
        anchors.top:          parent.top
        anchors.bottom:       parent.bottom
        anchors.topMargin:    Settings.us(2)
        anchors.bottomMargin: Settings.us(2)
        width: Settings.us(36)

        Rectangle {
            anchors.fill: parent; radius: 8
            color: addProfileMA.containsMouse ? Settings.accent(0.5)
                                              : Settings.accent(0.22)
            border.color: Settings.accent(0.6); border.width: 1.5
            Behavior on color { ColorAnimation { duration: 120 } }
        }
        Text {
            anchors.centerIn: parent; text: "+"
            color: "white"; font.pixelSize: Settings.fs(18); font.bold: true
        }
        opacity: (AndroidTV && AndroidTV.profileManager
                  && AndroidTV.profileManager.count >= 3) ? 0.35 : 1.0
        MouseArea {
            id: addProfileMA; anchors.fill: parent; hoverEnabled: true
            enabled: !(AndroidTV && AndroidTV.profileManager
                       && AndroidTV.profileManager.count >= 3)
            onClicked: root.addClicked()
        }
        ToolTip {
            visible: addProfileMA.containsMouse && !addProfileMA.enabled
            text:    Settings.safeTr("max_profiles_reached")
            delay:   300
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width; height: 1
        color: Settings.accent(0.25)
    }
}
