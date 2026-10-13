// modules/AndroidTVModule/AndroidTVLifecycle.cpp
//
// Запуск / остановка эмулятора, подготовка AVD, взаимодействие с adb.

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QTcpServer>
#include <QTextStream>
#include <QTimeZone>
#include <QTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDebug>
#include <QRegularExpression>
#include <memory>

// ── Paths ────────────────────────────────────────────────────────────────────

QString AndroidTVModule::adbPath() const
{
    return m_appDir + "/tools/android-sdk/platform-tools/adb.exe";
}

QString AndroidTVModule::emulatorPath() const
{
    return m_appDir + "/tools/android-sdk/emulator/emulator.exe";
}

QString AndroidTVModule::dnsProxyPath() const
{
    return m_appDir + "/tools/dnsproxy/dnsproxy.exe";
}

QString AndroidTVModule::avdHomePath() const
{
    return m_avdHome.isEmpty() ? resolveAvdHome() : m_avdHome;
}

QString AndroidTVModule::avdDirPath(const QString &avdName) const
{
    return avdHomePath() + "/" + avdName + ".avd";
}

QString AndroidTVModule::blocklistPath() const
{
    return m_appDir + "/tools/android-tv/blocklists/default.txt";
}

QString AndroidTVModule::jdkBinPath() const
{
    return m_appDir + "/tools/jdk/bin";
}

// Основное расположение AVD — tools/android-tv/avd. Старый setup_android_sdk.bat
// создавал AVD в <deploy>/android-tv/avd, поэтому, если в основном месте пусто,
// а в старом что-то есть, используем старое (иначе эмулятор стартует с
// «пустым» AVD без config.ini и падает).
QString AndroidTVModule::resolveAvdHome() const
{
    const QString primary = m_appDir + "/tools/android-tv/avd";
    const QString legacy  = m_appDir + "/android-tv/avd";

    auto hasAvd = [](const QString &dir) {
        QDir d(dir);
        return d.exists() && !d.entryList({"*.avd"}, QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    };

    if (hasAvd(primary)) return primary;
    if (hasAvd(legacy))  return legacy;
    return primary;
}

// Окружение для adb / emulator: портативные SDK и JDK из папки tools.
QProcessEnvironment AndroidTVModule::toolsEnvironment() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString sdkRoot = QDir::toNativeSeparators(m_appDir + "/tools/android-sdk");
    const QString avdHome = QDir::toNativeSeparators(avdHomePath());

    env.insert("JAVA_HOME",             QDir::toNativeSeparators(m_appDir + "/tools/jdk"));
    env.insert("ANDROID_SDK_ROOT",      sdkRoot);
    env.insert("ANDROID_HOME",          sdkRoot);
    env.insert("ANDROID_AVD_HOME",      avdHome);
    env.insert("ANDROID_EMULATOR_HOME", avdHome);
    env.insert("SKIP_JDK_VERSION_CHECK", "true");
    env.insert("PATH",
               QDir::toNativeSeparators(m_appDir + "/tools/android-sdk/platform-tools")
                   + QDir::listSeparator() + QDir::toNativeSeparators(jdkBinPath())
                   + QDir::listSeparator() + env.value("PATH"));
    return env;
}

// Консольный порт эмулятора: 5554, 5556, … Порт+1 — adb. Берём первую свободную
// пару, чтобы не конфликтовать с другим запущенным эмулятором, и всегда
// обращаемся к своему экземпляру через `adb -s emulator-<port>`.
int AndroidTVModule::pickFreeConsolePort() const
{
    for (int port = 5554; port <= 5584; port += 2) {
        QTcpServer a, b;
        const bool freeA = a.listen(QHostAddress::LocalHost, static_cast<quint16>(port));
        const bool freeB = b.listen(QHostAddress::LocalHost, static_cast<quint16>(port + 1));
        a.close(); b.close();
        if (freeA && freeB) return port;
    }
    return 5554;
}

void AndroidTVModule::loadEmulatorSettings(const AndroidTVProfile &profile)
{
    m_resolution = profile.resolution.trimmed();

    // "auto" или пусто → текущее разрешение экрана
    if (m_resolution.isEmpty() || m_resolution == QLatin1String("auto")) {
        QSize screen(0, 0);
#ifdef Q_OS_WIN
        screen = getCurrentScreenResolution();
#endif
        if (screen.width() > 0 && screen.height() > 0) {
            m_resolution = QString("%1x%2").arg(screen.width()).arg(screen.height());
            logToEmulatorFile(QString("Resolution '%1' → detected screen resolution %2")
                                  .arg(profile.resolution, m_resolution));
        } else {
            m_resolution = "1920x1080";
            logImportant(Logger::kLevelWarning,
                         "Could not detect screen resolution, using fallback 1920x1080");
        }
    }

    // Валидация WxH
    static const QRegularExpression kResRe(QStringLiteral("^(\\d{3,5})x(\\d{3,5})$"));
    if (!kResRe.match(m_resolution).hasMatch()) {
        logImportant(Logger::kLevelWarning,
                     QString("Invalid resolution '%1', using 1920x1080").arg(m_resolution));
        m_resolution = "1920x1080";
    }

    m_gpu            = profile.gpu.trimmed().isEmpty() ? QStringLiteral("auto") : profile.gpu.trimmed();
    m_ramMb          = qBound(512, profile.ramMb, 16384);
    m_cores          = qBound(1, profile.cores, 16);
    m_writableSystem = profile.writableSystem;
    m_noBootAnim     = profile.noBootAnim;
    m_noSnapshot     = profile.noSnapshot;
    m_noSnapshotSave = profile.noSnapshotSave;
    m_dnsServer      = profile.dnsServer;
    m_audioDriver    = profile.audioDriver;

    static const QStringList kValidAudioDrivers = {"dsound", "winaudio"};
    if (!kValidAudioDrivers.contains(m_audioDriver)) {
        logImportant(Logger::kLevelWarning,
                     QString("Audio driver '%1' not supported, falling back to 'dsound'").arg(m_audioDriver));
        m_audioDriver = QStringLiteral("dsound");
    }

    logImportant(Logger::kLevelInfo,
                 QString("Emulator settings: res=%1, gpu=%2, ram=%3, cores=%4, audio=%5")
                     .arg(m_resolution, m_gpu).arg(m_ramMb).arg(m_cores).arg(m_audioDriver));
}

// ── Launch / stop ────────────────────────────────────────────────────────────

void AndroidTVModule::launch()
{
    if (m_booting) {
        logImportant(Logger::kLevelInfo, "launch() ignored (already booting)");
        return;
    }
    if (m_running && m_emulatorProcess && m_emulatorProcess->state() == QProcess::Running) {
        // Эмулятор уже загружен и работает за MediaStation — просто показываем его.
        emit emulatorReadyBehind();
        return;
    }

    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) {
        logImportant(Logger::kLevelError, "No active profile, cannot launch");
        setStatus("Error: no active profile");
        return;
    }

    if (!QFile::exists(emulatorPath())) {
        logImportant(Logger::kLevelError, "emulator.exe not found: " + emulatorPath()
                     + " — run external_tools/setup_android_sdk.bat");
        setStatus("Error: emulator not found");
        return;
    }
    if (!QFile::exists(adbPath())) {
        logImportant(Logger::kLevelError, "adb.exe not found: " + adbPath());
        setStatus("Error: adb not found");
        return;
    }

    openEmulatorLogFile();
    logImportant(Logger::kLevelInfo, "launching Android TV emulator with profile " + profile->name);
    refreshQcow2Size();

    m_exitRequested = false;
    m_launchAvdName = profile->avdName;

    // Сразу сигнализируем UI — анимация запуска стартует мгновенно.
    setBooting(true);
    setStatus("Preparing ADB...");
    startKeepTopmostTimer();

    loadEmulatorSettings(*profile);
    startDnsProxy();

    // Эмулятор стартует ПОСЛЕ перезапуска adb-сервера, иначе первый запуск
    // заметно лагает (adb переподключается к уже работающему эмулятору).
    syncAdbKeyAndRestartServer(profile->avdName, [this]() {
        if (!m_booting || m_shutdownDone) {
            logToEmulatorFile("launch cancelled before emulator start");
            return;
        }
        startEmulator();
    });
}

void AndroidTVModule::stop()
{
    if (!m_running && !m_booting) {
        m_exitRequested = false;
        return;
    }
    setStatus("Stopping emulator...");
    logImportant(Logger::kLevelInfo, "stop() requested");

    // Эмулятор ещё не успел стартовать (идёт перезапуск adb) — просто отменяем запуск.
    if (!m_emulatorProcess || m_emulatorProcess->state() == QProcess::NotRunning) {
        stopKeepTopmostTimer();
        stopDnsProxy();
        setBooting(false);
        setRunning(false);
        setStatus("");
        restoreMediaStation();
        m_exitRequested = false;
        return;
    }

    restoreMediaStation();
    stopKeyShell();
    stopLogcat();
    adb({"emu", "kill"}, {}, 5000);

    QTimer::singleShot(6000, this, [this]() {
        if (m_emulatorProcess && m_emulatorProcess->state() != QProcess::NotRunning) {
            logImportant(Logger::kLevelWarning, "emulator didn't respond to 'emu kill', terminating process tree");
            killEmulatorTree();
        }
        m_exitRequested = false;
    });
}

// Ошибка до/во время старта: возвращаем UI в исходное состояние.
void AndroidTVModule::failLaunch(const QString &userStatus, const QString &logMessage)
{
    logImportant(Logger::kLevelError, logMessage);
    stopKeepTopmostTimer();
    stopBootPolling();
    stopWindowPolling();
    removeWinEventHook();
    stopDnsProxy();
    setBooting(false);
    setRunning(false);
    setStatus(userStatus);
    restoreMediaStation();
    m_exitRequested = false;
    if (m_bus)
        emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("launchFailed"),
                                QVariantMap{{"reason", userStatus}});
}

// ── DNS proxy ────────────────────────────────────────────────────────────────
// ВНИМАНИЕ: эмулятор получает DNS через -dns-server (порт всегда 53), поэтому
// dnsproxy на порту 5300 эмулятором не используется. Оставлен как задел; если
// он не установлен — просто пропускается.

void AndroidTVModule::startDnsProxy()
{
    if (!QFile::exists(dnsProxyPath())) {
        logToEmulatorFile("dnsproxy.exe not found, skipping");
        m_dnsProxyPort = 0;
        return;
    }
    if (m_dnsProxyProcess && m_dnsProxyProcess->state() != QProcess::NotRunning) {
        logToEmulatorFile("DNS proxy already running");
        return;
    }
    if (m_dnsProxyProcess) {
        m_dnsProxyProcess->deleteLater();
        m_dnsProxyProcess = nullptr;
    }

    m_dnsProxyPort = 5300;

    QStringList upstreams;
    for (const QString &s : m_dnsServer.split(',', Qt::SkipEmptyParts))
        if (!s.trimmed().isEmpty()) upstreams << s.trimmed();
    if (!upstreams.contains("8.8.8.8")) upstreams << "8.8.8.8";
    if (!upstreams.contains("1.1.1.1")) upstreams << "1.1.1.1";

    QStringList args;
    args << "-l" << "127.0.0.1" << "-p" << QString::number(m_dnsProxyPort);
    for (const QString &u : upstreams) args << "-u" << u;
    args << "--cache";

    logToEmulatorFile(QString("dnsproxy config: port %1, upstreams: %2")
                          .arg(m_dnsProxyPort).arg(upstreams.join(", ")));

    m_dnsProxyProcess = new QProcess(this);
    m_dnsProxyProcess->setProgram(dnsProxyPath());
    m_dnsProxyProcess->setArguments(args);
    m_dnsProxyProcess->setWorkingDirectory(m_appDir + "/tools");
    m_dnsProxyProcess->setProcessChannelMode(QProcess::MergedChannels);

    QProcess *proc = m_dnsProxyProcess;
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) logToEmulatorFile("[dnsproxy] " + out);
    });
    connect(proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (m_dnsProxyPort != 0) {
            logImportant(Logger::kLevelWarning, "DNS proxy exited unexpectedly with code " + QString::number(code));
            m_dnsProxyPort = 0;
        }
    });
    connect(proc, &QProcess::started, this, [this, proc]() {
        logToEmulatorFile("DNS proxy started PID " + QString::number(proc->processId()) +
                          " on port " + QString::number(m_dnsProxyPort));
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            logImportant(Logger::kLevelWarning, "failed to start dnsproxy: " + proc->errorString());
            m_dnsProxyPort = 0;
        }
    });

    // Без waitForStarted(): не блокируем GUI-поток
    proc->start();
}

void AndroidTVModule::stopDnsProxy()
{
    if (m_dnsProxyProcess && m_dnsProxyProcess->state() != QProcess::NotRunning) {
        m_dnsProxyPort = 0;
        m_dnsProxyProcess->kill();
        m_dnsProxyProcess->waitForFinished(2000);
        logToEmulatorFile("DNS proxy stopped");
    }
}

// ── AVD preparation ──────────────────────────────────────────────────────────

namespace {

// Читает key=value файл (source.properties, config.ini, *.ini)
QMap<QString, QString> readProperties(const QString &path)
{
    QMap<QString, QString> props;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return props;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        props.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return props;
}

QString abiToCpuArch(const QString &abi)
{
    if (abi == QLatin1String("x86"))         return QStringLiteral("x86");
    if (abi == QLatin1String("x86_64"))      return QStringLiteral("x86_64");
    if (abi == QLatin1String("arm64-v8a"))   return QStringLiteral("arm64");
    if (abi.startsWith(QLatin1String("armeabi"))) return QStringLiteral("arm");
    return QString();
}

// Плотность экрана как у штатных TV-профилей AVD Manager
QString densityForHeight(int h)
{
    if (h >= 2160) return QStringLiteral("640");
    if (h >= 1080) return QStringLiteral("320");
    return QStringLiteral("213");
}

} // namespace

// Создаёт AVD (папку, <name>.ini, config.ini), если её нет. Раньше при
// отсутствии AVD создавался config.ini из трёх строк без hw.cpu.arch/abi.type —
// эмулятор считал образ ARM-овым и не запускался.
bool AndroidTVModule::ensureAvdExists(const AndroidTVProfile &profile)
{
    const QString avdDir = avdDirPath(profile.avdName);
    if (!QDir(avdDir).exists()) {
        if (!QDir().mkpath(avdDir)) {
            logImportant(Logger::kLevelError, "cannot create AVD directory: " + avdDir);
            return false;
        }
        logImportant(Logger::kLevelInfo, "Created AVD directory " + avdDir);
    }
    return patchAvdIni(profile);
}

bool AndroidTVModule::patchAvdIni(const AndroidTVProfile &profile)
{
    const QString iniPath = avdHomePath() + "/" + profile.avdName + ".ini";
    const QString avdDir  = QDir::toNativeSeparators(avdDirPath(profile.avdName));
    const QString imgDir  = m_profileManager ? m_profileManager->systemImageDir(profile)
                                             : m_appDir + "/" + profile.systemImagePath;
    const QMap<QString, QString> imgProps = readProperties(imgDir + "/source.properties");
    const QString api = imgProps.value("AndroidVersion.ApiLevel");

    QStringList lines;
    bool hasPath = false, hasTarget = false, hasEncoding = false;

    QFile file(iniPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            const QString key  = line.section('=', 0, 0).trimmed();
            if (key == QLatin1String("path")) {
                lines << "path=" + avdDir;
                hasPath = true;
            } else if (key == QLatin1String("path.rel")) {
                continue;   // устаревший относительный путь — абсолютного достаточно
            } else {
                if (key == QLatin1String("target"))            hasTarget = true;
                if (key == QLatin1String("avd.ini.encoding"))  hasEncoding = true;
                lines << line;
            }
        }
        file.close();
    }

    if (!hasEncoding) lines.prepend("avd.ini.encoding=UTF-8");
    if (!hasPath)     lines << "path=" + avdDir;
    if (!hasTarget && !api.isEmpty()) lines << "target=android-" + api;

    QDir().mkpath(avdHomePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logImportant(Logger::kLevelError, "cannot write AVD ini: " + iniPath);
        return false;
    }
    QTextStream out(&file);
    for (const QString &l : lines) out << l << "\n";
    file.close();

    logToEmulatorFile("AVD ini ready: path=" + avdDir);
    return true;
}

void AndroidTVModule::patchAvdConfig(const AndroidTVProfile &profile, const QString &resolutionOverride)
{
    const QString resolution = resolutionOverride.isEmpty() ? profile.resolution : resolutionOverride;

    int w = resolution.section('x', 0, 0).toInt();
    int h = resolution.section('x', 1, 1).toInt();
    if (w <= 0 || h <= 0) {
        w = 1920; h = 1080;
        logToEmulatorFile(QString("WARNING: invalid resolution '%1', using 1920x1080").arg(resolution));
    }

    const QString avdDir  = avdDirPath(profile.avdName);
    const QString cfgPath = avdDir + "/config.ini";
    QDir().mkpath(avdDir);

    // Путь к образу: внутри SDK — относительный (как пишет avdmanager), иначе абсолютный.
    const QString imgDirAbs = m_profileManager ? m_profileManager->systemImageDir(profile)
                                               : m_appDir + "/" + profile.systemImagePath;
    const QString sdkRoot   = m_appDir + "/tools/android-sdk";
    QString imageSysDir;
    {
        const QString rel = QDir(sdkRoot).relativeFilePath(imgDirAbs);
        if (!rel.startsWith(QLatin1String("..")) && !QDir::isAbsolutePath(rel))
            imageSysDir = QDir::toNativeSeparators(rel) + QDir::separator();
        else
            imageSysDir = QDir::toNativeSeparators(imgDirAbs) + QDir::separator();
        imageSysDir = QDir::toNativeSeparators(imageSysDir);
    }

    const QMap<QString, QString> imgProps = readProperties(imgDirAbs + "/source.properties");
    const QString abi     = imgProps.value("SystemImage.Abi", "x86");
    const QString tagId   = imgProps.value("SystemImage.TagId", "android-tv");
    const QString tagDisp = imgProps.value("SystemImage.TagDisplay", "Android TV");
    const QString api     = imgProps.value("AndroidVersion.ApiLevel");
    const QString cpuArch = abiToCpuArch(abi).isEmpty() ? QStringLiteral("x86") : abiToCpuArch(abi);

    // Всегда перезаписываем
    QMap<QString, QString> overrides = {
        { "hw.lcd.width",   QString::number(w) },
        { "hw.lcd.height",  QString::number(h) },
        { "skin.name",      QString("%1x%2").arg(w).arg(h) },
        { "skin.path",      "_no_skin"          },
        { "skin.dynamic",   "yes"               },
        { "showDeviceFrame","no"                },
        { "image.sysdir.1", imageSysDir         },
        { "hw.ramSize",     QString::number(m_ramMb) },
        { "hw.cpu.ncore",   QString::number(m_cores) },
        { "hw.audioInput",  "no"                },
        { "hw.audioOutput", "yes"               },
        { "hw.gpu.enabled", "yes"               },
        { "hw.gpu.mode",    m_gpu.isEmpty() ? QStringLiteral("auto") : m_gpu },
        { "vm.heapSize",    "256"               },
        { "fastboot.forceFastBoot", "yes"       },
        { "hw.lcd.density", densityForHeight(h) },
    };

    // SD-карта: только если образ реально существует, иначе эмулятор ругается.
    const QString sdcardPath = avdDir + "/sdcard.img";
    if (QFile::exists(sdcardPath)) {
        overrides.insert("sdcard.path", QDir::toNativeSeparators(sdcardPath));
        overrides.insert("hw.sdCard", "yes");
    } else {
        overrides.insert("hw.sdCard", "no");
    }

    // Только если ключа нет (новый AVD или неполный config.ini)
    QMap<QString, QString> defaults = {
        { "avd.ini.encoding",     "UTF-8" },
        { "abi.type",             abi },
        { "hw.cpu.arch",          cpuArch },
        { "tag.id",               tagId },
        { "tag.display",          tagDisp },
        { "hw.keyboard",          "yes" },
        { "hw.dPad",              "yes" },
        { "hw.mainKeys",          "yes" },
        { "hw.initialOrientation","landscape" },
        { "hw.camera.back",       "none" },
        { "hw.camera.front",      "none" },
        { "hw.gps",               "no" },
        { "hw.sensors.orientation","no" },
        { "hw.sensors.proximity", "no" },
        { "hw.accelerometer",     "no" },
        { "hw.battery",           "no" },
        { "hw.trackBall",         "no" },
        { "disk.dataPartition.size", "6G" },
        { "PlayStore.enabled",    tagId.contains("google", Qt::CaseInsensitive) ? "yes" : "no" },
        { "hw.device.name",       "tv_1080p" },
        { "hw.device.manufacturer","Google" },
        { "runtime.network.latency", "none" },
        { "runtime.network.speed",   "full" },
    };
    if (!api.isEmpty())
        defaults.insert("image.androidVersion.api", api);

    QStringList lines;
    QSet<QString> written;
    QFile file(cfgPath);
    const bool existed = file.exists();
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            const QString key  = line.section('=', 0, 0).trimmed();
            if (key.isEmpty()) { lines << line; continue; }
            if (written.contains(key)) continue;            // дубликаты (bat дописывал ключи повторно)
            if (key == QLatin1String("sdcard.path") && !overrides.contains(key)) continue;
            if (overrides.contains(key)) {
                lines << key + "=" + overrides.value(key);
            } else {
                lines << line;
            }
            written.insert(key);
        }
        file.close();
    }

    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        if (!written.contains(it.key())) { lines << it.key() + "=" + it.value(); written.insert(it.key()); }
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it)
        if (!written.contains(it.key())) { lines << it.key() + "=" + it.value(); written.insert(it.key()); }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logToEmulatorFile("WARNING: cannot write config.ini: " + cfgPath);
        return;
    }
    QTextStream out(&file);
    for (const QString &l : lines) out << l << "\n";
    file.close();

    logToEmulatorFile(QString("AVD config.ini %1: lcd=%2x%3, sysdir=%4, abi=%5")
                          .arg(existed ? "patched" : "created")
                          .arg(w).arg(h).arg(imageSysDir, abi));
}

// ── ADB key / server ─────────────────────────────────────────────────────────

void AndroidTVModule::syncAdbKeys(const QString &avdName)
{
    const QString userAndroidDir = QDir::homePath() + "/.android";
    const QString adbKeyPubPath  = userAndroidDir + "/adbkey.pub";
    const QString savedKeyPath   = avdDirPath(avdName) + "/adb_keys.txt";

    QString currentKey, savedKey;
    {
        QFile kf(adbKeyPubPath);
        if (kf.open(QIODevice::ReadOnly | QIODevice::Text))
            currentKey = QString::fromUtf8(kf.readAll()).trimmed();
    }
    {
        QFile sf(savedKeyPath);
        if (sf.open(QIODevice::ReadOnly | QIODevice::Text))
            savedKey = QString::fromUtf8(sf.readAll()).trimmed();
    }

    if (currentKey.isEmpty()) {
        logToEmulatorFile("WARNING: " + adbKeyPubPath + " not found — adb may show the device as unauthorized");
        return;
    }

    // ANDROID_EMULATOR_HOME указывает на папку AVD. Эмулятор ищет ключ adb именно
    // там, а adb создаёт его в %USERPROFILE%\.android. Без копии на production-
    // образах (Google TV) устройство остаётся unauthorized и загрузка «зависает».
    for (const QString &name : {QStringLiteral("adbkey"), QStringLiteral("adbkey.pub")}) {
        const QString src = userAndroidDir + "/" + name;
        const QString dst = avdHomePath() + "/" + name;
        if (!QFile::exists(src)) continue;
        QFile s(src), d(dst);
        QByteArray srcData, dstData;
        if (s.open(QIODevice::ReadOnly)) srcData = s.readAll();
        if (d.open(QIODevice::ReadOnly)) dstData = d.readAll();
        d.close();
        if (!srcData.isEmpty() && srcData != dstData) {
            QFile::remove(dst);
            if (QFile::copy(src, dst))
                logToEmulatorFile("Copied " + name + " to emulator home");
            else
                logToEmulatorFile("WARNING: could not copy " + name + " to " + dst);
        }
    }

    const bool firstRun    = savedKey.isEmpty();
    const bool keyMismatch = !firstRun && currentKey != savedKey;

    if (keyMismatch) {
        // Ключ adb сменился: старый userdata хранит авторизацию со старым ключом.
        logImportant(Logger::kLevelWarning,
                     "ADB key changed — wiping userdata to re-authorize for " + avdName);
        const QString avdDir = avdDirPath(avdName);
        for (const QString &f : { avdDir + "/userdata.img.qcow2",
                                  avdDir + "/userdata-qemu.img",
                                  avdDir + "/userdata-qemu.img.qcow2" }) {
            if (QFile::exists(f)) {
                if (QFile::remove(f)) logToEmulatorFile("Wiped: " + QFileInfo(f).fileName());
                else                  logToEmulatorFile("WARNING: could not wipe: " + f);
            }
        }
        QDir(avdDir + "/snapshots/default_boot").removeRecursively();
        logToEmulatorFile("Wiped snapshot default_boot");
    }

    if (keyMismatch || firstRun) {
        QDir().mkpath(avdDirPath(avdName));
        QFile nf(savedKeyPath);
        if (nf.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            nf.write(currentKey.toUtf8());
            logToEmulatorFile(firstRun ? "First run: saved ADB key for " + avdName
                                       : "Saved new ADB key for " + avdName);
        }
    }
}

// Перезапуск adb-сервера полностью асинхронно (раньше — QThread с захватом this,
// который мог пережить модуль при закрытии приложения).
void AndroidTVModule::syncAdbKeyAndRestartServer(const QString &avdName, std::function<void()> onReady)
{
    adbRaw({"kill-server"}, [this, avdName, onReady](const QString &) {
        logToEmulatorFile("adb kill-server: done");
        adbRaw({"start-server"}, [this, avdName, onReady](const QString &out) {
            logToEmulatorFile("adb start-server: " + (out.trimmed().isEmpty() ? QStringLiteral("ok") : out.trimmed()));
            // Ключ adbkey создаётся adb при первом старте сервера — синхронизируем после.
            syncAdbKeys(avdName);
            if (onReady) onReady();
        }, 15000);
    }, 5000);
}

// ── system.img.qcow2 ─────────────────────────────────────────────────────────

bool AndroidTVModule::isSystemQcow2Valid(const AndroidTVProfile &profile)
{
    const QString qcow2Path = avdDirPath(profile.avdName) + "/system.img.qcow2";
    if (!QFile::exists(qcow2Path))
        return true;

    const QString qemuImgPath = QDir::toNativeSeparators(
        m_appDir + "/tools/android-sdk/emulator/qemu-img.exe");

    if (!QFile::exists(qemuImgPath)) {
        logToEmulatorFile("qemu-img.exe not found, cannot verify system.img.qcow2");
        return true;
    }

    QProcess proc;
    proc.setProgram(qemuImgPath);
    proc.setArguments({"info", QDir::toNativeSeparators(qcow2Path)});
    proc.start();
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        logToEmulatorFile("qemu-img info timeout, assuming system.img.qcow2 is valid");
        return true;
    }

    const QString output = QString::fromUtf8(proc.readAllStandardOutput());
    static const QRegularExpression re(QStringLiteral("backing file:\\s*([^\\r\\n]+)"));
    const QRegularExpressionMatch match = re.match(output);
    if (!match.hasMatch()) {
        logToEmulatorFile("Could not parse backing file from qemu-img info");
        return true;
    }

    // qemu-img может дописать "(actual path: ...)" — отрезаем
    QString actualBacking = match.captured(1).trimmed();
    const int paren = actualBacking.indexOf(QLatin1String(" (actual path"));
    if (paren > 0) actualBacking = actualBacking.left(paren).trimmed();

    const QString imgDir = m_profileManager ? m_profileManager->systemImageDir(profile)
                                            : m_appDir + "/" + profile.systemImagePath;
    const QString expectedBacking = QDir::toNativeSeparators(imgDir + "/system.img");

    const QString a = QFileInfo(actualBacking).canonicalFilePath();
    const QString e = QFileInfo(expectedBacking).canonicalFilePath();
    const bool valid = !a.isEmpty() && a.compare(e, Qt::CaseInsensitive) == 0;

    if (!valid) {
        logToEmulatorFile("system.img.qcow2 backing file mismatch:");
        logToEmulatorFile("  Actual:   " + actualBacking);
        logToEmulatorFile("  Expected: " + expectedBacking);
    } else {
        logToEmulatorFile("system.img.qcow2 backing file is valid");
    }
    return valid;
}

void AndroidTVModule::repairSystemQcow2(const AndroidTVProfile &profile)
{
    const QString qcow2Path = avdDirPath(profile.avdName) + "/system.img.qcow2";
    if (!QFile::exists(qcow2Path))
        return;

    if (isSystemQcow2Valid(profile))
        return;

    logImportant(Logger::kLevelWarning,
                 "system.img.qcow2 backing file mismatch (app folder moved?), deleting it to regenerate");

    if (QFile::remove(qcow2Path)) logToEmulatorFile("Deleted system.img.qcow2");
    else                          logToEmulatorFile("WARNING: failed to delete system.img.qcow2");

    QDir avdDir(avdDirPath(profile.avdName));
    const QStringList userdataFiles = avdDir.entryList(
        {"userdata*.img", "userdata*.img.qcow2"}, QDir::Files);
    for (const QString &f : userdataFiles) {
        if (QFile::remove(avdDir.absoluteFilePath(f))) logToEmulatorFile("Removed " + f);
        else                                           logToEmulatorFile("WARNING: could not remove " + f);
    }
    // Снапшот ссылается на удалённые диски — его тоже нужно убрать
    QDir(avdDirPath(profile.avdName) + "/snapshots/default_boot").removeRecursively();
}

// ── Emulator process ─────────────────────────────────────────────────────────

void AndroidTVModule::startEmulator()
{
    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) {
        failLaunch("Error: no active profile", "No active profile, cannot start emulator");
        return;
    }

    if (!QFile::exists(emulatorPath())) {
        failLaunch("Error: emulator not found", "emulator.exe not found: " + emulatorPath());
        return;
    }

    const QString imgDir = m_profileManager->systemImageDir(*profile);
    if (!QFile::exists(imgDir + "/system.img")) {
        failLaunch("Error: system image not found",
                   "System image not found: " + imgDir + "/system.img"
                   + " — run external_tools/setup_android_sdk.bat or pick another image in the profile");
        return;
    }

    setStatus("Preparing AVD...");

    if (!ensureAvdExists(*profile)) {
        failLaunch("Error preparing AVD", "Cannot prepare AVD " + profile->avdName);
        return;
    }
    repairSystemQcow2(*profile);
    removeAvdLocks(avdDirPath(profile->avdName));
    patchAvdConfig(*profile, m_resolution);

#ifdef Q_OS_WIN
    {
        HMODULE hWinHvPlatform = LoadLibraryW(L"WinHvPlatform.dll");
        if (hWinHvPlatform) {
            logToEmulatorFile("WHPX: WinHvPlatform.dll found — hardware virtualization available");
            FreeLibrary(hWinHvPlatform);
        } else {
            logImportant(Logger::kLevelWarning,
                         "WHPX: WinHvPlatform.dll NOT found — emulator may run without CPU acceleration! "
                         "Enable 'Windows Hypervisor Platform' in Windows Features.");
        }
    }
#endif

    // Старый объект процесса (от прошлого запуска) — удаляем вместе со связями
    if (m_emulatorProcess) {
        m_emulatorProcess->disconnect(this);
        if (m_emulatorProcess->state() != QProcess::NotRunning) {
            m_emulatorProcess->kill();
            m_emulatorProcess->waitForFinished(1000);
        }
        m_emulatorProcess->deleteLater();
        m_emulatorProcess = nullptr;
    }

    m_consolePort = pickFreeConsolePort();
    logToEmulatorFile(QString("Console port %1, adb serial %2").arg(m_consolePort).arg(adbSerial()));

    m_emulatorProcess = new QProcess(this);
    QProcess *proc = m_emulatorProcess;

    QProcessEnvironment env = toolsEnvironment();
    env.insert("ANDROID_EMULATOR_RENDERER", "host");
    env.insert("ANDROID_EMU_MEDIA_DECODER_CUDA_VPX", "1");

    // Audio timer / DSound latency: только если заданы вручную (int), иначе "auto"
    auto manualInt = [](const QVariant &v, int &out) -> bool {
        if (!v.isValid() || v.isNull()) return false;
        if (v.typeId() == QMetaType::QString) {
            bool ok = false;
            out = v.toString().toInt(&ok);
            return ok;
        }
        bool ok = false;
        out = v.toInt(&ok);
        return ok;
    };

    int audioTimerMs = 0;
    if (manualInt(profile->audioTimerMs, audioTimerMs) && audioTimerMs > 0) {
        env.insert("QEMU_AUDIO_TIMER_PERIOD", QString::number(audioTimerMs));
        logToEmulatorFile(QString("Audio timer override: %1 ms").arg(audioTimerMs));
    } else {
        logToEmulatorFile("Audio timer: auto");
    }

    int dsoundLatencyMs = 0;
    if (manualInt(profile->dsoundLatencyMs, dsoundLatencyMs) && dsoundLatencyMs > 0) {
        env.insert("QEMU_DSOUND_LATENCY_MILLIS", QString::number(dsoundLatencyMs));
        logToEmulatorFile(QString("DSound latency override: %1 ms").arg(dsoundLatencyMs));
    } else {
        logToEmulatorFile("DSound latency: auto");
    }

    logToEmulatorFile("Audio backend: " + m_audioDriver);

#ifdef Q_OS_WIN
    if (!m_timerResolutionSet) {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) {
            m_timerResolutionSet = true;
            logToEmulatorFile("Timer resolution set to 1 ms (timeBeginPeriod)");
        } else {
            logToEmulatorFile("WARNING: timeBeginPeriod(1) failed");
        }
    }
#endif

    env.insert("QT_SCALE_FACTOR",             "1");
    env.insert("QT_AUTO_SCREEN_SCALE_FACTOR", "0");
    env.insert("QT_SCREEN_SCALE_FACTORS",     "1");

    proc->setProcessEnvironment(env);
    proc->setProgram(emulatorPath());
    proc->setWorkingDirectory(QFileInfo(emulatorPath()).absolutePath());

    const QString dnsArg = m_dnsServer.trimmed().isEmpty() ? QStringLiteral("8.8.8.8,1.1.1.1")
                                                           : m_dnsServer.trimmed();
    logToEmulatorFile("Using DNS: " + dnsArg);

    QStringList args;
    args << "-avd"        << profile->avdName
         << "-port"       << QString::number(m_consolePort)
         << "-dns-server" << dnsArg
         << "-gpu"        << m_gpu
         << "-skin"       << m_resolution
         << "-memory"     << QString::number(m_ramMb)
         << "-cores"      << QString::number(m_cores)
         << "-netfast"
         << "-accel"      << "on"
         << "-skip-adb-auth"
         << "-no-metrics"
         << "-audio"      << m_audioDriver
         << "-feature"    << "GLDirectMem"
         << "-feature"    << "GLDMA"
         << "-feature"    << "VulkanNativeSwapchain";

    // Часовой пояс хоста (IANA). Время эмулятор и так берёт с хоста, а явный
    // пояс нужен потому, что Windows-имена зон эмулятор распознаёт не всегда.
    const QByteArray tzId = QTimeZone::systemTimeZoneId();
    if (!tzId.isEmpty() && tzId.contains('/'))
        args << "-timezone" << QString::fromLatin1(tzId);

    if (m_writableSystem)  args << "-writable-system";
    if (m_noSnapshot)      args << "-no-snapshot";
    if (m_noSnapshotSave)  args << "-no-snapshot-save";
    if (m_noBootAnim)      args << "-no-boot-anim";

    proc->setArguments(args);
    logToEmulatorFile("emulator " + args.join(' '));

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) logToEmulatorFile(out, true);
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!err.isEmpty()) logToEmulatorFile(err, true);
    });

    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        if (proc != m_emulatorProcess) return;
        onEmulatorFinished(code);
    });

    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError err) {
        if (proc != m_emulatorProcess) return;
        // Crashed/Timedout приходят вместе с finished — их обрабатывает onEmulatorFinished
        if (err == QProcess::FailedToStart)
            failLaunch("Error launching emulator", "emulator failed to start: " + proc->errorString());
        else
            logToEmulatorFile("emulator process error " + QString::number(err) + ": " + proc->errorString());
    });

    connect(proc, &QProcess::started, this, [this, proc]() {
        if (proc != m_emulatorProcess) return;
        m_emulatorPid        = proc->processId();
        m_initialEmulatorPid = m_emulatorPid;
        logToEmulatorFile("emulator process started PID " + QString::number(m_emulatorPid));

#ifdef Q_OS_WIN
        HANDLE hProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(m_emulatorPid));
        if (hProc) {
            if (SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS))
                logToEmulatorFile("Emulator process priority set to ABOVE_NORMAL");
            else
                logToEmulatorFile("WARNING: SetPriorityClass failed, error=" + QString::number(GetLastError()));
            CloseHandle(hProc);
        }

        // Через 3 с поднимаем приоритет потоков фактического процесса эмулятора
        // (emulator.exe перезапускает себя как qemu-system-*.exe; PID уточняется
        // в WinEvent-хуке / опросе окон).
        QTimer::singleShot(3000, this, [this]() {
            if (!m_emulatorPid) return;

            using GetThreadDescFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            static auto getThreadDesc = reinterpret_cast<GetThreadDescFn>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap == INVALID_HANDLE_VALUE) {
                logToEmulatorFile("WARNING: CreateToolhelp32Snapshot failed for thread boost");
                return;
            }

            THREADENTRY32 te = {};
            te.dwSize = sizeof(te);
            int boostedNormal = 0;
            int boostedAudio  = 0;

            if (Thread32First(hSnap, &te)) {
                do {
                    if (te.th32OwnerProcessID != static_cast<DWORD>(m_emulatorPid))
                        continue;

                    HANDLE hThread = OpenThread(
                        THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                        FALSE, te.th32ThreadID);
                    if (!hThread) continue;

                    bool isAudioThread = false;
                    if (getThreadDesc) {
                        PWSTR desc = nullptr;
                        if (SUCCEEDED(getThreadDesc(hThread, &desc)) && desc) {
                            const QString name = QString::fromWCharArray(desc);
                            LocalFree(desc);
                            if (name.contains(QLatin1String("audio"), Qt::CaseInsensitive) ||
                                name.contains(QLatin1String("sound"), Qt::CaseInsensitive) ||
                                name.contains(QLatin1String("winmm"), Qt::CaseInsensitive)) {
                                isAudioThread = true;
                                SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
                                logToEmulatorFile(
                                    QString("Audio thread detected: '%1' (TID %2) → TIME_CRITICAL")
                                        .arg(name).arg(te.th32ThreadID));
                                ++boostedAudio;
                            }
                        }
                    }
                    if (!isAudioThread) {
                        SetThreadPriority(hThread, THREAD_PRIORITY_ABOVE_NORMAL);
                        ++boostedNormal;
                    }
                    CloseHandle(hThread);
                } while (Thread32Next(hSnap, &te));
            }
            CloseHandle(hSnap);

            logToEmulatorFile(
                QString("Thread boost complete: %1 × ABOVE_NORMAL, %2 × TIME_CRITICAL (audio)")
                    .arg(boostedNormal).arg(boostedAudio));
        });
#endif

        installWinEventHook();
        startWindowPolling();
        startBootPolling();
    });

    setStatus("Starting emulator...");
    proc->start();
}

void AndroidTVModule::onEmulatorFinished(int exitCode)
{
    const bool wasBooting = m_booting;
    logImportant(Logger::kLevelInfo, "emulator finished, exit code " + QString::number(exitCode));

    stopKeepTopmostTimer();
    stopWindowPolling();
    stopBootPolling();
    stopHealthCheck();
    stopLogcat();
    stopKeyShell();
    removeWinEventHook();
    stopPositionSyncTimer();
    stopDnsProxy();
#ifdef Q_OS_WIN
    if (m_timerResolutionSet) {
        timeEndPeriod(1);
        m_timerResolutionSet = false;
        logToEmulatorFile("Timer resolution restored to default");
    }
#endif

    setBooting(false);
    setRunning(false);
    restoreMediaStation();          // при !running/!booting снимает TOPMOST

    // Сбрасываем состояние окна/процесса — иначе следующий запуск цепляется
    // к мёртвому HWND и старому PID.
#ifdef Q_OS_WIN
    m_emulatorHwnd = nullptr;
#endif
    m_emulatorPid        = 0;
    m_initialEmulatorPid = 0;
    setEmulatorShown(false);
    setSwitching(false);

    if (wasBooting && !m_exitRequested)
        setStatus(QString("Emulator exited during boot (code %1), see logs/Android_TV.log").arg(exitCode));
    else if (!m_status.startsWith(QLatin1String("Error")) && !m_status.startsWith(QLatin1String("Boot timeout")))
        setStatus("");

    if (!m_launchAvdName.isEmpty())
        removeAvdLocks(avdDirPath(m_launchAvdName));

    refreshQcow2Size();
    emit emulatorClosed();
    if (m_bus) emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("emulatorClosed"), {});
    m_exitRequested = false;
}

// ── Boot polling ─────────────────────────────────────────────────────────────

void AndroidTVModule::startBootPolling()
{
    stopBootPolling();
    m_bootPollCount    = 0;
    m_bootPollInFlight = false;
    setStatus("Loading Android TV...");
    m_bootPollTimer = new QTimer(this);
    m_bootPollTimer->setInterval(kBootPollIntervalMs);

    connect(m_bootPollTimer, &QTimer::timeout, this, [this]() {
        ++m_bootPollCount;
        if (m_bootPollCount > kBootPollMaxTries) {
            stopBootPolling();
            // Эмулятор, не загрузившийся за 5 минут, оставлять висеть бессмысленно
            killEmulatorTree();
            failLaunch("Boot timeout exceeded",
                       QString("boot polling timeout (%1 s)").arg(kBootPollMaxTries * kBootPollIntervalMs / 1000));
            return;
        }
        if (m_bootPollCount % 5 == 0) {
            const int elapsed = m_bootPollCount * kBootPollIntervalMs / 1000;
            setStatus(QString("Loading... %1 sec").arg(elapsed));
        }

        // Один запрос за раз: adb может отвечать дольше интервала опроса
        if (m_bootPollInFlight) return;
        m_bootPollInFlight = true;

        adb({"shell", "getprop", "sys.boot_completed"}, [this](const QString &out) {
            m_bootPollInFlight = false;
            if (!m_booting) return;   // уже загрузился / отменён
            const QString res = out.trimmed();
            if (res != QLatin1String("1")) {
                if (m_bootPollCount % 10 == 0 && !res.isEmpty())
                    logToEmulatorFile("boot poll: " + res.left(200));
                return;
            }
            onBootCompleted();
        }, 8000);
    });

    m_bootPollTimer->start();
}

void AndroidTVModule::stopBootPolling()
{
    if (m_bootPollTimer) {
        m_bootPollTimer->stop();
        m_bootPollTimer->deleteLater();
        m_bootPollTimer = nullptr;
    }
    m_bootPollInFlight = false;
}

void AndroidTVModule::onBootCompleted()
{
    logImportant(Logger::kLevelInfo, "sys.boot_completed = 1");
    stopBootPolling();
    stopKeepTopmostTimer();
    setBooting(false);
    setRunning(true);
    setStatus("Android TV ready");
    emit bootCompleted();
    startHealthCheck();
    if (m_bus) emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("bootCompleted"), {});

    logToEmulatorFile("Setting up ADB reverse port forwarding...");
    for (const char *port : {"80", "443", "8080", "8443"}) {
        const QString spec = QStringLiteral("tcp:") + QLatin1String(port);
        adb({"reverse", spec, spec}, [this, spec](const QString &res) {
            logToEmulatorFile("adb reverse " + spec + " -> " + res.trimmed());
        });
    }

    // Блоклист перезапускает adbd (adb root) — logcat стартует после него
    applyBlocklist();
    emit emulatorReadyBehind();
}

// ── Health check ─────────────────────────────────────────────────────────────

void AndroidTVModule::startHealthCheck()
{
    if (m_healthCheckTimer) return;
    m_healthCheckTimer = new QTimer(this);
    m_healthCheckTimer->setInterval(30 * 60 * 1000);

    connect(m_healthCheckTimer, &QTimer::timeout, this, [this]() {
        if (m_launchAvdName.isEmpty()) return;
        const QString qcow2Path = avdDirPath(m_launchAvdName) + "/userdata-qemu.img.qcow2";
        QFileInfo fi(qcow2Path);
        if (fi.exists()) {
            const qint64 sizeMb = fi.size() / 1024 / 1024;
            logImportant(Logger::kLevelInfo, QString("Health: userdata qcow2 = %1 MB").arg(sizeMb));
            if (m_qcow2SizeMb != sizeMb) {
                m_qcow2SizeMb = sizeMb;
                emit qcow2SizeMbChanged();
            }
            if (sizeMb > kMaxQcow2SizeMb) {
                logImportant(Logger::kLevelWarning,
                             QString("Health: qcow2 exceeds %1 MB — emulator may freeze soon, "
                                     "consider restarting").arg(kMaxQcow2SizeMb));
                if (m_bus)
                    emit m_bus->moduleEvent(
                        QStringLiteral("androidtv"), QStringLiteral("qcow2Warning"),
                        QVariantMap{{"sizeMb", sizeMb}, {"limitMb", static_cast<int>(kMaxQcow2SizeMb)}});
            }
        }

        adb({"shell", "cat", "/proc/meminfo"}, [this](const QString &out) {
            for (const QString &line : out.split(QLatin1Char('\n'))) {
                const QString l = line.trimmed();
                if (l.startsWith(QLatin1String("MemAvailable")) ||
                    l.startsWith(QLatin1String("SwapFree")))
                    logImportant(Logger::kLevelInfo, "Health: " + l);
            }
        });

        adb({"shell", "getprop", "sys.boot_completed"}, [this](const QString &out) {
            if (out.trimmed() != QLatin1String("1")) {
                logImportant(Logger::kLevelWarning, "Health: emulator not responding to adb — may be frozen");
                if (m_bus)
                    emit m_bus->moduleEvent(QStringLiteral("androidtv"),
                                            QStringLiteral("emulatorUnresponsive"), {});
            } else {
                logToEmulatorFile("Health: emulator adb ping OK");
            }
        });
    });

    m_healthCheckTimer->start();
    logToEmulatorFile("Health check started (interval: 30 min, qcow2 limit: "
                      + QString::number(kMaxQcow2SizeMb) + " MB)");
}

void AndroidTVModule::stopHealthCheck()
{
    if (m_healthCheckTimer) {
        m_healthCheckTimer->stop();
        m_healthCheckTimer->deleteLater();
        m_healthCheckTimer = nullptr;
        logToEmulatorFile("Health check stopped");
    }
}

// ── adb helpers ──────────────────────────────────────────────────────────────

void AndroidTVModule::adbRaw(const QStringList &args, std::function<void(QString)> callback, int timeoutMs)
{
    if (!QFile::exists(adbPath())) {
        logToEmulatorFile("adb.exe not found: " + adbPath());
        if (callback)
            QTimer::singleShot(0, this, [callback]() { callback(QString()); });
        return;
    }

    auto *proc = new QProcess(this);
    proc->setProgram(adbPath());
    proc->setArguments(args);
    proc->setProcessEnvironment(toolsEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);

    auto *watchdog = new QTimer(proc);
    watchdog->setSingleShot(true);
    watchdog->setInterval(timeoutMs);
    connect(watchdog, &QTimer::timeout, this, [this, proc, args, timeoutMs]() {
        logToEmulatorFile(QString("adb watchdog: timeout %1 ms, killing: adb %2")
                              .arg(timeoutMs).arg(args.join(QLatin1Char(' '))));
        proc->kill();
    });

    // finished и errorOccurred(FailedToStart) взаимоисключающие, но защищаемся от двойного вызова
    auto done = std::make_shared<bool>(false);
    auto finish = [this, proc, callback, done, watchdog]() {
        if (*done) return;
        *done = true;
        watchdog->stop();
        const QString out = QString::fromUtf8(proc->readAll());
        proc->deleteLater();
        if (callback && !m_shutdownDone) callback(out);
    };

    connect(proc, &QProcess::finished, this, [finish](int, QProcess::ExitStatus) { finish(); });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish();
    });

    proc->start();
    watchdog->start();
}

void AndroidTVModule::adb(const QStringList &args, std::function<void(QString)> callback, int timeoutMs)
{
    adbRaw(QStringList{QStringLiteral("-s"), adbSerial()} + args, std::move(callback), timeoutMs);
}

// ── Blocklist (/system/etc/hosts) ────────────────────────────────────────────
// Запись в /system возможна только при -writable-system и root-adb (образы
// без Google Play). Раньше push делался без root и всегда завершался ошибкой.

void AndroidTVModule::applyBlocklist()
{
    if (!m_writableSystem) {
        logToEmulatorFile("blocklist: profile has writable system disabled, skipping");
        startLogcat();
        return;
    }

    QFile list(blocklistPath());
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logToEmulatorFile("blocklist not found (" + blocklistPath() + "), skipping");
        startLogcat();
        return;
    }

    QStringList entries;
    entries << "127.0.0.1 localhost" << "::1 ip6-localhost";

    QTextStream in(&list);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        entries << "0.0.0.0 " + line.section(QRegularExpression("\\s+"), 0, 0);
    }

    const QString tmpDir  = m_appDir + "/tools/android-tv/config";
    const QString tmpPath = tmpDir + "/hosts.tmp";
    QDir().mkpath(tmpDir);
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logImportant(Logger::kLevelError, "cannot write temp hosts file: " + tmpPath);
        startLogcat();
        return;
    }
    QTextStream out(&tmp);
    for (const auto &e : entries) out << e << "\n";
    tmp.close();

    logToEmulatorFile(QString("blocklist: pushing %1 entries to /system/etc/hosts").arg(entries.size()));

    adb({"root"}, [this, tmpPath](const QString &rootOut) {
        logToEmulatorFile("adb root: " + rootOut.trimmed());
        if (rootOut.contains(QLatin1String("cannot run as root"), Qt::CaseInsensitive)) {
            logToEmulatorFile("blocklist: production image (no root) — hosts cannot be modified");
            startLogcat();
            return;
        }
        adb({"wait-for-device"}, [this, tmpPath](const QString &) {
            adb({"remount"}, [this, tmpPath](const QString &remountOut) {
                logToEmulatorFile("adb remount: " + remountOut.trimmed());
                adb({"push", QDir::toNativeSeparators(tmpPath), "/system/etc/hosts"},
                    [this](const QString &pushOut) {
                        logToEmulatorFile("hosts push: " + pushOut.trimmed());
                        startLogcat();
                    }, 20000);
            }, 20000);
        }, 30000);
    }, 15000);
}

// ── qcow2 ────────────────────────────────────────────────────────────────────

void AndroidTVModule::refreshQcow2Size()
{
    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    qint64 sizeMb = -1;
    if (profile) {
        QFileInfo fi(avdDirPath(profile->avdName) + "/userdata-qemu.img.qcow2");
        if (fi.exists()) sizeMb = fi.size() / 1024 / 1024;
    }
    if (m_qcow2SizeMb != sizeMb) {
        m_qcow2SizeMb = sizeMb;
        emit qcow2SizeMbChanged();
    }
}

bool AndroidTVModule::resetQcow2()
{
    if (m_running || m_booting) {
        logImportant(Logger::kLevelWarning, "resetQcow2: cannot delete while emulator is running");
        return false;
    }

    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) return false;

    const QString avdDir = avdDirPath(profile->avdName);
    const QStringList targets = {
        avdDir + "/userdata-qemu.img.qcow2",
        avdDir + "/userdata.img.qcow2"
    };

    bool anyDeleted = false;
    for (const QString &f : targets) {
        if (QFile::exists(f)) {
            if (QFile::remove(f)) {
                logImportant(Logger::kLevelInfo, "resetQcow2: deleted " + QFileInfo(f).fileName());
                anyDeleted = true;
            } else {
                logImportant(Logger::kLevelError, "resetQcow2: failed to delete " + f);
            }
        }
    }

    QDir snapDir(avdDir + "/snapshots/default_boot");
    if (snapDir.exists()) {
        snapDir.removeRecursively();
        logImportant(Logger::kLevelInfo, "resetQcow2: removed snapshot default_boot");
    }

    refreshQcow2Size();
    return anyDeleted;
}

// ── logcat ───────────────────────────────────────────────────────────────────

void AndroidTVModule::startLogcat()
{
    if (!m_running) return;
    if (m_logcatProcess && m_logcatProcess->state() != QProcess::NotRunning) {
        logToEmulatorFile("logcat already running");
        return;
    }

    if (!m_logcatLogFile.isOpen()) {
        m_logcatLogFile.setFileName(logcatLogPath());
        if (!m_logcatLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            logToEmulatorFile("ERROR: cannot open logcat log file: " + logcatLogPath());
            return;
        }
        m_logcatLogStream.setDevice(&m_logcatLogFile);
    }
    m_logcatLogStream << "=== Android TV logcat started at "
                      << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
                      << " AVD: " << m_launchAvdName << " ===\n";
    m_logcatLogStream.flush();

    m_logcatProcess = new QProcess(this);
    QProcess *proc = m_logcatProcess;
    proc->setProgram(adbPath());
    proc->setArguments({"-s", adbSerial(), "logcat", "-v", "threadtime"});
    proc->setProcessEnvironment(toolsEnvironment());

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        const QByteArray data = proc->readAllStandardOutput();
        if (!data.isEmpty() && m_logcatLogFile.isOpen()) {
            m_logcatLogStream << QString::fromUtf8(data);
            m_logcatLogStream.flush();
        }
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QByteArray data = proc->readAllStandardError();
        if (!data.isEmpty())
            logToEmulatorFile("[logcat stderr] " + QString::fromUtf8(data).trimmed());
    });
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        logToEmulatorFile("logcat process finished, exit code " + QString::number(code));
        if (m_logcatLogFile.isOpen()) {
            m_logcatLogStream << "=== logcat ended ===\n\n";
            m_logcatLogStream.flush();
            m_logcatLogFile.close();
        }
        if (m_logcatProcess == proc) m_logcatProcess = nullptr;
        proc->deleteLater();
    });

    proc->start();
    logToEmulatorFile("logcat started, writing to " + logcatLogPath());
}

void AndroidTVModule::stopLogcat()
{
    if (m_logcatProcess && m_logcatProcess->state() != QProcess::NotRunning) {
        logToEmulatorFile("Stopping logcat...");
        // У adb.exe нет обработчика WM_CLOSE → terminate() на Windows бесполезен
        m_logcatProcess->kill();
        m_logcatProcess->waitForFinished(2000);
    } else if (m_logcatLogFile.isOpen()) {
        m_logcatLogStream << "=== logcat ended (forced) ===\n\n";
        m_logcatLogStream.flush();
        m_logcatLogFile.close();
    }
}
