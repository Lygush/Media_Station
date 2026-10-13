// BootAnimationPlayer.qml
// ─────────────────────────────────────────────────────────────────────────────
// Логика desc.txt (512x416):
//
//   c 1 0  part0   ┐
//   c 1 15 part1   │ анимация логотипа — проигрывается ОДИН РАЗ подряд
//   c 1 0  part2   │
//   c 1 0  part3   ┘
//   f 0 0  part4   → полоска ожидания, крутится по кругу пока booting
//                    когда running=true — доигрывает текущий цикл до конца → done()
//                    после этого держит последний кадр до скрытия
// ─────────────────────────────────────────────────────────────────────────────

import QtQuick

Item {
    id: root

    property bool booting: false
    property bool running: false

    // Размер кадра из desc.txt
    readonly property int sourceWidth: 512
    readonly property int sourceHeight: 416

    signal done()

    // ── Сегменты ─────────────────────────────────────────────────────────────
    readonly property var _cSegs: [
        { part: 0, start: 0,    count: 1,   pause: 0  },
        { part: 1, start: 1000, count: 34,  pause: 15 },
        { part: 2, start: 2000, count: 39,  pause: 0  },
        { part: 3, start: 3000, count: 185, pause: 0  }
    ]
    readonly property var _fSeg:
        ({ part: 4, start: 4000, count: 102, pause: 0 })

    // ── Состояние ────────────────────────────────────────────────────────────
    // _phase: "idle" | "logo" | "wait" | "finishing" | "finished"
    //   logo      — проигрывает part0→1→2→3 один раз
    //   wait      — крутит part4 по кругу
    //   finishing — доигрывает текущий цикл part4 до конца
    //   finished  — последний кадр part4 остаётся на экране, done() уже отправлен
    property string _phase:     "idle"
    property int    _segIdx:    0      // индекс в _cSegs (только для фазы logo)
    property int    _frameIdx:  0
    property int    _pauseLeft: 0
    property string _src:       ""

    function _frameSrc(part, n) {
        var s = n.toString()
        while (s.length < 5) s = "0" + s
        return "qrc:/assets/animations/ATVBoot/part" + part + "/" + s + ".png"
    }

    // ── Таймер 60 FPS ────────────────────────────────────────────────────────
    Timer {
        id: clock
        interval: 17
        repeat:   true
        running:  root._phase !== "idle" && root._phase !== "finished"
        onTriggered: root._tick()
    }

    function _tick() {
        if (_pauseLeft > 0) { _pauseLeft--; return }

        if (_phase === "logo") {
            var seg = _cSegs[_segIdx]
            _src = _frameSrc(seg.part, seg.start + _frameIdx)
            _frameIdx++

            if (_frameIdx >= seg.count) {
                _frameIdx  = 0
                _pauseLeft = seg.pause

                if (_segIdx < _cSegs.length - 1) {
                    _segIdx++
                } else {
                    _phase    = "wait"
                    _frameIdx = 0
                }
            }

        } else if (_phase === "wait") {
            _src = _frameSrc(_fSeg.part, _fSeg.start + _frameIdx)
            _frameIdx++

            if (_frameIdx >= _fSeg.count) {
                _frameIdx = 0
                if (running) {
                    _phase = "finishing"
                }
            }

        } else if (_phase === "finishing") {
            // Доигрываем последний цикл part4
            _src = _frameSrc(_fSeg.part, _fSeg.start + _frameIdx)
            _frameIdx++

            if (_frameIdx >= _fSeg.count) {
                // Завершили цикл — замираем на последнем кадре
                _phase = "finished"
                root.done()
                // _src остаётся последним кадром, таймер останавливается
            }
        }
    }

    onBootingChanged: {
        if (booting && _phase === "idle") {
            _phase     = "logo"
            _segIdx    = 0
            _frameIdx  = 0
            _pauseLeft = 0
            _src       = ""
        }
        // Скрываем анимацию, только когда оба флага сброшены (эмулятор полностью активен)
        if (!booting && !running && (_phase === "finished" || _phase !== "idle")) {
            _phase = "idle"
            _src   = ""
        }
    }

    onRunningChanged: {
        // running пришёл — флаг будет обработан в конце текущего цикла wait
        if (!running && !booting && (_phase === "finished" || _phase !== "idle")) {
            _phase = "idle"
            _src   = ""
        }
    }

    // ── Отображение ──────────────────────────────────────────────────────────
    // Видим во всех активных фазах, включая finished (последний кадр)
    visible: _phase !== "idle"

    Image {
        anchors.fill: parent
        source:       root._src
        fillMode:     Image.PreserveAspectFit
        smooth:       false
        asynchronous: false
        cache:        true
        // Исходный размер кадра из desc.txt – для корректного масштабирования
        sourceSize.width:  root.sourceWidth
        sourceSize.height: root.sourceHeight
    }
}
