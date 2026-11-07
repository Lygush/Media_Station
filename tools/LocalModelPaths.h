// tools/LocalModelPaths.h
// ─────────────────────────────────────────────────────────────────────────────
// Пути к локальным моделям нейросетей — одно место для всех модулей.
//
// Раньше GameInfoModule и SongNameResolver читали llamaModelPath из
// QSettings("MediaStation", "MediaStation"), а SettingsManager пишет в
// QSettings("MyCompany", "MediaStation") — настройка была мёртвой, и путь по
// умолчанию был скопирован в двух файлах (аудит §4 п.3, §9.3).
//
// Организацию "MyCompany" не переименовываем: в этой ветке реестра лежат все
// настройки пользователя.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QCoreApplication>
#include <QSettings>
#include <QString>

namespace LocalModels {

// Ветка реестра/ini, куда пишет SettingsManager
inline QSettings appSettings() { return QSettings(QStringLiteral("MyCompany"), QStringLiteral("MediaStation")); }

// LLM (Qwen2.5-7B): настройка llamaModelPath, иначе модель из поставки
inline QString llamaModelPath()
{
    const QString fromSettings = appSettings().value(QStringLiteral("llamaModelPath")).toString();
    if (!fromSettings.isEmpty()) return fromSettings;
    return QCoreApplication::applicationDirPath()
           + QStringLiteral("/tools/llama/models/qwen2.5-7b-instruct-q4_k_m-00001-of-00002.gguf");
}

} // namespace LocalModels
