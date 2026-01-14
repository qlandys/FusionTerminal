#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>

class QAbstractItemModel;

class GpuTrailItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *circlesModel READ circlesModel WRITE setCirclesModel NOTIFY circlesModelChanged)
    Q_PROPERTY(int sidePadding READ sidePadding WRITE setSidePadding NOTIFY sidePaddingChanged)
    Q_PROPERTY(int maxSegments READ maxSegments WRITE setMaxSegments NOTIFY maxSegmentsChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal opacity READ opacity WRITE setOpacity NOTIFY opacityChanged)
    Q_PROPERTY(qreal lineWidth READ lineWidth WRITE setLineWidth NOTIFY lineWidthChanged)

public:
    explicit GpuTrailItem(QQuickItem *parent = nullptr);

    QObject *circlesModel() const;
    void setCirclesModel(QObject *obj);

    int sidePadding() const { return m_sidePadding; }
    void setSidePadding(int v);

    int maxSegments() const { return m_maxSegments; }
    void setMaxSegments(int v);

    QColor color() const { return m_color; }
    void setColor(const QColor &c);

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal v);

    qreal lineWidth() const { return m_lineWidth; }
    void setLineWidth(qreal v);

signals:
    void circlesModelChanged();
    void sidePaddingChanged();
    void maxSegmentsChanged();
    void colorChanged();
    void opacityChanged();
    void lineWidthChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    void reconnectModel();
    void resolveRoles();
    float xForRatio(float ratio) const;

    QPointer<QAbstractItemModel> m_model;
    int m_roleXRatio = -1;
    int m_roleY = -1;
    int m_sidePadding = 6;
    int m_maxSegments = 256;
    QColor m_color = QColor("#ffffff");
    qreal m_opacity = 0.35;
    qreal m_lineWidth = 1.25;
};

