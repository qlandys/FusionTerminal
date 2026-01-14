#pragma once

#include <QColor>
#include <QQuickItem>

class GpuMarkerItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QColor fillColor READ fillColor WRITE setFillColor NOTIFY fillColorChanged)
    Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor NOTIFY borderColorChanged)
    Q_PROPERTY(qreal phase READ phase WRITE setPhase NOTIFY phaseChanged)
    Q_PROPERTY(qreal tipBias READ tipBias WRITE setTipBias NOTIFY tipBiasChanged)

public:
    explicit GpuMarkerItem(QQuickItem *parent = nullptr);

    QColor fillColor() const { return m_fillColor; }
    void setFillColor(const QColor &c);

    QColor borderColor() const { return m_borderColor; }
    void setBorderColor(const QColor &c);

    qreal phase() const { return m_phase; }
    void setPhase(qreal v);

    qreal tipBias() const { return m_tipBias; }
    void setTipBias(qreal v);

signals:
    void fillColorChanged();
    void borderColorChanged();
    void phaseChanged();
    void tipBiasChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    QColor m_fillColor = QColor("#4caf50");
    QColor m_borderColor = QColor("#2f6c37");
    qreal m_phase = 0.0;
    qreal m_tipBias = 0.0;
};

