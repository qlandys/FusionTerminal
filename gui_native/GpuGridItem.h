#pragma once

#include <QColor>
#include <QQuickItem>

class GpuGridItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool vertical READ vertical WRITE setVertical NOTIFY verticalChanged)
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)
    Q_PROPERTY(int step READ step WRITE setStep NOTIFY stepChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY opacityChanged)

public:
    explicit GpuGridItem(QQuickItem *parent = nullptr);

    bool vertical() const { return m_vertical; }
    void setVertical(bool v);

    int count() const { return m_count; }
    void setCount(int v);

    int step() const { return m_step; }
    void setStep(int v);

    QColor color() const { return m_color; }
    void setColor(const QColor &c);

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal v);

signals:
    void verticalChanged();
    void countChanged();
    void stepChanged();
    void colorChanged();
    void opacityChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    bool m_vertical = false;
    int m_count = 0;
    int m_step = 20;
    QColor m_color = QColor("#303030");
    qreal m_opacity = 0.35;
};

