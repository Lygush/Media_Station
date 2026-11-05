// PromptDialog.qml
// Этап 3: фон/тень/свечение/анимации → BaseDialog (явные размеры).

import QtQuick
import AppSettings 1.0
import "../base"

Item {
    id: dialog

    anchors.fill: parent
    z: 900
    visible: false

    property string title:      ""
    property string message:    ""
    property string iconSource: ""
    property var    buttons:    []

    signal closed()

    function show(options) {
        title      = options.title   || ""
        message    = options.message || ""
        iconSource = options.icon    || ""
        buttons    = options.buttons || []
        visible    = true
        base.open()
    }

    function close() {
        base.close()
    }

    // Высота: заголовок (~60) + разделитель (1) + сообщение (переменно) + кнопки (52) + отступы
    // Берём достаточный максимум; содержимое прокручивается если не влезает.
    readonly property int _contentW: Math.min(Settings.us(520), parent.width - Settings.us(40))

    BaseDialog {
        id: base
        dialogWidth:  dialog._contentW
        // Высота по содержимому (раньше фиксированные us(320): при длинном
        // тексте или кнопках столбиком содержимое не помещалось).
        dialogHeight: Math.min(dialog.parent ? dialog.parent.height - Settings.us(40) : Settings.us(600),
                               Math.max(Settings.us(220), contentColumn.implicitHeight + 2 * Settings.us(28)))

        closeOnClickOutside: true

        onVisibleChanged: if (!visible) {
            dialog.visible = false
            dialog.closed()
        }

        Column {
            id: contentColumn
            width: parent.width   // parent = contentArea, заполняет dialogBox минус margins
            spacing: Settings.us(18)

            // ── Заголовок ─────────────────────────────────────────────────────
            Row {
                visible: dialog.title !== "" || dialog.iconSource !== ""
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Settings.us(10)

                Image {
                    source: dialog.iconSource
                    width: 24; height: 24
                    sourceSize.width: 100; sourceSize.height: 100
                    fillMode: Image.PreserveAspectFit
                    anchors.verticalCenter: parent.verticalCenter
                    visible: dialog.iconSource !== ""
                }
                Text {
                    text: dialog.title
                    color: "white"
                    font.pixelSize: Settings.fs(22)
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                    visible: dialog.title !== ""
                }
            }

            Rectangle {
                visible: dialog.title !== "" || dialog.iconSource !== ""
                width: parent.width; height: 1
                color: Settings.accent(0.267)
            }

            // ── Сообщение ─────────────────────────────────────────────────────
            Text {
                width: parent.width
                text: dialog.message
                color: "white"
                font.pixelSize: Settings.fs(16)
                wrapMode: Text.WordWrap
                lineHeight: 1.4
                visible: dialog.message !== ""
            }

            // ── Кнопки ────────────────────────────────────────────────────────
            // В ряд, если помещаются; иначе столбиком на всю ширину. Раньше три
            // длинные кнопки («Удалить вместе с файлами» и т.п.) выходили за
            // край диалога.
            Item {
                id: buttonsBox
                width: parent.width
                height: buttonsGrid.height
                visible: dialog.buttons.length > 0

                // Невидимый ряд для измерения естественной ширины кнопок
                Row {
                    id: measureRow
                    visible: false
                    spacing: Settings.us(12)
                    Repeater {
                        model: dialog.buttons
                        delegate: StyledButton { text: modelData.label }
                    }
                }
                readonly property bool stacked: measureRow.implicitWidth > width

            Grid {
                id: buttonsGrid
                anchors.horizontalCenter: parent.horizontalCenter
                columns: buttonsBox.stacked ? 1 : Math.max(1, dialog.buttons.length)
                spacing: Settings.us(12)

                Repeater {
                    model: dialog.buttons
                    delegate: StyledButton {
                        width: buttonsBox.stacked ? buttonsBox.width : naturalWidth
                        text: modelData.label
                        role: modelData.role === "primary" ? "primary"
                            : modelData.role === "danger"  ? "danger"
                            : "default"
                        onClicked: {
                            if (typeof modelData.action === "function") modelData.action()
                            dialog.close()
                        }
                    }
                }
            }
            }
        }
    }
}
