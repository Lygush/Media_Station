// FileBrowserDialog.qml
// ─────────────────────────────────────────────────────────────────────────────
// FileBrowserDialog — модальный диалог выбора файлов и папок.
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick
import QtQuick.Controls
import AppSettings 1.0
import "../base"

Item {
    id: dialog

    anchors.fill: parent
    z: 500; visible: false

    property string currentPath:  ""
    property string filterExt:    ""
    property string selectedFile: ""
    property var    _caller:      null
    property bool   drivesMode:   false
    property string _listRequest: ""     // id последнего запроса чтения папки
    property bool   listing:      false  // идёт чтение папки в фоне

    // Чтение папки — в фоне (TileManager.listDirAsync): на сетевом или
    // «уснувшем» диске синхронный listDir замораживал весь интерфейс.
    // Ответы на устаревшие запросы (быстрые переходы) отбрасываются.
    Connections {
        target: TileManager
        function onDirListReady(requestId, entries) {
            if (requestId !== dialog._listRequest) return
            dialog.listing = false
            dialog._loadModel(entries)
        }
    }

    function _requestList(path) {
        _listRequest = Date.now().toString() + Math.random().toString().substring(2, 8)
        listing = true
        dirModel.clear()
        TileManager.listDirAsync(_listRequest, path, path === "" ? "" : filterExt)
    }

    function openFor(caller, startPath, ext) {
        _caller      = caller
        filterExt    = ext || ""
        selectedFile = ""
        drivesMode   = false
        _navigate(startPath || "C:/")
        dialog.visible = true
        base.open()
    }

    function open(startPath, ext) { openFor(null, startPath, ext) }
    function close()               { base.close() }

    function _navigate(path) {
        drivesMode     = false
        currentPath    = path
        pathField.text = path
        selectedFile   = ""
        selectedPathText.text = ""
        _requestList(path)
    }

    function _showDrives() {
        drivesMode    = true
        selectedFile  = ""
        selectedPathText.text = ""
        pathField.text = Settings.tr("drives")
        _requestList("")
    }

    function _goUp() {
        if (drivesMode) return
        var parent = AppController.parentPath(currentPath)
        if (parent === "") _showDrives()
        else               _navigate(parent)
    }

    function _loadModel(entries) {
        dirModel.clear()
        for (var i = 0; i < entries.length; i++)
            dirModel.append(entries[i])
    }

    function _dispatchResult(filePath) {
        if (_caller && typeof _caller.onFilePicked === "function")
            _caller.onFilePicked(filePath)
    }

    BaseDialog {
        id: base
        dialogWidth:  Settings.us(760)
        dialogHeight: Settings.us(580)
        closeOnClickOutside: false
        onVisibleChanged: if (!visible) dialog.visible = false

        Column {
            width: parent.width; spacing: Settings.us(10)
            height: parent.height

            // ── Заголовок ─────────────────────────────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: 8
                Image { source: "qrc:/assets/icons/ic_folder.png"; sourceSize.width: 26; sourceSize.height: 26; anchors.verticalCenter: parent.verticalCenter }
                Text  { text: Settings.tr("file_picker"); color: "white"; font.pixelSize: Settings.fs(22); font.bold: true }
            }

            // ── Навигационная панель ──────────────────────────────────────────
            Row {
                width: parent.width; spacing: 6; height: Settings.us(44)

                NavBtn {
                    Image {
                        source: "qrc:/assets/icons/ic_up.png"; sourceSize.width: 24; sourceSize.height: 24
                        anchors.centerIn: parent
                    }
                    onActivated: dialog._goUp()
                }

                NavBtn { label: Settings.tr("drives"); highlighted: dialog.drivesMode; onActivated: dialog._showDrives() }

                Item {
                    width: parent.width - 150; height: Settings.us(44)

                    Rectangle {
                        anchors.fill: parent; radius: 8
                        color: Settings.accent(0.133)
                        border.color: pathField.activeFocus ? Settings.accent(0.667) : Settings.accent(0.333)
                        border.width: 1.5
                    }
                    TextInput {
                        id: pathField
                        anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                        verticalAlignment: TextInput.AlignVCenter
                        color: "#E0F8FF"; font.pixelSize: Settings.fs(15); clip: true
                        selectionColor: Settings.accent(0.333); selectedTextColor: "white"
                        onAccepted: dialog._navigate(text)
                    }
                }
            }

            // ── Список файлов и папок ─────────────────────────────────────────
            Rectangle {
                width: parent.width
                height: parent.height - Settings.us(210)
                radius: 12; color: Settings.accent(0.102)
                border.color: Settings.accent(0.2); border.width: 1; clip: true

                ListModel { id: dirModel }

                // Чтение папки идёт в фоне — крутилка вместо замершего интерфейса
                Spinner {
                    anchors.centerIn: parent
                    running: dialog.listing; visible: dialog.listing
                    size: 28
                }

                ListView {
                    id: dirList
                    anchors { fill: parent; margins: 4 }
                    model: dirModel; spacing: 2; clip: true

                    delegate: Item {
                        width: dirList.width; height: Settings.us(40)
                        property bool isSelected: !model.isDir && model.path === dialog.selectedFile

                        Rectangle {
                            anchors.fill: parent; radius: 8
                            color: isSelected ? Settings.accent(0.4) : itemMA.containsMouse ? Settings.accent(0.2) : "transparent"
                            Behavior on color { ColorAnimation { duration: 100 } }
                        }

                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left; anchors.leftMargin: 12; spacing: 10

                            Image {
                                source: model.isDir ? "qrc:/assets/icons/ic_folder.png" : "qrc:/assets/icons/ic_label.png"
                                width: 22; height: 22
                                sourceSize.width: 100; sourceSize.height: 100
                                fillMode: Image.PreserveAspectFit
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: model.name
                                color: model.isDir ? Settings.accent(1.0) : "white"
                                font.pixelSize: Settings.fs(15); font.bold: model.isDir
                                anchors.verticalCenter: parent.verticalCenter
                                elide: Text.ElideRight; width: dirList.width - 70
                            }
                        }

                        MouseArea {
                            id: itemMA; anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (model.isDir) dialog._navigate(model.path)
                                else             { dialog.selectedFile = model.path; selectedPathText.text = model.path }
                            }
                            onDoubleClicked: {
                                if (!model.isDir) { _dispatchResult(model.path); dialog.close() }
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: Settings.tr("empty_folder")
                        color: Settings.accent(0.4); font.pixelSize: Settings.fs(15)
                        visible: dirModel.count === 0
                    }
                }
            }

            // ── Строка выбранного файла ───────────────────────────────────────
            Rectangle {
                width: parent.width; height: Settings.us(42); radius: 8
                color: Settings.accent(0.082); border.color: Settings.accent(0.2); border.width: 1

                Text {
                    id: selectedPathText
                    anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
                    verticalAlignment: Text.AlignVCenter
                    text: dialog.selectedFile !== "" ? dialog.selectedFile : Settings.tr("no_file")
                    color: dialog.selectedFile !== "" ? "#E0F8FF" : Settings.accent(0.4)
                    font.pixelSize: Settings.fs(14); elide: Text.ElideLeft
                }
            }

            // ── Кнопки (используем StyledButton) ──────────────────────────────
            Row {
                anchors.horizontalCenter: parent.horizontalCenter; spacing: Settings.us(16)

                StyledButton {
                    text: Settings.tr("cancel")
                    width: Math.max(Settings.us(150), naturalWidth); height: Settings.us(48)
                    onClicked: dialog.close()
                }
                StyledButton {
                    text: Settings.tr("select")
                    role: "primary"
                    width: Math.max(Settings.us(150), naturalWidth); height: Settings.us(48)
                    enabled: dialog.selectedFile !== ""
                    onClicked: {
                        if (dialog.selectedFile !== "") {
                            _dispatchResult(dialog.selectedFile)
                            dialog.close()
                        }
                    }
                }
            }
        }
    }

    // ── Компонент навигационной кнопки (оставлен без изменений) ──────────────
    component NavBtn: Item {
        property string label:       ""
        property bool   highlighted: false
        signal activated()

        width:  label.length <= 2 ? Settings.us(44) : Settings.us(90)
        height: Settings.us(44)

        Rectangle {
            anchors.fill: parent; radius: 8
            color: navMA.containsMouse ? Settings.accent(0.267) : Settings.accent(0.133)
            border.color: parent.highlighted ? Settings.accent(0.8) : Settings.accent(0.2)
            border.width: 1.5
            Behavior on color { ColorAnimation { duration: 120 } }
        }
        Text {
            anchors.centerIn: parent; text: parent.label; color: "white"
            font.pixelSize: Settings.fs(parent.label.length <= 2 ? 18 : 14)
        }
        MouseArea { id: navMA; anchors.fill: parent; hoverEnabled: true; onClicked: parent.activated() }
    }
}
