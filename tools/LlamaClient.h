// tools/LlamaClient.h
#pragma once

#include <QObject>
#include <QString>
#include <functional>

struct llama_model;
struct llama_context;

class EventBus;

class LlamaClient : public QObject
{
    Q_OBJECT
public:
    explicit LlamaClient(QObject *parent = nullptr);
    ~LlamaClient();

    // Передать EventBus для логирования через LoggerModule.
    // Вызывать до initialize().
    void setBus(EventBus *bus) { m_bus = bus; }

    // Загружает модель в VRAM. Вызвать перед runInference().
    bool initialize(const QString &modelPath);

    // Выгружает модель из VRAM.
    void shutdown();

    bool isReady() const { return m_ready; }

    // Колбэк вызывается из потока генерации на каждый новый токен.
    // Для передачи в GUI используй QMetaObject::invokeMethod(..., Qt::QueuedConnection).
    using TokenCallback = std::function<void(const QString &token)>;

    // Выполняет один inference. Возвращает полный текст ответа.
    // onToken вызывается для каждого сгенерированного токена (опционально).
    QString runInference(const QString &systemPrompt, const QString &userPrompt,
                         const TokenCallback &onToken = nullptr);

    // Логирование через EventBus → LoggerModule. Безопасно из любого потока.
    void log(const char *level, const QString &message);

private:
    llama_model   *m_model = nullptr;
    llama_context *m_ctx   = nullptr;
    bool           m_ready = false;
    EventBus      *m_bus   = nullptr;
};
