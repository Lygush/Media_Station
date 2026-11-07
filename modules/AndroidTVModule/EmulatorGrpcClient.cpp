// modules/AndroidTVModule/EmulatorGrpcClient.cpp
#include "EmulatorGrpcClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

constexpr auto kService = "android.emulation.control.EmulatorController";

// ── protobuf: только то, что нужно ──────────────────────────────────────────
void putVarint(QByteArray &out, quint64 v)
{
    while (v >= 0x80) { out.append(char((v & 0x7F) | 0x80)); v >>= 7; }
    out.append(char(v));
}
void putTag(QByteArray &out, int field, int wireType) { putVarint(out, quint64(field) << 3 | quint64(wireType)); }
void putVarintField(QByteArray &out, int field, quint64 v) { putTag(out, field, 0); putVarint(out, v); }
void putBytesField(QByteArray &out, int field, const QByteArray &b)
{
    putTag(out, field, 2);
    putVarint(out, quint64(b.size()));
    out.append(b);
}

bool getVarint(const QByteArray &in, int &pos, quint64 *v)
{
    quint64 r = 0;
    for (int shift = 0; shift < 64 && pos < in.size(); shift += 7) {
        const quint8 b = quint8(in.at(pos++));
        r |= quint64(b & 0x7F) << shift;
        if (!(b & 0x80)) { *v = r; return true; }
    }
    return false;
}

} // namespace

EmulatorGrpcClient::EmulatorGrpcClient(QObject *parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this))
{
}

void EmulatorGrpcClient::configure(int port, const QByteArray &token)
{
    m_port  = port;
    m_token = token;
}

// KeyboardEvent { codeType = Evdev(1); eventType = keypress(2); keyCode }
QByteArray EmulatorGrpcClient::encodeKeyPress(int evdevCode)
{
    QByteArray m;
    putVarintField(m, 1, 1);                   // codeType = Evdev
    putVarintField(m, 2, 2);                   // eventType = keypress
    putVarintField(m, 3, quint64(quint32(evdevCode)));
    return m;
}

// KeyboardEvent { text }
QByteArray EmulatorGrpcClient::encodeText(const QString &text)
{
    QByteArray m;
    putBytesField(m, 5, text.toUtf8());
    return m;
}

QByteArray EmulatorGrpcClient::frame(const QByteArray &message)
{
    QByteArray f;
    f.append(char(0));                          // без сжатия
    const quint32 n = quint32(message.size());
    f.append(char(n >> 24)); f.append(char(n >> 16)); f.append(char(n >> 8)); f.append(char(n));
    f.append(message);
    return f;
}

bool EmulatorGrpcClient::unframe(const QByteArray &body, QByteArray *message)
{
    if (body.size() < 5 || body.at(0) != 0) return false;   // сжатые ответы не запрашиваем
    const quint32 n = (quint32(quint8(body.at(1))) << 24) | (quint32(quint8(body.at(2))) << 16)
                    | (quint32(quint8(body.at(3))) << 8)  |  quint32(quint8(body.at(4)));
    if (qint64(n) > qint64(body.size()) - 5) return false;
    *message = body.mid(5, int(n));
    return true;
}

// EmulatorStatus { string version = 1; uint64 uptime = 2; bool booted = 3; … }
EmulatorGrpcClient::Status EmulatorGrpcClient::decodeStatus(const QByteArray &m)
{
    Status st;
    int pos = 0;
    while (pos < m.size()) {
        quint64 key = 0;
        if (!getVarint(m, pos, &key)) { st.error = QStringLiteral("bad protobuf"); return st; }
        const int field = int(key >> 3), wire = int(key & 7);
        if (wire == 0) {
            quint64 v = 0;
            if (!getVarint(m, pos, &v)) { st.error = QStringLiteral("bad protobuf"); return st; }
            if (field == 3) st.booted = (v != 0);
        } else if (wire == 2) {
            quint64 len = 0;
            if (!getVarint(m, pos, &len) || qint64(len) > qint64(m.size() - pos)) {
                st.error = QStringLiteral("bad protobuf"); return st;
            }
            if (field == 1) st.version = QString::fromUtf8(m.mid(pos, int(len)));
            pos += int(len);
        } else if (wire == 1) { pos += 8; }
        else if (wire == 5)   { pos += 4; }
        else { st.error = QStringLiteral("bad protobuf"); return st; }
    }
    st.ok = true;
    return st;
}

void EmulatorGrpcClient::call(const QString &method, const QByteArray &message, Done done)
{
    if (m_port <= 0) { done(false, {}, QStringLiteral("not configured")); return; }

    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/%2/%3")
                                 .arg(m_port).arg(QLatin1String(kService), method)));
    // HTTP/2 сразу, без TLS и без Upgrade (gRPC «prior knowledge»)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
    req.setAttribute(QNetworkRequest::Http2DirectAttribute, true);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/grpc"));
    req.setRawHeader("te", "trailers");
    if (!m_token.isEmpty()) req.setRawHeader("authorization", "Bearer " + m_token);
    req.setTransferTimeout(m_timeoutMs);
    // Без HTTP-кэша: иначе Qt пишет «caching was enabled after some bytes had been written»
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);

    QNetworkReply *reply = m_nam->post(req, frame(message));
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && http != 200) {
            done(false, {}, reply->errorString());
            return;
        }
        // Ошибка без тела приходит «trailers-only» — статус в заголовках.
        // Qt склеивает одинаковые поля заголовков и трейлеров через запятую:
        // «0, 0» — это успех (эмулятор 36.4 так и отвечает)
        const QByteArray grpcStatus = reply->rawHeader("grpc-status");
        if (!statusIsOk(grpcStatus)) {
            done(false, {}, QStringLiteral("grpc-status %1: %2")
                                .arg(QString::fromLatin1(grpcStatus),
                                     QString::fromUtf8(QByteArray::fromPercentEncoding(
                                         reply->rawHeader("grpc-message")))));
            return;
        }
        if (http != 200) { done(false, {}, QStringLiteral("HTTP %1").arg(http)); return; }
        QByteArray msg;
        const QByteArray body = reply->readAll();
        if (!body.isEmpty() && !unframe(body, &msg)) { done(false, {}, QStringLiteral("bad gRPC frame")); return; }
        done(true, msg, {});
    });
}

bool EmulatorGrpcClient::statusIsOk(const QByteArray &grpcStatus)
{
    for (const QByteArray &part : grpcStatus.split(',')) {
        const QByteArray v = part.trimmed();
        if (!v.isEmpty() && v != "0") return false;
    }
    return true;
}

void EmulatorGrpcClient::getStatus(std::function<void(const Status &)> done)
{
    call(QStringLiteral("getStatus"), {}, [done](bool ok, const QByteArray &m, const QString &err) {
        if (!ok) { Status st; st.error = err; done(st); return; }
        done(decodeStatus(m));
    });
}

void EmulatorGrpcClient::sendKeyPress(int evdevCode)
{
    enqueue(QStringLiteral("sendKey"), encodeKeyPress(evdevCode));
}

void EmulatorGrpcClient::sendText(const QString &text)
{
    if (!text.isEmpty()) enqueue(QStringLiteral("sendKey"), encodeText(text));
}

void EmulatorGrpcClient::enqueue(const QString &method, const QByteArray &message)
{
    if (m_queue.size() >= 32) m_queue.dequeue();     // залипшая очередь не растёт
    m_queue.enqueue({ method, message });
    pump();
}

// По одному вызову за раз: параллельные потоки HTTP/2 могли бы переставить нажатия
void EmulatorGrpcClient::pump()
{
    if (m_inFlight || m_queue.isEmpty()) return;
    const Pending p = m_queue.dequeue();
    m_inFlight = true;
    call(p.method, p.message, [this, method = p.method](bool ok, const QByteArray &, const QString &err) {
        m_inFlight = false;
        if (!ok) emit callFailed(method, err);
        pump();
    });
}
