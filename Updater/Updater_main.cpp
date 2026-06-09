// Updater_main.cpp
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include "UpdaterWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MediaStation Updater");

    QCommandLineParser parser;
    parser.addOption({"archive",  "Path to update archive.", "archive"});
    parser.addOption({"target",   "Path to MediaStation installation folder.", "target"});
    parser.addOption({"hash",     "Expected SHA-256 of the archive.", "hash"});
    parser.addOption({"wait-pid", "PID of MediaStation to wait for.", "wait-pid"});
    parser.addOption({"version",  "New version string.", "version"});
    parser.addOption({"temp-dir", "Temporary directory to clean up after update.", "temp-dir"});
    parser.process(app);

    const QString archive = parser.value("archive");
    const QString target  = parser.value("target");
    const QString hash    = parser.value("hash");
    const qint64  pid     = parser.value("wait-pid").toLongLong();
    const QString version = parser.value("version");
    const QString tempDir = parser.value("temp-dir");

    if (archive.isEmpty() || target.isEmpty()) {
        QMessageBox::critical(nullptr, "Updater",
                              "Неверные аргументы запуска.\n"
                              "Это приложение запускается автоматически из MediaStation.");
        return 1;
    }

    UpdaterWindow w(archive, target, hash, pid, version, tempDir);
    w.startUpdate();
    return app.exec();
}
