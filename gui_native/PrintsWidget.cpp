#include "PrintsWidget.h"

#include <QFontMetrics>
#include <QFontInfo>
#include <QPainter>
#include <QPaintEvent>
#include <QTimerEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QtGlobal>
#include <QDebug>
#include <QDateTime>
#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlError>
#include <QQmlContext>
#include <QVariant>
#include <QUrl>
#include "PrintsModel.h"
#include "ThemeManager.h"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
QString formatQty(double v)
{
    const double av = std::abs(v);
    if (av >= 1000000.0) return QString::number(av / 1000000.0, 'f', 1) + "M";
    if (av >= 1000.0) return QString::number(av / 1000.0, 'f', 1) + "K";
    if (av >= 10.0) {
        return QString::number(av, 'f', 0);
    }
    const QString out = QString::number(av, 'f', 1);
    if (out == QStringLiteral("0.0") && av > 0.0) {
        return QStringLiteral("<0.1");
    }
    return out;
}
} // namespace

static bool printsUseQml()
{
    // Default: use QML (old visuals).
    // Compatibility: allow explicit enable/disable via env.
    if (qEnvironmentVariableIsSet("PRINTS_USE_QML") || qEnvironmentVariableIsSet("GPU_PRINTS_QML")) {
        return qEnvironmentVariableIntValue("PRINTS_USE_QML") > 0
            || qEnvironmentVariableIntValue("GPU_PRINTS_QML") > 0;
    }
    return !(qEnvironmentVariableIntValue("PRINTS_DISABLE_QML") > 0
             || qEnvironmentVariableIntValue("GPU_PRINTS_DISABLE_QML") > 0);
}

PrintsWidget::PrintsWidget(QWidget *parent)
    : QWidget(parent)
{
    m_useGpuPrints = (qEnvironmentVariableIntValue("PRINTS_GPU") > 0)
                     || (qEnvironmentVariableIntValue("DOM_GPU") > 0);
    bool ok = false;
    const int capFromEnv = qEnvironmentVariableIntValue("PRINTS_MAX_VISIBLE", &ok);
    if (ok && capFromEnv > 0) {
        m_maxVisiblePrints = std::max(4, std::min(capFromEnv, 256));
    } else {
        // Slightly lower default to reduce scene graph load on heavy symbols.
        m_maxVisiblePrints = 48;
    }
    m_disableAnimations = qEnvironmentVariableIsSet("PRINTS_DISABLE_ANIM")
                          || qEnvironmentVariableIsSet("PRINTS_NO_ANIM");

    const int bucketMsEnv = qEnvironmentVariableIntValue("CLUSTERS_BUCKET_MS", &ok);
    if (ok && bucketMsEnv > 0) {
        m_clusterBucketMs = std::clamp(bucketMsEnv, 100, 86400000);
    }
    ok = false;
    const int bucketCntEnv = qEnvironmentVariableIntValue("CLUSTERS_BUCKETS", &ok);
    if (ok && bucketCntEnv > 0) {
        m_clusterBucketCount = std::clamp(bucketCntEnv, 1, kMaxClusterBuckets);
    }
    if (qEnvironmentVariableIsSet("CLUSTERS_LIGHT")) {
        m_clusterBucketMs = std::max(200, m_clusterBucketMs);
        m_clusterBucketCount = std::min(3, m_clusterBucketCount);
    }

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (printsUseQml()) {
        ensureQuickInitialized();
    } else {
        // QWidget paint path: keep background transparent (same as QQuickWidget clearColor).
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAutoFillBackground(false);
        // In QWidget mode, animations are expensive; keep them off unless explicitly enabled.
        if (qEnvironmentVariableIntValue("PRINTS_ENABLE_ANIM") > 0) {
            m_disableAnimations = false;
        } else {
            m_disableAnimations = true;
        }
    }
    connect(ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        syncQuickProperties();
        updatePrintsQml();
        updateOrdersQml();
        updateHoverQml();
        updateClustersQml(true);
    });
    m_clusterBoundaryTimer.setSingleShot(true);
    connect(&m_clusterBoundaryTimer, &QTimer::timeout, this, [this]() {
        updateClustersQml(true);
    });
}

void PrintsWidget::resetClusterAgg()
{
    m_clusterCurrentBucket = 0;
    m_clusterAggBucketMs = 0;
    m_clusterAggBucketCount = 0;
    m_clusterAggByCol.clear();
    m_clusterAggTotalsByCol.clear();
    m_clusterAggBuiltRevision = 0;
    m_clusterCells.clear();
    m_clustersModel.setEntries({});
    m_clusterBucketTotals.clear();
    m_clusterBucketStartMs.clear();
}

void PrintsWidget::advanceClusterBuckets(qint64 nowMs)
{
    const int bucketMs = std::clamp(m_clusterBucketMs, 100, 86400000);
    const int bucketCount = std::clamp(m_clusterBucketCount, 1, kMaxClusterBuckets);
    const qint64 currentBucket = nowMs / static_cast<qint64>(bucketMs);

    if (m_clusterCurrentBucket == 0 || m_clusterAggBucketMs != bucketMs || m_clusterAggBucketCount != bucketCount) {
        m_clusterCurrentBucket = currentBucket;
        m_clusterAggBucketMs = bucketMs;
        m_clusterAggBucketCount = bucketCount;
        m_clusterAggByCol = QVector<QHash<qint64, ClusterAgg>>(bucketCount);
        m_clusterAggTotalsByCol = QVector<double>(bucketCount, 0.0);
        m_clusterBucketTotals = QVector<double>(bucketCount, 0.0);
        m_clusterBucketStartMs = QVector<qint64>(bucketCount, 0);
        for (int col = 0; col < bucketCount; ++col) {
            const qint64 b = currentBucket - (bucketCount - 1 - col);
            m_clusterBucketStartMs[col] = b * static_cast<qint64>(bucketMs);
        }
        return;
    }

    if (currentBucket <= m_clusterCurrentBucket) {
        return;
    }

    qint64 shift = currentBucket - m_clusterCurrentBucket;
    if (shift >= bucketCount) {
        for (auto &m : m_clusterAggByCol) {
            m.clear();
        }
        std::fill(m_clusterAggTotalsByCol.begin(), m_clusterAggTotalsByCol.end(), 0.0);
        std::fill(m_clusterBucketTotals.begin(), m_clusterBucketTotals.end(), 0.0);
        for (int col = 0; col < bucketCount; ++col) {
            const qint64 b = currentBucket - (bucketCount - 1 - col);
            m_clusterBucketStartMs[col] = b * static_cast<qint64>(bucketMs);
        }
        m_clusterCurrentBucket = currentBucket;
        return;
    }

    // Shift left by `shift` columns (oldest buckets drop off), clear new buckets on the right.
    for (int col = 0; col < bucketCount - static_cast<int>(shift); ++col) {
        m_clusterAggByCol[col] = std::move(m_clusterAggByCol[col + static_cast<int>(shift)]);
        m_clusterAggTotalsByCol[col] = m_clusterAggTotalsByCol[col + static_cast<int>(shift)];
    }
    for (int col = bucketCount - static_cast<int>(shift); col < bucketCount; ++col) {
        m_clusterAggByCol[col].clear();
        m_clusterAggTotalsByCol[col] = 0.0;
    }

    for (int col = 0; col < bucketCount; ++col) {
        const qint64 b = currentBucket - (bucketCount - 1 - col);
        m_clusterBucketStartMs[col] = b * static_cast<qint64>(bucketMs);
        m_clusterBucketTotals[col] = m_clusterAggTotalsByCol[col];
    }

    m_clusterCurrentBucket = currentBucket;
}

void PrintsWidget::ingestClusterTrade(const ClusterTrade &t, qint64 nowMs)
{
    if (m_prices.isEmpty()) {
        return;
    }
    if (m_tickSize <= 0.0 && m_rawTickSize <= 0.0) {
        return;
    }
    if (t.qty <= 0.0) {
        return;
    }

    advanceClusterBuckets(nowMs);
    const int bucketMs = m_clusterAggBucketMs;
    const int bucketCount = m_clusterAggBucketCount;
    if (bucketMs <= 0 || bucketCount <= 0 || m_clusterCurrentBucket == 0) {
        return;
    }

    const qint64 bucket = t.timeMs / static_cast<qint64>(bucketMs);
    const qint64 offset = m_clusterCurrentBucket - bucket;
    if (offset < 0 || offset >= bucketCount) {
        return;
    }
    const int col = (bucketCount - 1) - static_cast<int>(offset);
    if (col < 0 || col >= bucketCount) {
        return;
    }

    const qint64 rawTick = (t.tick != 0) ? t.tick : tickForPrice(t.price);
    const qint64 tick = bucketizeTick(rawTick);
    if (rowForTick(tick) < 0) {
        return;
    }

    auto &map = m_clusterAggByCol[col];
    auto it = map.find(tick);
    if (it == map.end()) {
        ClusterAgg agg;
        agg.total = t.qty;
        agg.delta = t.buy ? t.qty : -t.qty;
        map.insert(tick, agg);
    } else {
        it->total += t.qty;
        it->delta += t.buy ? t.qty : -t.qty;
    }
    m_clusterAggTotalsByCol[col] += t.qty;
}

void PrintsWidget::rebuildClusterAggFromTrades(qint64 nowMs)
{
    resetClusterAgg();
    advanceClusterBuckets(nowMs);
    if (m_clusterAggBucketCount <= 0) {
        return;
    }
    for (const auto &t : m_clusterTrades) {
        ingestClusterTrade(t, nowMs);
    }
    m_clusterAggBuiltRevision = m_clusterMappingRevision;
}

void PrintsWidget::setPrints(const QVector<PrintItem> &items)
{
    // Keep rowHint as provided by LadderClient for diagnostics, but don't trust it for row alignment.
    // Row alignment must be derived from the active ladder price mapping (price -> row), otherwise a
    // backend/offline hint can introduce a systematic 1-tick shift.
    m_items = items;
    for (const auto &it : m_items) {
        if (it.seq > m_lastClusterSeq) {
            ClusterTrade t;
            t.price = it.price;
            t.tick = it.tick;
            t.qty = it.qty;
            t.buy = it.buy;
            t.timeMs = it.timeMs > 0 ? it.timeMs : QDateTime::currentMSecsSinceEpoch();
            t.seq = it.seq;
            m_clusterTrades.push_back(t);
            m_lastClusterSeq = it.seq;
            ingestClusterTrade(t, QDateTime::currentMSecsSinceEpoch());
        }
    }

    if (m_disableAnimations) {
        m_spawnProgress.clear();
        m_animTimer.stop();
    } else {
        QHash<QString, double> nextProgress;
        nextProgress.reserve(items.size());
        bool hasNew = false;
        for (const auto &it : m_items) {
            const QString key = makeKey(it);
            if (m_spawnProgress.contains(key)) {
                nextProgress.insert(key, m_spawnProgress.value(key));
            } else {
                nextProgress.insert(key, 0.0);
                hasNew = true;
            }
        }
        m_spawnProgress = nextProgress;
        bool needTimer = false;
        for (auto it = m_spawnProgress.cbegin(); it != m_spawnProgress.cend(); ++it) {
            if (it.value() < 0.999) {
                needTimer = true;
                break;
            }
        }
        if (needTimer) {
            if (!m_animTimer.isActive()) {
                m_animTimer.start(16, this);
            }
        } else {
            m_animTimer.stop();
        }
    }
    updatePrintsQml();
    updateClustersQml();
}

void PrintsWidget::setLadderPrices(const QVector<double> &prices,
                                   const QVector<qint64> &rowTicks,
                                   int rowHeight,
                                   double rowTickSize,
                                   qint64 minTick,
                                   qint64 maxTick,
                                   qint64 compressionTicks,
                                   double rawTickSize)
{
    const bool resetMapping = m_prices.isEmpty() || prices.isEmpty();
    m_prices = prices;
    m_rowTicks = rowTicks;
    m_firstPrice = m_prices.isEmpty() ? 0.0 : m_prices.first();
    m_descending = m_prices.size() < 2 ? true : (m_prices.first() > m_prices.last());
    m_minTick = minTick;
    m_maxTick = maxTick;
    m_compressionTicks = std::max<qint64>(1, compressionTicks);
    // IMPORTANT: keep the last known raw tick size if the new mapping doesn't provide one
    // (some transient frames can arrive without tickSize during volatility).
    if (rawTickSize > 0.0 && std::isfinite(rawTickSize)) {
        m_rawTickSize = rawTickSize;
    } else if (m_prices.isEmpty()) {
        m_rawTickSize = 0.0;
    }

    if (rowTickSize > 0.0) {
        m_tickSize = rowTickSize;
    } else {
        m_tickSize = 0.0;
        for (int i = 1; i < m_prices.size(); ++i) {
            const double diff = std::abs(m_prices[i - 1] - m_prices[i]);
            if (diff > 1e-9) {
                m_tickSize = diff;
                break;
            }
        }
    }

    m_priceToRow.clear();
    m_priceToRow.reserve(m_prices.size());
    m_tickToRow.clear();
    m_tickToRow.reserve(m_prices.size());
    for (int i = 0; i < m_prices.size(); ++i) {
        m_priceToRow.insert(m_prices[i], i);
        if (i < m_rowTicks.size() && m_rowTicks[i] != 0) {
            m_tickToRow.insert(bucketizeTick(m_rowTicks[i]), i);
        } else if (m_tickSize > 0.0) {
            m_tickToRow.insert(tickForPrice(m_prices[i]), i);
        }
    }
    m_rowHeight = std::max(10, std::min(rowHeight, 40));
    ++m_clusterMappingRevision;

    const int totalHeight = m_prices.size() * m_rowHeight + m_domInfoAreaHeight;
    // The visible ladder mapping changed (scroll/zoom/compression). Reset row offset so
    // prints/clusters remain aligned with the current price rows; it can be recalibrated
    // from hover immediately if needed.
    Q_UNUSED(resetMapping);
    m_rowOffset = 0;
    m_rowOffsetValid = false;
    if (m_hoverRow >= m_prices.size()) {
        m_hoverRow = -1;
        m_hoverText.clear();
    }
    setMinimumHeight(totalHeight);
    setMaximumHeight(totalHeight);
    updateGeometry();
    syncQuickProperties();
    updatePrintsQml();
    updateOrdersQml();
    updateHoverQml();
    rebuildClusterAggFromTrades(QDateTime::currentMSecsSinceEpoch());
    updateClustersQml(true);
}

void PrintsWidget::setRowHeightOnly(int rowHeight)
{
    m_rowHeight = std::max(10, std::min(rowHeight, 40));
    const int totalHeight = m_prices.size() * m_rowHeight + m_domInfoAreaHeight;
    setMinimumHeight(totalHeight);
    setMaximumHeight(totalHeight);
    updateGeometry();
    syncQuickProperties();
    updatePrintsQml();
    updateOrdersQml();
    updateClustersQml(true);
}

void PrintsWidget::setDomInfoAreaHeight(int height)
{
    const int clamped = std::clamp(height, 0, 60);
    if (m_domInfoAreaHeight == clamped) {
        return;
    }
    m_domInfoAreaHeight = clamped;
    const int totalHeight = m_prices.size() * m_rowHeight + m_domInfoAreaHeight;
    setMinimumHeight(totalHeight);
    setMaximumHeight(totalHeight);
    updateGeometry();
    syncQuickProperties();
    updatePrintsQml();
    updateOrdersQml();
    updateHoverQml();
}

void PrintsWidget::setLocalOrders(const QVector<LocalOrderMarker> &orders)
{
    m_orderMarkers = orders;
    updateOrdersQml();
}

void PrintsWidget::setHoverInfo(int row, double price, const QString &text)
{
    const int rowCount = m_prices.size();
    const bool domRowValid = (row >= 0 && row < rowCount);
    int resolvedRow = -1;
    const bool priceValid = (rowCount > 0) && std::isfinite(price);
    int priceRow = -1;
    if (priceValid) {
        priceRow = rowForPrice(price);
    }
    resolvedRow = domRowValid ? row : priceRow;
    QString newText = resolvedRow >= 0 ? text : QString();
    if (m_hoverRow == resolvedRow && m_hoverText == newText && m_hoverPriceValid == priceValid) {
        if (!priceValid || qFuzzyCompare(m_hoverPrice, price)) {
            return;
        }
    }
    m_hoverRow = resolvedRow;
    m_hoverText = newText;
    m_hoverPriceValid = priceValid;
    m_hoverPrice = priceValid ? price : 0.0;
    updateHoverQml();
}

void PrintsWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_quickWidget) {
        return;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    ThemeManager *theme = ThemeManager::instance();
    if (m_hoverRow >= 0 && m_rowHeight > 0) {
        const QRectF r(0.0,
                       m_hoverRow * m_rowHeight,
                       width(),
                       m_rowHeight);
        QColor hc = theme->selectionColor();
        hc.setAlpha(60);
        p.fillRect(r, hc);
    }

    const QFont fnt = font();
    p.setFont(fnt);
    const QFontMetrics fm(fnt);

    // Draw prints bubbles
    for (const auto &e : m_circlesModel.entries()) {
        const double x = std::clamp(e.xRatio, 0.0, 1.0) * std::max(1, width());
        const double y = e.y;
        const double r = e.radius;
        if (!(r > 0.0)) {
            continue;
        }
        const QRectF circle(x - r, y - r, 2 * r, 2 * r);
        p.setPen(QPen(e.borderColor, 1.0));
        p.setBrush(e.fillColor);
        p.drawEllipse(circle);
        p.setPen(Qt::white);
        const QString text = e.text;
        if (!text.isEmpty()) {
            QRectF tr = circle;
            // Slightly shrink to avoid clipping at high DPI.
            tr.adjust(1, 1, -1, -1);
            const int flags = Qt::AlignCenter | Qt::TextSingleLine;
            // If text doesn't fit, elide.
            const QString elided = fm.elidedText(text, Qt::ElideRight, static_cast<int>(tr.width() - 2));
            p.drawText(tr, flags, elided);
        }
    }

    // Draw local order markers as small pills on the right edge.
    const int markerW = std::clamp(width() / 3, 44, 84);
    const int markerPad = 2;
    for (const auto &e : m_ordersModel.entries()) {
        if (e.row < 0 || e.row >= m_prices.size()) {
            continue;
        }
        const double y = e.row * m_rowHeight;
        const QRectF rr(width() - markerW - markerPad,
                        y + markerPad,
                        markerW,
                        std::max(1, m_rowHeight - 2 * markerPad));
        p.setPen(QPen(e.borderColor, 1.0));
        p.setBrush(e.fillColor);
        p.drawRoundedRect(rr, 4.0, 4.0);
        p.setPen(theme->textPrimary());
        const QString elided = fm.elidedText(e.text, Qt::ElideRight, static_cast<int>(rr.width() - 6));
        p.drawText(rr.adjusted(3, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, elided);
    }
}

void PrintsWidget::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_animTimer.timerId()) {
        bool any = false;
        for (auto it = m_spawnProgress.begin(); it != m_spawnProgress.end(); ++it) {
            double value = it.value();
            if (value < 0.999) {
                value += (1.0 - value) * 0.3;
                if (value > 0.999) {
                    value = 1.0;
                } else {
                    any = true;
                }
                it.value() = value;
            }
        }
        if (!any) {
            m_animTimer.stop();
        }
        updatePrintsQml();
        return;
    }
    QWidget::timerEvent(event);
}

QString PrintsWidget::makeKey(const PrintItem &item) const
{
    return QStringLiteral("%1_%2_%3_%4")
        .arg(item.price, 0, 'f', 5)
        .arg(item.qty, 0, 'f', 3)
        .arg(item.buy ? QLatin1Char('B') : QLatin1Char('S'))
        .arg(item.seq, 0, 10);
}

qint64 PrintsWidget::tickForPrice(double price) const
{
    if (!(m_tickSize > 0.0) || !std::isfinite(price) || !std::isfinite(m_firstPrice)) {
        return 0;
    }
    // IMPORTANT: use a tick index relative to the current ladder anchor price (first visible row),
    // not an absolute price/tickSize division. Absolute division becomes numerically unstable on
    // tiny prices / tiny tick sizes and can produce a consistent ±1 tick drift.
    const double delta =
        m_descending ? ((m_firstPrice - price) / m_tickSize) : ((price - m_firstPrice) / m_tickSize);
    const double nudged = delta + (delta >= 0.0 ? 1e-9 : -1e-9);
    return static_cast<qint64>(std::llround(nudged));
}

int PrintsWidget::rowForPrice(double price) const
{
    if (m_prices.isEmpty()) {
        return -1;
    }
    // First try exact match; DOM and prints share the same ladder values.
    auto it = m_priceToRow.constFind(price);
    if (it != m_priceToRow.constEnd()) {
        return it.value();
    }

    // If ladder is sorted descending (high -> low), use tickSize to snap index.
    if (m_tickSize > 0.0 && m_prices.size() > 1) {
        const bool descending = m_prices.first() > m_prices.last();
        double delta = descending ? (m_prices.first() - price) / m_tickSize
                                  : (price - m_prices.first()) / m_tickSize;
        const double nudged = delta + (delta >= 0.0 ? 1e-9 : -1e-9);
        const int idx = static_cast<int>(std::llround(nudged));
        if (idx >= 0 && idx < m_prices.size()) {
            return idx;
        }
    }

    int bestIdx = 0;
    double bestDist = std::numeric_limits<double>::max();
    for (int i = 0; i < m_prices.size(); ++i) {
        const double d = std::abs(m_prices[i] - price);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}

int PrintsWidget::applyRowOffset(int row) const
{
    return row;
}

qint64 PrintsWidget::bucketizeTick(qint64 tick) const
{
    const qint64 compression = std::max<qint64>(1, m_compressionTicks);
    if (compression == 1) {
        return tick;
    }
    if (tick >= 0) {
        return (tick / compression) * compression;
    }
    const qint64 absTick = -tick;
    const qint64 buckets = (absTick + compression - 1) / compression;
    return -buckets * compression;
}

int PrintsWidget::rowForTick(qint64 tick) const
{
    if (m_prices.isEmpty()) {
        return -1;
    }
    const qint64 bucketTick = bucketizeTick(tick);
    const auto it = m_tickToRow.constFind(bucketTick);
    if (it != m_tickToRow.constEnd()) {
        return it.value();
    }
    return -1;
}

void PrintsWidget::calibrateRowOffset(int domRow, int priceRow)
{
    Q_UNUSED(domRow);
    Q_UNUSED(priceRow);
}

QSize PrintsWidget::sizeHint() const
{
    return QSize(120, 400);
}

QSize PrintsWidget::minimumSizeHint() const
{
    return QSize(0, 200);
}
void PrintsWidget::resizeEvent(QResizeEvent *event)
{
    if (m_quickWidget) {
        m_quickWidget->setGeometry(rect());
    }
    updatePrintsQml();
    QWidget::resizeEvent(event);
}

void PrintsWidget::ensureQuickInitialized()
{
    if (!printsUseQml()) {
        return;
    }
    if (m_quickWidget) {
        return;
    }
    m_quickWidget = new QQuickWidget(this);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->setClearColor(Qt::transparent);
    if (auto *engine = m_quickWidget->engine()) {
        connect(engine,
                &QQmlEngine::warnings,
                this,
                [](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings) {
                        qWarning().noquote() << "[Prints QML warning]" << w.toString();
                    }
                });
    }
    connect(m_quickWidget,
            &QQuickWidget::statusChanged,
            this,
            [this](QQuickWidget::Status status) {
                m_quickReady = (status == QQuickWidget::Ready);
                if (status == QQuickWidget::Error) {
                    const auto errs = m_quickWidget->errors();
                    for (const auto &e : errs) {
                        qWarning().noquote() << "[Prints QML error]" << e.toString();
                    }
                }
                if (m_quickReady) {
                    // Propagate domContainerPtr to QQuick internals so global eventFilter can
                    // route wheel/hover events to the correct ladder column.
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
                    updatePrintsQml();
                    updateOrdersQml();
                    updateHoverQml();
                }
            });
    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/GpuPrintsView.qml")));
    m_quickWidget->setGeometry(rect());
}

void PrintsWidget::syncQuickProperties()
{
    if (!m_quickWidget || !m_quickReady) {
        return;
    }
    if (auto *root = m_quickWidget->rootObject()) {
        ThemeManager *theme = ThemeManager::instance();
        root->setProperty("rowHeight", m_rowHeight);
        root->setProperty("rowCount", m_prices.size());
        root->setProperty("infoAreaHeight", m_domInfoAreaHeight);
        root->setProperty("useGpuPrints", m_useGpuPrints);
        root->setProperty("backgroundColor", theme->panelBackground());
        root->setProperty("gridColor", theme->gridColor());
        root->setProperty("connectionColor", theme->borderColor());
        root->setProperty("hoverColor", theme->selectionColor());
        root->setProperty("hoverTextColor", theme->textPrimary());
        const QFont fnt = font();
        const int px = fnt.pixelSize() > 0 ? fnt.pixelSize() : std::max(10, fnt.pointSize() + 2);
        root->setProperty("fontFamily", fnt.family());
        root->setProperty("fontPixelSize", px);
        root->setProperty("printsSquare", theme->printsSquare());
        root->setProperty("circlesModel", QVariant::fromValue(static_cast<QObject *>(&m_circlesModel)));
        root->setProperty("ordersModel", QVariant::fromValue(static_cast<QObject *>(&m_ordersModel)));
        root->setProperty("printsBridge", QVariant::fromValue(static_cast<QObject *>(this)));
    }
}

void PrintsWidget::requestCancel(const QVariant &orderIds,
                                const QString &label,
                                double price,
                                bool buy)
{
    QStringList ids;
    if (orderIds.canConvert<QStringList>()) {
        const QStringList list = orderIds.toStringList();
        for (const auto &s0 : list) {
            const QString s = s0.trimmed();
            if (!s.isEmpty()) {
                ids.push_back(s);
            }
        }
    } else {
        const QVariantList list = orderIds.toList();
        ids.reserve(list.size());
        for (const auto &v : list) {
            const QString s = v.toString().trimmed();
            if (!s.isEmpty()) {
                ids.push_back(s);
            }
        }
    }
    emit cancelMarkerRequested(ids, label, price, buy);
}

int PrintsWidget::snapRowFromY(qreal y) const
{
    if (m_rowHeight <= 0 || m_prices.isEmpty()) {
        return -1;
    }
    const int rowCount = m_prices.size();
    int row = static_cast<int>(std::floor(y / static_cast<qreal>(m_rowHeight)));
    row = std::clamp(row, 0, std::max(0, rowCount - 1));
    return row;
}

double PrintsWidget::priceForRow(int row) const
{
    if (m_prices.isEmpty() || row < 0) {
        return 0.0;
    }
    const int rowCount = m_prices.size();
    if (rowCount <= 0) {
        return 0.0;
    }
    const int priceRow = applyRowOffset(row);
    if (priceRow < 0 || priceRow >= rowCount) {
        return 0.0;
    }
    return m_prices[priceRow];
}

void PrintsWidget::requestBeginMove(const QString &orderId)
{
    const QString id = orderId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    emit beginMoveMarkerRequested(id);
}

void PrintsWidget::requestCommitMove(const QString &orderId, double newPrice, double fallbackPrice)
{
    const QString id = orderId.trimmed();
    if (id.isEmpty()) {
        return;
    }
    emit commitMoveMarkerRequested(id, newPrice, fallbackPrice);
}

void PrintsWidget::requestBeginMoveSltp(const QString &kind, double originPrice)
{
    const QString k = kind.trimmed().toUpper();
    if (k != QStringLiteral("SL") && k != QStringLiteral("TP")) {
        return;
    }
    emit beginMoveSltpMarkerRequested(k, originPrice);
}

void PrintsWidget::requestCommitMoveSltp(const QString &kind, double newPrice, double fallbackPrice)
{
    const QString k = kind.trimmed().toUpper();
    if (k != QStringLiteral("SL") && k != QStringLiteral("TP")) {
        return;
    }
    emit commitMoveSltpMarkerRequested(k, newPrice, fallbackPrice);
}

void PrintsWidget::requestDragPreview(double price)
{
    if (!(price > 0.0) || !std::isfinite(price)) {
        emit dragPreviewPriceRequested(0.0);
        return;
    }
    emit dragPreviewPriceRequested(price);
}

void PrintsWidget::requestClearDragPreview()
{
    emit dragPreviewPriceRequested(0.0);
}

int PrintsWidget::cursorY() const
{
    return mapFromGlobal(QCursor::pos()).y();
}

bool PrintsWidget::isLeftMouseDown() const
{
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
    return (QGuiApplication::mouseButtons() & Qt::LeftButton) != 0;
#endif
}

void PrintsWidget::updatePrintsQml()
{
    const bool qmlActive = (m_quickWidget && m_quickReady);
    bool throttled = false;
    if (m_quickUpdateThrottle.isValid()) {
        if (m_quickUpdateThrottle.nsecsElapsed() < kMinQuickUpdateIntervalNs) {
            throttled = true;
        } else {
            m_quickUpdateThrottle.restart();
        }
    } else {
        m_quickUpdateThrottle.start();
    }
    if (throttled) {
        return;
    }
    ThemeManager *theme = ThemeManager::instance();
    QVector<PrintCirclesModel::Entry> entries;
    const int rowCount = m_prices.size();
    const int count = m_items.size();
    if (rowCount > 0 && count > 0) {
        const int widgetWidth = std::max(1, width());
        const int slotCount = std::max(6, widgetWidth / 24);
        const int visibleSlots = std::max(1, std::min(slotCount, m_maxVisiblePrints));
        const int startIdx = count > visibleSlots ? (count - visibleSlots) : 0;
        entries.reserve(count - startIdx);
        for (int i = startIdx; i < count; ++i) {
            const PrintItem &item = m_items[i];
            const QString key = makeKey(item);
            const double spawn = std::clamp(m_spawnProgress.value(key, 1.0), 0.0, 1.0);
            const double eased = 1.0 - std::pow(1.0 - spawn, 3.0);
            const double magnitude = std::log10(1.0 + std::abs(item.qty));
            const int baseRadius = std::clamp(9 + static_cast<int>(std::round(magnitude * 5.0)), 10, 18);
            const double animatedRadius = baseRadius * (0.8 + 0.2 * eased);
            const int slotIndex = i - startIdx;
            const double ratio =
                visibleSlots > 0 ? (slotIndex + 0.5) / static_cast<double>(visibleSlots) : 0.5;
            int rowIdx = resolvedRowForItem(item);
            const double yCenter =
                rowIdx >= 0 ? rowIdx * m_rowHeight + (m_rowHeight / 2.0) : (m_rowHeight / 2.0);
            QColor fill = item.buy ? theme->bidColor() : theme->askColor();
            fill.setAlpha(static_cast<int>(210 * (0.7 + 0.3 * eased)));
            QColor border = fill;
            border = item.buy ? border.darker(140) : border.darker(160);
            PrintCirclesModel::Entry entry;
            entry.xRatio = ratio;
            entry.y = yCenter;
            entry.radius = animatedRadius;
            entry.fillColor = fill;
            entry.borderColor = border;
            entry.text = formatQty(item.qty);
            entries.append(entry);
        }
    }
    m_circlesModel.setEntries(std::move(entries));
    if (!qmlActive) {
        update();
    }
}

void PrintsWidget::updateOrdersQml()
{
    const bool qmlActive = (m_quickWidget && m_quickReady);
    QVector<PrintOrdersModel::Entry> entries;
    ThemeManager *theme = ThemeManager::instance();
    if (!m_orderMarkers.isEmpty() && !m_prices.isEmpty()) {
        const qint64 compression = std::max<qint64>(1, m_compressionTicks);
        auto floorBucketTickSigned = [&](qint64 tick) -> qint64 {
            if (compression == 1) {
                return tick;
            }
            if (tick >= 0) {
                return (tick / compression) * compression;
            }
            const qint64 absTick = -tick;
            const qint64 buckets = (absTick + compression - 1) / compression;
            return -buckets * compression;
        };
        auto ceilBucketTickSigned = [&](qint64 tick) -> qint64 {
            if (compression == 1) {
                return tick;
            }
            if (tick >= 0) {
                return ((tick + compression - 1) / compression) * compression;
            }
            const qint64 absTick = -tick;
            const qint64 buckets = absTick / compression;
            return -buckets * compression;
        };
        auto rowForBucketTick = [&](qint64 bucketTick) -> int {
            const auto it = m_tickToRow.constFind(bucketTick);
            return (it != m_tickToRow.constEnd()) ? it.value() : -1;
        };
        auto tryRowForOrder = [&](double price, bool buy) -> int {
            if (!(m_rawTickSize > 0.0) || !std::isfinite(m_rawTickSize) || !std::isfinite(price)) {
                return -1;
            }
            const double scaled = price / m_rawTickSize;
            if (!std::isfinite(scaled)) {
                return -1;
            }
            const double nudged = scaled + (scaled >= 0.0 ? 1e-9 : -1e-9);
            const qint64 rawTick = static_cast<qint64>(std::llround(nudged));
            const qint64 bucketTick = buy ? floorBucketTickSigned(rawTick) : ceilBucketTickSigned(rawTick);
            return rowForBucketTick(bucketTick);
        };

        struct Agg {
            double qty = 0.0;
            qint64 createdMs = 0;
            bool buy = true;
            int row = -1;
            double price = 0.0;
            QString label;
            QStringList orderIds;
            QColor fillColor;
            QColor borderColor;
            bool hasColors = false;
        };
        QHash<QString, Agg> aggregated;
        aggregated.reserve(m_orderMarkers.size());
        for (const auto &ord : m_orderMarkers) {
            // IMPORTANT: Map orders using the raw tick size (absolute tick) and side-aware bucketing.
            // Rounding by the bucket row size (or by an anchor price) can shift markers by 1 row and
            // create apparent duplicates (e.g. two bids/asks) that "fix themselves" on scroll.
            int rowIdx = tryRowForOrder(ord.price, ord.buy);
            if (rowIdx < 0) {
                rowIdx = rowForPrice(ord.price);
            }
            if (rowIdx >= 0) {
                rowIdx = applyRowOffset(rowIdx);
            }
            if (rowIdx < 0 || rowIdx >= m_prices.size()) {
                continue;
            }
            const QString label = ord.label.trimmed();
            // Dedupe: SL/TP labels can sometimes arrive twice (different "buy" hints, same row).
            // Keep them unique by (row,label) instead of (row,side,label).
            const QString key =
                (label.isEmpty()
                     ? (QString::number(rowIdx) + QLatin1Char('|')
                        + (ord.buy ? QLatin1Char('B') : QLatin1Char('S')) + QLatin1Char('|'))
                     : (QString::number(rowIdx) + QStringLiteral("|L|")))
                + label;
            Agg agg = aggregated.value(key, Agg{});
            if (label.isEmpty()) {
                agg.qty += std::max(0.0, ord.quantity);
            } else {
                agg.label = label;
                // Force consistent semantics: SL is always red, TP is always green,
                // regardless of order side (e.g. SL for shorts is a BUY).
                if (label.compare(QStringLiteral("SL"), Qt::CaseInsensitive) == 0) {
                    agg.fillColor = QColor("#e53935");
                    agg.borderColor = QColor("#992626");
                    agg.hasColors = true;
                } else if (label.compare(QStringLiteral("TP"), Qt::CaseInsensitive) == 0) {
                    agg.fillColor = QColor("#4caf50");
                    agg.borderColor = QColor("#2f6c37");
                    agg.hasColors = true;
                } else if (ord.fillColor.isValid() && ord.borderColor.isValid()) {
                    agg.fillColor = ord.fillColor;
                    agg.borderColor = ord.borderColor;
                    agg.hasColors = true;
                }
            }
            if (!ord.orderId.trimmed().isEmpty()) {
                const QString id = ord.orderId.trimmed();
                if (!agg.orderIds.contains(id)) {
                    agg.orderIds.push_back(id);
                }
            }
            agg.buy = ord.buy;
            agg.row = rowIdx;
            agg.price = ord.price;
            agg.createdMs = agg.createdMs == 0 ? ord.createdMs : std::min(agg.createdMs, ord.createdMs);
            aggregated.insert(key, agg);
        }
        entries.reserve(aggregated.size());
        for (const auto &agg : aggregated) {
            if (agg.row < 0) {
                continue;
            }
            QColor fill = agg.buy ? theme->bidColor() : theme->askColor();
            static constexpr int kMarkerAlpha = 205;
            static constexpr int kBorderAlpha = 235;
            int alpha = kMarkerAlpha;
            if (agg.hasColors) {
                fill = agg.fillColor;
            }
            fill.setAlpha(alpha);
            QColor border = agg.buy ? fill.darker(140) : fill.darker(160);
            if (agg.hasColors) {
                border = agg.borderColor;
            }
            border.setAlpha(kBorderAlpha);
            PrintOrdersModel::Entry entry;
            entry.row = agg.row;
            entry.price = agg.price;
            entry.text = agg.label.isEmpty() ? formatQty(agg.qty) : agg.label;
            entry.orderIds = agg.orderIds;
            entry.fillColor = fill;
            entry.borderColor = border;
            entry.buy = agg.buy;
            entries.append(entry);
        }
    }
    // Important: keep a stable ordering to avoid QML delegate reuse artifacts when the set of markers
    // stays the same size but QHash iteration order changes (e.g. during scrolling/row remap).
    std::sort(entries.begin(), entries.end(), [](const PrintOrdersModel::Entry &a, const PrintOrdersModel::Entry &b) {
        if (a.row != b.row) return a.row < b.row;
        if (a.price != b.price) return a.price < b.price;
        if (a.text != b.text) return a.text < b.text;
        if (a.buy != b.buy) return a.buy < b.buy;
        return a.orderIds.join(QStringLiteral(",")).compare(b.orderIds.join(QStringLiteral(","))) < 0;
    });
    m_ordersModel.setEntries(std::move(entries));
    if (!qmlActive) {
        update();
    }
}

void PrintsWidget::updateHoverQml()
{
    if (!m_quickWidget || !m_quickReady) {
        update();
        return;
    }
    if (auto *root = m_quickWidget->rootObject()) {
        root->setProperty("hoverRow", m_hoverRow);
        root->setProperty("hoverText", m_hoverText);
    }
}

void PrintsWidget::updateClustersQml(bool force)
{
    if (m_prices.isEmpty()) {
        m_clusterCells.clear();
        m_clustersModel.setEntries({});
        return;
    }
    static constexpr qint64 kMinClusterUpdateMs = 50;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_lastClusterUpdateMs > 0 && nowMs - m_lastClusterUpdateMs < kMinClusterUpdateMs) {
        return;
    }
    m_lastClusterUpdateMs = nowMs;

    const QVector<qint64> startMsBefore = m_clusterBucketStartMs;

    // If mapping or cluster config changed, rebuild from the retained trade tail (bounded window).
    if (m_clusterAggBuiltRevision != m_clusterMappingRevision
        || m_clusterAggBucketMs != std::clamp(m_clusterBucketMs, 100, 86400000)
        || m_clusterAggBucketCount != std::clamp(m_clusterBucketCount, 1, kMaxClusterBuckets)) {
        rebuildClusterAggFromTrades(nowMs);
    } else {
        advanceClusterBuckets(nowMs);
    }

    const int bucketMs = m_clusterAggBucketMs;
    const int bucketCount = m_clusterAggBucketCount;
    const qint64 cutoff = nowMs - static_cast<qint64>(bucketMs) * static_cast<qint64>(bucketCount);
    while (!m_clusterTrades.empty() && m_clusterTrades.front().timeMs < cutoff) {
        m_clusterTrades.pop_front();
    }

    m_clusterCells.clear();
    for (int col = 0; col < bucketCount; ++col) {
        const auto &map = m_clusterAggByCol[col];
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const ClusterAgg &agg = it.value();
            if (!(agg.total > 0.0)) {
                continue;
            }
            ClusterCellAgg cell;
            cell.tick = it.key();
            cell.price = (m_tickSize > 0.0) ? (static_cast<double>(cell.tick) * m_tickSize) : 0.0;
            cell.col = col;
            cell.total = agg.total;
            cell.delta = agg.delta;
            m_clusterCells.push_back(cell);
        }
    }
    std::sort(m_clusterCells.begin(), m_clusterCells.end(), [](const auto &a, const auto &b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.col < b.col;
    });
    publishClustersModel();

    // Publish per-bucket totals for header/footer UI.
    QVector<double> totalsByCol(bucketCount, 0.0);
    for (int i = 0; i < bucketCount; ++i) {
        totalsByCol[i] = (i < m_clusterAggTotalsByCol.size()) ? m_clusterAggTotalsByCol[i] : 0.0;
    }
    const bool bucketsChanged = (m_clusterBucketTotals != totalsByCol);
    const bool startChanged = (m_clusterBucketStartMs != startMsBefore);
    m_clusterBucketTotals = std::move(totalsByCol);
    if (bucketsChanged || startChanged) {
        emit clusterBucketsChanged();
    }
    scheduleNextClusterBoundary();
}

void PrintsWidget::publishClustersModel()
{
    if (m_prices.isEmpty() || m_clusterCells.isEmpty()) {
        m_clustersModel.setEntries({});
        return;
    }

    QVector<PrintClustersModel::Entry> entries;
    entries.reserve(m_clusterCells.size());
    for (const auto &cell : m_clusterCells) {
        int rowIdx = cell.tick != 0 ? rowForTick(cell.tick) : -1;
        if (rowIdx < 0 && cell.price > 0.0) {
            rowIdx = rowForPrice(cell.price);
        }
        if (rowIdx < 0 || rowIdx >= m_prices.size()) {
            continue;
        }
        PrintClustersModel::Entry e;
        e.row = rowIdx;
        e.col = cell.col;
        e.text = formatQty(cell.total);
        e.textColor = QColor(QStringLiteral("#e0e0e0"));
        const double strength = std::clamp(std::abs(cell.delta) / std::max(1.0, cell.total), 0.0, 1.0);
        QColor bg = (cell.delta >= 0.0) ? QColor("#2e7d32") : QColor("#c62828");
        bg.setAlpha(std::clamp(static_cast<int>(35 + strength * 110), 35, 145));
        e.bgColor = bg;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        if (a.row != b.row) return a.row < b.row;
        return a.col < b.col;
    });
    m_clustersModel.setEntries(std::move(entries));
}

int PrintsWidget::resolvedRowForItem(const PrintItem &item, int *outRowIdx) const
{
    int rowIdx = -1;
    if (item.tick != 0) {
        rowIdx = rowForTick(item.tick);
    }
    if (rowIdx < 0) {
        rowIdx = rowForPrice(item.price);
    }
    if (outRowIdx) {
        *outRowIdx = rowIdx;
    }
    return rowIdx;
}

QString PrintsWidget::clusterLabel() const
{
    const int ms = std::max(100, m_clusterBucketMs);
    static constexpr int kDayMs = 86400000;
    static constexpr int kHourMs = 3600000;
    static constexpr int kMinMs = 60000;
    if (ms >= kDayMs && (ms % kDayMs) == 0) {
        return QStringLiteral("%1d").arg(ms / kDayMs);
    }
    if (ms >= kHourMs && (ms % kHourMs) == 0) {
        return QStringLiteral("%1h").arg(ms / kHourMs);
    }
    if (ms >= kMinMs && (ms % kMinMs) == 0) {
        return QStringLiteral("%1m").arg(ms / kMinMs);
    }
    if (ms >= 1000 && (ms % 1000) == 0) {
        return QStringLiteral("%1s").arg(ms / 1000);
    }
    return QStringLiteral("%1ms").arg(ms);
}

void PrintsWidget::setClusterWindowMs(int ms)
{
    const int clamped = std::clamp(ms, 100, 86400000);
    if (m_clusterBucketMs == clamped) {
        return;
    }
    m_clusterBucketMs = clamped;
    emit clusterLabelChanged(clusterLabel());
    resetClusterAgg();
    updateClustersQml(true);
}

void PrintsWidget::setClusterBucketCount(int count)
{
    const int clamped = std::clamp(count, 1, kMaxClusterBuckets);
    if (m_clusterBucketCount == clamped) {
        return;
    }
    m_clusterBucketCount = clamped;
    resetClusterAgg();
    updateClustersQml(true);
}

void PrintsWidget::scheduleNextClusterBoundary()
{
    const int bucketMs = std::clamp(m_clusterBucketMs, 100, 86400000);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 currentBucket = nowMs / bucketMs;
    const qint64 nextBoundary = (currentBucket + 1) * static_cast<qint64>(bucketMs);
    qint64 delay = nextBoundary - nowMs + 3;
    delay = std::clamp<qint64>(delay, 5, 60000);
    if (!m_clusterBoundaryTimer.isActive() || m_clusterBoundaryTimer.remainingTime() > delay + 50) {
        m_clusterBoundaryTimer.start(static_cast<int>(delay));
    }
}

void PrintsWidget::clearClusters()
{
    m_clusterTrades.clear();
    m_lastClusterSeq = 0;
    resetClusterAgg();
    updateClustersQml(true);
}
