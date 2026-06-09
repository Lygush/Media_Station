// modules/UpdateModule/UpdateManager.h
// ─────────────────────────────────────────────────────────────────────────────
// UpdateManager — модуль управления обновлениями MediaStation.
//
// Жизненный цикл обновления:
//   1. NetworkListener получает пакет с kind="mediastation_update"
//      и эмитирует bus->moduleEvent("updater", "updateReceived", data).
//   2. UpdateManager показывает диалог подтверждения (через QML-свойства).
//   3. Пользователь соглашается → acceptUpdate().
//   4. Проверяется SHA-256 архива, сравнивается версия.
//   5. Копируется папка Updater целиком во временную директорию.
//   6. Запускается Updater.exe с нужными аргументами.
//   7. Вызывается QCoreApplication::quit().
//
// Взаимодействие с QML:
//   Свойства newVersion, releaseNotes, updateReady + методы acceptUpdate()/declineUpdate().
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include "core/IModule.h"
#include <QObject>
#include <QString>

class EventBus;
class SettingsManager;

class UpdateManager : public QObject, public IModule
{
    Q_OBJECT

    Q_PROPERTY(bool    updateReady   READ updateReady   NOTIFY updateReadyChanged)
    Q_PROPERTY(QString newVersion    READ newVersion    NOTIFY updateReadyChanged)
    Q_PROPERTY(QString releaseNotes  READ releaseNotes  NOTIFY updateReadyChanged)
    Q_PROPERTY(qreal   downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(bool    downloading   READ isDownloading NOTIFY downloadingChanged)
    Q_PROPERTY(bool    preparing     READ isPreparing   NOTIFY preparingChanged)

public:
    explicit UpdateManager(QObject *parent = nullptr);
    ~UpdateManager() override = default;

    // ── IModule ───────────────────────────────────────────────────────────────
    QString  moduleId() const override { return QStringLiteral("updater"); }
    void     initialize(EventBus *bus, SettingsManager *settings) override;
    void     shutdown()  override {}
    QObject *qmlObject() override       { return this; }
    QString  qmlName()   const override { return QStringLiteral("UpdateManager"); }

    bool    updateReady()     const { return m_updateReady; }
    QString newVersion()      const { return m_newVersion; }
    QString releaseNotes()    const { return m_releaseNotes; }
    qreal   downloadProgress()const { return m_downloadProgress; }
    bool    isDownloading()   const { return m_downloading; }
    bool    isPreparing()     const { return m_preparing; }

    // Вызывается из NetworkListener через moduleEvent.
    // Сохраняет метаданные и устанавливает флаг готовности.
    void onUpdateReceived(const QString &version,
                          const QString &archivePath,
                          const QString &expectedHash,
                          const QString &releaseNotes);

    // Обновление прогресса загрузки (0.0–1.0).
    void onDownloadProgress(qreal progress);

public slots:
    Q_INVOKABLE void acceptUpdate();
    Q_INVOKABLE void declineUpdate();

signals:
    void updateReadyChanged();
    void downloadProgressChanged();
    void downloadingChanged();
    void preparingChanged();

private:
    bool    verifyHash(const QString &filePath, const QString &expectedHash) const;
    QString prepareTempUpdater();          // создаёт временную папку и копирует папку Updater целиком
    bool    copyDirectory(const QString &srcPath, const QString &dstPath);
    QString currentVersion() const;

    EventBus *m_bus = nullptr;

    bool    m_updateReady      = false;
    bool    m_downloading      = false;
    bool    m_preparing        = false;
    QString m_newVersion;
    QString m_releaseNotes;
    QString m_archivePath;
    QString m_expectedHash;
    qreal   m_downloadProgress = 0.0;

    QString m_appDir;
};
