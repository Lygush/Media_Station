// modules/GameMediaModule/TrailerFetcher.h
// ─────────────────────────────────────────────────────────────────────────────
// TrailerFetcher — получает трейлер через Steam appdetails API, скачивает
//                  прямой mp4 или конвертирует HLS/DASH стрим через ffmpeg.
//
// Запускается в отдельном QThread через moveToThread().
// Прогресс передаётся сигналом downloadProgress (байты или проценты * 1000).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QObject>
#include <QString>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>

class QProcess;   // forward declaration

class TrailerFetcher : public QObject
{
    Q_OBJECT

public:
    explicit TrailerFetcher(QObject *parent = nullptr);
    ~TrailerFetcher() override;

    // Запустить загрузку/конвертацию. Вызывать из рабочего потока.
    // mediaDir   — папка для сохранения trailer.mp4 (launcher/media/)
    // steamAppId — числовой ID приложения Steam
    Q_INVOKABLE void fetch(const QString &mediaDir, const QString &steamAppId);

    // Отмена — вызывается из GUI-потока; thread-safe через QMetaObject
    void cancel();

    // Позволяет задать нестандартный путь к ffmpeg.exe (по умолчанию
    // <AppDir>/tools/ffmpeg/ffmpeg.exe)
    void setFfmpegPath(const QString &path);

signals:
    // Возвращает путь к готовому trailer.mp4 (file://) или пустую строку при ошибке.
    // Больше не возвращает URL стрима.
    void finished(const QString &trailerUrl);

    // Прогресс:
    //   - при скачивании mp4: received/total (байты)
    //   - при конвертации ffmpeg: (percent*1000, 1000), где 0..1000
    //   - если общий размер неизвестен: (0, 0) — неопределённый прогресс
    void downloadProgress(qint64 received, qint64 total);

private slots:
    void onMetaReplyFinished();
    void onTrailerReadyRead();
    void onTrailerProgress(qint64 received, qint64 total);
    void onTrailerFinished();

    // Слоты для ffmpeg-конвертации
    void onFfmpegProgress();
    void onFfmpegFinished(int exitCode, QProcess::ExitStatus status);

private:
    // Разбор JSON appdetails и извлечение URL трейлера
    QString parseTrailerUrl(const QByteArray &json, const QString &appId) const;

    // Запустить конвертацию HLS/DASH потока через ffmpeg в m_destPath
    void startFfmpegConversion(const QString &streamUrl);

    // Завершить скачивание: переименовать .tmp → .mp4 или удалить .tmp при ошибке
    void finalize(bool success);

    // Сетевые объекты
    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply         *m_metaReply    = nullptr;
    QNetworkReply         *m_trailerReply = nullptr;

    // Конвертация через ffmpeg
    QProcess *m_ffmpegProcess = nullptr;
    QString   m_ffmpegPath;            // полный путь к ffmpeg.exe
    qint64    m_durationMs = 0;        // общая длительность видео (из вывода ffmpeg)

    // Файловые операции
    QFile   m_tmpFile;
    QString m_destPath;   // итоговый путь trailer.mp4
    QString m_tmpPath;    // путь к .tmp файлу

    bool    m_cancelled = false;
};
