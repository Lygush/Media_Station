// modules/AndroidTVModule/EmulatorGrpcClient.h
// ─────────────────────────────────────────────────────────────────────────────
// Клиент gRPC-интерфейса Android Emulator (android.emulation.control.
// EmulatorController, emulator/lib/emulator_controller.proto) — тот же канал,
// через который эмулятором управляет Android Studio.
//
// Без библиотек gRPC/protobuf: unary-вызов gRPC — это HTTP/2 POST с телом
// «1 байт флага сжатия + 4 байта длины + сообщение protobuf». HTTP/2 без TLS
// (h2c) умеет QNetworkAccessManager, а нужные сообщения (KeyboardEvent,
// EmulatorStatus) кодируются вручную в несколько строк.
//
// Зачем: консоль эмулятора 36.4 нажатия (`event send`) принимает, но до
// Android не доставляет, а `adb input keyevent` запускает JVM в госте на
// каждое нажатие. sendKey идёт тем же путём, что клавиатура окна эмулятора.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <QObject>
#include <QByteArray>
#include <QQueue>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

class EmulatorGrpcClient : public QObject
{
    Q_OBJECT
public:
    explicit EmulatorGrpcClient(QObject *parent = nullptr);

    /// Порт gRPC эмулятора и токен (содержимое .emulator_console_auth_token
    /// при запуске с -grpc-use-token; пусто — без авторизации).
    void configure(int port, const QByteArray &token);
    int  port() const { return m_port; }

    struct Status { bool ok = false; bool booted = false; QString version; QString error; };
    void getStatus(std::function<void(const Status &)> done);

    /// Нажатие клавиши (нажал+отпустил) кодом evdev (KEY_UP = 103 …).
    /// Вызовы выполняются строго по очереди — порядок нажатий сохраняется.
    void sendKeyPress(int evdevCode);
    /// Строка как набранная с клавиатуры (печатаемые ASCII надёжно)
    void sendText(const QString &text);

    int  callTimeoutMs() const { return m_timeoutMs; }
    void setCallTimeoutMs(int ms) { m_timeoutMs = ms; }

    // ── protobuf/gRPC — открыто для тестов ──────────────────────────────────
    static QByteArray encodeKeyPress(int evdevCode);
    static QByteArray encodeText(const QString &text);
    static QByteArray frame(const QByteArray &message);
    /// Первое сообщение из тела ответа; false — тело повреждено
    static bool unframe(const QByteArray &body, QByteArray *message);
    static Status decodeStatus(const QByteArray &message);
    /// Значение grpc-status (возможно склеенное Qt: «0, 0») означает успех
    static bool statusIsOk(const QByteArray &grpcStatus);

signals:
    /// Вызов sendKey/sendText не удался (сеть, авторизация, статус gRPC)
    void callFailed(const QString &method, const QString &error);

private:
    using Done = std::function<void(bool ok, const QByteArray &message, const QString &error)>;
    void call(const QString &method, const QByteArray &message, Done done);
    void enqueue(const QString &method, const QByteArray &message);
    void pump();

    QNetworkAccessManager *m_nam = nullptr;
    int        m_port = 0;
    QByteArray m_token;
    int        m_timeoutMs = 1500;

    struct Pending { QString method; QByteArray message; };
    QQueue<Pending> m_queue;
    bool m_inFlight = false;
};
