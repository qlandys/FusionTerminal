#include "ClustersWidget.h"

#include "PrintsWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QQuickItem>
#include <QQuickWidget>
#include <QDateTime>
#include <QWheelEvent>

#include "ThemeManager.h"

namespace {
QString formatQtyShort(double v)
{
    const double av = std::abs(v);
    if (av >= 1000000.0) return QString::number(av / 1000000.0, 'f', 1) + "M";
    if (av >= 1000.0) return QString::number(av / 1000.0, 'f', 1) + "K";
    return QString::number(av, 'f', av >= 10.0 ? 0 : 1);
}
} // namespace

static bool clustersUseQml()
{
    // Default: use QML (old visuals).
    // Compatibility: allow explicit enable/disable via env.
    if (qEnvironmentVariableIsSet("CLUSTERS_USE_QML") || qEnvironmentVariableIsSet("GPU_CLUSTERS_QML")) {
        return qEnvironmentVariableIntValue("CLUSTERS_USE_QML") > 0
            || qEnvironmentVariableIntValue("GPU_CLUSTERS_QML") > 0;
    }
    return !(qEnvironmentVariableIntValue("CLUSTERS_DISABLE_QML") > 0
             || qEnvironmentVariableIntValue("GPU_CLUSTERS_DISABLE_QML") > 0);
}

ClustersWidget::ClustersWidget(QWidget *parent)
    : QWidget(parent)
{
    m_useGpuClusters = (qEnvironmentVariableIntValue("CLUSTERS_GPU") > 0)
                       || (qEnvironmentVariableIntValue("DOM_GPU") > 0);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setMinimumWidth(48);
    setMaximumWidth(4000);
    auto *theme = ThemeManager::instance();
    setStyleSheet(QStringLiteral("background-color: %1;")
                      .arg(theme->windowBackground().name(QColor::HexRgb)));
    connect(theme, &ThemeManager::themeChanged, this, [this, theme]() {
        setStyleSheet(QStringLiteral("background-color: %1;")
                          .arg(theme->windowBackground().name(QColor::HexRgb)));
        syncQuickProperties();
        update();
    });
    ensureQuickInitialized();
}

void ClustersWidget::bindPrints(PrintsWidget *prints)
{
    if (m_prints == prints) {
        return;
    }
    if (m_prints) {
        disconnect(m_prints, nullptr, this, nullptr);
    }
    m_prints = prints;
    if (m_prints) {
        connect(m_prints, &PrintsWidget::clusterLabelChanged, this, [this](const QString &) {
            syncQuickProperties();
        });
        connect(m_prints, &PrintsWidget::clusterBucketsChanged, this, [this]() {
            syncQuickProperties();
        });
    }
    syncQuickProperties();
}

void ClustersWidget::setRowLayout(int rowCount, int rowHeight, int infoAreaHeight)
{
    m_rowCount = std::max(0, rowCount);
    m_rowHeight = std::clamp(rowHeight, 10, 40);
    m_infoAreaHeight = std::clamp(infoAreaHeight, 0, 60);
    const int totalHeight = m_rowCount * m_rowHeight + m_infoAreaHeight;
    setMinimumHeight(totalHeight);
    setMaximumHeight(totalHeight);
    updateGeometry();
    if (m_quickWidget) {
        m_quickWidget->setGeometry(rect());
    }
    recomputeLayout();
    syncQuickProperties();
}

QSize ClustersWidget::sizeHint() const
{
    return QSize(70, 400);
}

QSize ClustersWidget::minimumSizeHint() const
{
    return QSize(0, 200);
}

void ClustersWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_quickWidget) {
        return;
    }
    QPainter p(this);
    ThemeManager *theme = ThemeManager::instance();
    p.fillRect(rect(), theme->panelBackground());

    if (!m_prints) {
        return;
    }

    recomputeLayout();
    const int bucketCount = std::clamp(m_prints->clusterBucketCount(), 1, PrintsWidget::kMaxClusterBuckets);
    const int colWidth = std::max(18, m_cachedColumnWidth);
    const int visibleCount = std::clamp(m_cachedVisibleBucketCount, 1, bucketCount);
    const int bucketOffset = std::clamp(m_cachedBucketOffset, 0, std::max(0, bucketCount - visibleCount));
    const int xOrigin = m_cachedXOrigin;
    const int gridHeight = m_rowCount * m_rowHeight;
    const QRect gridRect(0, 0, width(), gridHeight);

    // Grid lines
    p.setPen(QPen(theme->gridColor(), 1.0));
    for (int r = 0; r <= m_rowCount; ++r) {
        const int y = r * m_rowHeight;
        p.drawLine(0, y, width(), y);
    }
    for (int c = 0; c <= visibleCount; ++c) {
        const int x = xOrigin + c * colWidth;
        p.drawLine(x, 0, x, gridHeight);
    }

    // Cells
    const auto &cells = static_cast<PrintClustersModel *>(m_prints->clustersModelObject())->entries();
    QFont f = font();
    p.setFont(f);
    const QFontMetrics fm(f);
    for (const auto &e : cells) {
        if (e.row < 0 || e.row >= m_rowCount) {
            continue;
        }
        if (e.col < 0 || e.col >= bucketCount) {
            continue;
        }
        if (e.col < bucketOffset || e.col >= bucketOffset + visibleCount) {
            continue;
        }
        const int localCol = e.col - bucketOffset;
        const QRect shiftedRect(xOrigin + localCol * colWidth, e.row * m_rowHeight, colWidth, m_rowHeight);
        p.fillRect(shiftedRect, e.bgColor);
        if (!e.text.isEmpty()) {
            p.setPen(e.textColor);
            const QString elided = fm.elidedText(e.text, Qt::ElideRight, std::max(0, colWidth - 4));
            p.drawText(shiftedRect.adjusted(2, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
    }

    // Footer labels/totals
    const int footerY = gridHeight;
    const int footerH = std::max(0, height() - footerY);
    if (footerH > 0) {
        p.fillRect(QRect(0, footerY, width(), footerH), theme->panelBackground());
        p.setPen(theme->gridColor());
        p.drawLine(0, footerY, width(), footerY);

        const int ms = std::max(100, m_prints->clusterWindowMs());
        const QVector<qint64> startMs = m_prints->clusterBucketStartMs();
        const QVector<double> totals = m_prints->clusterBucketTotals();
        const bool showSeconds = ms < 60000;
        for (int c = 0; c < visibleCount; ++c) {
            const int srcCol = bucketOffset + c;
            const QRect colRect(xOrigin + c * colWidth, footerY, colWidth, footerH);
            QString timeText;
            const qint64 ts = (srcCol < startMs.size()) ? startMs[srcCol] : 0;
            if (ts > 0) {
                const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ts);
                if (ms >= 86400000) {
                    timeText = dt.toString(QStringLiteral("dd.MM"));
                } else {
                    timeText = dt.toString(showSeconds ? QStringLiteral("HH:mm:ss")
                                                       : QStringLiteral("HH:mm"));
                }
            }
            const double tv = (srcCol < totals.size()) ? totals[srcCol] : 0.0;
            const QString totText = tv > 0.0 ? formatQtyShort(tv) : QString();

            p.setPen(theme->textPrimary());
            p.drawText(colRect.adjusted(2, 2, -2, -2), Qt::AlignTop | Qt::AlignLeft, totText);
            p.setPen(theme->textSecondary());
            p.drawText(colRect.adjusted(2, 2, -2, -2), Qt::AlignBottom | Qt::AlignLeft, timeText);
        }
    }
}

void ClustersWidget::resizeEvent(QResizeEvent *event)
{
    if (m_quickWidget) {
        m_quickWidget->setGeometry(rect());
    }
    recomputeLayout();
    syncQuickProperties();
    update();
    QWidget::resizeEvent(event);
}

void ClustersWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_prints) {
        return;
    }
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral("QMenu { background:#1f1f1f; color:#e0e0e0; }"
                                      "QMenu::item:selected { background:#2c2c2c; }"));
    const QList<int> windows = {1000, 10000, 30000, 60000, 300000, 900000, 1800000, 3600000, 86400000};
    QActionGroup *group = new QActionGroup(&menu);
    group->setExclusive(true);
    for (int ms : windows) {
        QString label;
        if (ms < 1000) {
            label = QStringLiteral("%1ms").arg(ms);
        } else if (ms < 60000) {
            label = QStringLiteral("%1s").arg(ms / 1000);
        } else if (ms < 3600000) {
            label = QStringLiteral("%1m").arg(ms / 60000);
        } else if (ms < 86400000) {
            label = QStringLiteral("%1m").arg(ms / 60000);
        } else {
            label = QStringLiteral("%1d").arg(ms / 86400000);
        }
        QAction *a = menu.addAction(QStringLiteral("Clusters: %1").arg(label));
        a->setCheckable(true);
        a->setChecked(ms == m_prints->clusterWindowMs());
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, ms]() {
            if (m_prints) {
                m_prints->setClusterWindowMs(ms);
            }
        });
    }
    menu.exec(event->globalPos());
}

void ClustersWidget::wheelEvent(QWheelEvent *event)
{
    if (!event) {
        return;
    }
    int steps = event->angleDelta().y() / 120;
    if (event->inverted()) {
        steps = -steps;
    }
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    if (!adjustColumnWidthByWheelSteps(steps)) {
        QWidget::wheelEvent(event);
        return;
    }
    event->accept();
}

bool ClustersWidget::adjustColumnWidthByWheelSteps(int steps)
{
    // Don't let columns get too narrow to read.
    const int minW = 34;
    const int maxW = 160;
    if (m_userColumnWidth <= 0) {
        recomputeLayout();
        m_userColumnWidth = std::clamp(m_cachedColumnWidth, minW, maxW);
    }
    m_userColumnWidth = std::clamp(m_userColumnWidth + steps * 4, minW, maxW);

    recomputeLayout();
    syncQuickProperties();
    update();
    return true;
}

void ClustersWidget::ensureQuickInitialized()
{
    if (!clustersUseQml()) {
        return;
    }
    if (m_quickWidget) {
        return;
    }
    m_quickWidget = new QQuickWidget(this);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->setClearColor(Qt::transparent);
    m_quickWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    connect(m_quickWidget, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status status) {
        m_quickReady = (status == QQuickWidget::Ready);
        if (m_quickReady) {
            // Propagate domContainerPtr to QQuick internals so global eventFilter can
            // route wheel events to the correct ladder column.
            const QVariant ptr = property("domContainerPtr");
            if (ptr.isValid()) {
                m_quickWidget->setProperty("domContainerPtr", ptr);
                if (auto *win = m_quickWidget->quickWindow()) {
                    win->setProperty("domContainerPtr", ptr);
                }
                if (auto *root = m_quickWidget->rootObject()) {
                    root->setProperty("domContainerPtr", ptr);
                }
            }
            syncQuickProperties();
        }
    });
    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/GpuClustersView.qml")));
    m_quickWidget->setGeometry(rect());
}

void ClustersWidget::syncQuickProperties()
{
    if (!m_quickWidget || !m_quickReady) {
        update();
        return;
    }
    recomputeLayout();
    if (auto *root = m_quickWidget->rootObject()) {
        ThemeManager *theme = ThemeManager::instance();
        root->setProperty("rowHeight", m_rowHeight);
        root->setProperty("rowCount", m_rowCount);
        // Footer is drawn by a viewport overlay (MainWindow) to keep it pinned to the bottom.
        root->setProperty("infoAreaHeight", 0);
        root->setProperty("useGpuClusters", m_useGpuClusters);
        root->setProperty("backgroundColor", theme->panelBackground());
        root->setProperty("gridColor", theme->gridColor());
        const int bucketCount =
            m_prints ? std::clamp(m_prints->clusterBucketCount(), 1, PrintsWidget::kMaxClusterBuckets) : 1;
        const int colWidth = std::max(18, m_cachedColumnWidth);
        const int visibleCount = std::clamp(m_cachedVisibleBucketCount, 1, bucketCount);
        const int bucketOffset = std::clamp(m_cachedBucketOffset, 0, std::max(0, bucketCount - visibleCount));
        const int xOrigin = m_cachedXOrigin;
        root->setProperty("columnCount", visibleCount);
        root->setProperty("columnWidth", colWidth);
        root->setProperty("columnOffset", bucketOffset);
        root->setProperty("xOrigin", xOrigin);
        const QFont fnt = font();
        const int px = fnt.pixelSize() > 0 ? fnt.pixelSize() : std::max(10, fnt.pointSize() + 2);
        root->setProperty("fontFamily", fnt.family());
        root->setProperty("fontPixelSize", px);
        root->setProperty("clustersModel",
                          QVariant::fromValue(static_cast<QObject *>(m_prints ? m_prints->clustersModelObject() : nullptr)));
        root->setProperty("label", m_prints ? m_prints->clusterLabel() : QString());
        if (m_prints) {
            const int ms = std::max(100, m_prints->clusterWindowMs());
            const QVector<qint64> startMs = m_prints->clusterBucketStartMs();
            const QVector<double> totals = m_prints->clusterBucketTotals();

            QVariantList labels;
            labels.reserve(bucketCount);
            QVariantList totalLabels;
            totalLabels.reserve(bucketCount);

            const bool showSeconds = ms < 60000;
            for (int i = 0; i < bucketCount; ++i) {
                const qint64 ts = (i < startMs.size()) ? startMs[i] : 0;
                QString timeText;
                if (ts > 0) {
                    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ts);
                    if (ms >= 86400000) {
                        timeText = dt.toString(QStringLiteral("dd.MM"));
                    } else {
                        timeText = dt.toString(showSeconds ? QStringLiteral("HH:mm:ss")
                                                           : QStringLiteral("HH:mm"));
                    }
                } else {
                    timeText = QString();
                }
                labels.append(timeText);
                const double tv = (i < totals.size()) ? totals[i] : 0.0;
                totalLabels.append(tv > 0.0 ? formatQtyShort(tv) : QString());
            }
            root->setProperty("columnLabels", labels);
            root->setProperty("columnTotals", totalLabels);
        } else {
            root->setProperty("columnLabels", QVariantList{});
            root->setProperty("columnTotals", QVariantList{});
        }
    }
}

void ClustersWidget::refreshFontProperties()
{
    syncQuickProperties();
    update();
}

void ClustersWidget::recomputeLayout()
{
    if (!m_prints) {
        m_cachedColumnWidth = std::max(18, width());
        m_cachedVisibleBucketCount = 1;
        m_cachedBucketOffset = 0;
        m_cachedXOrigin = 0;
        emit layoutChanged();
        return;
    }

    int bucketCount = std::clamp(m_prints->clusterBucketCount(), 1, PrintsWidget::kMaxClusterBuckets);
    const int minW = 34;
    const int autoW = std::max(minW, width() / std::max(1, bucketCount));
    const int desiredW = std::max(minW, (m_userColumnWidth > 0 ? m_userColumnWidth : autoW));
    const int desiredBuckets =
        std::clamp((width() + desiredW - 1) / std::max(1, desiredW), 1, PrintsWidget::kMaxClusterBuckets);
    // When the column is widened, add more time buckets so the empty space fills with new columns.
    if (!m_adjustingBucketCount && desiredBuckets > bucketCount) {
        m_adjustingBucketCount = true;
        m_prints->setClusterBucketCount(desiredBuckets);
        m_adjustingBucketCount = false;
        bucketCount = std::clamp(m_prints->clusterBucketCount(), 1, PrintsWidget::kMaxClusterBuckets);
    }

    // Show the next (oldest) column partially if it doesn't fully fit.
    const int visible =
        std::clamp((width() + desiredW - 1) / std::max(1, desiredW), 1, bucketCount);
    const int offset = std::max(0, bucketCount - visible);
    // Keep the newest bucket pinned to the right edge; extra space (if any) goes to the left.
    const int colW = desiredW;
    const int xOrigin = width() - visible * colW;

    const bool changed = (m_cachedColumnWidth != colW) || (m_cachedVisibleBucketCount != visible)
                         || (m_cachedBucketOffset != offset) || (m_cachedXOrigin != xOrigin);
    m_cachedColumnWidth = colW;
    m_cachedVisibleBucketCount = visible;
    m_cachedBucketOffset = offset;
    m_cachedXOrigin = xOrigin;
    if (changed) {
        emit layoutChanged();
    }
}
