// JoystickModule.cpp (исправленная финальная версия)
#include "JoystickModule.h"
#include "core/EventBus.h"

#include <QCoreApplication>
#include <QSet>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>
#include <QFutureWatcher>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#endif

static constexpr const char *kProfileXInput  = "XInput";
static constexpr const char *kProfileDS4     = "DS4";
static constexpr const char *kProfileDefault = "Default";

// ── Win32: проверка наличия процесса по имени ─────────────────────────────────
static bool isProcessRunning(const QString &exeName)
{
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    const std::wstring target = exeName.toStdWString();

    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, target.c_str()) == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    Q_UNUSED(exeName)
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────

JoystickModule::JoystickModule(QObject *parent)
    : QObject(parent)
    , m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(kPollIntervalMs);
    m_pollTimer->setSingleShot(false);
    connect(m_pollTimer, &QTimer::timeout, this, &JoystickModule::onPollTimer);
}

JoystickModule::~JoystickModule()
{
    shutdown();
}

// ── IModule ───────────────────────────────────────────────────────────────────

void JoystickModule::initialize(EventBus *bus, SettingsManager *)
{
    // В репозитории папка называется DS4-Windows (external_tools/DS4-Windows),
    // а код искал только tools/DS4Windows — поддерживаем оба варианта
    const QString appDir = QCoreApplication::applicationDirPath();
    QString ds4Dir = appDir + "/tools/DS4Windows";
    for (const QString &candidate : {appDir + "/tools/DS4Windows", appDir + "/tools/DS4-Windows"}) {
        if (QFile::exists(candidate + "/DS4Windows.exe")) { ds4Dir = candidate; break; }
    }
    m_ds4ExePath  = ds4Dir + "/DS4Windows.exe";
    m_ds4CmdPath  = ds4Dir + "/DS4WindowsCmd.exe";
    m_dataDirPath = QCoreApplication::applicationDirPath() + "/data";
    m_bus = bus;

    loadCustomNames();

    connect(bus, &EventBus::moduleEvent, this,
            [this](const QString &mod, const QString &ev, const QVariantMap &data) {
                // Android TV на экране: панель геймпадов не видна, а каждый опрос —
                // несколько запусков DS4WindowsCmd.exe, которые отнимают процессор
                // у эмулятора (одна из причин рывков звука)
                if (mod == QLatin1String("androidtv")) {
                    if (ev == QLatin1String("shown"))
                        m_androidTvShown = data.value("shown").toBool();
                    else if (ev == QLatin1String("emulatorClosed") || ev == QLatin1String("launchFailed"))
                        m_androidTvShown = false;
                    return;
                }
                if (mod != "game") return;
                if (ev == "inputType")
                    onGameInputType(data["inputTypes"].toStringList());
                else if (ev == "appeared")
                    m_gameRunning = true;   // опрос на паузе — см. onPollTimer
                else if (ev == "closed" || ev == "failed") {
                    m_gameRunning = false;
                    if (ev == "closed") onGameClosed();
                }
            });
}

void JoystickModule::shutdown()
{
    m_activated = false;
    m_stopping.store(true);
    m_pollTimer->stop();

    // cancel() у QtConcurrent::run НЕ останавливает уже запущенную задачу, а она
    // держит `this` (пути, сигналы, m_weStartedDs4). Раньше при выходе в первые
    // секунды (запуск DS4Windows) задача обращалась к уже удалённому модулю.
    // Теперь: флаг m_stopping заставляет циклы ожидания и IPC-вызовы выйти
    // быстро, а ниже мы дожидаемся их завершения (до долей секунды).
    QFuture<QList<ControllerInfo>> pendingStartup;
    QFuture<QList<ControllerInfo>> pendingPoll;
    if (m_startupWatcher) pendingStartup = m_startupWatcher->future();
    if (m_pollWatcher)    pendingPoll    = m_pollWatcher->future();

    // Отменяем фоновые задачи без блокирующего ожидания.
    // QFutureWatcher::cancel() помечает future как отменённую,
    // но фоновый поток может ещё работать — это нормально,
    // он завершится сам через kCmdTimeoutMs.
    if (m_startupWatcher) {
        m_startupWatcher->cancel();
        // Не вызываем waitForFinished() — startupSync делает IPC-запросы
        // к DS4Windows с таймаутом 15 секунд, ждать нельзя.
        // deleteLater безопасен: Qt удалит объект когда future завершится.
        m_startupWatcher->disconnect();
        m_startupWatcher->deleteLater();
        m_startupWatcher = nullptr;
    }
    if (m_pollWatcher) {
        m_pollWatcher->cancel();
        m_pollWatcher->disconnect();
        m_pollWatcher->deleteLater();
        m_pollWatcher = nullptr;
    }
    pendingStartup.waitForFinished();
    pendingPoll.waitForFinished();
    // Смена профиля после закрытия игры: задачи держат `this`. С m_stopping
    // runCmdSync выходит за доли секунды — ждём, чтобы не обратиться к
    // удалённому модулю при выходе сразу после игры.
    for (QFuture<void> &f : m_profileTasks)
        f.waitForFinished();
    m_profileTasks.clear();

    // Если мы запустили DS4Windows — останавливаем его асинхронно.
    // QThreadPool::globalInstance() живёт до конца приложения,
    // задача успеет завершиться даже после выхода из этой функции.
    if (m_weStartedDs4.load()) {
        m_weStartedDs4.store(false);
        const QString cmdPath = m_ds4CmdPath;
        QThreadPool::globalInstance()->start([cmdPath]() {
            auto run = [&](const QString &cmd) {
                QProcess p;
                p.setProgram(cmdPath);
                p.setArguments({ "-command", cmd });
                p.start();
                p.waitForFinished(3000);
            };
            run("Stop");
            QThread::msleep(500);
            run("Shutdown");
        });
    }
}

// ── Кастомные имена ───────────────────────────────────────────────────────────

void JoystickModule::loadCustomNames()
{
    QFile f(m_dataDirPath + "/controller_names.json");
    if (!f.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return;

    m_customNames.clear();
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it)
        m_customNames[it.key().toUpper()] = it.value().toString();

    qDebug() << "[Joystick] Loaded" << m_customNames.size() << "custom names";
}

void JoystickModule::saveCustomNames() const
{
    QDir().mkpath(m_dataDirPath);
    QJsonObject obj;
    for (auto it = m_customNames.begin(); it != m_customNames.end(); ++it)
        obj[it.key()] = it.value();

    QSaveFile f(m_dataDirPath + "/controller_names.json");
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    f.commit();
}

QString JoystickModule::resolvedName(const QString &rawName, const QString &mac) const
{
    if (!mac.isEmpty()) {
        auto it = m_customNames.find(mac.toUpper());
        if (it != m_customNames.end())
            return it.value();
    }
    return rawName;
}

void JoystickModule::ensureMacRegistered(const QString &mac, const QString &rawName)
{
    const QString key = mac.toUpper();
    if (key.isEmpty() || m_customNames.contains(key)) return;

    m_customNames[key] = rawName;
    saveCustomNames();
    qDebug() << "[Joystick] New controller registered:" << key << "->" << rawName;
}

void JoystickModule::setControllerName(const QString &mac, const QString &name)
{
    const QString key = mac.toUpper();
    if (key.isEmpty()) return;

    if (name.trimmed().isEmpty())
        m_customNames.remove(key);
    else
        m_customNames[key] = name.trimmed();

    saveCustomNames();

    for (auto &c : m_controllers) {
        if (c.macAddress.toUpper() == key) {
            c.name = resolvedName(c.name, c.macAddress);
            break;
        }
    }
    emit controllersChanged();
}

// ── activate / deactivate ─────────────────────────────────────────────────────

void JoystickModule::activate()
{
    if (m_activated) return;
    m_activated = true;

    if (!QFile::exists(m_ds4ExePath)) {
        qWarning() << "[Joystick] DS4Windows.exe not found";
        emit ds4NotFound();
        return;
    }

    m_startupWatcher = new QFutureWatcher<QList<ControllerInfo>>(this);
    connect(m_startupWatcher, &QFutureWatcher<QList<ControllerInfo>>::finished,
            this, [this]() {
                if (!m_startupWatcher) return;
                const bool ok   = !m_startupWatcher->isCanceled();
                const auto list = ok ? m_startupWatcher->result() : QList<ControllerInfo>{};
                m_startupWatcher->deleteLater();
                m_startupWatcher = nullptr;
                if (!m_activated) return;
                applyControllers(list, ok);
                if (ok) m_pollTimer->start();
            });

    m_startupWatcher->setFuture(QtConcurrent::run([this]() -> QList<ControllerInfo> {
        if (!startupSync()) return {};
        return scanControllersSync();
    }));
}

void JoystickModule::deactivate()
{
    if (!m_activated) return;
    m_activated = false;
    m_pollTimer->stop();
}

// ── startupSync (исправлен: используется startDetached) ───────────────────────

bool JoystickModule::startupSync()
{
    // Проверяем, отвечает ли уже запущенный экземпляр DS4Windows
    {
        const QString resp = runCmdSync("Query.1.AppRunning");
        if (resp.compare("True", Qt::CaseInsensitive) == 0)
            return true;
    }

    // Если процесс DS4Windows.exe уже существует, но не отвечает на IPC
    if (isProcessRunning("DS4Windows.exe")) {
        qDebug() << "[Joystick] External DS4Windows found, waiting for IPC...";
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 10'000;
        while (!m_stopping.load() && QDateTime::currentMSecsSinceEpoch() < deadline) {
            QThread::msleep(500);
            if (runCmdSync("Query.1.AppRunning").compare("True", Qt::CaseInsensitive) == 0)
                return true;
        }
        if (m_stopping.load()) return false;
        qWarning() << "[Joystick] Cannot reach running DS4Windows via IPC.";
        emit ds4LaunchFailed("DS4Windows already running but not responding");
        return false;
    }

    // Проверяем наличие исполняемого файла
    if (!QFile::exists(m_ds4ExePath)) {
        emit ds4NotFound();
        return false;
    }

    // Запускаем DS4Windows detached
    qDebug() << "[Joystick] Launching DS4Windows...";
    if (!QProcess::startDetached(m_ds4ExePath, { "-m" })) {
        qWarning() << "[Joystick] startDetached failed";
        emit ds4LaunchFailed("Failed to start DS4Windows process");
        return false;
    }
    m_weStartedDs4.store(true);

    // Ждём, пока процесс ответит на IPC-запросы
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kDs4StartupTimeoutMs;
    int attempt = 0;
    while (!m_stopping.load() && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QThread::msleep(500);
        ++attempt;
        if (runCmdSync("Query.1.AppRunning").compare("True", Qt::CaseInsensitive) == 0) {
            qDebug() << "[Joystick] DS4Windows ready after" << attempt << "polls";
            QThread::msleep(1000);  // Даём время на полную инициализацию
            return true;
        }
    }

    if (m_stopping.load()) return false;   // выходим — не шумим ошибкой
    qWarning() << "[Joystick] DS4Windows did not respond in" << kDs4StartupTimeoutMs << "ms";
    emit ds4LaunchFailed("DS4Windows did not respond in time");
    return false;
}

// ── scanControllersSync (исправлен: используем OutputSlotInputBound) ─────────

QList<ControllerInfo> JoystickModule::scanControllersSync() const
{
    QList<ControllerInfo> result;
    const bool refreshDynamic = (m_pollCycle++ % kDynamicRefreshCycles) == 0;

    for (int slot = 1; slot <= 4 && !m_stopping.load(); ++slot) {
        const QString bound = runCmdSync(
                                  QString("Query.%1.OutputSlotInputBound").arg(slot)).trimmed();
        if (bound.compare("Bound", Qt::CaseInsensitive) != 0) {
            QMutexLocker lock(&m_slotCacheMutex);
            m_slotCache.remove(slot);
            continue;
        }

        ControllerInfo info;
        bool cached = false;
        {
            QMutexLocker lock(&m_slotCacheMutex);
            auto it = m_slotCache.constFind(slot);
            if (it != m_slotCache.constEnd()) { info = it.value(); cached = true; }
        }

        if (cached && refreshDynamic) {
            const QString pfx = QString("Query.%1.").arg(slot);
            // Другой контроллер в том же слоте между опросами — перечитываем всё
            const QString mac = runCmdSync(pfx + "MacAddress").trimmed().toUpper();
            if (!mac.isEmpty() && mac != info.macAddress) {
                cached = false;
            } else {
                bool ok = false;
                const int bat = runCmdSync(pfx + "Battery").trimmed().toInt(&ok);
                info.battery  = ok ? bat : -1;
                info.charging = runCmdSync(pfx + "Charging").trimmed()
                                    .compare("True", Qt::CaseInsensitive) == 0;
            }
        }
        if (!cached) {
            const auto fresh = querySlotSync(slot);
            if (!fresh.has_value()) continue;
            info = fresh.value();
        }

        {
            QMutexLocker lock(&m_slotCacheMutex);
            m_slotCache.insert(slot, info);
        }
        result.append(info);
    }
    return result;
}

void JoystickModule::stopDs4Sync() const
{
    runCmdSync("Stop");
    QThread::msleep(500);
    runCmdSync("Shutdown");
}

std::optional<ControllerInfo> JoystickModule::querySlotSync(int slot) const
{
    const QString pfx = QString("Query.%1.").arg(slot);

    ControllerInfo info;
    info.slot       = slot;
    info.name       = runCmdSync(pfx + "DisplayName").trimmed();
    info.macAddress = runCmdSync(pfx + "MacAddress").trimmed().toUpper();
    info.connType   = runCmdSync(pfx + "ConnType").trimmed();

    bool ok = false;
    info.battery  = runCmdSync(pfx + "Battery").trimmed().toInt(&ok);
    if (!ok) info.battery = -1;

    info.charging = runCmdSync(pfx + "Charging").trimmed()
                        .compare("True", Qt::CaseInsensitive) == 0;

    return info;
}

QString JoystickModule::runCmdSync(const QString &cmd) const
{
    if (m_ds4CmdPath.isEmpty() || !QFile::exists(m_ds4CmdPath)) {
        qWarning() << "[Joystick] DS4WindowsCmd not found:" << m_ds4CmdPath;
        return {};
    }

    if (m_stopping.load()) return {};

    QProcess proc;
    proc.setProgram(m_ds4CmdPath);
    proc.setArguments({ "-command", cmd });
    proc.start();

    // Ждём короткими кусками, чтобы shutdown() не зависал на подвисшем
    // DS4WindowsCmd.exe (раньше — до kCmdTimeoutMs на каждый вызов).
    if (!proc.waitForStarted(kCmdTimeoutMs))
        return {};
    QElapsedTimer waited;
    waited.start();
    while (!proc.waitForFinished(100)) {
        if (proc.state() == QProcess::NotRunning)
            break;                                   // завершился/упал
        if (m_stopping.load() || waited.elapsed() > kCmdTimeoutMs) {
            proc.kill();
            proc.waitForFinished(500);
            return {};
        }
    }

    if (proc.exitCode() == 1000) return {};

    return QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
}

// ── applyControllers (главный поток) ─────────────────────────────────────────

void JoystickModule::applyControllers(const QList<ControllerInfo> &list, bool ds4Ready)
{
    QSet<int> freshSlots;
    for (const ControllerInfo &c : list)
        freshSlots.insert(c.slot);

    for (const ControllerInfo &existing : std::as_const(m_controllers)) {
        if (!freshSlots.contains(existing.slot))
            qDebug() << "[Joystick] Slot" << existing.slot << "disconnected";
    }

    QList<ControllerInfo> resolved;
    for (ControllerInfo c : list) {
        ensureMacRegistered(c.macAddress, c.name);
        c.name = resolvedName(c.name, c.macAddress);

        bool wasKnown = std::any_of(m_controllers.begin(), m_controllers.end(),
                                    [&](const ControllerInfo &o){ return o.slot == c.slot; });
        if (!wasKnown)
            qDebug() << "[Joystick] Controller connected: slot" << c.slot
                     << c.name << c.macAddress << "bat:" << c.battery << c.connType;

        resolved.append(c);
    }

    m_controllers = resolved;

    if (m_ds4Running != ds4Ready) {
        m_ds4Running = ds4Ready;
        emit ds4RunningChanged();
    }
    emit controllersChanged();
}

void JoystickModule::onPollTimer()
{
    if (m_pollWatcher && m_pollWatcher->isRunning()) return;
    // Пока идёт игра, панель геймпадов не видна, а каждый опрос — это 4+
    // запуска DS4WindowsCmd.exe (.NET) раз в 5 с, отнимающих процессор у игры
    if (m_gameRunning || m_androidTvShown) return;

    m_pollWatcher = new QFutureWatcher<QList<ControllerInfo>>(this);
    connect(m_pollWatcher, &QFutureWatcher<QList<ControllerInfo>>::finished,
            this, [this]() {
                if (!m_pollWatcher) return;
                const auto list = m_pollWatcher->isCanceled()
                                      ? m_controllers
                                      : m_pollWatcher->result();
                m_pollWatcher->deleteLater();
                m_pollWatcher = nullptr;
                if (!m_activated) return;
                applyControllers(list, m_ds4Running);
            });

    m_pollWatcher->setFuture(
        QtConcurrent::run([this]() { return scanControllersSync(); }));
}

// ── QML API ───────────────────────────────────────────────────────────────────

QVariantList JoystickModule::controllers() const
{
    QVariantList result;
    for (const ControllerInfo &c : m_controllers) {
        result.append(QVariantMap{
                                  { "slot",       c.slot       },
                                  { "name",       c.name       },
                                  { "macAddress", c.macAddress },
                                  { "battery",    c.battery    },
                                  { "charging",   c.charging   },
                                  { "connType",   c.connType   },
                                  });
    }
    return result;
}

// ── Профили ───────────────────────────────────────────────────────────────────

void JoystickModule::loadProfile(const QString &requestedProfile) const
{
    if (m_controllers.isEmpty()) return;

    // Профиля "XInput" в комплекте нет (есть DS4.xml и Default.xml, где Default —
    // эмуляция X360, т.е. XInput). Запрос несуществующего профиля DS4Windows
    // молча игнорировал, и у XInput-игр оставался прежний (например DS4) профиль.
    QString profileName = requestedProfile;
    const QString ds4Dir = QFileInfo(m_ds4ExePath).absolutePath();
    const QStringList profileDirs = {
        ds4Dir + "/Profiles",
        QDir::homePath() + "/AppData/Roaming/DS4Windows/Profiles"
    };
    bool exists = false, anyDir = false;
    for (const QString &d : profileDirs) {
        if (QDir(d).exists()) anyDir = true;
        if (QFile::exists(d + "/" + profileName + ".xml")) { exists = true; break; }
    }
    // Если папку профилей найти не удалось — не угадываем, шлём как есть
    if (anyDir && !exists && profileName != QLatin1String(kProfileDefault)) {
        qDebug() << "[Joystick] Profile" << profileName << "not found, using" << kProfileDefault;
        profileName = QString::fromLatin1(kProfileDefault);
    }

    m_profileTasks.erase(std::remove_if(m_profileTasks.begin(), m_profileTasks.end(),
                                        [](const QFuture<void> &f) { return f.isFinished(); }),
                         m_profileTasks.end());
    for (const ControllerInfo &c : m_controllers) {
        const QString cmd = QString("LoadProfile.%1.%2").arg(c.slot).arg(profileName);
        m_profileTasks.append(QtConcurrent::run([this, cmd]() { runCmdSync(cmd); }));
    }
}

void JoystickModule::onGameInputType(const QStringList &inputTypes)
{
    // Приоритет: directinput → DS4-профиль, xinput → XInput-профиль, иначе → Default.
    // Если оба присутствуют одновременно — directinput важнее.
    if (inputTypes.contains("directinput")) {
        qDebug() << "[Joystick] Input type: directinput → loading DS4 profile";
        loadProfile(kProfileDS4);
    } else if (inputTypes.contains("xinput")) {
        qDebug() << "[Joystick] Input type: xinput → loading XInput profile";
        loadProfile(kProfileXInput);
    } else {
        qDebug() << "[Joystick] Input type: unknown/empty → loading Default profile";
        loadProfile(kProfileDefault);
    }
}

void JoystickModule::onGameClosed()
{
    loadProfile(kProfileDefault);
}
