// G72BatteryModule.h
#pragma once

#include "core/IModule.h"
#include "core/EventBus.h"

#include <QObject>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <array>

// Worker: синхронный WinRT через .get() — безопасно в отдельном потоке
class G72BatteryWorker : public QObject
{
    Q_OBJECT
public:
    explicit G72BatteryWorker(QObject *parent = nullptr);
    ~G72BatteryWorker() override;

    void setBusyFlag(std::atomic<bool> *flag)    { m_busy = flag; }

    // Устанавливается из главного потока перед quit() — воркер проверяет
    // этот флаг перед каждой WinRT-операцией и прерывает работу.
    void setShuttingDown() { m_shuttingDown.store(true); }

public slots:
    void doSearch();
    void doRead();
    // cleanupState вызывается только из потока воркера через Qt::QueuedConnection
    // (НЕ BlockingQueuedConnection) — просто освобождает WinRT объекты
    void cleanupState();

signals:
    void rawSample(quint8 raw);
    void deviceReady();
    void deviceFailed(const QString &reason);

private:
    void *m_device = nullptr;   // WinRTState* (BluetoothLEDevice + GattCharacteristic)
    void *m_charac = nullptr;   // указывает на тот же WinRTState что и m_device

    std::atomic<bool> *m_busy       = nullptr;
    std::atomic<bool>  m_shuttingDown { false };
};

class G72BatteryModule : public QObject, public IModule
{
    Q_OBJECT
    Q_PROPERTY(int  battery   READ battery   NOTIFY batteryChanged)
    Q_PROPERTY(int  zone      READ zone      NOTIFY zoneChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    enum Zone { Green = 0, Yellow = 1, Red = 2 };
    Q_ENUM(Zone)

    explicit G72BatteryModule(QObject *parent = nullptr);
    ~G72BatteryModule() override;

    QString  moduleId()  const override { return QStringLiteral("g72battery"); }
    void     initialize(EventBus *bus, SettingsManager *) override;
    void     shutdown()  override;
    QObject *qmlObject() override       { return this; }
    QString  qmlName()   const override { return QStringLiteral("G72Battery"); }

    int  battery()   const { return m_battery; }
    int  zone()      const { return static_cast<int>(m_zone); }
    bool connected() const { return m_connected; }

signals:
    void batteryChanged(int percent);
    void zoneChanged(int zone);
    void connectedChanged(bool connected);

private slots:
    void onRawSample(quint8 raw);
    void onDeviceReady();
    void onDeviceFailed(const QString &reason);
    void onPollTick();
    void onRetryTick();

private:
    static constexpr int    BATTERY_MAX    = 0xBB;
    static constexpr int    BATTERY_MIN    = 0x87;
    static constexpr double EMA_ALPHA      = 0.3;
    static constexpr int    DROP_THRESHOLD = 4;
    static constexpr int    VAR_WIN        = 6;
    static constexpr double VAR_THRESHOLD  = 0.5;

    double m_ema     = 0.0;
    double m_prevRaw = 0.0;
    bool   m_emaInited = false;
    bool   m_frozen    = false;
    double m_varBuf[VAR_WIN] = {};  int m_varIdx = 0;  int m_varCount = 0;
    void   pushVar(double v);
    double variance() const;
    static int liionPercent(double ratio);

    int  m_battery = -1;  Zone m_zone = Green;  bool m_connected = false;
    void applyBattery(int p);
    void setConnected(bool v);

    QThread           m_thread;
    G72BatteryWorker *m_worker    = nullptr;
    QTimer            m_pollTimer;
    QTimer            m_retryTimer;
    bool              m_shutdownDone = false;
    std::atomic<bool> m_workerBusy  { false };

    EventBus *m_bus = nullptr;

    // Диагностический буфер последних сырых значений
    static constexpr int RAW_HISTORY_SIZE = 10;
    std::array<quint8, RAW_HISTORY_SIZE> m_rawHistory{};
    int m_rawHistoryIdx = 0;
    int m_rawHistoryCount = 0;
    quint8 m_lastRawSample = 0;

    void addRawToHistory(quint8 raw);
    QString rawHistoryString() const;
    QString filterStateString() const;

    // Флаг подавления повторяющихся сообщений "Connection failed"
    bool m_failureLogged = false;
};
