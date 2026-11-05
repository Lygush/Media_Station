#pragma once

// GameInfoStore — единая точка чтения и записи <папка игры>/game_info.json.
//
// Файл пишут несколько модулей (описание — GameInfoModule в фоновом потоке,
// обложки и трейлер — GameMediaModule, архивация/плитки — GameLibraryModule,
// тип ввода — InstallManager). Раньше каждый делал «прочитать → поменять →
// записать целиком» без общей блокировки, и кто записал вторым, затирал поля
// первого. Теперь каждое изменение — это короткая операция под общим мьютексом
// над свежей копией файла, а меняются только переданные поля.

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

namespace GameInfoStore {

/// Путь к game_info.json в папке игры.
QString filePath(const QString &gameRoot);

/// Текущее содержимое (пустой объект, если файла нет или он повреждён).
QJsonObject read(const QString &gameRoot);

/// Записывает поля patch поверх текущего содержимого. Поле со значением
/// QJsonValue::Null удаляется. Остальные поля файла не трогаются.
bool update(const QString &gameRoot, const QJsonObject &patch);

/// То же, но с произвольным изменением свежей копии под блокировкой.
/// modify возвращает false, если записывать ничего не нужно.
bool modify(const QString &gameRoot, const std::function<bool(QJsonObject &)> &fn);

/// Полная перезапись (для создания файла при установке).
bool write(const QString &gameRoot, const QJsonObject &obj);

/// Путь для хранения: внутри папки игры — относительный ("launcher/media/cover.jpg"),
/// снаружи или URL — как есть.
QString toStored(const QString &gameRoot, const QString &path);

/// Обратное преобразование: относительный путь → абсолютный от папки игры.
QString resolve(const QString &gameRoot, const QString &stored);

/// Поля с путями к медиа, которые хранятся относительно папки игры.
const QStringList &mediaPathKeys();

/// Разворачивает относительные пути медиа в абсолютные (для QML).
void resolveMediaPaths(const QString &gameRoot, QVariantMap &info);

} // namespace GameInfoStore
