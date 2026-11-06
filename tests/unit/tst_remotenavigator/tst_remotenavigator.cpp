// Навигация стрелками пульта (аудит §11 п.1): выбор соседа по направлению,
// модальные слои, скрытые/обрезанные элементы, щелчок по OK.
// Запуск без экрана: QT_QPA_PLATFORM=offscreen (ставится в main).
#include <QtTest>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlEngine>
#include "RemoteNavigator.h"

static const char *kScene = R"QML(
import QtQuick
Item {
    id: root
    width: 800; height: 600
    property int clicks: 0
    property string lastClicked: ""
    // Сетка 3×2 «плиток» 100×80, шаг 150 по x и 130 по y
    Repeater {
        model: 6
        delegate: MouseArea {
            objectName: "tile" + index
            x: 50 + (index % 3) * 150; y: 50 + Math.floor(index / 3) * 130
            width: 100; height: 80
            onClicked: { root.clicks++; root.lastClicked = objectName }
        }
    }
    // Невидимая и выключенная — не кандидаты
    MouseArea { objectName: "hidden";   x: 600; y: 50; width: 80; height: 80; visible: false }
    MouseArea { objectName: "disabled"; x: 600; y: 180; width: 80; height: 80; enabled: false }
    // Подложка на всё окно — не кандидат
    MouseArea { objectName: "backdrop"; anchors.fill: parent; z: -1 }
    // Прокручиваемая область: второй элемент обрезан полностью
    Item {
        x: 50; y: 400; width: 300; height: 100; clip: true
        MouseArea { objectName: "inClip";  x: 0;   y: 0; width: 100; height: 80 }
        MouseArea { objectName: "outClip"; x: 350; y: 0; width: 100; height: 80 }
    }
    // Диалог: пока открыт, навигация только по его кнопкам
    Item {
        id: dialog
        objectName: "dialog"
        anchors.fill: parent; z: 500; visible: false
        readonly property bool navModal: true
        MouseArea { anchors.fill: parent }   // затемнение — слишком большое, не кандидат
        MouseArea { objectName: "ok";     x: 300; y: 300; width: 90; height: 40 }
        MouseArea { objectName: "cancel"; x: 420; y: 300; width: 90; height: 40 }
    }
}
)QML";

class TstRemoteNavigator : public QObject
{
    Q_OBJECT
    QQuickView *view = nullptr;
    RemoteNavigator *nav = nullptr;

    QString targetName() const { return nav->target() ? nav->target()->objectName() : QString(); }
    QQuickItem *find(const char *name) const { return view->rootObject()->findChild<QQuickItem *>(name); }

private slots:
    void init()
    {
        view = new QQuickView;
        view->setResizeMode(QQuickView::SizeRootObjectToView);
        QQmlComponent c(view->engine());
        c.setData(kScene, QUrl("qrc:/scene.qml"));
        QObject *obj = c.create();
        QVERIFY2(obj, qPrintable(c.errorString()));
        view->setContent(QUrl(), &c, obj);
        view->resize(800, 600);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view));
        nav = new RemoteNavigator(view);
        nav->setWindow(view);
        // Курсор вне окна → первое нажатие ставит рамку на верхний левый элемент
        QCursor::setPos(view->mapToGlobal(QPoint(-500, -500)));
    }
    void cleanup() { delete view; view = nullptr; nav = nullptr; }

    void candidates()
    {
        QStringList names;
        for (const auto &c : nav->collectCandidates()) names << c.item->objectName();
        names.sort();
        QCOMPARE(names, (QStringList{"inClip", "tile0", "tile1", "tile2", "tile3", "tile4", "tile5"}));
    }

    void gridMovement()
    {
        QVERIFY(nav->move("right"));              // первое нажатие — только рамка
        QCOMPARE(targetName(), QString("tile0"));
        QVERIFY(nav->active());
        QVERIFY(nav->move("right"));  QCOMPARE(targetName(), QString("tile1"));
        QVERIFY(nav->move("right"));  QCOMPARE(targetName(), QString("tile2"));
        QVERIFY(!nav->move("right")); QCOMPARE(targetName(), QString("tile2"));   // край
        QVERIFY(nav->move("down"));   QCOMPARE(targetName(), QString("tile5"));   // тот же столбец
        QVERIFY(nav->move("left"));   QCOMPARE(targetName(), QString("tile4"));
        QVERIFY(nav->move("up"));     QCOMPARE(targetName(), QString("tile1"));
        QVERIFY(nav->move("left"));   QCOMPARE(targetName(), QString("tile0"));
        QVERIFY(nav->move("down"));   QCOMPARE(targetName(), QString("tile3"));
        QVERIFY(nav->move("down"));   QCOMPARE(targetName(), QString("inClip"));
        // Справа в своём ряду ничего (outClip обрезан) — ближайший в другом ряду
        QVERIFY(nav->move("right"));  QCOMPARE(targetName(), QString("tile4"));
    }

    void activateClicks()
    {
        nav->move("down");                         // рамка на tile0
        nav->move("right");                        // tile1
        QVERIFY(nav->activate());
        QCOMPARE(view->rootObject()->property("clicks").toInt(), 1);
        QCOMPARE(view->rootObject()->property("lastClicked").toString(), QString("tile1"));
    }

    void modalLayer()
    {
        nav->move("down");
        QCOMPARE(targetName(), QString("tile0"));
        find("dialog")->setVisible(true);
        QTest::qWait(250);                         // refresh: tile0 перекрыт → рамка снята
        QVERIFY(!nav->active());
        QVERIFY(nav->move("right"));
        QVERIFY(targetName() == "ok" || targetName() == "cancel");
        nav->move("left");  QCOMPARE(targetName(), QString("ok"));
        nav->move("right"); QCOMPARE(targetName(), QString("cancel"));
        QVERIFY(!nav->move("down"));               // за пределы диалога не уходим
    }

    void mouseMoveClears()
    {
        nav->move("down");
        QVERIFY(nav->active());
        QMouseEvent mv(QEvent::MouseMove, QPointF(700, 550), QPointF(700, 550),
                       QPointF(view->mapToGlobal(QPoint(700, 550))),
                       Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view, &mv);
        QVERIFY(!nav->active());
    }

    void pickGeometry()
    {
        // Соседний ряд выбирается, только если в своём ряду ничего нет
        const QRectF from(0, 0, 100, 100);
        const QList<QRectF> c = { QRectF(400, 0, 100, 100),     // далеко, тот же ряд
                                  QRectF(150, 120, 100, 100) }; // близко, ниже
        QCOMPARE(RemoteNavigator::pickInDirection(from, c, "right"), 0);
        QCOMPARE(RemoteNavigator::pickInDirection(from, c, "left"), -1);
        QCOMPARE(RemoteNavigator::pickInDirection(from, c, "down"), 1);
    }
};

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TstRemoteNavigator t;
    return QTest::qExec(&t, argc, argv);
}
#include "tst_remotenavigator.moc"
