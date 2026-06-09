// modules/AndroidTVModule/AndroidTVLifecycle.cpp

#include "AndroidTVModule.h"
#include "LoggerModule.h"
#include "core/EventBus.h"
#include "SettingsManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDebug>
#include <QThread>
#include <QRegularExpression>

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
    return m_appDir + "/tools/android-tv/avd";
}

QString AndroidTVModule::blocklistPath() const
{
    return m_appDir + "/tools/android-tv/blocklists/default.txt";
}

QString AndroidTVModule::jdkBinPath() const
{
    return m_appDir + "/tools/jdk/bin";
}

void AndroidTVModule::loadEmulatorSettings(const AndroidTVProfile &profile)
{
    m_resolution = profile.resolution;

    // FIXED: replace "auto" or empty string with actual screen resolution
    if (m_resolution.isEmpty() || m_resolution == QLatin1String("auto")) {
        QSize screen = getCurrentScreenResolution();
        if (screen.width() > 0 && screen.height() > 0) {
            m_resolution = QString("%1x%2").arg(screen.width()).arg(screen.height());
            logImportant(Logger::kLevelWarning,
                         QString("Resolution '%1' replaced with detected screen resolution: %2")
                             .arg(profile.resolution).arg(m_resolution));
        } else {
            m_resolution = "1920x1080";
            logImportant(Logger::kLevelWarning,
                         "Could not detect screen resolution, using fallback 1920x1080");
        }
    }

    m_gpu        = profile.gpu;
    m_ramMb      = profile.ramMb;
    m_cores      = profile.cores;
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

void AndroidTVModule::launch()
{
    if (m_booting) {
        logImportant(Logger::kLevelInfo, "launch() ignored (already booting)");
        return;
    }
    if (m_running && m_emulatorProcess && m_emulatorProcess->state() == QProcess::Running) {
        emit emulatorReadyBehind();
        return;
    }

    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) {
        logImportant(Logger::kLevelError, "No active profile, cannot launch");
        return;
    }

    logImportant(Logger::kLevelInfo, "launching Android TV emulator with profile " + profile->name);
    refreshQcow2Size();

    // Сразу сигнализируем UI — анимация запуска стартует мгновенно.
    // Сам эмулятор запустится чуть позже, после того как ADB сервер поднят.
    setBooting(true);
    setStatus("Preparing ADB...");

    startDnsProxy();
    // startEmulator() вызывается из callback syncAdbKeyAndRestartServer,
    // ПОСЛЕ завершения adb start-server — чтобы эмулятор не стартовал
    // пока ADB сервер убит, что вызывало лаги при первом запуске.
    syncAdbKeyAndRestartServer(profile->avdName, [this]() {
        startEmulator();
    });
}

void AndroidTVModule::stop()
{
    if (!m_running && !m_booting) return;
    setStatus("Stopping emulator...");
    logImportant(Logger::kLevelInfo, "stop() requested");
    restoreMediaStation();
    stopLogcat();
    adb({"emu", "kill"});
    QTimer::singleShot(5000, this, [this]() {
        if (m_emulatorProcess && m_emulatorProcess->state() == QProcess::Running) {
            logImportant(Logger::kLevelWarning, "emulator didn't respond, killing");
            m_emulatorProcess->kill();
        }
        m_exitRequested = false;
    });
}

void AndroidTVModule::startDnsProxy()
{
    if (!QFile::exists(dnsProxyPath())) {
        logToEmulatorFile("dnsproxy.exe not found, skipping");
        m_dnsProxyPort = 0;
        return;
    }
    if (m_dnsProxyProcess && m_dnsProxyProcess->state() == QProcess::Running) {
        logToEmulatorFile("DNS proxy already running");
        return;
    }

    m_dnsProxyPort = 5300;

    QStringList upstreams;
    if (!m_dnsServer.isEmpty()) {
        for (const QString &s : m_dnsServer.split(',', Qt::SkipEmptyParts))
            upstreams << s.trimmed();
    }
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
    m_dnsProxyProcess->start();

    if (!m_dnsProxyProcess->waitForStarted(2000)) {
        logImportant(Logger::kLevelError, "failed to start dnsproxy: " + m_dnsProxyProcess->errorString());
        m_dnsProxyPort = 0;
        return;
    }

    logToEmulatorFile("DNS proxy started PID " + QString::number(m_dnsProxyProcess->processId()) +
                      " on port " + QString::number(m_dnsProxyPort));

    connect(m_dnsProxyProcess, &QProcess::readyReadStandardError, this, [this]() {
        const QString err = QString::fromUtf8(m_dnsProxyProcess->readAllStandardError()).trimmed();
        if (!err.isEmpty()) logToEmulatorFile("[dnsproxy] " + err);
    });
    connect(m_dnsProxyProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString out = QString::fromUtf8(m_dnsProxyProcess->readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) logToEmulatorFile("[dnsproxy] " + out);
    });

    connect(m_dnsProxyProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                if (m_dnsProxyPort != 0) {
                    logImportant(Logger::kLevelWarning, "DNS proxy exited unexpectedly with code " + QString::number(code));
                    m_dnsProxyPort = 0;
                }
            });
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

bool AndroidTVModule::patchAvdIni(const AndroidTVProfile &profile)
{
    const QString iniPath = avdHomePath() + "/" + profile.avdName + ".ini";
    const QString avdDir  = QDir::toNativeSeparators(avdHomePath() + "/" + profile.avdName + ".avd");

    QFile file(iniPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // maybe file doesn't exist, create it
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logImportant(Logger::kLevelError, "cannot create AVD ini: " + iniPath);
            return false;
        }
        QTextStream out(&file);
        out << "path=" + avdDir << "\n";
        file.close();
        logToEmulatorFile("Created AVD ini: " + iniPath);
        return true;
    }

    QStringList lines;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.startsWith("path=")) lines << "path=" + avdDir;
        else lines << line;
    }
    file.close();

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logImportant(Logger::kLevelError, "cannot write AVD ini: " + iniPath);
        return false;
    }
    QTextStream out(&file);
    for (const QString &l : lines) out << l << "\n";
    file.close();

    logToEmulatorFile("AVD ini patched: path=" + avdDir);
    return true;
}

// FIXED: added resolutionOverride parameter
void AndroidTVModule::patchAvdConfig(const AndroidTVProfile &profile, const QString &resolutionOverride)
{
    QString resolution = resolutionOverride.isEmpty() ? profile.resolution : resolutionOverride;

    int w = resolution.section('x', 0, 0).toInt();
    int h = resolution.section('x', 1, 1).toInt();
    if (w <= 0 || h <= 0) {
        w = 1920; h = 1080;
        logToEmulatorFile(QString("WARNING: invalid resolution '%1', using 1920x1080").arg(resolution));
    }

    const QString cfgPath = avdHomePath() + "/" + profile.avdName + ".avd/config.ini";
    QFile file(cfgPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QDir().mkpath(avdHomePath() + "/" + profile.avdName + ".avd");
        file.setFileName(cfgPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logToEmulatorFile("WARNING: cannot create config.ini: " + cfgPath);
            return;
        }
        QTextStream out(&file);
        out << "hw.lcd.width=" << w << "\n";
        out << "hw.lcd.height=" << h << "\n";
        out << "image.sysdir.1=" << QDir::toNativeSeparators(m_appDir + "/" + profile.systemImagePath) << "\n";
        file.close();
        return;
    }

    const QString imageSysDir = QDir::toNativeSeparators(m_appDir + "/" + profile.systemImagePath);
    const QString sdcardPath = QDir::toNativeSeparators(avdHomePath() + "/" + profile.avdName + ".avd/sdcard.img");

    QMap<QString, QString> overrides = {
        { "hw.lcd.width",   QString::number(w) },
        { "hw.lcd.height",  QString::number(h) },
        { "skin.name",      resolution },
        { "skin.path",      "_no_skin"          },
        { "skin.dynamic",   "yes"               },
        { "image.sysdir.1", imageSysDir         },
        { "sdcard.path",    sdcardPath          },
        { "hw.audioInput",  "no"                },
        { "hw.audioOutput", "yes"               },
        { "hw.soundcard",   "hda"               },
        { "hw.gpu.enabled", "yes"               },
        { "hw.gpu.mode",    "host"              },
        { "vm.heapSize",    "256"               },
        { "fastboot.forceFastBoot", "yes"       }
    };

    QStringList lines;
    QSet<QString> written;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QString key  = line.section('=', 0, 0).trimmed();
        if (overrides.contains(key)) {
            lines << key + " = " + overrides[key];
            written.insert(key);
        } else {
            lines << line;
        }
    }
    file.close();

    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        if (!written.contains(it.key()))
            lines << it.key() + " = " + it.value();

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logToEmulatorFile("WARNING: cannot write config.ini: " + cfgPath);
        return;
    }
    QTextStream out(&file);
    for (const QString &l : lines) out << l << "\n";
    file.close();

    logToEmulatorFile(QString("AVD config.ini patched: lcd=%1x%2, sysdir=%3")
                          .arg(w).arg(h).arg(imageSysDir));
}

void AndroidTVModule::syncAdbKeyAndRestartServer(const QString &avdName, std::function<void()> onReady)
{
    const QString adbKeyPubPath = QDir::homePath() + "/.android/adbkey.pub";
    const QString savedKeyPath  = avdHomePath() + "/" + avdName + ".avd/adb_keys.txt";

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

    const bool keyMismatch   = !currentKey.isEmpty() && (currentKey != savedKey);
    const bool firstRun      = !currentKey.isEmpty() && savedKey.isEmpty();

    if (keyMismatch) {
        logImportant(Logger::kLevelWarning,
                     "ADB key changed — wiping userdata to re-authorize for " + avdName);

        const QString avdDir = avdHomePath() + "/" + avdName + ".avd";
        for (const QString &f : {
                                 avdDir + "/userdata.img.qcow2",
                                 avdDir + "/userdata-qemu.img",
                                 avdDir + "/userdata-qemu.img.qcow2" }) {
            if (QFile::exists(f)) {
                QFile::remove(f)
                ? logToEmulatorFile("Wiped: " + QFileInfo(f).fileName())
                : logToEmulatorFile("WARNING: could not wipe: " + f);
            }
        }
        QDir(avdDir + "/snapshots/default_boot").removeRecursively();
        logToEmulatorFile("Wiped snapshot default_boot");
    }

    if (keyMismatch || firstRun) {
        QFile nf(savedKeyPath);
        if (nf.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            nf.write(currentKey.toUtf8());
            logToEmulatorFile(firstRun
                                  ? "First run: saved ADB key for " + avdName
                                  : "Saved new ADB key for " + avdName);
        }
    }

    auto *thread = QThread::create([this, onReady]() {
        QProcessEnvironment adbEnv = QProcessEnvironment::systemEnvironment();
        adbEnv.insert("JAVA_HOME",             QDir::toNativeSeparators(m_appDir + "/tools/jdk"));
        adbEnv.insert("ANDROID_SDK_ROOT",      QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
        adbEnv.insert("ANDROID_HOME",          QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
        adbEnv.insert("ANDROID_AVD_HOME",      QDir::toNativeSeparators(avdHomePath()));
        adbEnv.insert("ANDROID_EMULATOR_HOME", QDir::toNativeSeparators(avdHomePath()));
        adbEnv.insert("PATH",
                      QDir::toNativeSeparators(m_appDir + "/tools/android-sdk/platform-tools")
                          + ";" + QDir::toNativeSeparators(jdkBinPath())
                          + ";" + adbEnv.value("PATH"));

        QProcess kill;
        kill.setProgram(adbPath());
        kill.setArguments({"kill-server"});
        kill.setProcessEnvironment(adbEnv);
        kill.start();
        kill.waitForFinished(3000);
        QMetaObject::invokeMethod(this, [this]() {
            logToEmulatorFile("adb kill-server: done");
        }, Qt::QueuedConnection);

        QProcess start;
        start.setProgram(adbPath());
        start.setArguments({"start-server"});
        start.setProcessEnvironment(adbEnv);
        start.start();
        start.waitForFinished(5000);
        const QString out = QString::fromUtf8(start.readAllStandardOutput()).trimmed();
        QMetaObject::invokeMethod(this, [this, out, onReady]() {
            logToEmulatorFile("adb start-server: " + (out.isEmpty() ? "ok" : out));
            if (onReady) onReady();
        }, Qt::QueuedConnection);
    });
    thread->setObjectName("adb-restart");
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

bool AndroidTVModule::isSystemQcow2Valid(const AndroidTVProfile &profile)
{
    const QString qcow2Path = avdHomePath() + "/" + profile.avdName + ".avd/system.img.qcow2";
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
        logToEmulatorFile("qemu-img info timeout, assuming system.img.qcow2 is valid");
        return true;
    }

    QString output = proc.readAllStandardOutput();
    QRegularExpression re("backing file:\\s*(.+)");
    QRegularExpressionMatch match = re.match(output);
    if (!match.hasMatch()) {
        logToEmulatorFile("Could not parse backing file from qemu-img info");
        return true;
    }

    QString actualBacking = match.captured(1).trimmed();
    QString expectedBacking = QDir::toNativeSeparators(
        m_appDir + "/" + profile.systemImagePath + "/system.img");

    bool valid = (QFileInfo(actualBacking).canonicalFilePath() ==
                  QFileInfo(expectedBacking).canonicalFilePath());

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
    const QString qcow2Path = avdHomePath() + "/" + profile.avdName + ".avd/system.img.qcow2";
    if (!QFile::exists(qcow2Path))
        return;

    if (isSystemQcow2Valid(profile))
        return;

    logImportant(Logger::kLevelWarning,
                 "system.img.qcow2 backing file mismatch, deleting it to regenerate");

    if (QFile::remove(qcow2Path)) {
        logToEmulatorFile("Deleted system.img.qcow2");
    } else {
        logToEmulatorFile("WARNING: failed to delete system.img.qcow2");
    }

    QDir avdDir(avdHomePath() + "/" + profile.avdName + ".avd");
    QStringList userdataFiles = avdDir.entryList(
        {"userdata*.img", "userdata*.img.qcow2"}, QDir::Files);
    for (const QString &f : userdataFiles) {
        if (QFile::remove(avdDir.absoluteFilePath(f)))
            logToEmulatorFile("Removed " + f);
        else
            logToEmulatorFile("WARNING: could not remove " + f);
    }
}

void AndroidTVModule::startEmulator()
{
    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) {
        logImportant(Logger::kLevelError, "No active profile, cannot start emulator");
        return;
    }

    if (!QFile::exists(emulatorPath())) {
        logImportant(Logger::kLevelError, "emulator.exe not found: " + emulatorPath());
        setStatus("Error: emulator not found");
        return;
    }

    loadEmulatorSettings(*profile);   // m_resolution is already corrected here

    repairSystemQcow2(*profile);

    {
        QDir avdDir(avdHomePath() + "/" + profile->avdName + ".avd");
        const auto locks = avdDir.entryList({"*.lock"}, QDir::Files);
        for (const QString &lock : locks) {
            if (QFile::remove(avdDir.absoluteFilePath(lock)))
                logToEmulatorFile("Removed stale AVD lock: " + lock);
            else
                logToEmulatorFile("WARNING: could not remove AVD lock: " + lock);
        }
    }

    if (!patchAvdIni(*profile)) {
        setBooting(false);
        setStatus("Error preparing AVD");
        return;
    }
    // FIXED: pass m_resolution (already corrected) to patchAvdConfig
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

    startKeepTopmostTimer();

    m_emulatorProcess = new QProcess(this);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString jdkBin  = QDir::toNativeSeparators(jdkBinPath());
    const QString sdkRoot = QDir::toNativeSeparators(m_appDir + "/tools/android-sdk");
    const QString avdHome = QDir::toNativeSeparators(avdHomePath());

    env.insert("JAVA_HOME",              QDir::toNativeSeparators(m_appDir + "/tools/jdk"));
    env.insert("ANDROID_SDK_ROOT",       sdkRoot);
    env.insert("ANDROID_HOME",           sdkRoot);
    env.insert("ANDROID_AVD_HOME",       avdHome);
    env.insert("ANDROID_EMULATOR_HOME",  avdHome);
    env.insert("SKIP_JDK_VERSION_CHECK", "true");
    env.insert("PATH", jdkBin + ";" + env.value("PATH"));

    env.insert("ANDROID_EMULATOR_RENDERER", "host");
    env.insert("ANDROID_EMU_MEDIA_DECODER_CUDA_VPX", "1");

    // Audio timer: only set if manually specified
    QVariant audioTimerVal = profile->audioTimerMs;
    if (audioTimerVal.type() == QVariant::Int) {
        int audioTimerMs = audioTimerVal.toInt();
        env.insert("QEMU_AUDIO_TIMER_PERIOD", QString::number(audioTimerMs));
        logToEmulatorFile(QString("Audio timer override: %1 ms").arg(audioTimerMs));
    } else {
        logToEmulatorFile("Audio timer: auto");
    }

    // Dsound latency: only set if manually specified
    QVariant dsoundLatencyVal = profile->dsoundLatencyMs;
    if (dsoundLatencyVal.type() == QVariant::Int) {
        int dsoundLatencyMs = dsoundLatencyVal.toInt();
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

    m_emulatorProcess->setProcessEnvironment(env);
    m_emulatorProcess->setProgram(emulatorPath());

    const QString dnsArg = profile->dnsServer.isEmpty() ? "8.8.8.8,1.1.1.1" : profile->dnsServer;
    logToEmulatorFile("Using DNS: " + dnsArg);

    QStringList args;
    args << "-avd"        << profile->avdName
         << "-dns-server" << dnsArg
         << "-gpu"        << m_gpu
         << "-skin"       << m_resolution   // m_resolution is already correct
         << "-memory"     << QString::number(m_ramMb)
         << "-cores"      << QString::number(m_cores)
         << "-netfast"
         << "-accel"      << "on"
         << "-skip-adb-auth"
         << "-feature"    << "GLDirectMem"
         << "-feature"    << "GLDMA"
         << "-feature"    << "VulkanNativeSwapchain";

    if (m_writableSystem)  args << "-writable-system";
    if (m_noSnapshot)      args << "-no-snapshot";
    if (m_noSnapshotSave)  args << "-no-snapshot-save";

    args << "-no-boot-anim";
    args << "-no-metrics";
    args << "-audio" << m_audioDriver;

    m_emulatorProcess->setArguments(args);

    connect(m_emulatorProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const QString out = QString::fromUtf8(m_emulatorProcess->readAllStandardOutput()).trimmed();
        if (!out.isEmpty()) logToEmulatorFile(out, true);
    });
    connect(m_emulatorProcess, &QProcess::readyReadStandardError, this, [this]() {
        const QString err = QString::fromUtf8(m_emulatorProcess->readAllStandardError()).trimmed();
        if (!err.isEmpty()) logToEmulatorFile(err, true);
    });

    connect(m_emulatorProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                logImportant(Logger::kLevelInfo, "emulator finished, exit code " + QString::number(code));
                stopKeepTopmostTimer();
                stopWindowPolling();
                stopBootPolling();
                stopHealthCheck();
                stopLogcat();
                removeWinEventHook();
#ifdef Q_OS_WIN
                if (m_timerResolutionSet) {
                    timeEndPeriod(1);
                    m_timerResolutionSet = false;
                    logToEmulatorFile("Timer resolution restored to default");
                }
#endif
                setBooting(false);
                setRunning(false);
                setStatus("");
                restoreMediaStation();
                emit emulatorClosed();
                if (m_bus) emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("emulatorClosed"), {});
                m_exitRequested = false;
            });

    connect(m_emulatorProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        logImportant(Logger::kLevelError, "emulator error: " + QString::number(err) + " - " + m_emulatorProcess->errorString());
        stopKeepTopmostTimer();
        stopLogcat();
        setBooting(false);
        setRunning(false);
        setStatus("Error launching emulator");
        m_exitRequested = false;
    });

    connect(m_emulatorProcess, &QProcess::started, this, [this]() {
        m_emulatorPid        = m_emulatorProcess->processId();
        m_initialEmulatorPid = m_emulatorPid;
        logToEmulatorFile("emulator process started PID " + QString::number(m_emulatorPid));

#ifdef Q_OS_WIN
        HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(m_emulatorPid));
        if (hProc) {
            if (SetPriorityClass(hProc, ABOVE_NORMAL_PRIORITY_CLASS))
                logToEmulatorFile("Emulator process priority set to ABOVE_NORMAL");
            else
                logToEmulatorFile("WARNING: SetPriorityClass failed, error=" + QString::number(GetLastError()));
            CloseHandle(hProc);
        }

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
                        THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                        FALSE, te.th32ThreadID);
                    if (!hThread) continue;

                    bool isAudioThread = false;

                    if (getThreadDesc) {
                        PWSTR desc = nullptr;
                        if (SUCCEEDED(getThreadDesc(hThread, &desc)) && desc) {
                            const QString name = QString::fromWCharArray(desc);
                            LocalFree(desc);
                            if (name.contains(QLatin1String("audio"), Qt::CaseInsensitive) ||
                                name.contains(QLatin1String("dsound"), Qt::CaseInsensitive) ||
                                name.contains(QLatin1String("winmm"), Qt::CaseInsensitive) ||
                                name.contains(QLatin1String("sound"), Qt::CaseInsensitive)) {
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

            if (boostedAudio == 0) {
                logToEmulatorFile(
                    "NOTE: audio thread not identified by name — "
                    "all threads set to ABOVE_NORMAL.");
            }
        });
#endif

        installWinEventHook();
        startWindowPolling();
        startBootPolling();
    });

    m_emulatorProcess->start();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
}

void AndroidTVModule::startBootPolling()
{
    m_bootPollCount = 0;
    setStatus("Loading Android TV...");
    m_bootPollTimer = new QTimer(this);
    m_bootPollTimer->setInterval(kBootPollIntervalMs);

    connect(m_bootPollTimer, &QTimer::timeout, this, [this]() {
        ++m_bootPollCount;
        if (m_bootPollCount > kBootPollMaxTries) {
            logImportant(Logger::kLevelError, "boot polling timeout");
            stopBootPolling();
            setBooting(false);
            setStatus("Boot timeout exceeded");
            return;
        }
        if (m_bootPollCount % 5 == 0) {
            const int elapsed = m_bootPollCount * kBootPollIntervalMs / 1000;
            setStatus(QString("Loading... %1 sec").arg(elapsed));
            logToEmulatorFile(QString("boot polling: %1 sec").arg(elapsed));
        }
        adb({"shell", "getprop", "sys.boot_completed"}, [this](const QString &out) {
            if (out.trimmed() != QLatin1String("1")) return;

            logImportant(Logger::kLevelInfo, "sys.boot_completed = 1");
            stopBootPolling();
            setBooting(false);
            setRunning(true);
            setStatus("Android TV ready");
            emit bootCompleted();
            startHealthCheck();
            startLogcat();
            if (m_bus) emit m_bus->moduleEvent(QStringLiteral("androidtv"), QStringLiteral("bootCompleted"), {});

            // Синхронизируем timezone и время с хост-машиной.
            // Используем service call alarm — работает без root на production образе.
            {
                const QTimeZone hostTz = QTimeZone::systemTimeZone();
                const QString tzId = QString::fromLatin1(hostTz.id());

                adb({"shell", "settings", "put", "global", "auto_time", "0"}, {});
                adb({"shell", "settings", "put", "global", "auto_time_zone", "0"}, {});

                // service call alarm 3 = AlarmManager.setTimeZone(String)
                adb({"shell", "service", "call", "alarm", "3", "s16", tzId},
                    [this, tzId](const QString &) {
                        logToEmulatorFile("[Time] timezone synced: " + tzId);
                });

                // service call alarm 2 = AlarmManager.setTime(long millis)
                const qint64 epochMs = QDateTime::currentMSecsSinceEpoch();
                adb({"shell", "service", "call", "alarm", "2", "i64", QString::number(epochMs)},
                    [this, epochMs](const QString &) {
                        logToEmulatorFile("[Time] time synced: " + QString::number(epochMs));
                });
            }

            logToEmulatorFile("Setting up ADB reverse port forwarding...");
            adb({"reverse", "tcp:80", "tcp:80"}, [this](const QString &res) {
                logToEmulatorFile("adb reverse tcp:80 -> " + res.trimmed());
            });
            adb({"reverse", "tcp:443", "tcp:443"}, [this](const QString &res) {
                logToEmulatorFile("adb reverse tcp:443 -> " + res.trimmed());
            });
            adb({"reverse", "tcp:53", "tcp:53"}, [this](const QString &res) {
                logToEmulatorFile("adb reverse tcp:53 -> " + res.trimmed());
            });
            adb({"reverse", "tcp:8080", "tcp:8080"}, [this](const QString &res) {
                logToEmulatorFile("adb reverse tcp:8080 -> " + res.trimmed());
            });
            adb({"reverse", "tcp:8443", "tcp:8443"}, [this](const QString &res) {
                logToEmulatorFile("adb reverse tcp:8443 -> " + res.trimmed());
            });

            applyBlocklist();
            emit emulatorReadyBehind();
        });
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
}

void AndroidTVModule::startHealthCheck()
{
    if (m_healthCheckTimer) return;
    m_healthCheckTimer = new QTimer(this);
    m_healthCheckTimer->setInterval(30 * 60 * 1000);

    connect(m_healthCheckTimer, &QTimer::timeout, this, [this]() {
        const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
        if (!profile) return;
        const QString qcow2Path = avdHomePath() + "/" + profile->avdName + ".avd/userdata-qemu.img.qcow2";
        QFileInfo fi(qcow2Path);
        if (fi.exists()) {
            const qint64 sizeMb = fi.size() / 1024 / 1024;
            logImportant(Logger::kLevelInfo,
                         QString("Health: userdata qcow2 = %1 MB").arg(sizeMb));
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
                        QStringLiteral("androidtv"),
                        QStringLiteral("qcow2Warning"),
                        QVariantMap{{"sizeMb", sizeMb}, {"limitMb", (int)kMaxQcow2SizeMb}});
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
                logImportant(Logger::kLevelWarning,
                             "Health: emulator not responding to adb — may be frozen");
                if (m_bus)
                    emit m_bus->moduleEvent(
                        QStringLiteral("androidtv"),
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

void AndroidTVModule::adb(const QStringList &args, std::function<void(QString)> callback)
{
    auto *proc = new QProcess(this);
    proc->setProgram(adbPath());
    proc->setArguments(args);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("JAVA_HOME",             QDir::toNativeSeparators(m_appDir + "/tools/jdk"));
    env.insert("ANDROID_SDK_ROOT",      QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
    env.insert("ANDROID_HOME",          QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
    env.insert("ANDROID_AVD_HOME",      QDir::toNativeSeparators(avdHomePath()));
    env.insert("ANDROID_EMULATOR_HOME", QDir::toNativeSeparators(avdHomePath()));
    env.insert("PATH", QDir::toNativeSeparators(m_appDir + "/tools/android-sdk/platform-tools")
                           + ";" + jdkBinPath() + ";" + env.value("PATH"));
    proc->setProcessEnvironment(env);

    auto *watchdog = new QTimer(proc);
    watchdog->setSingleShot(true);
    watchdog->setInterval(10000);
    connect(watchdog, &QTimer::timeout, this, [proc, args, this]() {
        logToEmulatorFile(QString("adb watchdog: timeout 10s, killing: %1")
                              .arg(args.join(QLatin1Char(' '))));
        proc->kill();
    });

    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [proc, callback](int, QProcess::ExitStatus) {
                if (callback) callback(QString::fromUtf8(proc->readAllStandardOutput()));
                proc->deleteLater();
            });

    proc->start();
    watchdog->start();
}

void AndroidTVModule::applyBlocklist()
{
    QFile list(blocklistPath());
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logToEmulatorFile("blocklist not found, skipping");
        return;
    }

    QStringList entries;
    entries << "127.0.0.1 localhost";

    QTextStream in(&list);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        entries << "0.0.0.0 " + line;
    }

    const QString tmpPath = m_appDir + "/tools/android-tv/config/hosts.tmp";
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logImportant(Logger::kLevelError, "cannot write temp hosts file");
        return;
    }
    QTextStream out(&tmp);
    for (const auto &e : entries) out << e << "\n";
    tmp.close();

    logToEmulatorFile(QString("pushing %1 entries to /etc/hosts").arg(entries.size()));

    adb({"shell", "mount", "-o", "remount,rw", "/"}, [this, tmpPath](const QString &) {
        adb({"push", QDir::toNativeSeparators(tmpPath), "/etc/hosts"}, [this](const QString &out) {
            logToEmulatorFile("hosts push: " + out.trimmed());
        });
    });
}

void AndroidTVModule::refreshQcow2Size()
{
    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    if (!profile) {
        m_qcow2SizeMb = -1;
        emit qcow2SizeMbChanged();
        return;
    }
    const QString path = avdHomePath() + "/" + profile->avdName + ".avd/userdata-qemu.img.qcow2";
    QFileInfo fi(path);
    const qint64 sizeMb = fi.exists() ? fi.size() / 1024 / 1024 : -1;
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

    const QString avdDir = avdHomePath() + "/" + profile->avdName + ".avd";
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

void AndroidTVModule::startLogcat()
{
    const AndroidTVProfile *profile = m_profileManager ? m_profileManager->activeProfile() : nullptr;
    QString avdName = profile ? profile->avdName : "Unknown";
    if (m_logcatProcess && m_logcatProcess->state() != QProcess::NotRunning) {
        logToEmulatorFile("logcat already running");
        return;
    }

    m_logcatLogFile.setFileName(logcatLogPath());
    if (!m_logcatLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        logToEmulatorFile("ERROR: cannot open logcat log file: " + logcatLogPath());
        return;
    }
    m_logcatLogStream.setDevice(&m_logcatLogFile);
    m_logcatLogStream << "=== Android TV logcat started at "
                      << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")
                      << " Profile: " << avdName << " ===\n";
    m_logcatLogStream.flush();

    m_logcatProcess = new QProcess(this);
    m_logcatProcess->setProgram(adbPath());
    m_logcatProcess->setArguments({"logcat", "-v", "threadtime"});

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("JAVA_HOME",             QDir::toNativeSeparators(m_appDir + "/tools/jdk"));
    env.insert("ANDROID_SDK_ROOT",      QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
    env.insert("ANDROID_HOME",          QDir::toNativeSeparators(m_appDir + "/tools/android-sdk"));
    env.insert("ANDROID_AVD_HOME",      QDir::toNativeSeparators(avdHomePath()));
    env.insert("ANDROID_EMULATOR_HOME", QDir::toNativeSeparators(avdHomePath()));
    env.insert("PATH", QDir::toNativeSeparators(m_appDir + "/tools/android-sdk/platform-tools")
                           + ";" + jdkBinPath() + ";" + env.value("PATH"));
    m_logcatProcess->setProcessEnvironment(env);

    connect(m_logcatProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        QByteArray data = m_logcatProcess->readAllStandardOutput();
        if (!data.isEmpty()) {
            m_logcatLogStream << QString::fromUtf8(data);
            m_logcatLogStream.flush();
        }
    });

    connect(m_logcatProcess, &QProcess::readyReadStandardError, this, [this]() {
        QByteArray data = m_logcatProcess->readAllStandardError();
        if (!data.isEmpty()) {
            logToEmulatorFile("[logcat stderr] " + QString::fromUtf8(data).trimmed());
        }
    });

    connect(m_logcatProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
                logToEmulatorFile("logcat process finished, exit code " + QString::number(code));
                if (m_logcatLogFile.isOpen()) {
                    m_logcatLogStream << "=== logcat ended ===\n\n";
                    m_logcatLogStream.flush();
                    m_logcatLogFile.close();
                }
                if (m_logcatProcess) {
                    m_logcatProcess->deleteLater();
                    m_logcatProcess = nullptr;
                }
            });

    m_logcatProcess->start();
    logToEmulatorFile("logcat started, writing to " + logcatLogPath());
}

void AndroidTVModule::stopLogcat()
{
    if (m_logcatProcess && m_logcatProcess->state() != QProcess::NotRunning) {
        logToEmulatorFile("Stopping logcat...");
        m_logcatProcess->terminate();
        if (!m_logcatProcess->waitForFinished(3000)) {
            m_logcatProcess->kill();
            m_logcatProcess->waitForFinished(2000);
        }
    } else if (m_logcatLogFile.isOpen()) {
        m_logcatLogStream << "=== logcat ended (forced) ===\n\n";
        m_logcatLogStream.flush();
        m_logcatLogFile.close();
    }
}
