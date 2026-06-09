#include <QtTest>
#include <QSettings>
#include "SettingsManager.h"

class tst_SettingsManager : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        // Очищаем настройки перед каждым тестом
        QSettings settings("MyCompany", "MediaStation");
        settings.clear();
        mgr = new SettingsManager(this);
    }

    void cleanup() {
        delete mgr;
        mgr = nullptr;
    }

    void defaults() {
        QCOMPARE(mgr->accentColor(), QString("#87CEEB"));   // заглавные
        QCOMPARE(mgr->uiScale(), 1.0);
        QCOMPARE(mgr->fullscreen(), false);
        QCOMPARE(mgr->language(), QString("ru"));
    }

    void setAndGet() {
        mgr->setAccentColor("#FF0000");
        QCOMPARE(mgr->accentColor(), QString("#FF0000"));   // заглавные
    }

    void uiScaleClamping() {
        mgr->setUiScale(0.5);
        QCOMPARE(mgr->uiScale(), 1.0);
        mgr->setUiScale(2.0);
        QCOMPARE(mgr->uiScale(), 1.5);
    }

private:
    SettingsManager *mgr = nullptr;
};

QTEST_MAIN(tst_SettingsManager)
#include "tst_settingsmanager.moc"
