// ui_snapshots — снимки экранов настоящего интерфейса MediaStation с заглушками
// C++-модулей. Нужен, чтобы видеть наложения и обрезку текста без запуска
// полного приложения (и без Windows).
//
//   ui_snapshots <папка_для_снимков> [WxH ...]
//
// Для каждого размера окна и набора настроек (масштаб, кегль, язык) открывает
// основные экраны и диалоги и сохраняет PNG.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTimer>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QEventLoop>
#include <QElapsedTimer>
#include "RemoteNavigator.h"
#include <cstdio>
#include <functional>
#include <algorithm>

static void wait(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// Проверка текста: вылезает ли подпись за свой элемент/родителя, обрезана ли «…»,
// вплотную ли к краю рамки. Пишет найденное в отчёт рядом со снимком.
static void checkTexts(QQuickItem *root, QStringList &report)
{
    // clipped: предок обрезает содержимое (прокрутка) — «за окном» не ошибка
    std::function<void(QQuickItem *, qreal, bool)> walk = [&](QQuickItem *it, qreal op, bool clipped) {
        if (!it->isVisible()) return;
        op *= it->opacity();
        if (op < 0.05) return;
        const QString cls = it->metaObject()->className();
        if (cls == QLatin1String("QQuickText")) {
            const QString text = it->property("text").toString().simplified();
            QQuickItem *par = it->parentItem();
            if (!text.isEmpty() && par && it->width() > 0) {
                const qreal cw = it->property("contentWidth").toReal();
                const bool elided = it->property("truncated").toBool();
                const QRectF own = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
                // Фактически нарисованная полоса текста (с учётом выравнивания)
                const int ha = it->property("horizontalAlignment").toInt();
                qreal x0 = 0;
                if (ha & Qt::AlignHCenter) x0 = (it->width() - cw) / 2;
                else if (ha & Qt::AlignRight) x0 = it->width() - cw;
                const QRectF painted = it->mapRectToScene(QRectF(x0, 0, cw, it->height()));
                const QRectF box = par->mapRectToScene(QRectF(0, 0, par->width(), par->height()));
                const QString t = text.left(40);
                // Текст за пределами окна (уехал за край экрана)
                const QRectF win(0, 0, root->width(), root->height());
                if (!clipped && !win.contains(painted.adjusted(1, 1, -1, -1)) && painted.intersects(win.adjusted(-2000, -2000, 2000, 2000)))
                    report << QString("OFFSCREEN '%1' [%2,%3 %4x%5]").arg(t)
                                  .arg(painted.left(), 0, 'f', 0).arg(painted.top(), 0, 'f', 0)
                                  .arg(painted.width(), 0, 'f', 0).arg(painted.height(), 0, 'f', 0);
                if (elided)
                    report << QString("ELIDED   '%1' (w=%2)").arg(t).arg(it->width());
                if (cw > it->width() + 1.5 && !elided && it->property("wrapMode").toInt() == 0)
                    report << QString("OVERFLOW '%1' text %2 > item %3").arg(t).arg(cw, 0, 'f', 0).arg(it->width());
                else if (box.width() > 0 && (painted.left() < box.left() - 1.5 || painted.right() > box.right() + 1.5)
                         && par->width() < 1500)
                    report << QString("OUTSIDE  '%1' text [%2..%3] parent %4 [%5..%6]")
                                  .arg(t).arg(painted.left(), 0, 'f', 0).arg(painted.right(), 0, 'f', 0)
                                  .arg(par->metaObject()->className())
                                  .arg(box.left(), 0, 'f', 0).arg(box.right(), 0, 'f', 0);
                else {
                    // «Рамка» — ближайший предок, который не просто обёртка
                    // вокруг текста (Row/Column по размеру содержимого)
                    QQuickItem *b = par;
                    auto isWrapper = [&](QQuickItem *x) {
                        const QString c = x->metaObject()->className();
                        const QRectF r = x->mapRectToScene(QRectF(0, 0, x->width(), x->height()));
                        Q_UNUSED(r);
                        return c.startsWith("QQuickRow") || c.startsWith("QQuickColumn")
                               || c.startsWith("QQuickText");
                    };
                    while (b && b->parentItem() && isWrapper(b)) b = b->parentItem();
                    const QRectF fb = b ? b->mapRectToScene(QRectF(0, 0, b->width(), b->height())) : QRectF();
                    const qreal gl = painted.left() - fb.left(), gr = fb.right() - painted.right();
                    if (fb.width() > 0 && fb.width() < 600 && fb.height() < 90 && painted.width() > 20
                            && (gl < 5 || gr < 5) && gl > -2 && gr > -2 && b != par->window()->contentItem())
                        report << QString("TIGHT    '%1' gap L%2 R%3 in %4px box")
                                      .arg(t).arg(gl, 0, 'f', 0).arg(gr, 0, 'f', 0).arg(fb.width(), 0, 'f', 0);
                }
                Q_UNUSED(own);
            }
        }
        for (QQuickItem *c : it->childItems()) walk(c, op, clipped || it->clip());
    };
    walk(root, 1.0, false);
}

static QVariant js(QObject *root, const QString &code)
{
    QQmlExpression expr(qmlContext(root), root, code);
    const QVariant v = expr.evaluate();
    if (expr.hasError())
        std::fprintf(stderr, "JS error in [%s]: %s\n", qPrintable(code),
                     qPrintable(expr.error().toString()));
    return v;
}

int main(int argc, char *argv[])
{
    // По умолчанию — OpenGL (под Xvfb + llvmpipe), чтобы рисовались эффекты
    // слоёв (тени, маски, размытие). SNAP_SOFTWARE=1 — программный рендер
    // (быстрее, но layer.effect не отображается).
    if (qEnvironmentVariableIsSet("SNAP_SOFTWARE"))
        qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    const QString outDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("snapshots");
    QDir().mkpath(outDir);

    QStringList sizes;
    for (int i = 2; i < argc; ++i) sizes << QString::fromLocal8Bit(argv[i]);
    if (sizes.isEmpty()) sizes = { "1280x720", "1920x1080" };

    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral(SNAPSHOT_SRC_DIR "/stubs"));

    QQmlComponent mocksComp(&engine, QUrl::fromLocalFile(QStringLiteral(SNAPSHOT_SRC_DIR "/mocks/Mocks.qml")));
    QObject *mocks = mocksComp.create();
    if (!mocks) {
        std::fprintf(stderr, "Mocks.qml: %s\n", qPrintable(mocksComp.errorString()));
        return 1;
    }
    auto obj = [mocks](const char *name) { return mocks->property(name).value<QObject *>(); };

    QObject *settings = obj("settingsManager");
    qmlRegisterSingletonInstance("AppSettings", 1, 0, "SettingsManager", settings);
    qmlRegisterSingletonInstance("AppSettings", 1, 0, "AppController", obj("appController"));
    qmlRegisterSingletonType(QUrl("qrc:/qml/Settings.qml"), "AppSettings", 1, 0, "Settings");

    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty("TileManager",        obj("tileManager"));
    ctx->setContextProperty("NetworkListener",    obj("networkListener"));
    ctx->setContextProperty("GameLibraryModule",  obj("gameLibrary"));
    ctx->setContextProperty("JoystickModule",     obj("joystick"));
    ctx->setContextProperty("G72Battery",         obj("g72Battery"));
    ctx->setContextProperty("GameInfoModule",     obj("gameInfo"));
    ctx->setContextProperty("GameMediaModule",    obj("gameMedia"));
    ctx->setContextProperty("KaraokeModule",      obj("karaoke"));
    ctx->setContextProperty("OverlayModule",      obj("overlay"));
    ctx->setContextProperty("NotificationModule", obj("notification"));
    ctx->setContextProperty("UpdateManager",      obj("updateManager"));
    ctx->setContextProperty("AndroidTV",          obj("androidTV"));
    QObject dummy;
    ctx->setContextProperty("LoggerModule",   &dummy);
    ctx->setContextProperty("G72Keys",        obj("g72Keys"));
    ctx->setContextProperty("InstallManager", &dummy);
    // Настоящий навигатор: проверяем и рамку фокуса. Живёт дольше движка QML.
    RemoteNavigator &remoteNav = *new RemoteNavigator(&app);
    ctx->setContextProperty("RemoteNav", &remoteNav);

    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!win) return 3;
    QObject *root = win;
    remoteNav.setWindow(win);

    struct Config { QString tag; double scale; int font; QString lang; };
    const QList<Config> configs = {
        { "base",       1.0, 0, "ru" },
        { "big_font",   1.0, 6, "ru" },
        { "max_font",   1.0, 12, "ru" },  // самый крупный кегль в настройках
        { "scale150",   1.5, 0, "ru" },
        { "en",         1.0, 0, "en" },
        { "tv_big",     1.25, 4, "ru" },   // типичный ТВ: крупнее и интерфейс, и кегль
    };

    struct Shot { QString name; QString openJs; int waitMs; };
    const QList<Shot> shots = {
        { "01_home",        "",                                                           900 },
        { "02_games",       "root.openScreen(Settings.tr('Games'), 'games', false)",       1200 },
        { "03_library",     "root.openLibraryScreen()",                                    1500 },
        { "03b_library_menu", "root.openLibraryScreen()",                                  1500 },
        { "04_settings",    "root.openSettingsScreen()",                                   1200 },
        { "05_game_detail", "root.openGameDetail({title:'The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade', installFolder:'C:/g/2', id:'t2', kind:'app'})", 1500 },
        { "05b_gd_candidates", "root.openGameDetail({title:'Universe Sandbox', installFolder:'C:/g/3', id:'t3', kind:'app'})", 1200 },
        { "05c_gd_generating", "root.openGameDetail({title:'Universe Sandbox', installFolder:'C:/g/3', id:'t3', kind:'app'})", 1200 },
        { "06_karaoke",     "root.openKaraokeScreen()",                                    1200 },
        { "07_install_dialog", "installRequestDialog.open({title:'Ведьмак 3: Дикая Охота — Издание «Игра года»', kind:'app', exePath:'bin/x64/witcher3.exe', iconPath:'', socket:null})", 900 },
        { "09_tv",          "root.openScreen(Settings.tr('TV'), 'tv', false)",             1200 },
        { "10_set_general", "root.openSettingsScreen(); stack.currentItem.activeCategory = 'general'",    1400 },
        { "11_set_appearance", "root.openSettingsScreen(); stack.currentItem.activeCategory = 'appearance'", 1400 },
        { "12_set_games",   "root.openSettingsScreen(); stack.currentItem.activeCategory = 'games'",      1400 },
        { "13_set_androidtv", "root.openSettingsScreen(); stack.currentItem.activeCategory = 'androidtv'", 1400 },
        { "13b_set_atv_profiles", "root.openSettingsScreen(); stack.currentItem.activeCategory = 'androidtv'; stack.currentItem.sectionItem.subView = 'profiles'", 1400 },
        { "13c_set_atv_common", "root.openSettingsScreen(); stack.currentItem.activeCategory = 'androidtv'; stack.currentItem.sectionItem.subView = 'common'", 1400 },
        { "14_set_logs",    "root.openSettingsScreen(); stack.currentItem.activeCategory = 'logs'",       1400 },
        { "15_edit_tile",   "root.openScreen(Settings.tr('Games'), 'games', false)",                       1200 },
        { "15b_tile_menu",  "root.openScreen(Settings.tr('Games'), 'games', false)",                       1200 },
        { "15c_new_shortcut", "root.openScreen(Settings.tr('Games'), 'games', false)",                     1200 },
        { "16_power_menu",  "powerMenu.menuVisible = true",                                                900 },
        // Навигация стрелками: рамка фокуса на плитках, в меню питания и в диалоге
        { "19_nav_games",   "root.openScreen(Settings.tr('Games'), 'games', false)",                       1200 },
        { "19b_nav_power",  "powerMenu.toggle()",                                                          900 },
        // Обложка, полученная на странице игры, появляется на плитке сразу
        { "20_cover_refresh", "root.openScreen(Settings.tr('Games'), 'games', false)",                    1200 },
        { "19c_nav_prompt", "root.promptDialog.show({title:'Удаление', message:'Удалить ярлык?', buttons:[{label:'Отмена',role:'cancel',action:function(){}},{label:'Удалить',role:'danger',action:function(){}}]})", 900 },
        { "17_notification", "NotificationModule.visible = true; NotificationModule.showRequested()",     1100 },
        { "18_update",      "UpdateManager.updateReady = true",                                            1100 },
        { "08_prompt",      "root.promptDialog.show({title:'Удаление', message:'Удалить «The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade» вместе с файлами? Это действие нельзя отменить.', buttons:[{label:'Отмена',role:'cancel',action:function(){}},{label:'Удалить только ярлык',role:'primary',action:function(){}},{label:'Удалить вместе с файлами',role:'danger',action:function(){}}]})", 900 },
    };

    // Фильтры для быстрых прогонов: SNAP_ONLY="13b,14_" (префиксы снимков),
    // SNAP_CONFIGS="base,big_font"
    const QStringList onlyShots = QString::fromLocal8Bit(qgetenv("SNAP_ONLY")).split(',', Qt::SkipEmptyParts);
    const QStringList onlyCfg   = QString::fromLocal8Bit(qgetenv("SNAP_CONFIGS")).split(',', Qt::SkipEmptyParts);
    auto wanted = [&](const QString &name) {
        if (onlyShots.isEmpty()) return true;
        for (const QString &p : onlyShots) if (name.startsWith(p)) return true;
        return false;
    };

    for (const QString &sz : sizes) {
        const QStringList wh = sz.split('x');
        if (wh.size() != 2) continue;
        win->resize(wh[0].toInt(), wh[1].toInt());
        for (const Config &c : configs) {
            if (!onlyCfg.isEmpty() && !onlyCfg.contains(c.tag)) continue;
            settings->setProperty("uiScale", c.scale);
            settings->setProperty("fontOffset", c.font);
            settings->setProperty("language", c.lang);
            for (const Shot &s : shots) {
                if (!wanted(s.name)) continue;
                js(root, "stack.pop(null)");
                remoteNav.clear();
                js(root, "powerMenu.menuVisible = false");
                js(root, "NotificationModule.visible = false; NotificationModule.hideRequested()");
                js(root, "UpdateManager.updateReady = false");
                js(root, "GameInfoModule.fetching = false; GameMediaModule.fetchingTrailer = false");
                js(root, "installRequestDialog.visible = false");
                js(root, "if (root.promptDialog.close) root.promptDialog.close(); else root.promptDialog.visible = false");
                wait(700);
                QElapsedTimer openT; openT.start();
                if (!s.openJs.isEmpty()) js(root, s.openJs);
                const qint64 openMs = openT.elapsed();
                // Замер: время открытия и частота/длительность кадров после
                int frames = 0; qint64 worst = 0; QElapsedTimer ft; ft.start();
                auto conn = QObject::connect(win, &QQuickWindow::frameSwapped, [&]() {
                    worst = std::max(worst, ft.restart()); ++frames; });
                if (qEnvironmentVariableIsSet("SNAP_PERF")) {
                    js(root, "1"); // no-op
                }
                wait(s.waitMs);
                QObject::disconnect(conn);
                if (qEnvironmentVariableIsSet("SNAP_PERF"))
                    std::fprintf(stderr, "PERF %-22s open %4lld ms, frames %3d in %d ms, worst frame %lld ms\n",
                                 qPrintable(s.name), openMs, frames, s.waitMs, worst);
                if (s.name == "20_cover_refresh") {
                    js(root, "GameMediaModule.mediaFetched('t1', {coverPath: 'qrc:/assets/background.png'})");
                    wait(600);
                    js(root, "(function(){ var t = stack.currentItem.tilesModel;"
                             " for (var i = 0; i < t.count; i++) if (t.get(i).id === 't1')"
                             " console.log('COVER t1 =', t.get(i).coverPath) })()");
                }
                if (s.name.startsWith("19")) {
                    // Курсор — в левый верхний угол окна, затем стрелки
                    QCursor::setPos(win->mapToGlobal(QPoint(1, 1)));
                    const QStringList keys = s.name == "19_nav_games" ? QStringList{"down", "down", "right"}
                                            : s.name == "19b_nav_power" ? QStringList{"down", "down"}
                                            : QStringList{"down", "right"};
                    for (const QString &k : keys) { remoteNav.move(k); wait(150); }
                    QQuickItem *t = remoteNav.target();
                    std::fprintf(stderr, "NAV %-16s target %s rect %.0f,%.0f %.0fx%.0f\n",
                                 qPrintable(s.name),
                                 t ? t->metaObject()->className() : "none",
                                 remoteNav.targetRect().x(), remoteNav.targetRect().y(),
                                 remoteNav.targetRect().width(), remoteNav.targetRect().height());
                    wait(300);
                }
                if (s.name == "15c_new_shortcut") {
                    js(root, "stack.currentItem.editDialog.open({type:'shortcut', kind:'app', title:'', icon:'', path:'', id:''})");
                    wait(1000);
                }
                if (s.name == "15_edit_tile") {   // экран открыт — теперь диалог
                    js(root, "stack.currentItem.editDialog.open({type:'shortcut', kind:'app', title:'The Elder Scrolls V: Skyrim Special Edition — Anniversary Upgrade', icon:'', path:'C:/g/2/launcher/launch.ps1', id:'t2', installFolder:'C:/g/2'})");
                    wait(1000);
                }
                if (s.name.startsWith("05b") || s.name.startsWith("05c")) {
                    // Страница игры во время загрузки: горизонтальная шапка Steam
                    // вместо обложки + попап выбора игры / генерации
                    const QString coverFile = outDir + "/_landscape_cover.png";
                    if (!QFile::exists(coverFile)) {
                        QImage c(460, 215, QImage::Format_RGB32);
                        for (int y = 0; y < c.height(); ++y)
                            for (int x = 0; x < c.width(); ++x)
                                c.setPixel(x, y, qRgb(40 + x / 3, 60 + y / 2, 140));
                        c.save(coverFile);
                    }
                    js(root, "GameInfoModule.fetching = true");
                    if (s.name.startsWith("05c"))
                        js(root, "GameMediaModule.fetchingTrailer = true; GameMediaModule.trailerProgress = 0.35");
                    std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *it) -> QQuickItem * {
                        if (QString(it->metaObject()->className()).startsWith("GameDetailLeftPanel") && it->isVisible())
                            return it;
                        for (QQuickItem *c : it->childItems()) if (QQuickItem *r = find(c)) return r;
                        return nullptr;
                    };
                    if (QQuickItem *lp = find(win->contentItem())) {
                        lp->setProperty("cover", QDir(coverFile).absolutePath());
                        if (s.name.startsWith("05b")) {
                            QVariantList cands;
                            const char *names[] = { "Universe Sandbox", "The Sandbox Evolution - Craft a 2D Pixel Universe!",
                                                    "We Are the Universe", "Tortured Hearts - Or How I Saved The Universe. Again.",
                                                    "Sandbox 5", "Universe 6" };
                            int score[] = { 100, 36, 18, 18, 10, 9 };
                            for (int i = 0; i < 6; ++i)
                                cands << QVariantMap{{"appId", QString::number(1000 + i)}, {"name", names[i]},
                                                     {"title", names[i]}, {"score", score[i] / 100.0}};
                            lp->setProperty("_dbCandidates", cands);
                            lp->setProperty("_dbPopupVisible", true);
                        } else {
                            lp->setProperty("genre", "Симулятор");
                            lp->setProperty("year", "2015");
                            lp->setProperty("developer", "Giant Army");
                            lp->setProperty("rating", "95/100");
                            QString json = "{\n  \"genre\": \"Симулятор\",\n  \"year\": 2015,\n  \"description\": \"";
                            for (int i = 0; i < 40; ++i) json += "Длинное описание игры. ";
                            lp->setProperty("_jsonText", json);
                            lp->setProperty("_jsonPopupVisible", true);
                        }
                    }
                    wait(1200);
                }
                if (s.name == "03b_library_menu") {  // контекстное меню строки библиотеки
                    std::function<QQuickItem *(QQuickItem *)> findMenu = [&](QQuickItem *it) -> QQuickItem * {
                        if (QString(it->metaObject()->className()).startsWith("LibraryContextMenu")) return it;
                        for (QQuickItem *c : it->childItems()) if (QQuickItem *r = findMenu(c)) return r;
                        return nullptr;
                    };
                    if (QQuickItem *m = findMenu(win->contentItem())) {
                        js(m, "openForRow(0, width * 0.45, height * 0.3, {gameId:'g1', name:'The Elder Scrolls V: Skyrim Special Edition', "
                              "type:'installed', available:true, tileCount:1, legacyNoMetadata:true})");
                        if (qEnvironmentVariableIsSet("SNAP_DEBUG"))
                            std::fprintf(stderr, "menu visible=%d opacity=%f w=%f parent=%s pvis=%d\n", m->isVisible(),
                                         m->opacity(), m->width(), m->parentItem()->metaObject()->className(),
                                         m->parentItem()->isVisible());
                        if (qEnvironmentVariableIsSet("SNAP_DEBUG")) {
                            std::function<void(QQuickItem *)> dump = [&](QQuickItem *it) {
                                if (!it->isVisible()) return;
                                const QVariant l = it->property("label");
                                if (l.isValid()) std::fprintf(stderr, "  item: %s\n", qPrintable(l.toString()));
                                for (QQuickItem *c : it->childItems()) dump(c);
                            };
                            dump(m);
                        }
                    } else {
                        std::fprintf(stderr, "LibraryContextMenu not found\n");
                    }
                    wait(800);
                }
                if (s.name == "15b_tile_menu") {  // контекстное меню второй видимой плитки
                    QList<QQuickItem *> menus;
                    std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
                        if (!it->isVisible() && !QString(it->metaObject()->className()).startsWith("TileContextMenu")) return;
                        if (QString(it->metaObject()->className()).startsWith("TileContextMenu")
                                && it->property("tileMode").toString() == "normal")
                            menus << it;
                        for (QQuickItem *c : it->childItems()) walk(c);
                    };
                    walk(win->contentItem());
                    if (menus.size() > 1) QMetaObject::invokeMethod(menus[1], "open");
                    else if (!menus.isEmpty()) QMetaObject::invokeMethod(menus[0], "open");
                    wait(800);
                }
                QStringList report;
                checkTexts(win->contentItem(), report);
                report.removeDuplicates();
                if (!report.isEmpty()) {
                    QFile rf(QString("%1/%2_%3_%4.txt").arg(outDir, sz, c.tag, s.name));
                    if (rf.open(QIODevice::WriteOnly | QIODevice::Text))
                        rf.write(report.join('\n').toUtf8() + '\n');
                }
                const QImage img = win->grabWindow();
                const QString file = QString("%1/%2_%3_%4.png").arg(outDir, sz, c.tag, s.name);
                img.save(file);
                std::printf("%s\n", qPrintable(file));
                std::fflush(stdout);
            }
        }
    }
    // Проверка утечки экранов: после возврата на главный все созданные экраны
    // должны быть уничтожены
    js(root, "stack.pop(null)");
    wait(1500);
    int alive = 0;
    std::function<void(QObject *)> count = [&](QObject *o) {
        const QString c = o->metaObject()->className();
        if (c.startsWith("GameDetailScreen") || c.startsWith("KaraokeScreen") || c.startsWith("SettingsScreen")
                || c.startsWith("GameLibraryScreen") || c.startsWith("DynamicScreen"))
            ++alive;
        for (QObject *ch : o->children()) count(ch);
    };
    count(win);
    std::fprintf(stderr, "SCREENS ALIVE after pop(null): %d (owned list %s)\n", alive,
                 qPrintable(js(root, "_ownedScreens.length").toString()));
    return 0;
}
