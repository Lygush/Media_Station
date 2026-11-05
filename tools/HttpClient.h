// tools/HttpClient.h
// ─────────────────────────────────────────────────────────────────────────────
// HttpClient — синхронный HTTP GET для использования в фоновых потоках.
//
// ВАЖНО: вызывать ТОЛЬКО из фонового потока, не из GUI-потока.
//        Внутри используется QEventLoop — вызов из GUI заморозит интерфейс.
//
// Использование:
//
//   // Простой GET с таймаутом по умолчанию (15 сек):
//   QByteArray data = HttpClient::get("https://example.com/api");
//
//   // С кастомным таймаутом (30 сек):
//   QByteArray data = HttpClient::get(url, HttpClient::kTimeoutLong);
//
//   // С проверкой флага отмены (для CoverFetcher и т.п.):
//   QByteArray data = HttpClient::get(url, HttpClient::kTimeoutLong, &m_cancelled);
//
// Возвращает пустой QByteArray при ошибке, таймауте, 404 или отмене.
// Ошибки пишутся в qDebug/qWarning.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>

namespace HttpClient {

// Таймауты в миллисекундах
static constexpr int kTimeoutDefault = 15'000;  // для API-запросов
static constexpr int kTimeoutLong    = 30'000;  // для загрузки медиа

// Стандартный User-Agent (браузероподобный, обходит базовые блокировки)
extern const char *kUserAgent;

/// Выполняет синхронный HTTP GET запрос.
///
/// @param url        URL для запроса
/// @param timeoutMs  Таймаут в мс (по умолчанию kTimeoutDefault)
/// @param cancelled  Опциональный флаг отмены. Если установлен в true
///                   до или во время запроса — возвращает пустой массив.
///
/// @return Тело ответа при HTTP 200, иначе пустой QByteArray.
QByteArray get(const QString          &url,
               int                     timeoutMs = kTimeoutDefault,
               const std::atomic<bool> *cancelled = nullptr);

/// То же, но сообщает код ответа: HTTP-статус, либо 0 — сети нет / таймаут /
/// отмена. Нужен, чтобы не повторять запрос, когда интернета нет вовсе.
QByteArray get(const QString          &url,
               int                     timeoutMs,
               const std::atomic<bool> *cancelled,
               int                    *httpStatus);

} // namespace HttpClient
