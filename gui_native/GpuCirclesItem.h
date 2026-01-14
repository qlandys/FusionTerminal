#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>

class QAbstractItemModel;

class GpuCirclesItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *circlesModel READ circlesModel WRITE setCirclesModel NOTIFY circlesModelChanged)
    Q_PROPERTY(int sidePadding READ sidePadding WRITE setSidePadding NOTIFY sidePaddingChanged)
    Q_PROPERTY(bool printsSquare READ printsSquare WRITE setPrintsSquare NOTIFY printsSquareChanged)
    Q_PROPERTY(int maxCircles READ maxCircles WRITE setMaxCircles NOTIFY maxCirclesChanged)
    Q_PROPERTY(int circleSegments READ circleSegments WRITE setCircleSegments NOTIFY circleSegmentsChanged)

public:
    explicit GpuCirclesItem(QQuickItem *parent = nullptr);

    QObject *circlesModel() const;
    void setCirclesModel(QObject *obj);

    int sidePadding() const { return m_sidePadding; }
    void setSidePadding(int v);

    bool printsSquare() const { return m_printsSquare; }
    void setPrintsSquare(bool v);

    int maxCircles() const { return m_maxCircles; }
    void setMaxCircles(int v);

    int circleSegments() const { return m_circleSegments; }
    void setCircleSegments(int v);

signals:
    void circlesModelChanged();
    void sidePaddingChanged();
    void printsSquareChanged();
    void maxCirclesChanged();
    void circleSegmentsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    void reconnectModel();
    void resolveRoles();
    float xForRatio(float ratio) const;

    QPointer<QAbstractItemModel> m_model;
    int m_roleXRatio = -1;
    int m_roleY = -1;
    int m_roleRadius = -1;
    int m_roleFillColor = -1;
    int m_roleBorderColor = -1;

    int m_sidePadding = 6;
    bool m_printsSquare = false;
    int m_maxCircles = 96;
    int m_circleSegments = 72;
};
