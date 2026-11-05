// SettingsManager.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include "SettingsManager.h"
#include <qmath.h>
#include <QCoreApplication>
#include <QDir>

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent)
    , m_settings("MyCompany", "MediaStation")
{}

// ── Общие геттеры ───────────────────────────────────────────────────────────

QString SettingsManager::accentColor() const
{
    return m_settings.value("accentColor", "#87CEEB").toString();
}

QString SettingsManager::backgroundImage() const
{
    return m_settings.value("backgroundImage", QString()).toString();
}

bool SettingsManager::fullscreen() const
{
    return m_settings.value("fullscreen", false).toBool();
}

QString SettingsManager::language() const
{
    return m_settings.value("language", "ru").toString();
}

// Пустое значение (по умолчанию) раньше уходило в модули как есть, и каждый
// трактовал его по-своему: установка шла в <app>/data/games, а библиотека игр
// при пустом пути не сканировала ничего, поиск папки игры по tile_id тоже.
// Теперь пустое значение всегда означает каталог по умолчанию.
QString SettingsManager::defaultInstallPath()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/data/games");
}

QString SettingsManager::installPath() const
{
    const QString p = m_settings.value("installPath", QString()).toString().trimmed();
    return p.isEmpty() ? defaultInstallPath() : p;
}

double SettingsManager::uiScale() const
{
    return m_settings.value("uiScale", 1.0).toDouble();
}

int SettingsManager::fontOffset() const
{
    return m_settings.value("fontOffset", 0).toInt();
}

bool SettingsManager::enableFogShader() const
{
    return m_settings.value("enableFogShader", true).toBool();
}

// ── Общие сеттеры ───────────────────────────────────────────────────────────

void SettingsManager::setAccentColor(const QString &color)
{
    if (accentColor() == color) return;
    m_settings.setValue("accentColor", color);
    emit accentColorChanged();
}

void SettingsManager::setBackgroundImage(const QString &path)
{
    if (backgroundImage() == path) return;
    m_settings.setValue("backgroundImage", path);
    emit backgroundImageChanged();
}

void SettingsManager::setFullscreen(bool fs)
{
    if (fullscreen() == fs) return;
    m_settings.setValue("fullscreen", fs);
    emit fullscreenChanged();
}

void SettingsManager::setLanguage(const QString &lang)
{
    if (language() == lang) return;
    m_settings.setValue("language", lang);
    emit languageChanged();
}

void SettingsManager::setInstallPath(const QString &path)
{
    if (installPath() == path) return;
    m_settings.setValue("installPath", path.trimmed());
    emit installPathChanged();
}

void SettingsManager::setUiScale(double scale)
{
    const double clamped = (scale <= 1.0) ? 1.0 : (scale <= 1.25) ? 1.25 : 1.5;
    if (qFuzzyCompare(uiScale(), clamped)) return;
    m_settings.setValue("uiScale", clamped);
    emit uiScaleChanged();
}

void SettingsManager::setFontOffset(int offset)
{
    const int clamped = qBound(-4, offset, 12);
    if (fontOffset() == clamped) return;
    m_settings.setValue("fontOffset", clamped);
    emit fontOffsetChanged();
}

void SettingsManager::setEnableFogShader(bool enable)
{
    if (enableFogShader() == enable) return;
    m_settings.setValue("enableFogShader", enable);
    emit enableFogShaderChanged();
}

// ── Настройки отправки логов ────────────────────────────────────────────────

bool SettingsManager::logUploadEnabled() const
{
    return m_settings.value("logUploadEnabled", false).toBool();
}

QString SettingsManager::logServerAddress() const
{
    return m_settings.value("logServerAddress", QString()).toString();
}

int SettingsManager::logServerPort() const
{
    return m_settings.value("logServerPort", 12347).toInt();
}

QString SettingsManager::logUploadMinLevel() const
{
    return m_settings.value("logUploadMinLevel", "warning").toString();
}

void SettingsManager::setLogUploadEnabled(bool enabled)
{
    if (logUploadEnabled() == enabled) return;
    m_settings.setValue("logUploadEnabled", enabled);
    emit logUploadEnabledChanged();
}

void SettingsManager::setLogServerAddress(const QString &address)
{
    if (logServerAddress() == address) return;
    m_settings.setValue("logServerAddress", address);
    emit logServerAddressChanged();
}

void SettingsManager::setLogServerPort(int port)
{
    int clamped = qBound(1, port, 65535);
    if (logServerPort() == clamped) return;
    m_settings.setValue("logServerPort", clamped);
    emit logServerPortChanged();
}

void SettingsManager::setLogUploadMinLevel(const QString &level)
{
    static const QStringList validLevels = {"debug", "info", "warning", "error"};
    QString lvl = level.toLower();
    if (!validLevels.contains(lvl))
        lvl = "warning";
    if (logUploadMinLevel() == lvl) return;
    m_settings.setValue("logUploadMinLevel", lvl);
    emit logUploadMinLevelChanged();
}
