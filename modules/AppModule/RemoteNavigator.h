// modules/AppModule/RemoteNavigator.h
// ─────────────────────────────────────────────────────────────────────────────
// Навигация по интерфейсу стрелками пульта / клавиатуры (аудит §11 п.1).
//
// Вместо KeyNavigation в каждом из ~50 QML-файлов — пространственная навигация
// по тому, что уже кликается мышью:
//   • кандидаты — видимые и доступные MouseArea, кнопки Qt Quick Controls и
//     поля ввода в текущем «слое» (открытый диалог/всплывающее окно, иначе
//     весь экран);
//   • стрелка выбирает ближайший кандидат в этом направлении, курсор
//     переносится на его центр (срабатывает обычная подсветка наведения),
//     вокруг рисуется рамка фокуса (Main.qml);
//   • OK/Enter — щелчок в центр выбранного элемента.
// Движение мыши (аэромышь) убирает рамку — дальше всё как раньше.
//
// Разметка в QML (необязательно):
//   property bool navModal: true   — элемент перекрывает всё под собой
//                                    (диалоги, меню); навигация только внутри;
//   property bool navSkip:  true   — не выбирать этот элемент и его потомков.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QPointF>
#include <QTimer>
#include <QList>

#include <QQuickItem>
#include <QQuickWindow>

class RemoteNavigator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool   active     READ active     NOTIFY changed)
    Q_PROPERTY(QRectF targetRect READ targetRect NOTIFY changed)

public:
    explicit RemoteNavigator(QObject *parent = nullptr);

    void setWindow(QQuickWindow *window);

    bool   active()     const { return m_active; }
    QRectF targetRect() const { return m_targetRect; }

    /// dir: "left" / "right" / "up" / "down". false — в этом направлении
    /// ничего нет (Main.qml тогда прокручивает экран, как раньше).
    Q_INVOKABLE bool move(const QString &dir);
    /// Щелчок по выбранному элементу. false — рамки нет (OK обработает
    /// аэромышь сама) или фокус у поля ввода (Enter принадлежит ему).
    Q_INVOKABLE bool activate();
    /// Убрать рамку (переход между экранами, мышь).
    Q_INVOKABLE void clear();
    /// true — фокус у поля ввода: влево/вправо двигают курсор в тексте.
    Q_INVOKABLE bool textInputFocused() const;

    // Для тестов: текущий выбранный элемент
    QQuickItem *target() const { return m_target.data(); }

    struct Candidate {
        QQuickItem *item = nullptr;
        QRectF      rect;          // видимая часть в координатах окна
    };
    QList<Candidate> collectCandidates() const;

    /// Выбор следующего кандидата (чистая геометрия, вынесено для тестов).
    /// Возвращает индекс в cands или -1.
    static int pickInDirection(const QRectF &from, const QList<QRectF> &cands,
                               const QString &dir);

signals:
    void changed();

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void   setTarget(QQuickItem *item, const QRectF &rect);
    void   warpCursor(const QPointF &scenePos);
    void   refresh();
    QQuickItem *modalLayer() const;

    QPointer<QQuickWindow> m_window;
    QPointer<QQuickItem>   m_target;
    QRectF  m_targetRect;
    bool    m_active = false;
    QPointF m_warpedGlobal;      // куда переносили курсор (для отличия от движения мыши)
    QTimer  m_refreshTimer;      // рамка следует за элементом при прокрутке/анимации
};
