// JoystickBar.qml

import QtQuick
import AppSettings 1.0

// Корневой Item нужен чтобы тултип мог рендериться поверх Row,
// не обрезаясь clip-ом карточек.
Item {
    id: root

    implicitWidth:  cardsRow.implicitWidth
    implicitHeight: cardsRow.implicitHeight

    readonly property bool _hasModule: typeof JoystickModule !== "undefined"
                                       && JoystickModule !== null

    ListModel { id: localModel }

    Connections {
        target: root._hasModule ? JoystickModule : null
        function onControllersChanged() { root._syncModel() }
    }

    Component.onCompleted: root._syncModel()

    function playEnterAnimation() {
        for (let i = 0; i < repeater.count; i++) {
            const item = repeater.itemAt(i)
            if (item && !item.dying) item.runEnter()
        }
    }

    function _syncModel() {
        if (!root._hasModule) return
        const fresh = JoystickModule.controllers

        for (let i = 0; i < localModel.count; i++) {
            const item = localModel.get(i)
            if (item.dying) continue
            if (!fresh.some(c => c.slot === item.slot))
                localModel.setProperty(i, "dying", true)
        }

        for (let j = 0; j < fresh.length; j++) {
            const c = fresh[j]
            let found = false
            for (let k = 0; k < localModel.count; k++) {
                if (localModel.get(k).slot === c.slot) {
                    localModel.setProperty(k, "name",       c.name       ?? "")
                    localModel.setProperty(k, "battery",    c.battery    ?? -1)
                    localModel.setProperty(k, "charging",   c.charging   ?? false)
                    localModel.setProperty(k, "connType",   c.connType   ?? "")
                    localModel.setProperty(k, "macAddress", c.macAddress ?? "")
                    found = true
                    break
                }
            }
            if (!found) {
                localModel.append({
                    slot:       c.slot       ?? 0,
                    name:       c.name       ?? "",
                    battery:    c.battery    ?? -1,
                    charging:   c.charging   ?? false,
                    connType:   c.connType   ?? "",
                    macAddress: c.macAddress ?? "",
                    dying:      false
                })
            }
        }
    }

    // ── Ряд карточек ──────────────────────────────────────────────────────────
    Row {
        id: cardsRow
        spacing: Settings.us(12)

        Repeater {
            id: repeater
            model: localModel

            delegate: Item {
                id: card

                required property int    slot
                required property string name
                required property int    battery
                required property bool   charging
                required property string connType
                required property string macAddress
                required property bool   dying
                required property int    index

                width:  dying ? 0 : Settings.us(70)
                height: Settings.us(80)
                clip:   true

                Behavior on width {
                    NumberAnimation { duration: 280; easing.type: Easing.InCubic }
                }

                onDyingChanged: {
                    if (dying) {
                        tooltip.hide()
                        exitAnim.start()
                    }
                }

                // ── Анимации карточки ────────────────────────────────────────
                transform: Translate { id: cardTranslate }

                ParallelAnimation {
                    id: exitAnim
                    NumberAnimation { target: cardTranslate; property: "y"; to: -Settings.us(80); duration: 280; easing.type: Easing.InCubic }
                    NumberAnimation { target: card;          property: "opacity"; to: 0;      duration: 220; easing.type: Easing.InCubic }
                    onFinished: {
                        for (let i = 0; i < localModel.count; i++) {
                            if (localModel.get(i).slot === card.slot) {
                                localModel.remove(i); break
                            }
                        }
                    }
                }

                Component.onCompleted: enterAnim.start()

                function runEnter() {
                    cardTranslate.y = -Settings.us(80)
                    card.opacity    = 0
                    enterAnim.start()
                }

                ParallelAnimation {
                    id: enterAnim
                    NumberAnimation { target: cardTranslate; property: "y"; from: -Settings.us(80); to: 0;   duration: 320; easing.type: Easing.OutCubic }
                    NumberAnimation { target: card;          property: "opacity"; from: 0;      to: 1.0; duration: 220; easing.type: Easing.OutCubic }
                }

                // ── Содержимое карточки ──────────────────────────────────────
                Column {
                    anchors.top:       parent.top
                    anchors.topMargin: Settings.us(5)
                    anchors.left:      parent.left
                    anchors.right:     parent.right
                    spacing:           Settings.us(3)

                    Image {
                        source: "qrc:/assets/icons/ic_games.png"
                        width:  Settings.us(32); height: Settings.us(32)
                        sourceSize.width: 100; sourceSize.height: 100
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        anchors.horizontalCenter: parent.horizontalCenter
                    }

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: Settings.us(4)

                        Text {
                            text: {
                                var n = card.name || ""
                                return n.length > 14 ? n.substring(0, 14) + "…" : n
                            }
                            color:          (Settings.accentColor || "#6c63ff")
                            font.pixelSize: Settings.fs(11)
                            font.bold:      true
                            visible:        (card.name || "") !== ""
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Image {
                            source: {
                                var ct = (card.connType || "").toUpperCase()
                                return (ct === "BT" || ct === "BLUETOOTH" || ct === "WIRELESS")
                                       ? "qrc:/assets/icons/ic_bluetooth.png"
                                       : "qrc:/assets/icons/ic_usb.png"
                            }
                            width: Settings.us(13); height: Settings.us(13)
                            sourceSize.width: 100; sourceSize.height: 100
                            fillMode: Image.PreserveAspectFit
                            smooth: true; opacity: 0.5
                            anchors.verticalCenter: parent.verticalCenter
                            rotation: {
                                var ct = (card.connType || "").toUpperCase()
                                return (ct === "BT" || ct === "BLUETOOTH" || ct === "WIRELESS") ? 0 : -90
                            }
                        }
                    }

                    Item {
                        width:   parent.width * 0.6
                        height:  Settings.us(4)
                        visible: card.battery >= 0
                        anchors.horizontalCenter: parent.horizontalCenter

                        Rectangle { anchors.fill: parent; radius: 3; color: "#40FFFFFF" }

                        Rectangle {
                            width:  parent.width * Math.max(0, Math.min(100, card.battery)) / 100
                            height: parent.height; radius: 3
                            color:  card.battery < 20 ? "#F44336"
                                  : card.battery < 50 ? "#FFC107"
                                  :                     "#4CAF50"
                            Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.OutQuad } }

                            Rectangle {
                                anchors.fill: parent; radius: parent.radius
                                color: "white"; opacity: 0
                                SequentialAnimation on opacity {
                                    running: card.charging; loops: Animation.Infinite; alwaysRunToEnd: true
                                    NumberAnimation { to: 0.45; duration: 600; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 0.0;  duration: 600; easing.type: Easing.InOutSine }
                                }
                            }
                        }
                    }
                }

                // ── Hover — показываем/скрываем тултип ──────────────────────
                HoverHandler {
                    onHoveredChanged: hovered ? tooltip.showFor(card) : tooltip.startHideTimer()
                }
            }
        }
    }

    // ── Тултип — живёт на уровне root, не обрезается clip-ом карточек ────────
    Item {
        id: tooltip
        z: 999
        width:  Settings.us(210)
        visible: opacity > 0
        opacity: 0

        // Данные активной карточки
        property int    tSlot:   0
        property string tName:   ""
        property string tMac:    ""
        property int    tBat:    -1
        property bool   tCharge: false
        property string tConn:   ""
        property bool   editing: false

        // Ссылка на карточку, для которой открыт тултип
        property var activeCard: null

        // Таймер задержки скрытия — чтобы мышь могла переехать на тултип
        Timer {
            id: hideTimer
            interval: 120
            onTriggered: {
                if (!tooltip.editing)
                    tooltip.hide()
            }
        }

        function showFor(card) {
            hideTimer.stop()
            activeCard = card
            tSlot   = card.slot
            tName   = card.name
            tMac    = card.macAddress
            tBat    = card.battery
            tCharge = card.charging
            tConn   = card.connType

            // Позиция: по центру карточки, снизу
            const pos = card.mapToItem(root, 0, card.height)
            x = pos.x + card.width / 2 - width / 2
            y = pos.y + Settings.us(4)

            if (opacity > 0) return
            showAnim.start()
        }

        function startHideTimer() {
            hideTimer.restart()
        }

        function hide() {
            if (editing) return
            hideTimer.stop()
            hideAnim.start()
        }

        function forceHide() {
            editing = false
            hideTimer.stop()
            hideAnim.start()
        }

        // Перехватываем hover над тултипом — HoverHandler не теряет hovered
        // при переходе мыши на дочерние элементы (в отличие от MouseArea).
        HoverHandler {
            id: tipHover
            onHoveredChanged: hovered ? hideTimer.stop() : tooltip.startHideTimer()
        }

        // ── Анимации тултипа ──────────────────────────────────────────────────
        transform: [
            Scale    { id: tipScale; origin.x: tooltip.width / 2; origin.y: 0; xScale: 1; yScale: 1 },
            Translate { id: tipSlide; y: 0 }
        ]

        ParallelAnimation {
            id: showAnim
            NumberAnimation { target: tooltip;  property: "opacity"; from: 0;    to: 1;   duration: 180; easing.type: Easing.OutCubic }
            NumberAnimation { target: tipScale; property: "xScale";  from: 0.88; to: 1;   duration: 220; easing.type: Easing.OutBack }
            NumberAnimation { target: tipScale; property: "yScale";  from: 0.88; to: 1;   duration: 220; easing.type: Easing.OutBack }
            NumberAnimation { target: tipSlide; property: "y"; from: -Settings.us(6); to: 0;  duration: 180; easing.type: Easing.OutCubic }
        }

        ParallelAnimation {
            id: hideAnim
            NumberAnimation { target: tooltip;  property: "opacity"; to: 0;    duration: 150; easing.type: Easing.InCubic }
            NumberAnimation { target: tipScale; property: "xScale";  to: 0.93; duration: 150; easing.type: Easing.InCubic }
            NumberAnimation { target: tipScale; property: "yScale";  to: 0.93; duration: 150; easing.type: Easing.InCubic }
            NumberAnimation { target: tipSlide; property: "y"; to: -Settings.us(4); duration: 150; easing.type: Easing.InCubic }
        }

        // ── Хвостик ───────────────────────────────────────────────────────────
        Canvas {
            id: tipArrow
            width:  Settings.us(14); height: Settings.us(8)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.beginPath()
                ctx.moveTo(width / 2, 0)
                ctx.lineTo(width, height)
                ctx.lineTo(0, height)
                ctx.closePath()
                ctx.fillStyle = Settings.cardColor || "#1a1a2e"
                ctx.fill()
            }
        }

        // ── Пузырь ────────────────────────────────────────────────────────────
        Rectangle {
            id: bubble
            anchors.top:   tipArrow.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height:        bubbleContent.implicitHeight + Settings.us(20)
            radius:        Settings.us(12)
            color:         Settings.cardColor || "#1a1a2e"
            border.width:  1
            border.color:  Qt.alpha(Settings.accentColor || "#6c63ff", 0.22)

            Column {
                id: bubbleContent
                anchors {
                    top: parent.top; left: parent.left; right: parent.right
                    topMargin: Settings.us(10); leftMargin: Settings.us(12); rightMargin: Settings.us(12)
                }
                spacing: Settings.us(8)

                // ── Имя + кнопка редактирования ──────────────────────────────
                Row {
                    width: parent.width
                    spacing: Settings.us(6)

                    Image {
                        source: "qrc:/assets/icons/ic_games.png"
                        width: Settings.us(16); height: Settings.us(16)
                        sourceSize.width: 64; sourceSize.height: 64
                        fillMode: Image.PreserveAspectFit
                        smooth: true; opacity: 0.6
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // Текст имени
                    Text {
                        id: nameText
                        visible:        !tooltip.editing
                        text:           tooltip.tName || ("Slot " + tooltip.tSlot)
                        color:          (Settings.accentColor || "#6c63ff")
                        font.pixelSize: Settings.fs(12)
                        font.bold:      true
                        elide:          Text.ElideRight
                        width: parent.width - Settings.us(16) - Settings.us(6) - Settings.us(22) - Settings.us(12)
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // Поле ввода
                    Rectangle {
                        visible: tooltip.editing
                        width:   nameText.width
                        height:  Settings.us(22)
                        anchors.verticalCenter: parent.verticalCenter
                        color:         Qt.rgba(1, 1, 1, 0.07)
                        radius:        Settings.us(5)
                        border.width:  1
                        border.color:  Qt.alpha(Settings.accentColor || "#6c63ff", 0.5)
                        clip:          true

                        TextInput {
                            id: nameField
                            anchors {
                                left: parent.left; right: parent.right
                                verticalCenter: parent.verticalCenter
                                leftMargin: Settings.us(5); rightMargin: Settings.us(5)
                            }
                            text:              tooltip.tName || ""
                            color:             (Settings.accentColor || "#6c63ff")
                            selectionColor:    Qt.alpha(Settings.accentColor || "#6c63ff", 0.35)
                            selectedTextColor: "#ffffff"
                            font.pixelSize:    Settings.fs(12)
                            font.bold:         true
                            clip:              true
                            Keys.onReturnPressed: tooltip.confirmEdit()
                            Keys.onEscapePressed: tooltip.cancelEdit()
                        }
                    }

                    // Кнопка ✎ / ✓
                    Rectangle {
                        width:  Settings.us(22); height: Settings.us(22)
                        radius: Settings.us(6)
                        anchors.verticalCenter: parent.verticalCenter
                        color: editBtnMouse.containsMouse
                               ? ((Settings.accentColor || "#6c63ff") ? Qt.alpha(Settings.accentColor || "#6c63ff", 0.18) : "transparent")
                               : "transparent"
                        Behavior on color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text:            tooltip.editing ? "✓" : "✎"
                            font.pixelSize:  Settings.fs(13)
                            color:           (Settings.accentColor || "#6c63ff")
                        }
                        MouseArea {
                            id: editBtnMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: tooltip.editing ? tooltip.confirmEdit() : tooltip.startEdit()
                        }
                    }
                }

                // ── Разделитель ───────────────────────────────────────────────
                Rectangle { width: parent.width; height: 1; color: Qt.rgba(1,1,1,0.07) }

                // ── Заряд ─────────────────────────────────────────────────────
                Column {
                    width: parent.width
                    spacing: Settings.us(5)
                    visible: tooltip.tBat >= 0

                    Row {
                        width: parent.width

                        Text {
                            text: "Заряд"
                            color: Qt.rgba(1,1,1,0.4)
                            font.pixelSize: Settings.fs(10)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Item { width: parent.width - battLabel.implicitWidth - Settings.us(40); height: 1 }
                        Text {
                            id: battLabel
                            text:  (tooltip.tCharge ? "⚡ " : "") + tooltip.tBat + "%"
                            color: tooltip.tBat < 20 ? "#F44336"
                                 : tooltip.tBat < 50 ? "#FFC107"
                                 :                     "#4CAF50"
                            font.pixelSize: Settings.fs(10); font.bold: true
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Item {
                        width: parent.width; height: Settings.us(6)
                        Rectangle { anchors.fill: parent; radius: Settings.us(3); color: Qt.rgba(1,1,1,0.09) }
                        Rectangle {
                            width:  parent.width * Math.max(0, Math.min(100, tooltip.tBat)) / 100
                            height: parent.height; radius: Settings.us(3)
                            color: tooltip.tBat < 20 ? "#F44336"
                                 : tooltip.tBat < 50 ? "#FFC107"
                                 :                     "#4CAF50"
                            Behavior on width { NumberAnimation { duration: 500; easing.type: Easing.OutCubic } }

                        }
                    }
                }

                // ── Тип подключения + MAC ─────────────────────────────────────
                Column {
                    width: parent.width; spacing: Settings.us(4); bottomPadding: Settings.us(2)

                    Row {
                        spacing: Settings.us(5)
                        visible: (tooltip.tConn || "") !== ""
                        Image {
                            source: {
                                const ct = (tooltip.tConn || "").toUpperCase()
                                return (ct === "BT" || ct === "BLUETOOTH" || ct === "WIRELESS")
                                       ? "qrc:/assets/icons/ic_bluetooth.png"
                                       : "qrc:/assets/icons/ic_usb.png"
                            }
                            width: Settings.us(11); height: Settings.us(11)
                            sourceSize.width: 64; sourceSize.height: 64
                            fillMode: Image.PreserveAspectFit
                            smooth: true; opacity: 0.4
                            anchors.verticalCenter: parent.verticalCenter
                            rotation: {
                                const ct = (tooltip.tConn || "").toUpperCase()
                                return (ct === "BT" || ct === "BLUETOOTH" || ct === "WIRELESS") ? 0 : -90
                            }
                        }
                        Text {
                            text: {
                                const ct = (tooltip.tConn || "").toUpperCase()
                                if (ct === "BT" || ct === "BLUETOOTH" || ct === "WIRELESS") return "Bluetooth"
                                if (ct === "USB") return "USB"
                                return tooltip.tConn
                            }
                            color: Qt.rgba(1,1,1,0.38); font.pixelSize: Settings.fs(10)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Row {
                        spacing: Settings.us(5)
                        visible: (tooltip.tMac || "") !== ""
                        Text { text: "MAC"; color: Qt.rgba(1,1,1,0.22); font.pixelSize: Settings.fs(9); anchors.verticalCenter: parent.verticalCenter }
                        Text { text: tooltip.tMac; color: Qt.rgba(1,1,1,0.30); font.pixelSize: Settings.fs(9); font.family: "Consolas"; anchors.verticalCenter: parent.verticalCenter }
                    }
                }
            }
        }

        // Обновляем высоту самого Item под пузырь + хвостик
        height: tipArrow.height + bubble.height

        // ── Методы ───────────────────────────────────────────────────────────
        function startEdit() {
            editing = true
            nameField.text = tName || ""
            nameField.forceActiveFocus()
            nameField.selectAll()
        }

        function confirmEdit() {
            const newName = nameField.text.trim()
            editing = false
            if (root._hasModule && tMac)
                JoystickModule.setControllerName(tMac, newName)
        }

        function cancelEdit() {
            editing = false
        }
    }
}
