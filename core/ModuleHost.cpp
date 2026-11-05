// core/ModuleHost.cpp
#include "ModuleHost.h"
#include "IModule.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QElapsedTimer>
#include <QDebug>

ModuleHost::ModuleHost(QQmlApplicationEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_bus(this)
    , m_settings(this)
{
    // Пробрасываем сигналы SettingsManager в шину,
    // чтобы модули не зависели от SettingsManager напрямую.
    connect(&m_settings, &SettingsManager::accentColorChanged, &m_bus, [this] {
        emit m_bus.settingChanged("accentColor", m_settings.accentColor());
    });
    connect(&m_settings, &SettingsManager::backgroundImageChanged, &m_bus, [this] {
        emit m_bus.settingChanged("backgroundImage", m_settings.backgroundImage());
    });
    connect(&m_settings, &SettingsManager::fullscreenChanged, &m_bus, [this] {
        emit m_bus.settingChanged("fullscreen", m_settings.fullscreen());
    });
    connect(&m_settings, &SettingsManager::languageChanged, &m_bus, [this] {
        emit m_bus.settingChanged("language", m_settings.language());
    });
    connect(&m_settings, &SettingsManager::installPathChanged, &m_bus, [this] {
        emit m_bus.settingChanged("installPath", m_settings.installPath());
    });
}

ModuleHost::~ModuleHost()
{
    shutdownAll();
}

void ModuleHost::registerModule(IModule *module)
{
    Q_ASSERT_X(!m_initialized, "ModuleHost::registerModule",
               "Cannot register modules after initializeAll()");
    m_modules.push_back(module);
}

void ModuleHost::initializeAll()
{
    Q_ASSERT_X(!m_initialized, "ModuleHost::initializeAll",
               "initializeAll() called twice");
    m_initialized = true;

    // SettingsManager регистрируется как QML-синглтон отдельно —
    // он не является IModule, но должен быть доступен из QML.
    qmlRegisterSingletonInstance("AppSettings", 1, 0, "SettingsManager", &m_settings);

    // В журнал — одна итоговая строка; по модулю отдельно — только медленные
    QElapsedTimer total;
    total.start();
    for (IModule *mod : m_modules) {
        QElapsedTimer t;
        t.start();
        mod->initialize(&m_bus, &m_settings);
        mod->registerWithQml(m_engine);
        if (t.elapsed() > 200)
            qInfo().noquote() << "ModuleHost: slow initialize for" << mod->moduleId()
                              << "—" << t.elapsed() << "ms";
    }
    qInfo().noquote() << "ModuleHost:" << m_modules.size() << "modules initialized in"
                      << total.elapsed() << "ms";
}

void ModuleHost::shutdownAll()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;


    QElapsedTimer totalTimer;
    totalTimer.start();

    // Завершаем в порядке, обратном регистрации.
    // Порядок регистрации в main.cpp спроектирован так, что первым
    // завершается G72KeysModule (снимает хуки мыши/клавиатуры),
    // затем модули с внешними процессами, затем фоновые потоки,
    // последним — LoggerModule.
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        IModule *mod = *it;

        // Модули НЕ пропускаем даже при превышении общего лимита: прервать
        // зависший shutdown() таймаут всё равно не может, а пропуск оставлял
        // без завершения следующие модули — процессы эмулятора Android TV,
        // хуки клавиатуры, сброс журнала (Logger завершается последним).
        if (totalTimer.elapsed() > kTotalShutdownTimeoutMs) {
            qWarning().noquote() << "ModuleHost: total shutdown budget exceeded before"
                                 << mod->moduleId() << "— continuing anyway";
        }

        // Журнал завершается последним — итог пишем до него, иначе строка теряется
        if (mod == m_modules.front() && m_modules.size() > 1)
            qInfo().noquote() << "ModuleHost: modules shut down in" << totalTimer.elapsed() << "ms";

        QElapsedTimer modTimer;
        modTimer.start();

        mod->shutdown();

        const qint64 elapsed = modTimer.elapsed();
        if (elapsed > kModuleShutdownWarnMs) {
            qWarning().noquote() << "ModuleHost: slow shutdown for" << mod->moduleId()
                                 << "—" << elapsed << "ms";
        }
    }


    m_modules.clear();
}
