import QtQuick 2.15
import QtTest 1.2
import "../qml"  // путь к исходным QML файлам

TestCase {
    name: "TileTests"

    function init() {
        // Настройка окружения (например, создание синглтона Settings)
    }

    function test_tile_initialization() {
        var tile = createTemporaryObject(tileComponent, this,
                                         {title: "MyGame", kind: "app"});
        compare(tile.title, "MyGame");
        compare(tile.kind, "app");
        verify(!tile.launching);
        verify(tile.installProgress === 0);
    }

    function test_tile_click_launch() {
        var tile = createTemporaryObject(tileComponent, this,
                                         {title: "MyGame", path: "test.ps1"});
        var spy = signalSpy.create(tile, tile.launchRequested);
        tile.clicked(null);
        compare(spy.count, 1);
        compare(spy.signalArguments[0][0], "test.ps1");
    }

    function test_tile_longPress_contextMenu() {
        var tile = createTemporaryObject(tileComponent, this,
                                         {title: "MyGame"});
        var spyMenu = signalSpy.create(tile, tile.showContextMenuRequested);
        // Эмулируем долгое нажатие
        tile.longPressed();
        compare(spyMenu.count, 1);
    }

    Component {
        id: tileComponent
        Tile {
            // минимальные свойства
            title: ""
            kind: "app"
            icon: ""
            path: ""
        }
    }
}