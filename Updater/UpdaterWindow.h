#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QProcess>
#include <QTimer>
#include <QMouseEvent>
#include <QFile>
#include <QTextStream>
#include <functional>

class UpdaterWindow : public QWidget
{
    Q_OBJECT
public:
    explicit UpdaterWindow(const QString &archivePath,
                           const QString &targetDir,
                           const QString &expectedHash,
                           qint64         waitPid,
                           const QString &newVersion,
                           const QString &tempDir,     // путь к временной папке для удаления
                           QWidget *parent = nullptr);
    void startUpdate();

private slots:
    void tick();
    void extractProgress();
    void onExtractFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onRetryClicked();
    void onCancelClicked();

private:
    void setStatus(const QString &msg);
    void setProgress(int pct);
    void fatal(const QString &msg, bool removeArchive = false);
    void quitApp();
    void recoverableError(const QString &msg,
                          const QString &hint,
                          std::function<void()> retryStep);
    void dismissRetry();
    // SHA-256 архива: 1 — совпадает, 0 — не совпадает, -1 — не открыть файл.
    // Вызывается в фоновом потоке (многогигабайтный архив раньше считался
    // в GUI-потоке, и Windows помечала окно как «Не отвечает»).
    static int computeHashMatch(const QString &path, const QString &expected);
    void continueAfterHash();
    void doReplaceNow();
    void startExtract();
    void prepareNewDir();
    void doReplace();
    void launchNew();
    bool copyDirectory(const QString &srcPath, const QString &dstPath);
    bool moveDirectory(const QString &srcPath, const QString &dstPath);

    // Лог
    void log(const QString &level, const QString &msg);
    void logInfo (const QString &msg) { log("INFO ",  msg); }
    void logWarn (const QString &msg) { log("WARN ",  msg); }
    void logError(const QString &msg) { log("ERROR",  msg); }
    QFile        *m_logFile    = nullptr;
    QTextStream  *m_logStream  = nullptr;
    QString       m_logFileName;           // имя файла лога для копирования в конце

    // UI
    QLabel       *titleLabel   = nullptr;
    QLabel       *statusLabel  = nullptr;
    QProgressBar *progressBar  = nullptr;
    QLabel       *hintLabel    = nullptr;
    QPushButton  *retryBtn     = nullptr;
    QPushButton  *cancelBtn    = nullptr;

    // Параметры
    QString m_archivePath;
    QString m_targetDir;
    QString m_expectedHash;
    qint64  m_waitPid;
    QString m_newVersion;
    QString m_tempDir;          // путь к временной папке для удаления в конце

    // Состояние
    QString  m_oldDir;
    QString  m_newDir;
    QProcess *m_extractProc  = nullptr;
    QTimer   *m_pidTimer     = nullptr;
    int       m_pidWaitCount = 0;
    bool      m_keepOldDir   = false;   // не удалять oldDir (там может быть единственная копия данных)

    std::function<void()> m_retryStep;

    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    QPoint m_dragPos;
};
