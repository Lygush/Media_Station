// modules/GameMediaModule/CoverFetcher.h
// ─────────────────────────────────────────────────────────────────────────────
// CoverFetcher — скачивает обложку и backdrop игры со Steam CDN.
//
// Стратегия:
//   cover    : library_600x900.jpg → при 404: header.jpg → при 404: ""
//   backdrop : library_hero.jpg    → при 404: ""
//
// Файлы пишутся атомарно: .tmp → QFile::rename().
// Запускается в отдельном QThread через moveToThread().
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QObject>
#include <QString>
#include <atomic>

class CoverFetcher : public QObject
{
    Q_OBJECT

public:
    explicit CoverFetcher(QObject *parent = nullptr);

    // Запустить загрузку. Вызывать из рабочего потока (через QMetaObject::invokeMethod).
    // mediaDir  — папка launcher/media/ (уже должна существовать)
    // steamAppId — числовой ID приложения Steam
    Q_INVOKABLE void fetch(const QString &mediaDir, const QString &steamAppId);

    // Отмена (вызывается из GUI-потока)
    void cancel();

signals:
    // Эмитируется по завершении (из рабочего потока — GameMediaModule
    // должен обрабатывать через Qt::QueuedConnection)
    void finished(const QString &coverPath, const QString &backdropPath);

private:
    // Атомарная запись: пишет во .tmp, потом rename
    bool saveFile(const QByteArray &data, const QString &destPath);

    // std::atomic<bool> — cancel() вызывается из GUI-потока,
    // fetch() работает в рабочем потоке, нужна атомарность
    std::atomic<bool> m_cancelled { false };
};
