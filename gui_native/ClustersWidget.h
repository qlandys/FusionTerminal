#pragma once

#include <QWidget>

class QQuickWidget;
class PrintsWidget;

class ClustersWidget : public QWidget {
    Q_OBJECT
public:
    explicit ClustersWidget(QWidget *parent = nullptr);

    void bindPrints(PrintsWidget *prints);
    void setRowLayout(int rowCount, int rowHeight, int infoAreaHeight);
    void setInfoAreaHeight(int infoAreaHeight) { setRowLayout(m_rowCount, m_rowHeight, infoAreaHeight); }
    int rowCountValue() const { return m_rowCount; }
    int rowHeightValue() const { return m_rowHeight; }

    int effectiveColumnWidth() const { return m_cachedColumnWidth; }
    int visibleBucketCount() const { return m_cachedVisibleBucketCount; }
    int bucketOffset() const { return m_cachedBucketOffset; }
    int xOrigin() const { return m_cachedXOrigin; }

    bool adjustColumnWidthByWheelSteps(int steps);

signals:
    void layoutChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void ensureQuickInitialized();
    void syncQuickProperties();
    void recomputeLayout();

    PrintsWidget *m_prints = nullptr;
    QQuickWidget *m_quickWidget = nullptr;
    bool m_quickReady = false;
    bool m_useGpuClusters = false;
    int m_rowCount = 0;
    int m_rowHeight = 20;
    int m_infoAreaHeight = 26;

    int m_userColumnWidth = 0; // 0 = auto
    int m_cachedColumnWidth = 28;
    int m_cachedVisibleBucketCount = 1;
    int m_cachedBucketOffset = 0;
    int m_cachedXOrigin = 0;
    bool m_adjustingBucketCount = false;
};
