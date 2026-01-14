#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVector>

#include "DomLevelsModel.h"

class GpuDomItem : public QQuickItem
{
    Q_OBJECT

    Q_PROPERTY(int rowHeight READ rowHeight WRITE setRowHeight NOTIFY rowHeightChanged)
    Q_PROPERTY(int priceColumnWidth READ priceColumnWidth WRITE setPriceColumnWidth NOTIFY priceColumnWidthChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(QColor gridColor READ gridColor WRITE setGridColor NOTIFY gridColorChanged)
    Q_PROPERTY(QColor hoverColor READ hoverColor WRITE setHoverColor NOTIFY hoverColorChanged)
    Q_PROPERTY(QColor priceBorderColor READ priceBorderColor WRITE setPriceBorderColor NOTIFY priceBorderColorChanged)
    Q_PROPERTY(int hoverRow READ hoverRow WRITE setHoverRow NOTIFY hoverRowChanged)
    Q_PROPERTY(qreal orderHighlightPhase READ orderHighlightPhase WRITE setOrderHighlightPhase NOTIFY orderHighlightPhaseChanged)

    Q_PROPERTY(bool positionActive READ positionActive WRITE setPositionActive NOTIFY positionActiveChanged)
    Q_PROPERTY(qreal positionEntryPrice READ positionEntryPrice WRITE setPositionEntryPrice NOTIFY positionEntryPriceChanged)
    Q_PROPERTY(qreal positionMarkPrice READ positionMarkPrice WRITE setPositionMarkPrice NOTIFY positionMarkPriceChanged)
    Q_PROPERTY(qreal tickSize READ tickSize WRITE setTickSize NOTIFY tickSizeChanged)
    Q_PROPERTY(QColor positionPnlColor READ positionPnlColor WRITE setPositionPnlColor NOTIFY positionPnlColorChanged)

public:
    explicit GpuDomItem(QQuickItem *parent = nullptr);

    int rowHeight() const { return m_rowHeight; }
    void setRowHeight(int v);

    int priceColumnWidth() const { return m_priceColumnWidth; }
    void setPriceColumnWidth(int v);

    QColor backgroundColor() const { return m_backgroundColor; }
    void setBackgroundColor(const QColor &c);

    QColor gridColor() const { return m_gridColor; }
    void setGridColor(const QColor &c);

    QColor hoverColor() const { return m_hoverColor; }
    void setHoverColor(const QColor &c);

    QColor priceBorderColor() const { return m_priceBorderColor; }
    void setPriceBorderColor(const QColor &c);

    int hoverRow() const { return m_hoverRow; }
    void setHoverRow(int v);

    qreal orderHighlightPhase() const { return m_orderHighlightPhase; }
    void setOrderHighlightPhase(qreal v);

    bool positionActive() const { return m_positionActive; }
    void setPositionActive(bool v);

    qreal positionEntryPrice() const { return m_positionEntryPrice; }
    void setPositionEntryPrice(qreal v);

    qreal positionMarkPrice() const { return m_positionMarkPrice; }
    void setPositionMarkPrice(qreal v);

    qreal tickSize() const { return m_tickSize; }
    void setTickSize(qreal v);

    QColor positionPnlColor() const { return m_positionPnlColor; }
    void setPositionPnlColor(const QColor &c);

    void setRows(const QVector<DomLevelsModel::Row> &rows);
    void updateRows(const QVector<int> &indices, const QVector<DomLevelsModel::Row> &rows);

signals:
    void rowHeightChanged();
    void priceColumnWidthChanged();
    void backgroundColorChanged();
    void gridColorChanged();
    void hoverColorChanged();
    void priceBorderColorChanged();
    void hoverRowChanged();
    void orderHighlightPhaseChanged();
    void positionActiveChanged();
    void positionEntryPriceChanged();
    void positionMarkPriceChanged();
    void tickSizeChanged();
    void positionPnlColorChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
    QVector<DomLevelsModel::Row> m_rows;
    int m_rowHeight = 14;
    int m_priceColumnWidth = 80;
    QColor m_backgroundColor = QColor("#121212");
    QColor m_gridColor = QColor("#1f1f1f");
    QColor m_hoverColor = QColor("#2860ff");
    QColor m_priceBorderColor = QColor("#2b2b2b");
    int m_hoverRow = -1;
    qreal m_orderHighlightPhase = 0.0;

    bool m_positionActive = false;
    qreal m_positionEntryPrice = 0.0;
    qreal m_positionMarkPrice = 0.0;
    qreal m_tickSize = 0.0;
    QColor m_positionPnlColor = QColor("#e4e4e4");
};
