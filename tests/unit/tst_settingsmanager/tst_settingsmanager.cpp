#include <QtTest>
#include "SettingsManager.h"

class tst_SettingsManager : public QObject
{
    Q_OBJECT

private slots:
    void init() {
        mgr = new SettingsManager(this);
        // Сохраняем исходные настройки пользователя
        originalAccent   = mgr->accentColor();
        originalUiScale  = mgr->uiScale();
        originalFullscreen = mgr->fullscreen();
        originalLanguage = mgr->language();
        originalFontOffset = mgr->fontOffset();
        originalInstallPath = mgr->installPath();
    }

    void cleanup() {
        // Восстанавливаем исходные настройки
        mgr->setAccentColor(originalAccent);
        mgr->setUiScale(originalUiScale);
        mgr->setFullscreen(originalFullscreen);
        mgr->setLanguage(originalLanguage);
        mgr->setFontOffset(originalFontOffset);
        mgr->setInstallPath(originalInstallPath);
        delete mgr;
    }

    // Проверяем, что сеттеры и геттеры работают (без проверки значений по умолчанию)
    void setAndGet() {
        mgr->setAccentColor("#FF0000");
        QCOMPARE(mgr->accentColor(), QString("#FF0000"));

        mgr->setUiScale(1.25);
        QCOMPARE(mgr->uiScale(), 1.25);

        mgr->setFullscreen(true);
        QCOMPARE(mgr->fullscreen(), true);

        mgr->setLanguage("en");
        QCOMPARE(mgr->language(), QString("en"));
    }

    void uiScaleClamping() {
        mgr->setUiScale(0.5);
        QCOMPARE(mgr->uiScale(), 1.0);
        mgr->setUiScale(2.0);
        QCOMPARE(mgr->uiScale(), 1.5);
    }

    void fontOffsetClamping() {
        mgr->setFontOffset(-10);
        QCOMPARE(mgr->fontOffset(), -4);
        // Диапазон −4…12 — совпадает с кнопками в AppearanceSection.qml
        mgr->setFontOffset(20);
        QCOMPARE(mgr->fontOffset(), 12);
        mgr->setFontOffset(9);
        QCOMPARE(mgr->fontOffset(), 9);
    }

private:
    SettingsManager *mgr = nullptr;
    QString originalAccent;
    double originalUiScale;
    bool originalFullscreen;
    QString originalLanguage;
    int originalFontOffset;
    QString originalInstallPath;
};

QTEST_MAIN(tst_SettingsManager)
#include "tst_settingsmanager.moc"
