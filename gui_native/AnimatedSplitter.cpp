#include "AnimatedSplitter.h"

#include <algorithm>
#include <cmath>
#include <QPainter>
#include <QSplitterHandle>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QTimer>

static int lerpInt(int a, int b, qreal k)
{
    return static_cast<int>(std::lround(a + (b - a) * k));
}

static QColor lerpColor(const QColor &a, const QColor &b, qreal k)
{
    QColor out;
    out.setRed(lerpInt(a.red(), b.red(), k));
    out.setGreen(lerpInt(a.green(), b.green(), k));
    out.setBlue(lerpInt(a.blue(), b.blue(), k));
    out.setAlpha(lerpInt(a.alpha(), b.alpha(), k));
    return out;
}

class AnimatedSplitterOverlay final : public QWidget {
public:
    explicit AnimatedSplitterOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
    }

    void setColor(const QColor &c)
    {
        if (c == m_color) {
            return;
        }
        m_color = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect(), m_color);
    }

private:
    QColor m_color = QColor(QStringLiteral("#303030"));
};

class AnimatedSplitterHandle final : public QSplitterHandle {
public:
    AnimatedSplitterHandle(Qt::Orientation orientation, AnimatedSplitter *parent)
        : QSplitterHandle(orientation, parent)
        , m_parent(parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setCursor(orientation == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);

        m_overlay = new AnimatedSplitterOverlay(parent);
        m_overlay->hide();

        m_anim.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&m_anim, &QVariantAnimation::valueChanged, parent, [this](const QVariant &v) {
            m_progress = std::clamp(v.toReal(), 0.0, 1.0);
            updateOverlay();
            update();
        });

        m_hoverDelay.setSingleShot(true);
        m_hoverDelay.setInterval(250);
        QObject::connect(&m_hoverDelay, &QTimer::timeout, parent, [this]() {
            if (!m_hovering || m_pressed) {
                return;
            }
            retarget();
        });
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QSplitterHandle::enterEvent(event);
        m_hovering = true;
        if (m_pressed) {
            retarget();
            return;
        }
        m_hoverDelay.stop();
        m_hoverDelay.start();
    }

    void leaveEvent(QEvent *event) override
    {
        QSplitterHandle::leaveEvent(event);
        m_hovering = false;
        m_hoverDelay.stop();
        retarget();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        QSplitterHandle::mousePressEvent(event);
        if (event->button() == Qt::LeftButton) {
            m_pressed = true;
            m_hoverDelay.stop();
            retarget();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QSplitterHandle::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton) {
            m_pressed = false;
            retarget();
        }
    }

    bool event(QEvent *event) override
    {
        const bool ok = QSplitterHandle::event(event);
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            updateOverlay();
        }
        return ok;
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        if (!m_parent) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        const qreal t = std::clamp(m_progress, 0.0, 1.0);
        const int thick = lerpInt(m_parent->baseThickness(), m_parent->expandedThickness(), t);
        const QColor color = lerpColor(m_parent->baseColor(), m_parent->highlightColor(), t);

        painter.fillRect(rect(), Qt::transparent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);

        QRect r = rect();
        if (orientation() == Qt::Horizontal) {
            const int w = std::min(thick, r.width());
            const int x = r.center().x() - w / 2;
            r = QRect(x, r.top(), w, r.height());
        } else {
            const int h = std::min(thick, r.height());
            const int y = r.center().y() - h / 2;
            r = QRect(r.left(), y, r.width(), h);
        }
        painter.fillRect(r, color);
    }

private:
    void retarget()
    {
        const qreal target = (m_hovering || m_pressed) ? 1.0 : 0.0;
        m_anim.stop();
        m_anim.setDuration(m_parent ? m_parent->durationMs() : 140);
        m_anim.setStartValue(m_progress);
        m_anim.setEndValue(target);
        m_anim.start();
    }

    void updateOverlay()
    {
        if (!m_parent || !m_overlay) {
            return;
        }
        if (m_progress <= 0.001) {
            m_overlay->hide();
            return;
        }

        const qreal t = std::clamp(m_progress, 0.0, 1.0);
        const int thick = lerpInt(m_parent->baseThickness(), m_parent->expandedThickness(), t);
        const QColor color = lerpColor(m_parent->baseColor(), m_parent->highlightColor(), t);
        m_overlay->setColor(color);

        QRect handleRect = geometry();
        QRect r = handleRect;
        if (orientation() == Qt::Horizontal) {
            const int w = thick;
            const int x = handleRect.center().x() - w / 2;
            r = QRect(x, 0, w, m_parent->height());
        } else {
            const int h = thick;
            const int y = handleRect.center().y() - h / 2;
            r = QRect(0, y, m_parent->width(), h);
        }
        m_overlay->setGeometry(r);
        m_overlay->raise();
        m_overlay->show();
        m_overlay->update();
    }

    AnimatedSplitter *m_parent = nullptr;
    AnimatedSplitterOverlay *m_overlay = nullptr;
    QVariantAnimation m_anim;
    QTimer m_hoverDelay;
    qreal m_progress = 0.0;
    bool m_hovering = false;
    bool m_pressed = false;
};

AnimatedSplitter::AnimatedSplitter(Qt::Orientation orientation, QWidget *parent)
    : QSplitter(orientation, parent)
{
    setChildrenCollapsible(false);
    setOpaqueResize(true);
    setHandleWidth(m_baseThickness);
}

void AnimatedSplitter::setBaseThickness(int px)
{
    px = std::clamp(px, 1, 64);
    if (px == m_baseThickness) {
        return;
    }
    m_baseThickness = px;
    setHandleWidth(m_baseThickness);
}

void AnimatedSplitter::setExpandedThickness(int px)
{
    px = std::clamp(px, 1, 64);
    if (px == m_expandedThickness) {
        return;
    }
    m_expandedThickness = px;
}

void AnimatedSplitter::setBaseColor(const QColor &c)
{
    if (c == m_baseColor) {
        return;
    }
    m_baseColor = c;
}

void AnimatedSplitter::setHighlightColor(const QColor &c)
{
    if (c == m_highlightColor) {
        return;
    }
    m_highlightColor = c;
}

QSplitterHandle *AnimatedSplitter::createHandle()
{
    return new AnimatedSplitterHandle(orientation(), this);
}
