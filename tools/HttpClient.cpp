// tools/HttpClient.cpp
#include "HttpClient.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QDebug>

namespace HttpClient {

const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

QByteArray get(const QString &url, int timeoutMs, const std::atomic<bool> *cancelled)
{
    // Проверяем флаг отмены до отправки запроса
    if (cancelled && cancelled->load())
        return {};

    // NAM создаётся локально — должен жить в том же потоке что и используется.
    // Это правило Qt: QNetworkAccessManager не потокобезопасен.
    QNetworkAccessManager nam;

    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept",
        "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.setRawHeader("Accept-Language",
        "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = nam.get(request);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        reply->abort();
        loop.quit();
    });

    // Флаг отмены проверяем и ВО ВРЕМЯ запроса — раньше отмена срабатывала
    // только по окончании (до 30 с ожидания при загрузке трейлера/обложек).
    QTimer cancelPoll;
    if (cancelled) {
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
            if (cancelled->load()) {
                reply->abort();
                loop.quit();
            }
        });
        cancelPoll.start(100);
    }

    timer.start(timeoutMs);
    if (!reply->isFinished())
        loop.exec();
    cancelPoll.stop();
    timer.stop();

    // Проверяем флаг отмены после получения ответа
    if (cancelled && cancelled->load()) {
        reply->deleteLater();
        return {};
    }

    QByteArray data;

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        // Таймаут — не логируем как ошибку, это штатная ситуация
        qDebug() << "HttpClient: timeout after" << timeoutMs << "ms for" << url;
    } else if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "HttpClient: error" << reply->errorString() << "for" << url;
    } else {
        const int statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode == 200) {
            data = reply->readAll();
        } else {
            qDebug() << "HttpClient: HTTP" << statusCode << "for" << url;
        }
    }

    reply->deleteLater();
    return data;
}

} // namespace HttpClient
