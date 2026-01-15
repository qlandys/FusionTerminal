#pragma once

#include <QBasicTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <deque>

#include "PrintsModel.h"

struct PrintItem {
    double price = 0.0;
    double qty = 0.0;
    bool buy = true;
    int rowHint = -1;
    qint64 tick = 0; // raw tick index (price / rawTickSize)
    qint64 timeMs = 0;
    quint64 seq = 0;
};

struct LocalOrderMarker {
    double price = 0.0;
    double quantity = 0.0; // notional or size in quote units
    bool buy = true;
    qint64 createdMs = 0;
    QString label;
    QString orderId;
    QColor fillColor;
    QColor borderColor;
};

class QQuickWidget;
class PrintsWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMaxClusterBuckets = 60;
    explicit PrintsWidget(QWidget *parent = nullptr);

    void setPrints(const QVector<PrintItem> &items);
    void setLadderPrices(const QVector<double> &prices,
                         const QVector<qint64> &rowTicks,
                         int rowHeight,
                         double rowTickSize,
                         qint64 minTick,
                         qint64 maxTick,
                         qint64 compressionTicks,
                         double rawTickSize);
    void setRowHeightOnly(int rowHeight);
    void setDomInfoAreaHeight(int height);
    void setLocalOrders(const QVector<LocalOrderMarker> &orders);
    QObject *clustersModelObject() { return &m_clustersModel; }
    int clusterWindowMs() const { return m_clusterBucketMs; }
    int clusterBucketCount() const { return m_clusterBucketCount; }
    QVector<double> clusterBucketTotals() const { return m_clusterBucketTotals; }
    QVector<qint64> clusterBucketStartMs() const { return m_clusterBucketStartMs; }
    QString clusterLabel() const;
    void setClusterWindowMs(int ms);
    void setClusterBucketCount(int count);
    void clearClusters();

signals:
    void clusterLabelChanged(const QString &label);
    void clusterBucketsChanged();
    void cancelMarkerRequested(const QStringList &orderIds,
                               const QString &label,
                               double price,
                               bool buy);
    void beginMoveMarkerRequested(const QString &orderId);
    void commitMoveMarkerRequested(const QString &orderId, double newPrice, double fallbackPrice);
    void beginMoveSltpMarkerRequested(const QString &kind, double originPrice);
    void commitMoveSltpMarkerRequested(const QString &kind, double newPrice, double fallbackPrice);
    void dragPreviewPriceRequested(double price);

public slots:
    void setHoverInfo(int row, double price, const QString &text);

public:
    Q_INVOKABLE void requestCancel(const QVariant &orderIds,
                                   const QString &label,
                                   double price,
                                   bool buy);
    Q_INVOKABLE int snapRowFromY(qreal y) const;
    Q_INVOKABLE double priceForRow(int row) const;
    Q_INVOKABLE void requestBeginMove(const QString &orderId);
    Q_INVOKABLE void requestCommitMove(const QString &orderId, double newPrice, double fallbackPrice);
    Q_INVOKABLE void requestBeginMoveSltp(const QString &kind, double originPrice);
    Q_INVOKABLE void requestCommitMoveSltp(const QString &kind, double newPrice, double fallbackPrice);
    Q_INVOKABLE void requestDragPreview(double price);
    Q_INVOKABLE void requestClearDragPreview();
    Q_INVOKABLE int cursorY() const;
    Q_INVOKABLE bool isLeftMouseDown() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    QString makeKey(const PrintItem &item) const;
    qint64 tickForPrice(double price) const;
    int rowForPrice(double price) const;
    qint64 bucketizeTick(qint64 tick) const;
    int rowForTick(qint64 tick) const;
    int applyRowOffset(int row) const;
    void calibrateRowOffset(int domRow, int priceRow);
    void ensureQuickInitialized();
    void syncQuickProperties();
    void updatePrintsQml();
    void updateOrdersQml();
    void updateHoverQml();
    void updateClustersQml(bool force = false);
    void publishClustersModel();
    void scheduleNextClusterBoundary();
    int resolvedRowForItem(const PrintItem &item, int *outRowIdx = nullptr) const;

    QVector<PrintItem> m_items;
    QVector<double> m_prices;
    QVector<qint64> m_rowTicks;
    QHash<double, int> m_priceToRow;
    QHash<qint64, int> m_tickToRow;
    int m_rowHeight = 20;
    QHash<QString, double> m_spawnProgress;
    bool m_disableAnimations = false;
    QBasicTimer m_animTimer;
    int m_hoverRow = -1;
    double m_hoverPrice = 0.0;
    bool m_hoverPriceValid = false;
    QString m_hoverText;
    double m_tickSize = 0.0;
    double m_rawTickSize = 0.0;
    bool m_descending = true;
    double m_firstPrice = 0.0;
    int m_rowOffset = 0;
    bool m_rowOffsetValid = false;
    int m_domInfoAreaHeight = 0;
    QVector<LocalOrderMarker> m_orderMarkers;
    QQuickWidget *m_quickWidget = nullptr;
    bool m_quickReady = false;
    int m_maxVisiblePrints = 64;
    QElapsedTimer m_quickUpdateThrottle;
    static constexpr qint64 kMinQuickUpdateIntervalNs = 10000000; // ~100 Hz to reduce SG churn
    bool m_useGpuPrints = false;
    PrintCirclesModel m_circlesModel;
    PrintOrdersModel m_ordersModel;
    PrintClustersModel m_clustersModel;

    qint64 m_minTick = 0;
    qint64 m_maxTick = 0;
    qint64 m_compressionTicks = 1;

    struct ClusterCellAgg {
        double price = 0.0;
        qint64 tick = 0;
        int col = 0;
        double total = 0.0;
        double delta = 0.0;
    };
    QVector<ClusterCellAgg> m_clusterCells;

    struct ClusterTrade {
        double price = 0.0;
        qint64 tick = 0;
        double qty = 0.0;
        bool buy = true;
        qint64 timeMs = 0;
        quint64 seq = 0;
    };
    std::deque<ClusterTrade> m_clusterTrades;
    quint64 m_lastClusterSeq = 0;
    qint64 m_lastClusterUpdateMs = 0;
    int m_clusterBucketMs = 1000;
    int m_clusterBucketCount = 5;
    QVector<double> m_clusterBucketTotals;
    QVector<qint64> m_clusterBucketStartMs;
    QTimer m_clusterBoundaryTimer;

    struct ClusterAgg {
        double total = 0.0;
        double delta = 0.0;
    };
    qint64 m_clusterCurrentBucket = 0;
    int m_clusterAggBucketMs = 0;
    int m_clusterAggBucketCount = 0;
    QVector<QHash<qint64, ClusterAgg>> m_clusterAggByCol;
    QVector<double> m_clusterAggTotalsByCol;
    quint64 m_clusterMappingRevision = 0;
    quint64 m_clusterAggBuiltRevision = 0;

    void resetClusterAgg();
    void rebuildClusterAggFromTrades(qint64 nowMs);
    void advanceClusterBuckets(qint64 nowMs);
    void ingestClusterTrade(const ClusterTrade &t, qint64 nowMs);
};
