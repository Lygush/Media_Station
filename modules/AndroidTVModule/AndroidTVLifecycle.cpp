// modules/AndroidTVModule/AndroidTVLifecycle.cpp
//
// Запуск / остановка эмулятора, подготовка AVD, взаимодействие с adb.

#include "AndroidTVModule.h"
#include "AdGuardHome.h"
#include "tools/LocalModelPaths.h"
#include <QPointer>
#include "LoggerModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"
#include "modules/NotificationModule/NotificationModule.h"

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
#include <QDesktopServices>
#include <QUrl>
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

    // adb-сервер предлагает устройству все найденные ключи, а не только свой
    // %USERPROFILE%\.android\adbkey. Эмулятор берёт ключ из своей папки
    // (ANDROID_EMULATOR_HOME) — если там другой, без этого устройство остаётся
    // «unauthorized» и загрузка не завершается.
    QStringList vendorKeys;
    for (const QString &dir : adbKeyDirs()) {
        const QString key = QDir::toNativeSeparators(dir + "/adbkey");
        if (QFile::exists(key) && !vendorKeys.contains(key, Qt::CaseInsensitive))
            vendorKeys << key;
    }
    if (!vendorKeys.isEmpty())
        env.insert("ADB_VENDOR_KEYS", vendorKeys.join(QDir::listSeparator()));
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
            // Android TV рассчитан на 1080p. На мониторе 1440p/4K эмулятор
            // рисовал бы в полном разрешении — заметная лишняя нагрузка на
            // хост (и, как следствие, на звук). Окно растягивается видеокартой.
            // Пропорции экрана сохраняем, ширину выравниваем до чётной.
            if (screen.height() > 1080) {
                const int w = qRound(screen.width() * 1080.0 / screen.height()) & ~1;
                screen = QSize(w, 1080);
            }
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
    m_diagnosticLogcat = profile.diagnosticLogcat;
    m_consoleInput     = profile.consoleInput;

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
    m_launchTimer.start();
    m_lastBootPollOut.clear();
    m_emulatorLinesForwarded = 0;
    m_emuReportedBoot   = false;
    m_emuDiscoveryFile.clear();
    m_unauthorizedPolls = 0;
    m_adbUnauthorized   = false;
    logImportant(Logger::kLevelInfo, "launching Android TV emulator with profile " + profile->name
                 + " (details: logs/Android_TV.log)");
    refreshQcow2Size();

    m_exitRequested = false;
    m_launchAvdName = profile->avdName;

    // Сразу сигнализируем UI — анимация запуска стартует мгновенно.
    setBooting(true);
    setStatus("Preparing ADB...");
    startKeepTopmostTimer();

    loadEmulatorSettings(*profile);
    startAdBlockProbe();

    // Эмулятор стартует ПОСЛЕ перезапуска adb-сервера, иначе первый запуск
    // заметно лагает (adb переподключается к уже работающему эмулятору).
    syncAdbKeyAndRestartServer(profile->avdName, [this]() {
        if (!m_booting || m_shutdownDone) {
            logImportant(Logger::kLevelInfo, "launch cancelled before emulator start");
            return;
        }
        logImportant(Logger::kLevelInfo, QString("adb server restarted (%1 ms), starting emulator")
                                             .arg(m_launchTimer.elapsed()));
        // Проверка AdGuard Home (≤ 700 мс) обычно успевает раньше adb
        if (m_adBlockProbePending) m_afterAdBlockProbe = [this]() { startEmulator(); };
        else                       startEmulator();
    });
}

void AndroidTVModule::stop()
{
    if (!m_running && !m_booting) {
        m_exitRequested = false;
        return;
    }
    logImportant(Logger::kLevelInfo,
                 m_booting ? QString("stop requested while booting (%1 s, status \"%2\")")
                                 .arg(m_launchTimer.isValid() ? m_launchTimer.elapsed() / 1000 : 0).arg(m_status)
                           : QStringLiteral("stop requested"));
    setStatus("Stopping emulator...");

    // Эмулятор ещё не успел стартовать (идёт перезапуск adb) — просто отменяем запуск.
    if (!m_emulatorProcess || m_emulatorProcess->state() == QProcess::NotRunning) {
        stopKeepTopmostTimer();
        m_afterAdBlockProbe = nullptr;
        setBooting(false);
        setRunning(false);
        setStatus("");
        restoreMediaStation();
        m_exitRequested = false;
        return;
    }

    restoreMediaStation();
    stopKeyShell();
    stopConsole();
    stopLogcat();
    adb({"emu", "kill"}, {}, 5000);

    // Запоминаем, КАКОЙ процесс останавливали: если за 6 с пользователь успел
    // выйти и запустить Android TV снова, таймер раньше убивал новую сессию.
    const QPointer<QProcess> stopping(m_emulatorProcess);
    QTimer::singleShot(6000, this, [this, stopping]() {
        if (!stopping || stopping != m_emulatorProcess)
            return;   // уже завершился, а возможно, запущен новый эмулятор
        if (stopping->state() != QProcess::NotRunning) {
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
    m_afterAdBlockProbe = nullptr;
    setBooting(false);
    setRunning(false);
    setStatus(userStatus);
    restoreMediaStation();
    m_exitRequested = false;
    if (m_bus)
        emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("launchFailed"),
                                QVariantMap{{"reason", userStatus}});
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

QStringList AndroidTVModule::adbKeyDirs() const
{
    QStringList dirs;
    const QString userHome = qEnvironmentVariable("ANDROID_USER_HOME");
    if (!userHome.isEmpty()) dirs << userHome;
    const QString sdkHome = qEnvironmentVariable("ANDROID_SDK_HOME");
    if (!sdkHome.isEmpty()) dirs << sdkHome + "/.android";
    dirs << QDir::homePath() + "/.android";
    dirs << avdHomePath();
    return dirs;
}

// Эмулятор пишет в stdout «Sending adb public key [<ключ> @unknown]». Если этот
// ключ не совпадает ни с одним adbkey.pub, adb-сервер не сможет авторизоваться
// — пишем в основной журнал, где лежит совпадающий ключ (или что его нет).
void AndroidTVModule::checkEmulatorAdbKey(const QString &pubKey)
{
    const QString emuKey = pubKey.section(QLatin1Char(' '), 0, 0).trimmed();
    if (emuKey.isEmpty()) return;
    QStringList checked;
    for (const QString &dir : adbKeyDirs()) {
        QFile f(dir + "/adbkey.pub");
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString key = QString::fromUtf8(f.readAll()).section(QLatin1Char(' '), 0, 0).trimmed();
        checked << QDir::toNativeSeparators(f.fileName());
        if (key == emuKey) {
            logToEmulatorFile("emulator adb key matches " + QDir::toNativeSeparators(f.fileName()));
            return;
        }
    }
    logImportant(Logger::kLevelWarning,
                 "emulator sends an adb key that matches no adbkey.pub (checked: "
                 + (checked.isEmpty() ? QStringLiteral("none found") : checked.join(", "))
                 + ") — adb may stay unauthorized");
}

void AndroidTVModule::scanEmulatorOutput(const QString &out)
{
    // Эмулятор не понял флаги gRPC — запоминаем, следующий запуск без них
    if (m_grpcPort > 0 && out.contains(QLatin1String("grpc"), Qt::CaseInsensitive)
        && (out.contains(QLatin1String("unknown option"), Qt::CaseInsensitive)
            || out.contains(QLatin1String("invalid option"), Qt::CaseInsensitive)
            || out.contains(QLatin1String("unrecognized"), Qt::CaseInsensitive))) {
        const QString key = emulatorBuildKey();
        if (!key.isEmpty()) {
            QSettings st = LocalModels::appSettings();
            st.setValue("androidtv/grpcUnsupportedFor", key);
        }
        logImportant(Logger::kLevelError,
                     "Emulator does not support -grpc options — Android TV will start without "
                     "gRPC next time (restart Android TV)");
    }
    // Файл обнаружения (в нём токен gRPC): «Advertising in: C:\...\pid_720.ini»
    static const QRegularExpression reAdv(QStringLiteral(R"(Advertising in: (\S+\.ini))"));
    const QRegularExpressionMatch ma = reAdv.match(out);
    if (ma.hasMatch()) m_emuDiscoveryFile = QDir::fromNativeSeparators(ma.captured(1));
    static const QRegularExpression reKey(QStringLiteral(R"(Sending adb public key \[([^\]]+)\])"));
    static const QRegularExpression reBoot(QStringLiteral(R"(Boot completed in (\d+) ms)"));
    const QRegularExpressionMatch mk = reKey.match(out);
    if (mk.hasMatch()) {
        // Ключ может прийти разорванным на два куска вывода — сверяем, только если целый
        checkEmulatorAdbKey(mk.captured(1));
    }
    const QRegularExpressionMatch mb = reBoot.match(out);
    if (mb.hasMatch() && !m_emuReportedBoot) {
        m_emuReportedBoot = true;
        logImportant(Logger::kLevelInfo,
                     QString("emulator reports Android booted (%1 s), checking adb").arg(mb.captured(1).toInt() / 1000));
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
    // ANDROID_EMU_MEDIA_DECODER_CUDA_VPX убран: без NVIDIA бессмыслен, а
    // нестандартный декодер — лишний источник рывков.

    // ── Звук ────────────────────────────────────────────────────────────────
    // Эмулятор собран на QEMU 2.12. Там:
    //  • QEMU_AUDIO_TIMER_PERIOD — ЧАСТОТА таймера в Гц (по умолчанию 100 Гц =
    //    10 мс). В профиле хранится период в мс, переводим.
    //  • QEMU_DSOUND_LATENCY_MILLIS — отступ записи внутри кольцевого буфера
    //    DirectSound, размер которого задаёт QEMU_DSOUND_BUFSIZE_OUT (по
    //    умолчанию 16384 байта ≈ 93 мс). Без увеличения буфера большая
    //    «задержка» после деления по модулю давала запас в несколько мс.
    // По умолчанию ставим буфер ≈ 370 мс и отступ 60 мс: звук отстаёт на 60 мс
    // (для ТВ незаметно), зато кратковременные задержки хоста не дают щелчков.
    auto manualInt = [](const QVariant &v, int &out) -> bool {
        if (!v.isValid() || v.isNull()) return false;
        bool ok = false;
        out = (v.typeId() == QMetaType::QString) ? v.toString().toInt(&ok) : v.toInt(&ok);
        return ok;
    };

    int timerPeriodMs = 0;
    if (manualInt(profile->audioTimerMs, timerPeriodMs) && timerPeriodMs > 0) {
        timerPeriodMs = qBound(4, timerPeriodMs, 10);
        const int hz = qRound(1000.0 / timerPeriodMs);
        env.insert("QEMU_AUDIO_TIMER_PERIOD", QString::number(hz));
        logToEmulatorFile(QString("Audio timer: %1 ms (%2 Hz)").arg(timerPeriodMs).arg(hz));
    } else {
        logToEmulatorFile("Audio timer: default (10 ms / 100 Hz)");
    }

    constexpr int kDsoundBufBytes = 65536;                       // ≈ 371 мс при 44.1 кГц стерео 16 бит
    constexpr int kDsoundBufMs    = kDsoundBufBytes * 1000 / (44100 * 4);
    int latencyMs = 60;
    if (manualInt(profile->dsoundLatencyMs, latencyMs) && latencyMs > 0)
        latencyMs = qBound(20, latencyMs, kDsoundBufMs * 6 / 10);
    else
        latencyMs = 60;
    env.insert("QEMU_DSOUND_BUFSIZE_OUT",    QString::number(kDsoundBufBytes));
    env.insert("QEMU_DSOUND_LATENCY_MILLIS", QString::number(latencyMs));
    logToEmulatorFile(QString("DSound: buffer %1 bytes (~%2 ms), latency %3 ms")
                          .arg(kDsoundBufBytes).arg(kDsoundBufMs).arg(latencyMs));

    logToEmulatorFile("Audio backend: " + m_audioDriver);

    // timeBeginPeriod(1) здесь не вызываем: с Windows 10 2004 разрешение
    // таймера действует только на вызвавший процесс (MediaStation), эмулятору
    // оно не помогало. Для процесса эмулятора снимаем ограничения питания в
    // applyEmulatorProcessPriority().

    env.insert("QT_SCALE_FACTOR",             "1");
    env.insert("QT_AUTO_SCREEN_SCALE_FACTOR", "0");
    env.insert("QT_SCREEN_SCALE_FACTORS",     "1");

    proc->setProcessEnvironment(env);
    proc->setProgram(emulatorPath());
    proc->setWorkingDirectory(QFileInfo(emulatorPath()).absolutePath());

    // AdGuard Home ответил — весь DNS эмулятора через него (блокировка
    // рекламы); иначе DNS из профиля
    const QString profileDns = m_dnsServer.trimmed().isEmpty() ? QStringLiteral("8.8.8.8,1.1.1.1")
                                                               : m_dnsServer.trimmed();
    const QString dnsArg = m_adBlockDns.isEmpty() ? profileDns : m_adBlockDns;
    logToEmulatorFile("Using DNS: " + dnsArg + (m_adBlockDns.isEmpty() ? "" : " (AdGuard Home)"));

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
         << "-audio"      << m_audioDriver;

    // gRPC эмулятора — быстрый ввод с пульта (EmulatorGrpcClient). Токен —
    // тот же, что у консоли. Если эта сборка эмулятора флаги не понимает
    // (запомнено при прошлом запуске) — без них.
    m_grpcPort = 0;
    if (!grpcDisabled()) {
        m_grpcPort = pickFreeGrpcPort();
        if (m_grpcPort > 0)
            args << "-grpc" << QString::number(m_grpcPort) << "-grpc-use-token";
    }
    // Экспериментальные -feature GLDirectMem/GLDMA/VulkanNativeSwapchain убраны:
    // нестандартные режимы рендера — частая причина рывков картинки и звука.

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
        if (!out.isEmpty()) { scanEmulatorOutput(out); logToEmulatorFile(out, true); }
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc]() {
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        if (!err.isEmpty()) { scanEmulatorOutput(err); logToEmulatorFile(err, true); }
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
        logImportant(Logger::kLevelInfo, "emulator process started, PID " + QString::number(m_emulatorPid)
                                         + ", waiting for Android to boot");

        // Лаунчер emulator.exe перезапускает себя как qemu-system-*.exe;
        // настоящий PID приходит позже (WinEvent-хук / опрос окон) и там же
        // повторно вызывается applyEmulatorProcessPriority.
        applyEmulatorProcessPriority(m_emulatorPid);

        installWinEventHook();
        startWindowPolling();
        startBootPolling();
    });

    setStatus("Starting emulator...");
    proc->start();
}

void AndroidTVModule::applyEmulatorProcessPriority(quint64 pid)
{
#ifdef Q_OS_WIN
    if (!pid || pid == m_priorityAppliedPid) return;

    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, static_cast<DWORD>(pid));
    if (!h) {
        logToEmulatorFile(QString("WARNING: OpenProcess(%1) failed for priority, error=%2")
                              .arg(pid).arg(GetLastError()));
        return;
    }
    m_priorityAppliedPid = pid;

    // Класс приоритета не наследуется дочерним процессом, поэтому выставляем
    // его именно настоящему qemu, а не лаунчеру. Поток-специфичные приоритеты
    // (TIME_CRITICAL и т.п.) не трогаем: поиск аудиопотока по имени ни разу
    // не срабатывал, а чужие потоки с TIME_CRITICAL могут подвесить систему.
    if (SetPriorityClass(h, ABOVE_NORMAL_PRIORITY_CLASS))
        logToEmulatorFile(QString("Emulator PID %1 priority → ABOVE_NORMAL").arg(pid));
    else
        logToEmulatorFile(QString("WARNING: SetPriorityClass(%1) failed, error=%2")
                              .arg(pid).arg(GetLastError()));

    // Окно эмулятора лежит под прозрачным окном MediaStation, и Windows
    // считает процесс фоновым: может включить EcoQoS (E-ядра, сниженная
    // частота) и игнорировать запросы точного таймера. Явно отказываемся
    // от этого. Функция есть с Windows 8, нужный класс — с Windows 10 1709;
    // на старых системах вызов просто вернёт ошибку.
    struct PowerThrottlingState { ULONG Version; ULONG ControlMask; ULONG StateMask; };
    using SetProcessInformationFn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
    static const auto setProcInfo = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                "SetProcessInformation")));
    if (setProcInfo) {
        constexpr int   kProcessPowerThrottling   = 4;      // PROCESS_INFORMATION_CLASS
        constexpr ULONG kExecutionSpeed           = 0x1;    // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        constexpr ULONG kIgnoreTimerResolution    = 0x4;    // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
        PowerThrottlingState st { 1, kExecutionSpeed | kIgnoreTimerResolution, 0 };
        if (!setProcInfo(h, kProcessPowerThrottling, &st, sizeof(st))) {
            // Старые сборки Windows не знают флаг таймера — пробуем без него
            st.ControlMask = kExecutionSpeed;
            if (!setProcInfo(h, kProcessPowerThrottling, &st, sizeof(st)))
                logToEmulatorFile(QString("WARNING: power throttling opt-out failed, error=%1")
                                      .arg(GetLastError()));
            else
                logToEmulatorFile("Emulator power throttling disabled (execution speed)");
        } else {
            logToEmulatorFile("Emulator power throttling disabled (execution speed + timer)");
        }
    }
    CloseHandle(h);
#else
    Q_UNUSED(pid)
#endif
}

void AndroidTVModule::onEmulatorFinished(int exitCode)
{
    const bool wasBooting = m_booting;
    if (wasBooting && !m_exitRequested)
        logImportant(Logger::kLevelError,
                     QString("emulator exited during boot after %1 s, exit code %2, last adb: %3 "
                             "— see logs/Android_TV.log")
                         .arg(m_launchTimer.isValid() ? m_launchTimer.elapsed() / 1000 : 0)
                         .arg(exitCode)
                         .arg(m_lastBootPollOut.isEmpty() ? QStringLiteral("-") : m_lastBootPollOut.simplified()));
    else
        logImportant(Logger::kLevelInfo, "emulator finished, exit code " + QString::number(exitCode));

    stopKeepTopmostTimer();
    stopWindowPolling();
    stopBootPolling();
    stopHealthCheck();
    stopLogcat();
    stopKeyShell();
    stopConsole();
    removeWinEventHook();
    stopPositionSyncTimer();
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
    m_priorityAppliedPid = 0;
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
            m_exitRequested = true;   // это наше завершение, а не падение
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
                m_lastBootPollOut = res.left(200);
                // Android загрузился (так пишет сам эмулятор), но adb не пускают:
                // не ждём 5 минут впустую — показываем Android TV, а о том, что
                // команды через adb (кнопки пульта, блоклист) не работают, сообщаем.
                if (m_emuReportedBoot && res.contains(QLatin1String("unauthorized"))) {
                    if (++m_unauthorizedPolls >= 8) {
                        m_adbUnauthorized = true;
                        logImportant(Logger::kLevelError,
                                     "adb is not authorized by the device — showing Android TV without adb "
                                     "(remote keys via adb unavailable). Fix: Settings → Android TV → "
                                     "Profiles → Clear data, then launch again");
                        if (m_bus)
                            emit m_bus->moduleEvent(
                                Notifications::kModule, Notifications::kEventShow,
                                Notifications::makeEvent(
                                    QStringLiteral("Android TV запущен, но не разрешил подключение adb — "
                                                   "кнопки пульта могут не работать. Помогает «Очистить данные» "
                                                   "в Настройки → Android TV → Профили."),
                                    NotificationLevel::Warning, false, 0));
                        onBootCompleted();
                        return;
                    }
                } else {
                    m_unauthorizedPolls = 0;
                }
                if (m_bootPollCount % 10 == 0 && !res.isEmpty())
                    logToEmulatorFile("boot poll: " + m_lastBootPollOut);
                // Раз в 30 с — в основной журнал: видно, на чём застряла загрузка
                if (m_bootPollCount % 15 == 0)
                    logImportant(Logger::kLevelInfo,
                                 QString("still booting: %1 s, adb: %2")
                                     .arg(m_launchTimer.elapsed() / 1000)
                                     .arg(res.isEmpty() ? QStringLiteral("(no answer)")
                                                        : res.simplified().left(120)));
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
    if (!m_adbUnauthorized)
        logImportant(Logger::kLevelInfo, QString("Android booted in %1 s").arg(m_launchTimer.elapsed() / 1000));
    stopBootPolling();
    stopKeepTopmostTimer();
    setBooting(false);
    setRunning(true);
    setStatus("Android TV ready");
    emit bootCompleted();
    startHealthCheck();
    if (m_bus) emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("bootCompleted"), {});

    // adb reverse tcp:80/443/8080/8443 убран: ничего в программе этими портами
    // не пользуется, а запросы гостя к localhost:80/443 уходили через adb-туннель
    // на пустые порты хоста — лишняя работа adbd во время просмотра.

    // Блокировка рекламы — AdGuard Home на стороне Windows (см. startAdBlockProbe);
    // запись /system/etc/hosts через adb root убрана: на образах с Google Play
    // она не работала, а adb root перезапускал adbd
    startLogcat();
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

        if (m_adbUnauthorized) return;   // adb не пускают — проверять нечем

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

    // ── Метрика звука ─────────────────────────────────────────────────────
    // Раз в 10 минут спрашиваем AudioFlinger, сколько записей в аудиоустройство
    // задержалось и сколько было опустошений буфера. Рост этих чисел — это и
    // есть «заикание»; так его можно сравнивать между настройками, а не на слух.
    auto *audioStats = new QTimer(m_healthCheckTimer);   // удаляется вместе с ним
    audioStats->setInterval(10 * 60 * 1000);
    connect(audioStats, &QTimer::timeout, this, [this]() {
        if (m_adbUnauthorized) return;
        adb({"shell", "dumpsys", "media.audio_flinger"}, [this](const QString &out) {
            static const QRegularExpression kDelayed(QStringLiteral("Delayed writes:\\s*(\\d+)"));
            static const QRegularExpression kTotal(QStringLiteral("Total writes:\\s*(\\d+)"));
            static const QRegularExpression kUnder(QStringLiteral("underruns?[=:]\\s*(\\d+)"),
                                                   QRegularExpression::CaseInsensitiveOption);
            qint64 delayed = 0, total = 0, under = 0;
            for (auto it = kDelayed.globalMatch(out); it.hasNext();) delayed += it.next().captured(1).toLongLong();
            for (auto it = kTotal.globalMatch(out);   it.hasNext();) total   += it.next().captured(1).toLongLong();
            for (auto it = kUnder.globalMatch(out);   it.hasNext();) under   += it.next().captured(1).toLongLong();

            const qint64 dDelayed = delayed - m_audioDelayedWrites;
            const qint64 dUnder   = under   - m_audioUnderruns;
            m_audioDelayedWrites = delayed;
            m_audioUnderruns     = under;
            logImportant(Logger::kLevelInfo,
                         QString("Audio: total writes %1, delayed %2 (+%3), underruns %4 (+%5)")
                             .arg(total).arg(delayed).arg(qMax<qint64>(0, dDelayed))
                             .arg(under).arg(qMax<qint64>(0, dUnder)));
        });
    });
    m_audioDelayedWrites = 0;
    m_audioUnderruns     = 0;
    audioStats->start();

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
    // Полный logcat всё время работы ТВ — это постоянная работа adbd в госте,
    // adb-сервера и GUI-потока программы. Включается в профиле («Диагностика»).
    if (!m_diagnosticLogcat) {
        logToEmulatorFile("logcat: off (enable 'Android log' in profile for diagnostics)");
        return;
    }
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
            // Ограничение размера: многочасовая сессия давала сотни МБ
            constexpr qint64 kMaxLogcatBytes = 64LL * 1024 * 1024;
            if (m_logcatLogFile.size() > kMaxLogcatBytes) {
                m_logcatLogStream << "\n=== logcat stopped: file limit 64 MB reached ===\n";
                m_logcatLogStream.flush();
                m_logcatLogFile.close();
                QMetaObject::invokeMethod(this, [this]() { stopLogcat(); }, Qt::QueuedConnection);
                return;
            }
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
        // Файл журнала закроет обработчик finished; ждём только при выходе из программы
        if (m_shutdownDone) m_logcatProcess->waitForFinished(2000);
    } else if (m_logcatLogFile.isOpen()) {
        m_logcatLogStream << "=== logcat ended (forced) ===\n\n";
        m_logcatLogStream.flush();
        m_logcatLogFile.close();
    }
}

// ── Блокировка рекламы: AdGuard Home (аудит §23) ─────────────────────────────

namespace {
void notifyUser(EventBus *bus, const QString &text, bool error)
{
    if (!bus) return;
    emit bus->moduleEvent(Notifications::kModule, Notifications::kEventShow,
                          Notifications::makeEvent(text, error ? NotificationLevel::Error
                                                               : NotificationLevel::Info, true, 6000));
}
} // namespace

bool AndroidTVModule::adBlockEnabled() const
{
    return LocalModels::appSettings().value(QStringLiteral("androidtv/adBlock"), true).toBool();
}

void AndroidTVModule::setAdBlockEnabled(bool on)
{
    if (on == adBlockEnabled()) return;
    LocalModels::appSettings().setValue(QStringLiteral("androidtv/adBlock"), on);
    logImportant(Logger::kLevelInfo, QString("ad blocking %1 (applies on next TV launch)").arg(on ? "on" : "off"));
    emit adBlockChanged();
}

void AndroidTVModule::refreshAdBlockState()
{
    const QString st = m_adGuard ? AdGuardHome::stateName(m_adGuard->serviceState()) : QStringLiteral("missing");
    if (st != m_adBlockState) {
        m_adBlockState = st;
        emit adBlockChanged();
    }
}

void AndroidTVModule::startAdBlockProbe()
{
    m_adBlockDns.clear();
    m_afterAdBlockProbe = nullptr;
    m_adBlockProbePending = false;
    if (!adBlockEnabled()) { logToEmulatorFile("ad blocking: off in settings"); return; }
    if (!m_adGuard || !m_adGuard->isInstalled()) {
        logToEmulatorFile("ad blocking: AdGuard Home not installed (" +
                          (m_adGuard ? m_adGuard->exePath() : QString()) + ")");
        return;
    }
    m_adBlockProbePending = true;
    // До 3 попыток: служба могла только что запуститься
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, attempt](int n) {
    AdGuardHome::probeDns(QString::fromLatin1(AdGuardHome::kDnsHost), AdGuardHome::kDnsPort, 700, this,
                          [this, attempt, n](bool ok, int ms) {
        if (!ok && n < 3) { QTimer::singleShot(150, this, [attempt, n]() { (*attempt)(n + 1); }); return; }
        m_adBlockProbePending = false;
        if (ok) {
            m_adBlockDns = QString::fromLatin1(AdGuardHome::kDnsHost);
            logToEmulatorFile(QString("ad blocking: AdGuard Home answers on %1:53 (%2 ms)")
                                  .arg(m_adBlockDns).arg(ms));
        } else {
            refreshAdBlockState();
            logImportant(Logger::kLevelWarning,
                         QString("ad blocking: AdGuard Home does not answer on 127.0.0.1:53 (service %1) — "
                                 "using profile DNS").arg(m_adBlockState));
            if (m_adGuard)
                m_adGuard->diagnose([this](const QString &report) {
                    logImportant(Logger::kLevelWarning, "ad blocking diagnostics: " + report);
                });
        }
        if (m_afterAdBlockProbe) {
            auto next = std::move(m_afterAdBlockProbe);
            m_afterAdBlockProbe = nullptr;
            if (m_booting && !m_shutdownDone) next();
        }
    });
    };
    (*attempt)(1);
}

void AndroidTVModule::downloadAdBlock()
{
    if (!m_adGuard || m_adBlockBusy || m_adBlockDownload >= 0) return;
    m_adBlockDownload = 0.0;
    emit adBlockChanged();
    logImportant(Logger::kLevelInfo, QString("AdGuard Home: downloading %1").arg(AdGuardHome::kDownloadUrl));
    m_adGuard->download(
        [this](qreal p) {
            if (p - m_adBlockDownload >= 0.02) { m_adBlockDownload = p; emit adBlockChanged(); }
        },
        [this](bool ok, const QString &msg) {
            m_adBlockDownload = -1.0;
            refreshAdBlockState();
            emit adBlockChanged();
            logImportant(ok ? Logger::kLevelInfo : Logger::kLevelWarning,
                         "AdGuard Home download: " + (ok ? QStringLiteral("ok") : msg));
            notifyUser(m_bus, ok ? QStringLiteral("AdGuard Home скачан — теперь «Установить службу»")
                                 : QStringLiteral("AdGuard Home не скачан: ") + msg, !ok);
        });
}

void AndroidTVModule::installAdBlock()
{
    if (!m_adGuard || m_adBlockBusy) return;
    QString err;
    if (!m_adGuard->writeDefaultConfig(blocklistPath(), &err)) {
        logImportant(Logger::kLevelError, "AdGuard Home: cannot write config: " + err);
        notifyUser(m_bus, QStringLiteral("AdGuard Home: не удалось записать настройки — ") + err, true);
        return;
    }
    m_adBlockBusy = true;
    emit adBlockChanged();
    m_adGuard->installService([this](bool ok, const QString &msg) {
        m_adBlockBusy = false;
        refreshAdBlockState();
        emit adBlockChanged();
        logImportant(ok ? Logger::kLevelInfo : Logger::kLevelWarning,
                     "AdGuard Home service install: " + (ok ? QStringLiteral("ok") : msg));
        notifyUser(m_bus, ok ? QStringLiteral("Блокировка рекламы установлена — работает со следующего запуска ТВ")
                  : QStringLiteral("AdGuard Home не установлен: ") + msg, !ok);
    });
}

void AndroidTVModule::uninstallAdBlock()
{
    if (!m_adGuard || m_adBlockBusy) return;
    m_adBlockBusy = true;
    emit adBlockChanged();
    m_adGuard->uninstallService([this](bool ok, const QString &msg) {
        m_adBlockBusy = false;
        refreshAdBlockState();
        emit adBlockChanged();
        logImportant(Logger::kLevelInfo, "AdGuard Home service uninstall: " + (ok ? QStringLiteral("ok") : msg));
        if (!ok) notifyUser(m_bus, QStringLiteral("AdGuard Home: служба не удалена — ") + msg, true);
    });
}

void AndroidTVModule::openAdBlockPanel()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(AdGuardHome::kWebPort)));
}
