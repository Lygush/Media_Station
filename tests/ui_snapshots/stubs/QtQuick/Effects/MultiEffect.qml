// Заглушка MultiEffect для Qt < 6.5 (только для ui_snapshots):
// рисует источник без эффекта — на раскладку это не влияет.
import QtQuick

Item {
    property Item source: null
    property real blurMax: 0
    property real blur: 0
    property bool blurEnabled: false
    property bool shadowEnabled: false
    property real brightness: 0
    property real saturation: 0
    property bool autoPaddingEnabled: false

    ShaderEffectSource {
        anchors.fill: parent
        sourceItem: parent.source
        hideSource: parent.source !== null
    }
}
