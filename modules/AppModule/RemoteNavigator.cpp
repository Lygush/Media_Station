// modules/AppModule/RemoteNavigator.cpp
#include "RemoteNavigator.h"

#include <QQuickWindow>
#include <QQuickItem>
#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScreen>
#include <QDateTime>
#include <qpa/qwindowsysteminterface.h>
#include <QtMath>
#include <functional>
#include <algorithm>

namespace {

// Видимая доля, ниже которой элемент считается «за краем» прокрутки
constexpr qreal kMinVisibleFraction = 0.3;
// MouseArea больше этой доли окна — подложка/«ловушка» кликов, а не кнопка
constexpr qreal kMaxMouseAreaFraction = 0.4;
constexpr qreal kMinSide = 6.0;
// Движение мыши дальше этого (px) от места, куда мы перенесли курсор, — рамку убираем
constexpr qreal kMouseMoveTolerance = 12.0;   // дрожание аэромыши не сбрасывает рамку

bool flagProperty(const QObject *o, const char *name)
{
    const QVariant v = o->property(name);
    return v.isValid() && v.toBool();
}

bool isCandidateItem(const QQuickItem *item)
{
    if (!item->isEnabled()) return false;
    // У MouseArea своё свойство enabled (не Item::enabled): выключенная
    // MouseArea остаётся isEnabled() == true
    const QVariant ownEnabled = item->property("enabled");
    if (ownEnabled.isValid() && !ownEnabled.toBool()) return false;
    if (flagProperty(item, "navFocusable")) return true;

    if (item->inherits("QQuickMouseArea")) {
        const int buttons = item->property("acceptedButtons").toInt();
        return (buttons & Qt::LeftButton) != 0;
    }
    if (item->inherits("QQuickAbstractButton"))
        return true;
    if (item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit"))
        return !flagProperty(item, "readOnly");
    return false;
}

qreal area(const QRectF &r) { return r.isEmpty() ? 0.0 : r.width() * r.height(); }

qreal overlapRatio(const QRectF &a, const QRectF &b)   // доля меньшего, покрытая пересечением
{
    const qreal inter = area(a.intersected(b));
    const qreal smaller = qMin(area(a), area(b));
    return smaller > 0 ? inter / smaller : 0.0;
}

// Дети в порядке отрисовки: по z, при равном z — по порядку объявления
QList<QQuickItem *> paintOrderChildren(const QQuickItem *item)
{
    QList<QQuickItem *> children = item->childItems();
    std::stable_sort(children.begin(), children.end(),
                     [](const QQuickItem *a, const QQuickItem *b) { return a->z() < b->z(); });
    return children;
}

} // namespace

RemoteNavigator::RemoteNavigator(QObject *parent) : QObject(parent)
{
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &RemoteNavigator::refresh);
}

void RemoteNavigator::setWindow(QQuickWindow *window)
{
    if (m_window) m_window->removeEventFilter(this);
    m_window = window;
    if (m_window) m_window->installEventFilter(this);
}

// ── Слой и кандидаты ─────────────────────────────────────────────────────────

QQuickItem *RemoteNavigator::modalLayer() const
{
    if (!m_window) return nullptr;
    QQuickItem *top = nullptr;
    std::function<void(QQuickItem *, qreal)> walk = [&](QQuickItem *item, qreal opacity) {
        if (!item->isVisible()) return;
        opacity *= item->opacity();
        if (opacity < 0.05) return;
        // Последний в порядке отрисовки — самый верхний
        if (flagProperty(item, "navModal") || item->inherits("QQuickPopupItem"))
            top = item;
        for (QQuickItem *c : paintOrderChildren(item))
            walk(c, opacity);
    };
    walk(m_window->contentItem(), 1.0);
    return top;
}

QList<RemoteNavigator::Candidate> RemoteNavigator::collectCandidates() const
{
    QList<Candidate> out;
    if (!m_window) return out;

    const QRectF windowRect(QPointF(0, 0), QSizeF(m_window->width(), m_window->height()));
    const qreal windowArea = area(windowRect);
    QQuickItem *root = modalLayer();
    if (!root) root = m_window->contentItem();

    std::function<void(QQuickItem *, QRectF, qreal)> walk =
        [&](QQuickItem *item, QRectF clip, qreal opacity) {
            if (!item->isVisible()) return;
            opacity *= item->opacity();
            if (opacity < 0.05) return;
            if (flagProperty(item, "navSkip")) return;

            const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            if (isCandidateItem(item)) {
                const QRectF vis = scene.intersected(clip);
                const bool bigCatcher = item->inherits("QQuickMouseArea")
                                        && area(scene) > kMaxMouseAreaFraction * windowArea;
                if (!bigCatcher && vis.width() >= kMinSide && vis.height() >= kMinSide
                    && area(vis) >= kMinVisibleFraction * area(scene)) {
                    // Вложенные почти совпадающие (MouseArea внутри кнопки) — один кандидат
                    bool dup = false;
                    for (const Candidate &c : std::as_const(out))
                        if (overlapRatio(c.rect, vis) > 0.9
                            && qAbs(area(c.rect) - area(vis)) < 0.2 * area(vis)) { dup = true; break; }
                    if (!dup) out.append({ item, vis });
                }
            }
            const QRectF childClip = item->clip() ? clip.intersected(scene) : clip;
            if (childClip.isEmpty()) return;
            for (QQuickItem *c : paintOrderChildren(item))
                walk(c, childClip, opacity);
        };

    // Прозрачность и обрезка предков слоя тоже учитываются
    qreal opacity = 1.0;
    QRectF clip = windowRect;
    for (QQuickItem *p = root->parentItem(); p; p = p->parentItem()) {
        opacity *= p->opacity();
        if (p->clip())
            clip = clip.intersected(p->mapRectToScene(QRectF(0, 0, p->width(), p->height())));
    }
    walk(root, clip, opacity);
    return out;
}

// ── Выбор направления ────────────────────────────────────────────────────────

/*static*/ int RemoteNavigator::pickInDirection(const QRectF &from, const QList<QRectF> &cands,
                                                const QString &dir)
{
    const bool horizontal = (dir == QLatin1String("left") || dir == QLatin1String("right"));
    const int  sign = (dir == QLatin1String("right") || dir == QLatin1String("down")) ? 1 : -1;
    const QPointF fc = from.center();

    int best = -1;
    qreal bestScore = 0;
    for (int i = 0; i < cands.size(); ++i) {
        const QRectF &r = cands[i];
        if (overlapRatio(r, from) > 0.5) continue;     // тот же элемент или вложенный в него
        const QPointF c = r.center();

        // Основная ось: кандидат должен быть дальше по направлению
        const qreal primary = horizontal ? (c.x() - fc.x()) * sign : (c.y() - fc.y()) * sign;
        if (primary <= 1.0) continue;
        // Зазор между краями по основной оси (0 — края заходят друг на друга)
        const qreal edgeGap = horizontal
            ? qMax<qreal>(0, sign > 0 ? r.left() - from.right() : from.left() - r.right())
            : qMax<qreal>(0, sign > 0 ? r.top() - from.bottom() : from.top() - r.bottom());

        // Поперечная ось: зазор между проекциями (0 — в одном ряду/столбце)
        const qreal orthoGap = horizontal
            ? qMax<qreal>(0, qMax(r.top() - from.bottom(), from.top() - r.bottom()))
            : qMax<qreal>(0, qMax(r.left() - from.right(), from.left() - r.right()));
        const qreal orthoCenter = horizontal ? qAbs(c.y() - fc.y()) : qAbs(c.x() - fc.x());

        // Тот же ряд/столбец (проекции перекрываются) важнее близости:
        // элемент соседнего ряда выбирается, только если в своём ряду в этом
        // направлении ничего нет.
        const qreal score = edgeGap + 0.25 * primary + 0.3 * orthoCenter
                            + (orthoGap > 0 ? 100000.0 + 4.0 * orthoGap : 0.0);
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    return best;
}

// ── Действия ─────────────────────────────────────────────────────────────────

bool RemoteNavigator::move(const QString &dir)
{
    if (!m_window) return false;
    const QList<Candidate> cands = collectCandidates();
    if (cands.isEmpty()) { clear(); return false; }

    int cur = -1;
    if (m_active && m_target) {
        for (int i = 0; i < cands.size(); ++i)
            if (cands[i].item == m_target) { cur = i; break; }
    }

    if (cur < 0) {
        // Первое нажатие: рамка на элемент под курсором (тогда сразу шаг) или
        // на ближайший к курсору; если курсор вне окна — на верхний левый.
        const QPointF cursor = m_window->mapFromGlobal(QCursor::pos());
        const bool inWindow = QRectF(0, 0, m_window->width(), m_window->height()).contains(cursor);
        int under = -1, nearest = -1;
        qreal nearestDist = 0;
        for (int i = 0; i < cands.size(); ++i) {
            const QRectF &r = cands[i].rect;
            if (inWindow && r.contains(cursor)
                && (under < 0 || area(r) < area(cands[under].rect)))
                under = i;
            const QPointF ref = inWindow ? cursor : QPointF(0, 0);
            const QPointF d = r.center() - ref;
            const qreal dist = inWindow ? qSqrt(d.x() * d.x() + d.y() * d.y())
                                        : r.top() * 4 + r.left();   // порядок чтения
            if (nearest < 0 || dist < nearestDist) { nearest = i; nearestDist = dist; }
        }
        if (under < 0) {
            setTarget(cands[nearest].item, cands[nearest].rect);
            return true;
        }
        cur = under;
    }

    QList<QRectF> rects;
    rects.reserve(cands.size());
    for (const Candidate &c : cands) rects.append(c.rect);
    const int next = pickInDirection(cands[cur].rect, rects, dir);
    if (next < 0) {
        setTarget(cands[cur].item, cands[cur].rect);   // остаёмся, рамку показываем
        return false;
    }
    setTarget(cands[next].item, cands[next].rect);
    return true;
}

bool RemoteNavigator::activate()
{
    if (!m_window || !m_active || !m_target) return false;
    if (textInputFocused()) return false;            // Enter принадлежит полю ввода
    refresh();
    if (!m_active || !m_target) return false;

    const QPointF scenePos = m_targetRect.center();
    const QPointF global   = m_window->mapToGlobal(scenePos);

    // Через QWindowSystemInterface — тем же путём, что настоящий ввод от ОС:
    // QGuiApplication ведёт учёт нажатий устройства, и отпускание доходит до
    // того, кто принял нажатие. QCoreApplication::sendEvent(QMouseEvent) этого
    // не делает — MouseArea получала pressed, но не clicked.
    using WSI = QWindowSystemInterface;
    const ulong ts = ulong(QDateTime::currentMSecsSinceEpoch());
    WSI::handleMouseEvent<WSI::SynchronousDelivery>(m_window, ts, scenePos, global,
        Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
    WSI::handleMouseEvent<WSI::SynchronousDelivery>(m_window, ts + 1, scenePos, global,
        Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress);
    WSI::handleMouseEvent<WSI::SynchronousDelivery>(m_window, ts + 60, scenePos, global,
        Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease);
    return true;
}

void RemoteNavigator::clear()
{
    m_refreshTimer.stop();
    if (!m_active && !m_target) return;
    m_active = false;
    m_target = nullptr;
    m_targetRect = {};
    emit changed();
}

bool RemoteNavigator::textInputFocused() const
{
    if (!m_window) return false;
    const QQuickItem *f = m_window->activeFocusItem();
    if (!f) return false;
    if (!(f->inherits("QQuickTextInput") || f->inherits("QQuickTextEdit"))) return false;
    return !flagProperty(f, "readOnly");
}

// ── Внутреннее ───────────────────────────────────────────────────────────────

void RemoteNavigator::setTarget(QQuickItem *item, const QRectF &rect)
{
    m_target = item;
    m_targetRect = rect;
    m_active = true;
    warpCursor(rect.center());
    if (!m_refreshTimer.isActive()) m_refreshTimer.start();
    emit changed();
}

void RemoteNavigator::warpCursor(const QPointF &scenePos)
{
    if (!m_window) return;
    const QPoint global = m_window->mapToGlobal(scenePos.toPoint());
    m_warpedGlobal = global;
    QCursor::setPos(m_window->screen(), global);

    // Наведение сразу, не дожидаясь системного WM_MOUSEMOVE (и на платформах,
    // где перенос курсора запрещён)
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        m_window, scenePos, QPointF(global), Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
}

void RemoteNavigator::refresh()
{
    if (!m_active) { m_refreshTimer.stop(); return; }
    if (!m_target || !m_window) { clear(); return; }

    // Элемент мог исчезнуть, перекрыться диалогом или уехать при прокрутке
    for (const Candidate &c : collectCandidates()) {
        if (c.item != m_target) continue;
        if (c.rect != m_targetRect) {
            m_targetRect = c.rect;
            emit changed();
        }
        return;
    }
    clear();
}

bool RemoteNavigator::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_window && m_active && ev->type() == QEvent::MouseMove) {
        const QPointF g = static_cast<QMouseEvent *>(ev)->globalPosition();
        const QPointF d = g - QPointF(m_warpedGlobal);
        if (qAbs(d.x()) > kMouseMoveTolerance || qAbs(d.y()) > kMouseMoveTolerance)
            clear();   // аэромышь/мышь взяла управление
    }
    return QObject::eventFilter(obj, ev);
}
