#pragma once

#include <QWidget>
#include <QVector>
#include <QString>
#include <QVariant>
#include <QElapsedTimer>
#include <QRect>
#include <QSet>
#include <QHash>
#include <QPointer>
#include "DomTypes.h"
#include "TradeTypes.h"
#include "DomLevelsModel.h"
#include "GpuDomItem.h"

class QMouseEvent;
class QEvent;
class QQuickWidget;
class QQuickItem;
class QFrame;
class QLabel;

struct DomLevel {
    qint64 tick = 0;
    double price = 0.0;
    double bidQty = 0.0;
    double askQty = 0.0;
};

struct DomSnapshot {
    QVector<DomLevel> levels;
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double tickSize = 0.0;
    qint64 minTick = 0; // inclusive (bucketized)
    qint64 maxTick = 0; // inclusive (bucketized)
    qint64 compression = 1; // ticks per row used to build levels
};

struct DomStyle {
    QColor background = QColor("#202020");
    QColor text = QColor("#f0f0f0");
    QColor bid = QColor(170, 255, 190);
    QColor ask = QColor(255, 180, 190);
    QColor grid = QColor("#303030");
};

class DomWidget : public QWidget {
    Q_OBJECT

public:
    struct LocalOrderMarker
    {
        double price = 0.0;
        double quantity = 0.0;
        OrderSide side = OrderSide::Buy;
        qint64 createdMs = 0;
        QString orderId;
    };

    struct PriceTextMarker
    {
        double price = 0.0;
        QString text;
        QColor fillColor;
        QColor textColor = QColor("#ffffff");
    };

    explicit DomWidget(QWidget *parent = nullptr);

    void updateSnapshot(const DomSnapshot &snapshot);
    void refreshFontProperties();
    void setStyle(const DomStyle &style);
    void centerToSpread();
    int rowHeight() const { return m_rowHeight; }
    void setRowHeight(int h);
    void setVolumeHighlightRules(const QVector<VolumeHighlightRule> &rules);
    void setTradePosition(const TradePosition &position);
    int infoAreaHeight() const { return m_infoAreaHeight; }
    double bestBid() const { return m_snapshot.bestBid; }
    double bestAsk() const { return m_snapshot.bestAsk; }
    double tickSize() const { return m_snapshot.tickSize; }
    void setLocalOrders(const QVector<LocalOrderMarker> &orders);
    void setHighlightPrices(const QVector<double> &prices);
    void setPriceTextMarkers(const QVector<PriceTextMarker> &markers);
    void setActionOverlayText(const QString &text);
    Q_INVOKABLE void handleRowClick(int row, int button, double price, double bidQty, double askQty);
    Q_INVOKABLE void handleRowClickIndex(int row, int button);
    Q_INVOKABLE void handleRowHover(int row);
    Q_INVOKABLE void handleExitClick();
    void notifyBucketTicksUpdated(const QVector<qint64> &bucketTicks);

signals:
    void rowClicked(Qt::MouseButton button, int row, double price, double bidQty, double askQty);
    void rowHovered(int row, double price, double bidQty, double askQty);
    void hoverInfoChanged(int row, double price, const QString &text);
    void infoAreaHeightChanged(int height);
    void exitPositionRequested();

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    void resizeEvent(QResizeEvent *event) override;

private:
    DomSnapshot m_snapshot;
    DomSnapshot m_pendingSnapshot;
    bool m_hasPendingSnapshot = false;
    bool m_snapshotUpdateScheduled = false;
    DomStyle m_style;
    QVector<VolumeHighlightRule> m_volumeRules;
    int m_hoverRow = -1;
    QString m_hoverInfoText;
    int m_rowHeight = 12;
    TradePosition m_position;
    int m_infoAreaHeight = 26;
    QRect m_exitButtonRect;
    QVector<LocalOrderMarker> m_localOrders;
    QVector<double> m_highlightPrices;
    QVector<PriceTextMarker> m_priceTextMarkers;
    QString m_actionOverlayText;
    QQuickWidget *m_quickWidget = nullptr;
    bool m_quickReady = false;
    bool m_quickAllowed = true;
    bool m_useGpuDom = false;
    QPointer<GpuDomItem> m_gpuDomItem;
    bool m_disableIncremental = false;
    QWidget *m_actionOverlayWidget = nullptr;
    QLabel *m_actionOverlayLabel = nullptr;
    QWidget *m_actionOverlayParent = nullptr;
    QElapsedTimer m_snapshotThrottle;
    static constexpr qint64 kMinSnapshotIntervalNs = 8000000; // ~120 Hz
    DomLevelsModel m_levelsModel;
    int m_cachedTotalHeight = -1;
    int m_cachedPriceColumnWidth = -1;
    double m_cachedLevelTickSize = 0.0;
    qint64 m_cachedLayoutMinTick = 0;
    qint64 m_cachedLayoutMaxTick = 0;
    qint64 m_cachedLayoutCompression = 1;
    int m_cachedLayoutRowsCount = 0;
    bool m_cachedLayoutValid = false;
    int m_cachedWidthForPrice = -1;
    QString m_cachedFontKeyForPrice;
    double m_cachedTickForPrice = 0.0;
    QSet<qint64> m_dirtyBucketTicks;
    bool m_forceFullRecalc = false;
    bool m_quickSnapshotScheduled = false;

    int m_orderHighlightCount = 0;
    bool m_qmlHasOrderHighlights = false;

    struct MarkerAgg {
        double notional = 0.0;
        bool buy = true;
        qint64 createdMs = 0;
    };
    QHash<qint64, MarkerAgg> m_cachedMarkersByTick;
    int m_cachedMarkersOrdersHash = 0;
    qint64 m_cachedMarkersCompression = 1;
    double m_cachedMarkersTickSize = 0.0;

    QSet<qint64> m_cachedHighlightTicks;
    int m_cachedHighlightHash = 0;
    qint64 m_cachedHighlightCompression = 1;
    double m_cachedHighlightTickSize = 0.0;

    void updateHoverInfo(int row);
    double cumulativeNotionalForRow(int row) const;
    int rowForPrice(double price) const;
    void ensureQuickInitialized();
    void syncQuickProperties();
    void updateQuickOverlayProperties();
    void applyPendingSnapshot();
    void updateQuickSnapshot();
    void scheduleQuickSnapshotUpdate();
    void resolveGpuDomItem();
    void ensureActionOverlayParent();
    void updateActionOverlayGeometry();
};
