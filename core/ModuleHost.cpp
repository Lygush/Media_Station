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

    for (IModule *mod : m_modules) {
        mod->initialize(&m_bus, &m_settings);
        mod->registerWithQml(m_engine);
        qDebug() << "ModuleHost: initialized" << mod->moduleId();
    }
}

void ModuleHost::shutdownAll()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;

    qDebug() << "ModuleHost: shutdownAll started";

    QElapsedTimer totalTimer;
    totalTimer.start();

    // Завершаем в порядке, обратном регистрации.
    // Порядок регистрации в main.cpp спроектирован так, что первым
    // завершается G72KeysModule (снимает хуки мыши/клавиатуры),
    // затем модули с внешними процессами, затем фоновые потоки,
    // последним — LoggerModule.
    for (auto it = m_modules.rbegin(); it != m_modules.rend(); ++it) {
        IModule *mod = *it;

        // Жёсткий лимит на весь shutdown — если уже вышли за пределы,
        // лишь логируем и пропускаем оставшиеся модули.
        if (totalTimer.elapsed() > kTotalShutdownTimeoutMs) {
            qWarning() << "ModuleHost: total shutdown timeout exceeded, skipping"
                       << mod->moduleId();
            continue;
        }

        QElapsedTimer modTimer;
        modTimer.start();

        mod->shutdown();

        const qint64 elapsed = modTimer.elapsed();
        if (elapsed > kModuleShutdownWarnMs) {
            qWarning() << "ModuleHost: slow shutdown for" << mod->moduleId()
                       << "—" << elapsed << "ms";
        } else {
            qDebug() << "ModuleHost: shutdown" << mod->moduleId()
                     << "in" << elapsed << "ms";
        }
    }

    qDebug() << "ModuleHost: shutdownAll finished in" << totalTimer.elapsed() << "ms";

    m_modules.clear();
}
