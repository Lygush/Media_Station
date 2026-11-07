// SettingsManager.h
// ─────────────────────────────────────────────────────────────────────────────
// SettingsManager — синглтон для хранения общих настроек приложения.
// Настройки эмулятора теперь управляются AndroidTVProfileManager.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QSettings>

class SettingsManager : public QObject
{
    Q_OBJECT

    // Общие настройки
    Q_PROPERTY(QString accentColor     READ accentColor     WRITE setAccentColor     NOTIFY accentColorChanged)
    Q_PROPERTY(QString backgroundImage READ backgroundImage WRITE setBackgroundImage NOTIFY backgroundImageChanged)
    Q_PROPERTY(bool    fullscreen       READ fullscreen       WRITE setFullscreen       NOTIFY fullscreenChanged)
    Q_PROPERTY(QString language         READ language         WRITE setLanguage         NOTIFY languageChanged)
    Q_PROPERTY(QString installPath      READ installPath      WRITE setInstallPath      NOTIFY installPathChanged)
    // Папка архивов игр; пустая — архив внутри папки игры (<игра>/archive/)
    Q_PROPERTY(QString archivePath      READ archivePath      WRITE setArchivePath      NOTIFY archivePathChanged)
    Q_PROPERTY(double  uiScale          READ uiScale          WRITE setUiScale          NOTIFY uiScaleChanged)
    Q_PROPERTY(int     fontOffset       READ fontOffset       WRITE setFontOffset       NOTIFY fontOffsetChanged)
    Q_PROPERTY(bool    enableFogShader  READ enableFogShader  WRITE setEnableFogShader  NOTIFY enableFogShaderChanged)

    // Настройки отправки логов (оставлены)
    Q_PROPERTY(bool    logUploadEnabled    READ logUploadEnabled    WRITE setLogUploadEnabled    NOTIFY logUploadEnabledChanged)
    Q_PROPERTY(QString logServerAddress    READ logServerAddress    WRITE setLogServerAddress    NOTIFY logServerAddressChanged)
    Q_PROPERTY(int     logServerPort       READ logServerPort       WRITE setLogServerPort       NOTIFY logServerPortChanged)
    Q_PROPERTY(QString logUploadMinLevel   READ logUploadMinLevel   WRITE setLogUploadMinLevel   NOTIFY logUploadMinLevelChanged)

public:
    explicit SettingsManager(QObject *parent = nullptr);

    // Общие геттеры
    QString accentColor()     const;
    QString backgroundImage() const;
    bool    fullscreen()      const;
    QString language()        const;
    QString installPath()     const;
    static QString defaultInstallPath();   ///< <appDir>/data/games
    QString archivePath()     const;       ///< пусто = внутри папки игры
    double  uiScale()         const;
    int     fontOffset()      const;
    bool    enableFogShader() const;

    // Общие сеттеры
    void setAccentColor(const QString &color);
    void setBackgroundImage(const QString &path);
    void setFullscreen(bool fs);
    void setLanguage(const QString &lang);
    void setInstallPath(const QString &path);
    void setArchivePath(const QString &path);
    void setUiScale(double scale);
    void setFontOffset(int offset);
    void setEnableFogShader(bool enable);

    // Геттеры отправки логов
    bool    logUploadEnabled()    const;
    QString logServerAddress()    const;
    int     logServerPort()       const;
    QString logUploadMinLevel()   const;

    // Сеттеры отправки логов
    void setLogUploadEnabled(bool enabled);
    void setLogServerAddress(const QString &address);
    void setLogServerPort(int port);
    void setLogUploadMinLevel(const QString &level);

signals:
    // Общие сигналы
    void accentColorChanged();
    void backgroundImageChanged();
    void fullscreenChanged();
    void languageChanged();
    void installPathChanged();
    void archivePathChanged();
    void uiScaleChanged();
    void fontOffsetChanged();
    void enableFogShaderChanged();

    // Сигналы отправки логов
    void logUploadEnabledChanged();
    void logServerAddressChanged();
    void logServerPortChanged();
    void logUploadMinLevelChanged();

private:
    QSettings m_settings;
};
