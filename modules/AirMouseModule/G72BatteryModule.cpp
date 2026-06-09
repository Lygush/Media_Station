// G72BatteryModule.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Синхронный WinRT через .get() в отдельном QThread.
// Никаких корутин, никаких co_await — только блокирующие вызовы в worker-потоке.
// ─────────────────────────────────────────────────────────────────────────────

#include "G72BatteryModule.h"
#include "LoggerModule.h"
#include <QDebug>
#include <atomic>
#include <cmath>
#include <array>

// WinRT — компилируется только под Windows
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Storage::Streams;

namespace {
constexpr winrt::guid kBatteryService {
    0x0000180f, 0x0000, 0x1000,
    { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb }
};
constexpr winrt::guid kBatteryLevel {
    0x00002a19, 0x0000, 0x1000,
    { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb }
};
constexpr quint64 kFallbackMac = 0xDC6D8630E0EDULL;
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Worker
// ─────────────────────────────────────────────────────────────────────────────
struct WinRTState {
    BluetoothLEDevice  device { nullptr };
    GattCharacteristic charac { nullptr };
};

G72BatteryWorker::G72BatteryWorker(QObject *parent)
    : QObject(parent)
{}

G72BatteryWorker::~G72BatteryWorker()
{
    if (m_device) {
        qWarning() << "[G72Battery] ~Worker: m_device not null, leaking to avoid crash";
    }
}

void G72BatteryWorker::doSearch()
{
    // Проверяем флаг завершения до начала любых WinRT-операций
    if (m_shuttingDown.load()) return;

    try { winrt::init_apartment(apartment_type::multi_threaded); } catch (...) {}

    if (m_device) {
        delete static_cast<WinRTState*>(m_device);
        m_device = nullptr;
        m_charac = nullptr;
    }

    auto *st = new WinRTState();

    try {
        if (m_shuttingDown.load()) { delete st; return; }

        st->device = BluetoothLEDevice::FromBluetoothAddressAsync(kFallbackMac).get();

        if (!st->device) {
            if (m_shuttingDown.load()) { delete st; return; }
            try {
                auto sel  = BluetoothLEDevice::GetDeviceSelectorFromDeviceName(L"G72");
                auto devs = DeviceInformation::FindAllAsync(sel).get();
                if (devs.Size() > 0) {
                    auto d = BluetoothLEDevice::FromIdAsync(devs.GetAt(0).Id()).get();
                    if (d) {
                        quint64 mac = d.BluetoothAddress();
                        st->device = BluetoothLEDevice::FromBluetoothAddressAsync(mac).get();
                    }
                }
            } catch (...) {}
        }

        if (!st->device) {
            delete st;
            if (!m_shuttingDown.load())
                emit deviceFailed("Device not found");
            return;
        }

        if (m_shuttingDown.load()) { delete st; return; }

        auto svcResult = st->device.GetGattServicesForUuidAsync(
                                       kBatteryService,
                                       BluetoothCacheMode::Uncached).get();
        if (svcResult.Status() != GattCommunicationStatus::Success
            || svcResult.Services().Size() == 0)
        {
            delete st;
            if (!m_shuttingDown.load())
                emit deviceFailed("Battery service not found");
            return;
        }

        if (m_shuttingDown.load()) { delete st; return; }

        auto charResult = svcResult.Services().GetAt(0)
                              .GetCharacteristicsForUuidAsync(
                                  kBatteryLevel,
                                  BluetoothCacheMode::Uncached).get();
        if (charResult.Status() != GattCommunicationStatus::Success
            || charResult.Characteristics().Size() == 0)
        {
            delete st;
            if (!m_shuttingDown.load())
                emit deviceFailed("Battery characteristic not found");
            return;
        }

        st->charac = charResult.Characteristics().GetAt(0);
        m_device = st;
        m_charac = st;

        if (!m_shuttingDown.load()) {
            emit deviceReady();
            doRead();
        }

    } catch (winrt::hresult_error const &e) {
        QString msg = QString("WinRT error in doSearch: code=%1, message=%2")
        .arg(static_cast<long>(e.code()))
            .arg(QString::fromStdWString(e.message().c_str()));
        qWarning() << "[G72Battery]" << msg;
        delete st;
        if (!m_shuttingDown.load())
            emit deviceFailed(msg);
    } catch (...) {
        qWarning() << "[G72Battery] Unknown error in doSearch";
        delete st;
        if (!m_shuttingDown.load())
            emit deviceFailed("Unknown error in doSearch");
    }
}

void G72BatteryWorker::cleanupState()
{
    if (m_device) {
        auto *st = static_cast<WinRTState*>(m_device);
        st->charac = nullptr;
        st->device = nullptr;
        delete st;
        m_device = nullptr;
        m_charac = nullptr;
    }
}

void G72BatteryWorker::doRead()
{
    if (m_shuttingDown.load()) return;
    if (!m_device) return;
    auto *st = static_cast<WinRTState*>(m_device);
    if (!st->charac) return;

    if (!m_busy) return;
    m_busy->store(true);
    try {
        auto result = st->charac.ReadValueAsync(BluetoothCacheMode::Uncached).get();
        if (result.Status() == GattCommunicationStatus::Success) {
            auto reader = DataReader::FromBuffer(result.Value());
            if (reader.UnconsumedBufferLength() >= 1) {
                quint8 raw = reader.ReadByte();
                m_busy->store(false);
                if (!m_shuttingDown.load())
                    emit rawSample(raw);
                return;
            }
        }
        m_busy->store(false);
    } catch (winrt::hresult_error const &e) {
        QString msg = QString("WinRT read error: code=%1, message=%2")
        .arg(static_cast<long>(e.code()))
            .arg(QString::fromStdWString(e.message().c_str()));
        qWarning() << "[G72Battery]" << msg;
        m_busy->store(false);
        delete st;
        m_device = nullptr;
        m_charac = nullptr;
        if (!m_shuttingDown.load())
            emit deviceFailed(msg);
    } catch (...) {
        m_busy->store(false);
        qWarning() << "[G72Battery] Unknown read error";
        if (!m_shuttingDown.load())
            emit deviceFailed("Unknown read error");
    }
}

// ── G72BatteryModule::shutdown ────────────────────────────────────────────────
// Ключевые изменения:
//   1. setShuttingDown() прерывает текущую WinRT-операцию в doSearch/doRead
//      (они проверяют флаг между вызовами .get())
//   2. cleanupState() вызывается через Qt::QueuedConnection (не Blocking) —
//      просто ставим в очередь и сразу вызываем quit()
//   3. Ждём максимум 1500 мс вместо 5000 мс

// ─────────────────────────────────────────────────────────────────────────────
// Li-Ion таблица
// ─────────────────────────────────────────────────────────────────────────────
namespace {
struct LiPt { double ratio; int pct; };
constexpr std::array<LiPt,11> kLiIon {{
    {1.00,100},{0.90,95},{0.80,90},{0.72,85},{0.62,75},
    {0.50,60},{0.38,40},{0.25,20},{0.15,10},{0.05,3},{0.00,0}
}};
} // namespace

/*static*/ int G72BatteryModule::liionPercent(double ratio)
{
    ratio = std::clamp(ratio, 0.0, 1.0);
    for (std::size_t i = 0; i + 1 < kLiIon.size(); ++i) {
        if (ratio >= kLiIon[i+1].ratio) {
            double span = kLiIon[i].ratio - kLiIon[i+1].ratio;
            double t    = span > 0.0 ? (ratio - kLiIon[i+1].ratio) / span : 1.0;
            return static_cast<int>(
                std::lround(kLiIon[i+1].pct + t * (kLiIon[i].pct - kLiIon[i+1].pct)));
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// G72BatteryModule
// ─────────────────────────────────────────────────────────────────────────────
G72BatteryModule::G72BatteryModule(QObject *parent)
    : QObject(parent)
{
    m_worker = new G72BatteryWorker;
    m_worker->setBusyFlag(&m_workerBusy);
    m_worker->moveToThread(&m_thread);

    connect(m_worker, &G72BatteryWorker::rawSample,
            this,     &G72BatteryModule::onRawSample);
    connect(m_worker, &G72BatteryWorker::deviceReady,
            this,     &G72BatteryModule::onDeviceReady);
    connect(m_worker, &G72BatteryWorker::deviceFailed,
            this,     &G72BatteryModule::onDeviceFailed);

    m_pollTimer.setInterval(30'000);
    m_pollTimer.setSingleShot(false);
    connect(&m_pollTimer, &QTimer::timeout,
            m_worker, &G72BatteryWorker::doRead, Qt::QueuedConnection);

    m_retryTimer.setInterval(15'000);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &G72BatteryModule::onRetryTick);
}

G72BatteryModule::~G72BatteryModule()
{
    shutdown();
}

void G72BatteryModule::initialize(EventBus *bus, SettingsManager *)
{
    m_bus = bus;

    connect(this, &G72BatteryModule::batteryChanged, this,
            [bus](int pct) {
                emit bus->moduleEvent("g72battery", "batteryChanged", {{"percent", pct}});
            });
    connect(this, &G72BatteryModule::connectedChanged, this,
            [bus](bool c) {
                emit bus->moduleEvent("g72battery", "connectedChanged", {{"connected", c}});
            });

    m_thread.start();
    QMetaObject::invokeMethod(m_worker, "doSearch", Qt::QueuedConnection);

    qDebug() << "[G72Battery] Module initialized, search started";
    if (m_bus) {
        emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                Logger::makeEvent(Logger::kLevelInfo, moduleId().toUtf8().constData(),
                                                  "Module initialized, search started"));
    }
}

void G72BatteryModule::shutdown()
{
    if (m_shutdownDone) return;
    m_shutdownDone = true;

    m_bus = nullptr;
    m_pollTimer.stop();
    m_retryTimer.stop();

    if (!m_thread.isRunning()) {
        if (m_worker) {
            m_worker->setParent(nullptr);
            delete m_worker;
            m_worker = nullptr;
        }
        return;
    }

    if (m_worker) {
        // 1. Сигнализируем воркеру что нужно прекратить работу.
        //    Он проверяет m_shuttingDown между WinRT-вызовами.
        //    Текущий .get() всё равно заблокирован — его прервать нельзя,
        //    но следующая операция уже не запустится.
        m_worker->setShuttingDown();

        // 2. Отключаем все сигналы — слоты модуля не будут вызваны
        m_worker->disconnect();

        // 3. Ставим cleanupState в очередь воркера (QueuedConnection).
        //    Когда текущая WinRT-операция завершится (.get() вернётся),
        //    воркер обработает очередь: сначала cleanupState, затем quit.
        QMetaObject::invokeMethod(m_worker, "cleanupState", Qt::QueuedConnection);
    }

    // 4. Просим поток завершить event loop
    m_thread.quit();

    // 5. Ждём завершения потока.
    //    Время ожидания = время оставшейся WinRT-операции (обычно < 500 мс).
    //    Максимум — 1500 мс. Если не успел — terminate (безопасно для WinRT
    //    объектов, т.к. cleanupState уже поставлен в очередь до quit).
    if (!m_thread.wait(1500)) {
        qWarning() << "[G72Battery] Worker thread did not finish in 1500ms,"
                   << "forcing termination (WinRT objects may leak)";
        m_thread.terminate();
        m_thread.wait(300);
    }

    if (m_worker) {
        m_worker->setParent(nullptr);
        delete m_worker;
        m_worker = nullptr;
    }

    m_connected = false;
    m_battery   = -1;
}

void G72BatteryModule::onDeviceReady()
{
    bool wasConnected = m_connected;
    m_failureLogged = false;   // сброс флага при успешном подключении
    setConnected(true);
    m_retryTimer.stop();
    m_pollTimer.start();

    if (!wasConnected) {
        qDebug() << "[G72Battery] Connected";
        if (m_bus) {
            emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                    Logger::makeEvent(Logger::kLevelInfo, moduleId().toUtf8().constData(),
                                                      "Connected"));
        }
    }
}

void G72BatteryModule::onDeviceFailed(const QString &reason)
{
    bool wasConnected = m_connected;
    setConnected(false);
    m_pollTimer.stop();
    m_retryTimer.start();

    // Собираем диагностику
    QString diag;
    if (m_rawHistoryCount > 0) {
        diag = QString("Last raw values: %1").arg(rawHistoryString());
    }
    diag += QString("; Filter state: %1").arg(filterStateString());
    diag += QString("; Reason: %1").arg(reason);

    if (wasConnected) {
        // Реальная потеря связи – выводим всегда
        qDebug() << "[G72Battery] Disconnected:" << diag;
        if (m_bus) {
            QByteArray msg = QString("Disconnected: %1").arg(diag).toUtf8();
            emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                    Logger::makeEvent(Logger::kLevelError, moduleId().toUtf8().constData(),
                                                      msg.constData()));
        }
        m_failureLogged = false;   // сброс для будущих попыток
    } else {
        // Неудачная попытка подключения – выводим только один раз
        if (!m_failureLogged) {
            qDebug() << "[G72Battery] Connection failed:" << diag;
            if (m_bus) {
                QByteArray msg = QString("Connection failed: %1").arg(diag).toUtf8();
                emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                        Logger::makeEvent(Logger::kLevelWarning, moduleId().toUtf8().constData(),
                                                          msg.constData()));
            }
            m_failureLogged = true;
        }
    }
}

void G72BatteryModule::onRetryTick()
{
    QMetaObject::invokeMethod(m_worker, "doSearch", Qt::QueuedConnection);
}

void G72BatteryModule::onPollTick() {}

void G72BatteryModule::onRawSample(quint8 raw)
{
    addRawToHistory(raw);
    m_lastRawSample = raw;

    if (raw == 0x00) return;

    double rawD = static_cast<double>(raw);

    // Детектор просадки
    if (m_emaInited) {
        double drop = m_prevRaw - rawD;
        if (!m_frozen && drop >= DROP_THRESHOLD) {
            m_frozen = true;
            if (m_bus) {
                QByteArray msg = QString("Frozen: drop=%1 (prevRaw=%2, raw=%3)")
                .arg(drop, 0, 'f', 1)
                    .arg(m_prevRaw, 0, 'f', 1)
                    .arg(rawD, 0, 'f', 1)
                    .toUtf8();
                emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                        Logger::makeEvent(Logger::kLevelWarning, moduleId().toUtf8().constData(),
                                                          msg.constData()));
            }
        }
    }
    m_prevRaw = rawD;

    // EMA
    if (!m_emaInited) { m_ema = rawD; m_emaInited = true; }
    else m_ema = EMA_ALPHA * rawD + (1.0 - EMA_ALPHA) * m_ema;

    pushVar(m_ema);
    if (m_frozen && m_varCount >= VAR_WIN && variance() < VAR_THRESHOLD) {
        m_frozen = false;
        if (m_bus) {
            QByteArray msg = QString("Unfrozen: variance=%1, ema=%2")
            .arg(variance(), 0, 'f', 3)
                .arg(m_ema, 0, 'f', 1)
                .toUtf8();
            emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                    Logger::makeEvent(Logger::kLevelInfo, moduleId().toUtf8().constData(),
                                                      msg.constData()));
        }
    }

    if (!m_frozen) {
        int rawRounded = static_cast<int>(std::round(m_ema));
        double ratio = std::clamp(
            static_cast<double>(rawRounded - BATTERY_MIN) / (BATTERY_MAX - BATTERY_MIN),
            0.0, 1.0);
        int pct = liionPercent(ratio);
        applyBattery(pct);
    }
}

void G72BatteryModule::addRawToHistory(quint8 raw)
{
    m_rawHistory[m_rawHistoryIdx] = raw;
    m_rawHistoryIdx = (m_rawHistoryIdx + 1) % RAW_HISTORY_SIZE;
    if (m_rawHistoryCount < RAW_HISTORY_SIZE) ++m_rawHistoryCount;
}

QString G72BatteryModule::rawHistoryString() const
{
    if (m_rawHistoryCount == 0) return "none";
    QStringList list;
    int start = (m_rawHistoryIdx - m_rawHistoryCount + RAW_HISTORY_SIZE) % RAW_HISTORY_SIZE;
    for (int i = 0; i < m_rawHistoryCount; ++i) {
        int idx = (start + i) % RAW_HISTORY_SIZE;
        list << QString::number(m_rawHistory[idx]);
    }
    return list.join(", ");
}

QString G72BatteryModule::filterStateString() const
{
    return QString("ema=%1, prevRaw=%2, frozen=%3, varCount=%4, variance=%5")
    .arg(m_ema, 0, 'f', 2)
        .arg(m_prevRaw, 0, 'f', 2)
        .arg(m_frozen)
        .arg(m_varCount)
        .arg(variance(), 0, 'f', 3);
}

void G72BatteryModule::pushVar(double v)
{
    m_varBuf[m_varIdx] = v;
    m_varIdx = (m_varIdx + 1) % VAR_WIN;
    if (m_varCount < VAR_WIN) ++m_varCount;
}

double G72BatteryModule::variance() const
{
    if (m_varCount < 2) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < m_varCount; ++i) sum += m_varBuf[i];
    double mean = sum / m_varCount;
    double var  = 0.0;
    for (int i = 0; i < m_varCount; ++i) { double d = m_varBuf[i]-mean; var += d*d; }
    return var / m_varCount;
}

void G72BatteryModule::applyBattery(int percent)
{
    if (percent == m_battery) return;
    m_battery = percent;
    emit batteryChanged(percent);
    Zone z = percent > 50 ? Green : percent >= 20 ? Yellow : Red;
    if (z != m_zone) { m_zone = z; emit zoneChanged(static_cast<int>(m_zone)); }

    if (m_bus) {
        QByteArray msg = QString("Battery changed to %1%, Raw sample: %2")
        .arg(percent)
            .arg(m_lastRawSample)
            .toUtf8();
        emit m_bus->moduleEvent(Logger::kModule, Logger::kEventLog,
                                Logger::makeEvent(Logger::kLevelInfo, moduleId().toUtf8().constData(),
                                                  msg.constData()));
    }
}

void G72BatteryModule::setConnected(bool v)
{
    if (v == m_connected) return;
    m_connected = v;
    emit connectedChanged(v);
}
