#include "UpdaterWindow.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QScreen>
#include <QRegularExpression>
#include <QThread>
#include <QTextStream>
#include <QDebug>
#include <functional>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Конструктор
// ─────────────────────────────────────────────────────────────────────────────
UpdaterWindow::UpdaterWindow(const QString &archivePath,
                             const QString &targetDir,
                             const QString &expectedHash,
                             qint64         waitPid,
                             const QString &newVersion,
                             const QString &tempDir,
                             QWidget *parent)
    : QWidget(parent)
    , m_archivePath(archivePath)
    , m_targetDir(QDir::cleanPath(targetDir))
    , m_expectedHash(expectedHash)
    , m_waitPid(waitPid)
    , m_newVersion(newVersion)
    , m_tempDir(tempDir)
{
    // ★★★ СМЕНА ТЕКУЩЕГО КАТАЛОГА НА ВРЕМЕННУЮ ПАПКУ ★★★
    // Это освобождает целевую папку MediaStation от блокировки.
    if (!m_tempDir.isEmpty()) {
        QDir::setCurrent(m_tempDir);
        qDebug() << "[Updater] CWD ->" << m_tempDir;
    } else {
        QDir::setCurrent(QDir::tempPath());
        qDebug() << "[Updater] CWD -> temp root";
    }

    // ★★★ ЛОГ-ФАЙЛ ★★★
    // Пишем во временную папку (tempDir) — targetDir будет переименовываться
    // в процессе обновления, и открытый файл внутри него блокирует rename.
    // После успешного обновления лог копируется в targetDir/logs/.
    {
        const QString logsDir = (m_tempDir.isEmpty() ? QDir::tempPath() : m_tempDir) + "/logs";
        QDir().mkpath(logsDir);
        const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        m_logFileName = "updater_" + ts + ".log";
        const QString logPath = logsDir + "/" + m_logFileName;
        m_logFile   = new QFile(logPath, this);
        if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
            m_logStream = new QTextStream(m_logFile);
            m_logStream->setEncoding(QStringConverter::Utf8);
            logInfo("=== Updater started, version: " + m_newVersion + " ===");
            logInfo("archive:    " + m_archivePath);
            logInfo("targetDir:  " + m_targetDir);
            logInfo("tempDir:    " + m_tempDir);
            logInfo("waitPid:    " + QString::number(m_waitPid));
        } else {
            qWarning() << "[Updater] Cannot open log file:" << logPath;
            delete m_logFile;
            m_logFile = nullptr;
        }
    }

    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFixedSize(460, 200);

    setStyleSheet(R"(
        QWidget {
            background-color: #1a1d23;
            color: #e8eaf0;
            font-family: 'Segoe UI', sans-serif;
        }
        QLabel#title {
            font-size: 14px;
            font-weight: 600;
            color: #ffffff;
            padding: 0 0 2px 0;
        }
        QLabel#status {
            font-size: 11px;
            color: #8b92a5;
        }
        QLabel#hint {
            font-size: 10px;
            color: #e8b840;
            padding: 2px 0 0 0;
        }
        QProgressBar {
            border: none;
            border-radius: 4px;
            background-color: #2d3140;
            height: 8px;
            text-align: center;
        }
        QProgressBar::chunk {
            border-radius: 4px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #4f8ef7, stop:1 #6dc5ff);
        }
        QPushButton {
            border: 1px solid #3d4255;
            border-radius: 6px;
            padding: 5px 18px;
            font-size: 11px;
            color: #e8eaf0;
            background-color: #2d3140;
        }
        QPushButton:hover  { background-color: #363b50; border-color: #5b6080; }
        QPushButton:pressed{ background-color: #252837; }
        QPushButton#retryBtn {
            background-color: #1e3a5f;
            border-color: #4f8ef7;
            color: #7ec8ff;
        }
        QPushButton#retryBtn:hover  { background-color: #264a78; }
        QPushButton#retryBtn:pressed{ background-color: #1a3050; }
    )");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 22, 28, 22);
    root->setSpacing(10);

    titleLabel = new QLabel(QString("Обновление MediaStation %1").arg(m_newVersion));
    titleLabel->setObjectName("title");
    root->addWidget(titleLabel);

    progressBar = new QProgressBar;
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setFixedHeight(8);
    progressBar->setTextVisible(false);
    root->addWidget(progressBar);

    statusLabel = new QLabel("Инициализация...");
    statusLabel->setObjectName("status");
    root->addWidget(statusLabel);

    hintLabel = new QLabel;
    hintLabel->setObjectName("hint");
    hintLabel->setWordWrap(true);
    hintLabel->setVisible(false);
    root->addWidget(hintLabel);

    QHBoxLayout *btnRow = new QHBoxLayout;
    btnRow->setSpacing(10);
    btnRow->addStretch();

    retryBtn = new QPushButton("Попробовать снова");
    retryBtn->setObjectName("retryBtn");
    retryBtn->setVisible(false);
    btnRow->addWidget(retryBtn);

    cancelBtn = new QPushButton("Отмена");
    cancelBtn->setVisible(false);
    btnRow->addWidget(cancelBtn);

    root->addLayout(btnRow);

    connect(retryBtn,  &QPushButton::clicked, this, &UpdaterWindow::onRetryClicked);
    connect(cancelBtn, &QPushButton::clicked, this, [this]() {
        QApplication::quit();
    });

    if (QScreen *sc = QApplication::primaryScreen()) {
        const QRect sg = sc->availableGeometry();
        move(sg.center() - rect().center());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Рекурсивное копирование директории
// ─────────────────────────────────────────────────────────────────────────────
bool UpdaterWindow::copyDirectory(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    if (!srcDir.exists())
        return false;

    QDir dstDir;
    if (!dstDir.mkpath(dstPath))
        return false;

    bool success = true;
    for (const QString &entry : srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString srcItem = srcPath + "/" + entry;
        const QString dstItem = dstPath + "/" + entry;

        if (QFileInfo(srcItem).isDir()) {
            if (!copyDirectory(srcItem, dstItem))
                success = false;
        } else {
            if (QFile::exists(dstItem))
                QFile::remove(dstItem);
            if (!QFile::copy(srcItem, dstItem))
                success = false;
        }
    }
    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Лог
// ─────────────────────────────────────────────────────────────────────────────
void UpdaterWindow::log(const QString &level, const QString &msg)
{
    const QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                         + " [" + level + "] " + msg;
    qDebug().noquote() << line;
    if (m_logStream) {
        *m_logStream << line << "\n";
        m_logStream->flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Рекурсивное перемещение директории (файл за файлом)
// Работает даже если папка открыта в Проводнике Windows — Explorer держит
// хэндл на саму директорию, но не блокирует перемещение отдельных файлов.
// ─────────────────────────────────────────────────────────────────────────────
bool UpdaterWindow::moveDirectory(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    if (!srcDir.exists()) return false;

    QDir dstDir;
    if (!dstDir.mkpath(dstPath)) return false;

    bool success = true;
    for (const QString &entry : srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString srcItem = srcPath + "/" + entry;
        const QString dstItem = dstPath + "/" + entry;

        if (QFileInfo(srcItem).isDir()) {
            if (!moveDirectory(srcItem, dstItem))
                success = false;
        } else {
            if (QFile::exists(dstItem)) QFile::remove(dstItem);
            // Убеждаемся что родительская папка назначения существует
            QDir().mkpath(QFileInfo(dstItem).absolutePath());
            bool fileMoved = false;
#ifdef Q_OS_WIN
            fileMoved = MoveFileExW(srcItem.toStdWString().c_str(),
                                    dstItem.toStdWString().c_str(),
                                    MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING);
            if (!fileMoved) {
                const DWORD err = GetLastError();
                logWarn(QString("MoveFileEx failed for '%1', WinErr=%2, trying copy+delete").arg(entry).arg(err));
                // Fallback: copy + delete (работает даже если файл открыт на чтение,
                // но не освободит хэндл — удаление упадёт, тогда хотя бы скопируем)
                if (QFile::copy(srcItem, dstItem)) {
                    if (!QFile::remove(srcItem))
                        logWarn(QString("Cannot remove source '%1' after copy (handle held), will clean up later").arg(entry));
                    fileMoved = true; // dst скопирован — это главное
                } else {
                    logError(QString("copy+delete also failed for '%1'").arg(entry));
                }
            }
#else
            fileMoved = QFile::rename(srcItem, dstItem);
            if (!fileMoved) {
                logWarn(QString("rename failed for '%1', trying copy+delete").arg(entry));
                if (QFile::copy(srcItem, dstItem)) {
                    QFile::remove(srcItem);
                    fileMoved = true;
                } else {
                    logError(QString("copy+delete also failed for '%1'").arg(entry));
                }
            }
#endif
            if (!fileMoved) success = false;
        }
    }

    // Удаляем исходную директорию (должна быть пустой после переноса файлов).
    // Если Explorer или AV держит хэндл на директорию — removeRecursively вернёт
    // false, но это не критично: папка уже пуста и будет убрана при removeRecursively
    // на m_oldDir в launchNew().
    srcDir.removeRecursively();

    return success;
}

// ─────────────────────────────────────────────────────────────────────────────
// Старт обновления
// ─────────────────────────────────────────────────────────────────────────────
void UpdaterWindow::startUpdate()
{
    show();
    QApplication::processEvents();

    setStatus("Проверка архива...");
    setProgress(5);
    QApplication::processEvents();

    if (!m_expectedHash.isEmpty() && !verifyHash()) {
        fatal("Контрольная сумма архива не совпадает.\n"
              "Файл повреждён — обновление отменено.\n\n"
              "MediaStation не был изменён.");
        return;
    }
    setProgress(10);

    if (m_waitPid > 0) {
        setStatus(QString("Ожидание завершения MediaStation (PID %1)...").arg(m_waitPid));
        m_pidWaitCount = 0;
        m_pidTimer = new QTimer(this);
        m_pidTimer->setInterval(300);
        connect(m_pidTimer, &QTimer::timeout, this, &UpdaterWindow::tick);
        m_pidTimer->start();
    } else {
        setStatus("Распаковка архива...");
        setProgress(15);
        QApplication::processEvents();
        prepareNewDir();
        startExtract();
    }
}

void UpdaterWindow::prepareNewDir()
{
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QDir baseDir(QFileInfo(m_targetDir).absolutePath());
    m_newDir = baseDir.absoluteFilePath("MediaStation_New_" + ts);
    QDir().mkpath(m_newDir);
}

void UpdaterWindow::tick()
{
    ++m_pidWaitCount;
    if (m_pidWaitCount > 100) {
        m_pidTimer->stop();
        m_pidTimer->deleteLater();
        m_pidTimer = nullptr;

        recoverableError(
            "MediaStation не завершился за 30 секунд.",
            "Закройте MediaStation вручную и нажмите «Попробовать снова».",
            [this]() {
                dismissRetry();
                m_pidWaitCount = 0;
                m_pidTimer = new QTimer(this);
                m_pidTimer->setInterval(300);
                connect(m_pidTimer, &QTimer::timeout, this, &UpdaterWindow::tick);
                m_pidTimer->start();
                setStatus(QString("Повторное ожидание завершения MediaStation (PID %1)...").arg(m_waitPid));
            });
        return;
    }

    bool running = false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(m_waitPid));
    if (h) {
        running = (WaitForSingleObject(h, 0) == WAIT_TIMEOUT);
        CloseHandle(h);
    }
#endif

    if (!running) {
        m_pidTimer->stop();
        m_pidTimer->deleteLater();
        m_pidTimer = nullptr;

        // Пауза после завершения процесса: Windows Defender и другие антивирусы
        // продолжают держать хэндл на .exe ещё 1-3 секунды после выхода процесса.
        // Без паузы MoveFileEx немедленно получает ERROR_ACCESS_DENIED (WinErr=5).
        setStatus("MediaStation завершён, ожидание освобождения файлов...");
        QApplication::processEvents();
        QThread::msleep(3000);

        setStatus("Распаковка архива...");
        setProgress(15);
        QApplication::processEvents();

        prepareNewDir();
        startExtract();
    }
}

void UpdaterWindow::startExtract()
{
    dismissRetry();

    // ★★★ ПРИОРИТЕТНЫЙ ПОИСК 7zr.exe ★★★
    // Ищем сначала в папке, где лежит сам Updater (временная папка),
    // чтобы не обращаться к целевой MediaStation.
    const QString appDir = QCoreApplication::applicationDirPath();
    QString sevenZ = appDir + "/tools/7zr.exe";
    if (!QFile::exists(sevenZ))
        sevenZ = appDir + "/7zr.exe";
    if (!QFile::exists(sevenZ))
        sevenZ = m_targetDir + "/tools/7zr.exe";   // fallback
    if (!QFile::exists(sevenZ))
        sevenZ = "7zr";

    logInfo("Using 7zr: " + sevenZ);

    m_extractProc = new QProcess(this);
    connect(m_extractProc, &QProcess::readyReadStandardOutput, this, &UpdaterWindow::extractProgress);
    connect(m_extractProc, &QProcess::readyReadStandardError,  this, &UpdaterWindow::extractProgress);
    connect(m_extractProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdaterWindow::onExtractFinished);

    m_extractProc->start(sevenZ, {"x", m_archivePath, "-o" + m_newDir, "-y", "-bsp1"});
    if (!m_extractProc->waitForStarted(5000)) {
        fatal("Не удалось запустить 7zr.exe.\n"
              "Убедитесь, что он находится в папке tools рядом с MediaStation.");
    }
}

void UpdaterWindow::onExtractFinished(int exitCode, QProcess::ExitStatus)
{
    if (m_extractProc) {
        m_extractProc->deleteLater();
        m_extractProc = nullptr;
    }
    if (exitCode != 0) {
        recoverableError(
            QString("Ошибка распаковки архива (код %1).").arg(exitCode),
            "Убедитесь, что на диске достаточно места, и нажмите «Попробовать снова».",
            [this]() {
                QDir(m_newDir).removeRecursively();
                setStatus("Распаковка архива...");
                setProgress(15);
                QApplication::processEvents();
                prepareNewDir();
                startExtract();
            });
        return;
    }
    setProgress(80);
    setStatus("Замена файлов...");
    QApplication::processEvents();
    doReplace();
}

void UpdaterWindow::extractProgress()
{
    if (!m_extractProc) return;
    const QByteArray out = m_extractProc->readAllStandardOutput()
                           + m_extractProc->readAllStandardError();
    static QRegularExpression re("\\s*(\\d+)%");
    int last = -1;
    for (const QString &line : QString::fromLocal8Bit(out).split('\n', Qt::SkipEmptyParts)) {
        QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch()) last = m.captured(1).toInt();
    }
    if (last >= 0)
        setProgress(15 + last * 60 / 100);
}

// ─────────────────────────────────────────────────────────────────────────────
// doReplace с переименованием папок и минорными обновлениями
// ─────────────────────────────────────────────────────────────────────────────
void UpdaterWindow::doReplace()
{
    dismissRetry();

    // ★★★ НЕБОЛЬШАЯ ПАУЗА ПЕРЕД ПЕРЕИМЕНОВАНИЕМ ★★★
    QThread::msleep(500);

    // m_oldDir генерируется один раз при первом вызове.
    // При повторных вызовах (retry) используем тот же путь — иначе каждый
    // раз создаётся новая папка и предыдущая попытка пофайлового переноса
    // может оставить targetDir в частично перемещённом состоянии.
    if (m_oldDir.isEmpty()) {
        const QString ts   = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QDir baseDir = QFileInfo(m_targetDir).absoluteDir();
        m_oldDir           = baseDir.absoluteFilePath("MediaStation_Old_" + ts);
    }

    logInfo("doReplace: targetDir=" + m_targetDir + "  oldDir=" + m_oldDir);

    // Быстрый атомарный rename — работает когда папка не занята
    auto tryAtomicRename = [this](const QString &from, const QString &to) -> bool {
        if (QDir().rename(from, to)) return true;
#ifdef Q_OS_WIN
        if (MoveFileExW(from.toStdWString().c_str(),
                        to.toStdWString().c_str(),
                        MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING))
            return true;
        logWarn(QString("Atomic rename failed (%1 -> %2), WinErr=%3")
                    .arg(from, to).arg(GetLastError()));
#endif
        return false;
    };

    // ─── Шаг 1: убрать старую папку ───────────────────────────────────────────
    // Сначала пробуем атомарный rename — мгновенно и без копирования данных.
    // Если не прошёл (AV/Defender держит хэндл на .exe после завершения процесса)
    // — сразу переходим к пофайловому перемещению, которое работает надёжно.
    bool renamedOld = tryAtomicRename(m_targetDir, m_oldDir);

    if (!renamedOld) {
        // Атомарный rename не прошёл — пробуем пофайловое перемещение.
        // moveDirectory копирует все файлы в m_oldDir; файлы занятые AV/Defender
        // будут скопированы (dst создан), но src может не удалиться — это нормально,
        // мы продолжаем и в конце пробуем rename снова на опустевшую папку.
        logWarn("Atomic rename failed after 10 attempts, trying file-by-file move to oldDir.");
        setStatus("Перемещение файлов из старой папки (обход блокировки Explorer)...");
        QApplication::processEvents();

        QDir().mkpath(m_oldDir);
        const bool moveOk = moveDirectory(m_targetDir, m_oldDir);
        if (moveOk) {
            logInfo("File-by-file move to oldDir succeeded.");
            renamedOld = true;
        } else {
            // Частичное перемещение: часть файлов не удалилась из src (занятые AV).
            // Пробуем ещё раз атомарный rename — если занятые файлы уже скопированы
            // в dst, AV мог отпустить src к этому моменту.
            logWarn("File-by-file move partially failed, retrying atomic rename after move...");
            QThread::msleep(1500);
            if (tryAtomicRename(m_targetDir, m_oldDir)) {
                logInfo("Atomic rename succeeded after partial file-by-file move.");
                renamedOld = true;
            } else {
                logError("File-by-file move to oldDir failed.");
            }
        }
    }

    if (!renamedOld) {
        QString hint;
#ifdef Q_OS_WIN
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION)
            hint = "Один или несколько файлов MediaStation заняты другим процессом.\n"
                   "Закройте все окна Проводника с папкой MediaStation и другие программы.\n"
                   "Затем нажмите «Попробовать снова».";
        else if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND)
            hint = "Папка MediaStation не найдена по пути:\n" + m_targetDir;
        else
            hint = QString("Код ошибки Windows: %1.\n"
                           "Попробуйте запустить обновление ещё раз.").arg(err);
        logError(QString("Rename/move failed, WinErr=%1").arg(err));
#else
        hint = "Проверьте права доступа к папке и попробуйте снова.";
#endif
        recoverableError("Не удалось переместить текущую папку MediaStation.", hint,
                         [this]() {
                             // Убираем частично заполненную oldDir и сбрасываем путь,
                             // чтобы на следующей попытке создалась чистая папка.
                             if (!m_oldDir.isEmpty()) {
                                 QDir(m_oldDir).removeRecursively();
                                 m_oldDir.clear();
                             }
                             doReplace();
                         });
        return;
    }

    // ─── Шаг 2: переместить новую папку на место старой ──────────────────────
    bool renamedNew = tryAtomicRename(m_newDir, m_targetDir);

    if (!renamedNew) {
        // Атомарный rename newDir→targetDir не прошёл: возможно targetDir всё ещё
        // существует (пустая, с занятым файлом). Пробуем пофайловый перенос.
        logWarn("Atomic rename newDir->targetDir failed after 5 attempts, trying file-by-file.");
        setStatus("Установка новой версии (пофайловый перенос)...");
        QApplication::processEvents();
        QDir().mkpath(m_targetDir);
        renamedNew = moveDirectory(m_newDir, m_targetDir);
        if (renamedNew)
            logInfo("File-by-file install to targetDir succeeded.");
        else
            logError("File-by-file install to targetDir also failed.");
    }

    if (!renamedNew) {
        logError("Failed to install new version, attempting restore.");
        const bool restored = tryAtomicRename(m_oldDir, m_targetDir)
                              || moveDirectory(m_oldDir, m_targetDir);
        QDir(m_newDir).removeRecursively();

        if (restored) {
            logInfo("Old version restored successfully.");
            recoverableError(
                "Не удалось установить новую версию. Предыдущая версия восстановлена.",
                "Проверьте права доступа к папке и нажмите «Попробовать снова».\n"
                "Распаковывать архив заново не нужно.",
                [this]() {
                    setStatus("Распаковка архива...");
                    setProgress(15);
                    QApplication::processEvents();
                    prepareNewDir();
                    startExtract();
                });
        } else {
            logError("CRITICAL: could not install new version and could not restore old.");
            fatal("Критическая ошибка: не удалось установить новую версию\n"
                  "и не удалось восстановить старую.\n\n"
                  "Путь установки: " + m_targetDir + "\n"
                  "Старая версия сохранена в: " + m_oldDir);
        }
        return;
    }

    logInfo("Folder swap complete. oldDir=" + m_oldDir);

    // ─── Перенос папки data из старой версии ──────────────────────────────────
    // data/ содержит пользовательские настройки, базы данных и прочие данные.
    // Всегда переносится целиком из старой версии: содержимое архива (если есть)
    // заменяется данными пользователя, чтобы ничего не потерять.
    {
        const QString oldDataPath = m_oldDir    + "/data";
        const QString newDataPath = m_targetDir + "/data";

        if (QDir(oldDataPath).exists()) {
            setStatus("Перенос пользовательских данных (папка data)...");
            QApplication::processEvents();
            logInfo("Migrating data folder: " + oldDataPath + " -> " + newDataPath);

            // Если архив уже распаковал заготовку data/ — сносим её,
            // чтобы данные пользователя не смешивались с шаблонными файлами.
            if (QDir(newDataPath).exists()) {
                logInfo("New data folder exists (from archive), removing it before migration.");
                if (!QDir(newDataPath).removeRecursively())
                    logWarn("Could not fully remove new data folder before migration.");
            }

            // Сначала пробуем атомарный rename (мгновенно, без копирования)
            bool dataMoved = QDir().rename(oldDataPath, newDataPath);
#ifdef Q_OS_WIN
            if (!dataMoved)
                dataMoved = MoveFileExW(oldDataPath.toStdWString().c_str(),
                                        newDataPath.toStdWString().c_str(),
                                        MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING);
#endif
            if (dataMoved) {
                logInfo("Data folder moved (atomic rename).");
                setStatus("Пользовательские данные перенесены.");
            } else {
                // Fallback: пофайловое перемещение (работает между томами и при
                // частичной блокировке папки Проводником)
                logWarn("Atomic rename of data failed, falling back to file-by-file move.");
                setStatus("Перенос данных (пофайловое копирование)...");
                QApplication::processEvents();
                if (moveDirectory(oldDataPath, newDataPath)) {
                    logInfo("Data folder moved (file-by-file).");
                    setStatus("Пользовательские данные перенесены.");
                } else {
                    // Последний шанс — копирование без удаления источника
                    logWarn("File-by-file move of data failed, trying copy.");
                    setStatus("Копирование данных (резервный метод)...");
                    QApplication::processEvents();
                    if (copyDirectory(oldDataPath, newDataPath)) {
                        logInfo("Data folder copied (fallback copy).");
                        setStatus("Пользовательские данные скопированы.");
                    } else {
                        logError("All methods failed to migrate data folder!");
                        setStatus("⚠ Не удалось перенести папку data — проверьте её вручную.");
                    }
                }
            }
        } else {
            logInfo("No data folder in old version, skipping data migration.");
        }
        QApplication::processEvents();
    }

    const QString newToolsPath = m_targetDir + "/tools";
    const QString oldToolsPath = m_oldDir    + "/tools";

    if (!QDir(newToolsPath).exists()) {
        // ── Минорное обновление: tools в архиве нет — перемещаем из старой версии ──
        setStatus("Минорное обновление: перемещение папки tools из старой версии...");
        QApplication::processEvents();

        if (QDir(oldToolsPath).exists()) {
            if (QDir().rename(oldToolsPath, newToolsPath)) {
                logInfo("Tools folder successfully moved (minor update).");
                setStatus("Папка tools перемещена.");
            } else {
                // rename может не работать между томами — fallback на копирование
                logWarn("Tools move failed, falling back to copy.");
                setStatus("Перемещение не удалось, копирование папки tools...");
                QApplication::processEvents();
                if (copyDirectory(oldToolsPath, newToolsPath)) {
                    logInfo("Tools folder successfully copied (fallback).");
                    setStatus("Папка tools скопирована.");
                } else {
                    logError("Failed to copy tools from: " + oldToolsPath);
                    setStatus("Предупреждение: не удалось скопировать папку tools.");
                }
            }
        } else {
            logWarn("No tools folder in old version: " + oldToolsPath);
            setStatus("Предупреждение: папка tools отсутствует и в старой версии.");
        }
        QApplication::processEvents();
    } else {
        // ── Мажорное обновление: tools уже в архиве — переносим только AVD-настройки ──
        const QString oldAvdPath = oldToolsPath + "/android-tv/avd";
        const QString newAvdPath = newToolsPath  + "/android-tv/avd";

        if (QDir(oldAvdPath).exists()) {
            QDir().mkpath(newAvdPath);
            setStatus("Мажорное обновление: перенос пользовательских настроек AndroidTV...");
            QApplication::processEvents();

            // Копируем все AVD (у каждого профиля Android TV свой <name>.ini +
            // <name>.avd/) и ключи adb, не трогая системные образы. Раньше
            // переносился только MediaStation_TV — остальные профили теряли данные.
            QStringList avdItems = QDir(oldAvdPath).entryList(
                {"*.ini", "*.avd", "adbkey", "adbkey.pub"},
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            bool anyOk = false;
            for (const QString &item : avdItems) {
                const QString src = oldAvdPath + "/" + item;
                const QString dst = newAvdPath + "/" + item;
                const QFileInfo fi(src);
                if (!fi.exists()) continue;

                // Удаляем заготовку из нового дистрибутива перед заменой
                if (fi.isDir()) {
                    QDir(dst).removeRecursively();
                    if (copyDirectory(src, dst)) {
                        logInfo("AVD dir copied: " + item);
                        anyOk = true;
                    } else {
                        logError("Failed to copy AVD dir: " + item);
                    }
                } else {
                    QFile::remove(dst);
                    if (QFile::copy(src, dst)) {
                        logInfo("AVD file copied: " + item);
                        anyOk = true;
                    } else {
                        logError("Failed to copy AVD file: " + item);
                    }
                }
            }

            if (anyOk)
                setStatus("Настройки AndroidTV перенесены.");
            else
                setStatus("Предупреждение: не удалось перенести настройки AndroidTV.");

            QApplication::processEvents();
        } else {
            logInfo("AVD folder not found in old or new version, skipping AVD migration.");
        }
    }

    setProgress(90);
    setStatus("Запуск обновлённого MediaStation...");
    QApplication::processEvents();
    launchNew();
}

void UpdaterWindow::launchNew()
{
    const QString newExe = m_targetDir + "/MediaStation.exe";
    if (QFile::exists(newExe)) {
        logInfo("Launching: " + newExe);
        QProcess::startDetached(newExe, {});
    } else {
        logWarn("MediaStation.exe not found at: " + newExe);
    }

    setProgress(100);
    setStatus("Готово! Очистка временных файлов...");
    QApplication::processEvents();

    // Закрываем лог до удаления папок, затем копируем его в targetDir/logs/
    logInfo("=== Update finished successfully ===");
    if (m_logStream) { m_logStream->flush(); delete m_logStream; m_logStream = nullptr; }
    if (m_logFile) {
        const QString srcLog = m_logFile->fileName();
        m_logFile->close();
        delete m_logFile; m_logFile = nullptr;

        // Копируем лог в targetDir/logs/ — теперь targetDir уже новая версия
        const QString dstLogsDir = m_targetDir + "/logs";
        QDir().mkpath(dstLogsDir);
        QFile::copy(srcLog, dstLogsDir + "/" + m_logFileName);
    }

    QThread *t = QThread::create([this]() {
        QFile::remove(m_archivePath);
        if (!m_oldDir.isEmpty())
            QDir(m_oldDir).removeRecursively();
        if (!m_tempDir.isEmpty() && QDir(m_tempDir).exists()) {
            QDir(m_tempDir).removeRecursively();
            qDebug() << "[Updater] Removed temp dir:" << m_tempDir; // log уже закрыт
        }
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    connect(t, &QThread::finished, this, &QWidget::close);
    t->start();
}

bool UpdaterWindow::verifyHash() const
{
    QFile f(m_archivePath);
    if (!f.open(QIODevice::ReadOnly)) {
        const_cast<UpdaterWindow*>(this)->logError("verifyHash: cannot open archive: " + m_archivePath);
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) h.addData(f.read(4 * 1024 * 1024));
    const bool ok = QString::fromLatin1(h.result().toHex())
                        .compare(m_expectedHash, Qt::CaseInsensitive) == 0;
    const_cast<UpdaterWindow*>(this)->logInfo(
        QString("verifyHash: %1").arg(ok ? "OK" : "MISMATCH"));
    return ok;
}

void UpdaterWindow::fatal(const QString &msg)
{
    logError("FATAL: " + msg);
    if (m_logStream) { m_logStream->flush(); delete m_logStream; m_logStream = nullptr; }
    if (m_logFile) {
        const QString srcLog = m_logFile->fileName();
        m_logFile->close();
        delete m_logFile; m_logFile = nullptr;
        // Пробуем скопировать лог в targetDir/logs/ если папка уже существует
        const QString dstLogsDir = m_targetDir + "/logs";
        if (QDir(dstLogsDir).exists() || QDir().mkpath(dstLogsDir))
            QFile::copy(srcLog, dstLogsDir + "/" + m_logFileName);
    }
    QFile::remove(m_archivePath);
    setStatus("Ошибка обновления. MediaStation не был изменён.");
    progressBar->setValue(0);
    QMessageBox::critical(this, "Ошибка обновления", msg);
    QApplication::quit();
}

void UpdaterWindow::recoverableError(const QString &msg,
                                     const QString &hint,
                                     std::function<void()> retryStep)
{
    m_retryStep = retryStep;

    logWarn("Recoverable error: " + msg + (hint.isEmpty() ? "" : " | hint: " + hint));
    setStatus("⚠  " + msg);

    if (!hint.isEmpty()) {
        hintLabel->setText(hint);
        hintLabel->setVisible(true);
    }

    retryBtn->setVisible(true);
    cancelBtn->setVisible(true);

    adjustSize();
    setFixedHeight(qMax(height(), 220));

    QApplication::processEvents();
}

void UpdaterWindow::onRetryClicked()
{
    if (m_retryStep) {
        auto step = m_retryStep;
        m_retryStep = nullptr;
        step();
    }
}

void UpdaterWindow::dismissRetry()
{
    hintLabel->setVisible(false);
    retryBtn->setVisible(false);
    cancelBtn->setVisible(false);
    m_retryStep = nullptr;
}

void UpdaterWindow::setStatus(const QString &msg)
{
    statusLabel->setText(msg);
    logInfo(msg);
}

void UpdaterWindow::setProgress(int pct)
{
    progressBar->setValue(pct);
    QApplication::processEvents();
}

void UpdaterWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        m_dragPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
}

void UpdaterWindow::mouseMoveEvent(QMouseEvent *e)
{
    if (e->buttons() & Qt::LeftButton)
        move(e->globalPosition().toPoint() - m_dragPos);
}
