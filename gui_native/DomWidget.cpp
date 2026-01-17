#include "DomWidget.h"
#include "DomLevelsModel.h"

#include <QAbstractScrollArea>
#include <QDateTime>
#include <QFontDatabase>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPolygon>
#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QScrollBar>
#include <QResizeEvent>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QFrame>
#include <QLabel>
#include <QHBoxLayout>
#include <QSizePolicy>
#include <QEvent>
#include <algorithm>
#include <limits>
#include <cmath>
#include <utility>

#include "ThemeManager.h"

namespace {
int precisionForTick(double tick)
{
    if (tick <= 0.0) {
        return 5;
    }
    // Примерно количество знаков после запятой из шага цены, с жёстким ограничением.
    const double logv = -std::log10(std::max(tick, 1e-10));
    const int p = static_cast<int>(std::ceil(logv - 1e-6));
    return std::clamp(p, 0, 10);
}

QString formatPriceForDisplay(double price, double tickSize);

static qint64 pow10i(int exp)
{
    qint64 v = 1;
    for (int i = 0; i < exp; ++i) {
        v *= 10;
    }
    return v;
}

static QString formatPriceForDisplayFromTick(qint64 tick, double tickSize)
{
    if (!(tickSize > 0.0) || !std::isfinite(tickSize)) {
        return QString();
    }
    const int precision = precisionForTick(tickSize);
    if (precision <= 0) {
        const double price = static_cast<double>(tick) * tickSize;
        return QString::number(price, 'f', 0);
    }

    const qint64 scale = pow10i(precision);
    const double tickSizeScaledD = tickSize * static_cast<double>(scale);
    if (!std::isfinite(tickSizeScaledD)) {
        return formatPriceForDisplay(static_cast<double>(tick) * tickSize, tickSize);
    }
    const qint64 tickSizeScaled = static_cast<qint64>(std::llround(tickSizeScaledD));
    if (tickSizeScaled <= 0 || std::abs(tickSizeScaledD - static_cast<double>(tickSizeScaled)) > 1e-9) {
        return formatPriceForDisplay(static_cast<double>(tick) * tickSize, tickSize);
    }

    const qint64 absTick = tick < 0 ? -tick : tick;
    const qint64 absTickSizeScaled = tickSizeScaled < 0 ? -tickSizeScaled : tickSizeScaled;
    if (absTickSizeScaled != 0 && absTick > (std::numeric_limits<qint64>::max() / absTickSizeScaled)) {
        return formatPriceForDisplay(static_cast<double>(tick) * tickSize, tickSize);
    }
    const qint64 scaled = tick * tickSizeScaled;
    const bool neg = scaled < 0;
    const qint64 absScaled = neg ? -scaled : scaled;

    const qint64 intPart = absScaled / scale;
    const qint64 fracPart = absScaled % scale;
    QString frac = QString::number(fracPart).rightJustified(precision, QLatin1Char('0'));

    if (intPart == 0) {
        int zeroCount = 0;
        while (zeroCount < frac.size() && frac.at(zeroCount) == QLatin1Char('0')) {
            ++zeroCount;
        }
        QString remainder = frac.mid(zeroCount);
        if (remainder.isEmpty()) {
            remainder = QStringLiteral("0");
        }
        if (zeroCount > 0) {
            return QStringLiteral("%1(%2)%3")
                .arg(neg ? QStringLiteral("-") : QString())
                .arg(zeroCount)
                .arg(remainder);
        }
    }

    const QString prefix = neg ? QStringLiteral("-") : QString();
    return prefix + QString::number(intPart) + QLatin1Char('.') + frac;
}

QString formatPriceForDisplay(double price, double tickSize)
{
    const int precision = precisionForTick(tickSize);
    QString base = QString::number(price, 'f', precision);
    const int dot = base.indexOf('.');
    if (dot < 0) {
        return base;
    }

    QString intPart = base.left(dot);
    QString frac = base.mid(dot + 1);

    if (intPart == QLatin1String("0")) {
        int zeroCount = 0;
        while (zeroCount < frac.size() && frac.at(zeroCount) == QLatin1Char('0')) {
            ++zeroCount;
        }
        QString remainder = frac.mid(zeroCount);
        if (remainder.isEmpty()) {
            remainder = QStringLiteral("0");
        }
        if (zeroCount > 0) {
            return QStringLiteral("(%1)%2").arg(zeroCount).arg(remainder);
        }
    }

    return intPart + QLatin1Char('.') + frac;
}

QString formatQty(double v)
{
    const double av = std::abs(v);
    if (av >= 1000000.0) {
        return QString::number(av / 1000000.0, 'f', av >= 10000000.0 ? 0 : 1) + QStringLiteral("M");
    }
    if (av >= 1000.0) {
        return QString::number(av / 1000.0, 'f', av >= 10000.0 ? 0 : 1) + QStringLiteral("K");
    }
    if (av >= 100.0) {
        return QString::number(av, 'f', 0);
    }
    return QString::number(av, 'f', 1);
}

QString formatValueShort(double v)
{
    const double av = std::abs(v);
    QString suffix;
    double value = av;
    if (av >= 1000000000.0) {
        value = av / 1000000000.0;
        suffix = QStringLiteral("B");
    } else if (av >= 1000000.0) {
        value = av / 1000000.0;
        suffix = QStringLiteral("M");
    } else if (av >= 1000.0) {
        value = av / 1000.0;
        suffix = QStringLiteral("K");
    }
    const int precision = value >= 10.0 ? 1 : 2;
    const QString text = QString::number(value, 'f', precision);
    return suffix.isEmpty() ? text : text + suffix;
}


bool percentFromReference(double price, double bestBid, double bestAsk, double &outPercent)
{
    if (bestBid > 0.0 && price <= bestBid) {
        outPercent = (bestBid - price) / bestBid * 100.0;
        return true;
    }
    if (bestAsk > 0.0 && price >= bestAsk) {
        outPercent = (price - bestAsk) / bestAsk * 100.0;
        return true;
    }
    if (bestAsk > 0.0 && price < bestAsk) {
        outPercent = -((bestAsk - price) / bestAsk * 100.0);
        return true;
    }
    if (bestBid > 0.0 && price > bestBid) {
        outPercent = -((price - bestBid) / bestBid * 100.0);
        return true;
    }
    return false;
}

double priceTolerance(double tick)
{
    if (tick > 0.0) {
        return std::max(1e-8, tick * 0.25);
    }
    return 1e-8;
}

struct BestBucketTicks {
    bool hasBid = false;
    bool hasAsk = false;
    qint64 bidBucketTick = 0;
    qint64 askBucketTick = 0;
};

static bool priceToTick(double price, double tickSize, qint64 &outTick)
{
    if (!(tickSize > 0.0) || !std::isfinite(tickSize) || !std::isfinite(price)) {
        return false;
    }
    const double scaled = price / tickSize;
    if (!std::isfinite(scaled)) {
        return false;
    }
    outTick = static_cast<qint64>(std::llround(scaled));
    return true;
}

static qint64 floorBucketTick(qint64 tick, qint64 compression)
{
    compression = std::max<qint64>(1, compression);
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

static qint64 ceilBucketTick(qint64 tick, qint64 compression)
{
    compression = std::max<qint64>(1, compression);
    if (compression == 1) {
        return tick;
    }
    if (tick >= 0) {
        return ((tick + compression - 1) / compression) * compression;
    }
    const qint64 absTick = -tick;
    const qint64 buckets = absTick / compression;
    return -buckets * compression;
}

static BestBucketTicks bestBucketTicksForSnapshot(const DomSnapshot &snap)
{
    BestBucketTicks fromRows;
    BestBucketTicks fromPrice;
    const qint64 compression = std::max<qint64>(1, snap.compression);

    // Prefer deriving best buckets from the actual ladder rows: during volatility the backend can
    // publish bestBid/bestAsk out-of-sync with the aggregated depth updates, and in GPU mode we
    // update incrementally. Using row-derived ticks avoids duplicated "best" rows.
    qint64 bestBidBucket = 0;
    qint64 bestAskBucket = 0;
    bool haveBid = false;
    bool haveAsk = false;
    for (const auto &lvl : snap.levels) {
        if (lvl.tick == 0) {
            continue;
        }
        if (lvl.bidQty > 0.0) {
            if (!haveBid || lvl.tick > bestBidBucket) {
                bestBidBucket = lvl.tick;
                haveBid = true;
            }
        }
        if (lvl.askQty > 0.0) {
            if (!haveAsk || lvl.tick < bestAskBucket) {
                bestAskBucket = lvl.tick;
                haveAsk = true;
            }
        }
    }
    if (haveBid) {
        fromRows.hasBid = true;
        fromRows.bidBucketTick = floorBucketTick(bestBidBucket, compression);
    }
    if (haveAsk) {
        fromRows.hasAsk = true;
        fromRows.askBucketTick = ceilBucketTick(bestAskBucket, compression);
    }

    if (snap.bestBid > 0.0 && (snap.tickSize > 0.0) && std::isfinite(snap.tickSize)) {
        qint64 bestBidTick = 0;
        if (priceToTick(snap.bestBid, snap.tickSize, bestBidTick)) {
            fromPrice.hasBid = true;
            fromPrice.bidBucketTick = floorBucketTick(bestBidTick, compression);
        }
    }
    if (snap.bestAsk > 0.0 && (snap.tickSize > 0.0) && std::isfinite(snap.tickSize)) {
        qint64 bestAskTick = 0;
        if (priceToTick(snap.bestAsk, snap.tickSize, bestAskTick)) {
            fromPrice.hasAsk = true;
            fromPrice.askBucketTick = ceilBucketTick(bestAskTick, compression);
        }
    }

    const bool rowsOk =
        fromRows.hasBid && fromRows.hasAsk && fromRows.bidBucketTick < fromRows.askBucketTick;
    const bool priceOk =
        fromPrice.hasBid && fromPrice.hasAsk && fromPrice.bidBucketTick < fromPrice.askBucketTick;

    if (rowsOk) {
        return fromRows;
    }
    if (priceOk) {
        return fromPrice;
    }
    if (fromRows.hasBid || fromRows.hasAsk) {
        return fromRows;
    }
    return fromPrice;
}
} // namespace

DomWidget::DomWidget(QWidget *parent)
    : QWidget(parent)
{
    m_quickAllowed = !qEnvironmentVariableIsSet("DOM_DISABLE_QML")
                     && !qEnvironmentVariableIsSet("DOM_FORCE_WIDGET");
    m_disableIncremental = qEnvironmentVariableIsSet("DOM_DISABLE_INCREMENTAL")
                           || qEnvironmentVariableIsSet("DOM_FORCE_FULL");
    m_useGpuDom = qEnvironmentVariableIntValue("DOM_GPU") > 0;

    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Ladder font follows the application font (Inter by default) so the UI uses a single font.
    // QML gets the font family via syncQuickProperties().
    if (qApp) {
        setFont(qApp->font());
    }

    ThemeManager *theme = ThemeManager::instance();
    setStyle(theme->domStyle());
    connect(theme, &ThemeManager::themeChanged, this, [this, theme]() {
        setStyle(theme->domStyle());
    });

    if (m_quickAllowed) {
        ensureQuickInitialized();
        syncQuickProperties();
    }
}

void DomWidget::refreshFontProperties()
{
    const QFont next = qApp ? qApp->font() : font();
    if (next != font()) {
        setFont(next);
    }
    if (m_quickAllowed) {
        syncQuickProperties();
    }
    update();
}

void DomWidget::updateSnapshot(const DomSnapshot &snapshot)
{
    m_pendingSnapshot = snapshot;
    m_hasPendingSnapshot = true;

    if (m_snapshotUpdateScheduled) {
        return;
    }
    m_snapshotUpdateScheduled = true;
    QTimer::singleShot(0, this, [this]() { applyPendingSnapshot(); });
}

void DomWidget::applyPendingSnapshot()
{
    m_snapshotUpdateScheduled = false;
    if (!m_hasPendingSnapshot) {
        return;
    }
    m_hasPendingSnapshot = false;
    m_snapshot = m_pendingSnapshot;
    // Force a full per-row recompute for this snapshot. This avoids any chance of stale
    // GPU incremental rows "sticking" during volatility when deltas are out-of-order.
    m_forceFullRecalc = true;

    const int rows = m_snapshot.levels.size();
    const int rowHeight = m_rowHeight;
    const int totalHeight = std::max(rows * rowHeight + m_infoAreaHeight, minimumSizeHint().height());
    if (totalHeight != m_cachedTotalHeight) {
        setMinimumHeight(totalHeight);
        setMaximumHeight(totalHeight);
        updateGeometry();
        m_cachedTotalHeight = totalHeight;
    }

    // Keep hover stable while the ladder window changes:
    // - If the cursor is over the widget, recompute the hovered row from cursor Y (even if mouse doesn't move)
    //   so scrolling updates the hovered price.
    // - If the cursor is not over the widget, clear hover to avoid a "stuck" ray in Prints.
    {
        const QPoint localPos = mapFromGlobal(QCursor::pos());
        const int ladderHeight = rows * rowHeight;
        const bool cursorInside =
            isVisible() && rect().contains(localPos) && localPos.y() >= 0 && localPos.y() < ladderHeight;

        if (!cursorInside) {
            if (m_hoverRow != -1) {
                m_hoverRow = -1;
                updateHoverInfo(-1);
                emit rowHovered(-1, 0.0, 0.0, 0.0);
            } else if (!m_hoverInfoText.isEmpty()) {
                updateHoverInfo(-1);
            }
        } else {
            int row = -1;
            if (rows > 0 && localPos.y() >= 0 && localPos.y() < ladderHeight) {
                row = std::clamp(localPos.y() / rowHeight, 0, rows - 1);
            }
            const bool changed = (row != m_hoverRow);
            m_hoverRow = row;
            updateHoverInfo(row);
            if (row >= 0) {
                const DomLevel &lvl = m_snapshot.levels[row];
                emit rowHovered(row, lvl.price, lvl.bidQty, lvl.askQty);
            } else if (changed) {
                emit rowHovered(-1, 0.0, 0.0, 0.0);
            }
        }
    }

    updateQuickOverlayProperties();
    updateQuickSnapshot();
}

void DomWidget::setStyle(const DomStyle &style)
{
    m_style = style;
    syncQuickProperties();
}

void DomWidget::setRowHeight(int h)
{
    int clamped = std::clamp(h, 10, 40);
    if (clamped == m_rowHeight) {
        return;
    }
    m_rowHeight = clamped;
    syncQuickProperties();
    updateQuickSnapshot();
}

void DomWidget::setTradePosition(const TradePosition &position)
{
    m_position = position;
    // Keep a stable bottom padding to prevent QScrollArea scroll drift and
    // 1-row desync between ladder and prints when the position panel toggles.
    static constexpr int kInfoAreaHeight = 34;
    if (m_infoAreaHeight != kInfoAreaHeight) {
        m_infoAreaHeight = kInfoAreaHeight;
        emit infoAreaHeightChanged(m_infoAreaHeight);
    }
    updateSnapshot(m_snapshot);
    syncQuickProperties();
    update();
}

void DomWidget::handleExitClick()
{
    emit exitPositionRequested();
}

void DomWidget::centerToSpread()
{
    if (m_snapshot.levels.isEmpty()) {
        return;
    }

    double bestBid = 0.0;
    double bestAsk = 0.0;
    bool hasBid = false;
    bool hasAsk = false;

    for (const auto &lvl : m_snapshot.levels) {
        if (lvl.bidQty > 0.0) {
            if (!hasBid || lvl.price > bestBid) {
                bestBid = lvl.price;
                hasBid = true;
            }
        }
        if (lvl.askQty > 0.0) {
            if (!hasAsk || lvl.price < bestAsk) {
                bestAsk = lvl.price;
                hasAsk = true;
            }
        }
    }

    double centerPrice = 0.0;
    if (hasBid && hasAsk) {
        centerPrice = (bestBid + bestAsk) * 0.5;
    } else if (hasBid) {
        centerPrice = bestBid;
    } else if (hasAsk) {
        centerPrice = bestAsk;
    } else {
        return;
    }

    QWidget *w = parentWidget();
    QAbstractScrollArea *area = nullptr;
    while (w && !area) {
        area = qobject_cast<QAbstractScrollArea *>(w);
        w = w->parentWidget();
    }
    if (!area) {
        return;
    }

    QScrollBar *sb = area->verticalScrollBar();
    if (!sb) {
        return;
    }

    const int rows = m_snapshot.levels.size();
    if (rows <= 0) {
        return;
    }

    int bestRow = 0;
    double bestDist = std::numeric_limits<double>::max();
    for (int i = 0; i < rows; ++i) {
        double d = std::abs(m_snapshot.levels[i].price - centerPrice);
        if (d < bestDist) {
            bestDist = d;
            bestRow = i;
        }
    }

    const int centerPixel = bestRow * m_rowHeight + m_rowHeight / 2;
    const int viewportHeight = area->viewport()->height();
    int value = centerPixel - viewportHeight / 2;
    value = std::max(sb->minimum(), std::min(sb->maximum(), value));
    sb->setValue(value);
}

bool DomWidget::event(QEvent *event)
{
    if (event && event->type() == QEvent::ParentChange) {
        ensureActionOverlayParent();
    }
    return QWidget::event(event);
}

void DomWidget::paintEvent(QPaintEvent *event)
{
    if (event && event->type() == QEvent::ParentChange) {
        ensureActionOverlayParent();
    }
    if (m_quickReady) {
        event->accept();
        return;
    }
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Background
    p.fillRect(rect(), m_style.background);

    if (m_snapshot.levels.isEmpty()) {
        return;
    }

    const int w = width();
    const QRect clipRect = event->rect();
    const int rows = m_snapshot.levels.size();
    if (rows <= 0) {
        return;
    }

    const double rowHeight = static_cast<double>(m_rowHeight);
    const double bestPriceTolerance = priceTolerance(m_snapshot.tickSize);
    const BestBucketTicks bestTicks = bestBucketTicksForSnapshot(m_snapshot);
    QFontMetrics fm(font());

    const int firstRow = std::max(0, static_cast<int>(clipRect.top() / rowHeight));
    const int lastRow = std::min(rows - 1, static_cast<int>(clipRect.bottom() / rowHeight));

    // Подбираем ширину колонки по реально отображаемым ценам.
    int maxPriceWidth = 0;
    for (int i = firstRow; i <= lastRow; ++i) {
        const auto &lvl = m_snapshot.levels[i];
        const QString text =
            (lvl.tick != 0) ? formatPriceForDisplayFromTick(lvl.tick, m_snapshot.tickSize)
                            : formatPriceForDisplay(lvl.price, m_snapshot.tickSize);
        maxPriceWidth = std::max(maxPriceWidth, fm.horizontalAdvance(text));
    }
    // Резерв под сжатый формат малых цен "(3)12345".
    const QString tinySample = QStringLiteral("(5)12345");
    maxPriceWidth = std::max(maxPriceWidth, fm.horizontalAdvance(tinySample));
    const int priceColWidth = std::clamp(maxPriceWidth + 8, 52, std::max(60, w / 3));
    const int priceRight = w - 1;
    const int priceLeft = std::max(0, priceRight - priceColWidth);

    const bool hasPosition = m_position.hasPosition && m_position.quantity > 0.0 && m_position.averagePrice > 0.0;
    double bestReferencePrice = 0.0;
    double entryPrice = 0.0;
    int positionRow = -1;
    double positionPnl = 0.0;
    QColor positionPnlColor = Qt::transparent;
    QColor positionPriceBgColor = Qt::transparent;
    QColor positionRangeBgColor = Qt::transparent;
    double rangeMin = 0.0;
    double rangeMax = 0.0;
    if (hasPosition) {
        entryPrice = m_position.averagePrice;
        bestReferencePrice = (m_position.side == OrderSide::Buy) ? m_snapshot.bestBid : m_snapshot.bestAsk;
        if (bestReferencePrice > 0.0 && entryPrice > 0.0) {
            positionRow = rowForPrice(bestReferencePrice);
            positionPnl = (m_position.side == OrderSide::Buy)
                              ? (bestReferencePrice - m_position.averagePrice) * m_position.quantity
                              : (m_position.averagePrice - bestReferencePrice) * m_position.quantity;
            positionPnlColor = positionPnl >= 0.0 ? QColor("#4caf50") : QColor("#e53935");
            positionPriceBgColor = positionPnlColor;
            positionPriceBgColor.setAlpha(110);
            positionRangeBgColor = positionPnlColor;
            positionRangeBgColor.setAlpha(70);
            rangeMin = std::min(entryPrice, bestReferencePrice);
            rangeMax = std::max(entryPrice, bestReferencePrice);
        }
    }

    // Grid vertical lines for price column borders.
    p.setPen(m_style.grid);
    if (priceLeft > 0) {
        p.drawLine(QPoint(priceLeft, 0), QPoint(priceLeft, height()));
    }
    p.drawLine(QPoint(priceRight, 0), QPoint(priceRight, height()));

    for (int i = firstRow; i <= lastRow; ++i) {
        const DomLevel &lvl = m_snapshot.levels[i];

        const int y = static_cast<int>(i * rowHeight);
        const int rowIntHeight = std::max(1, static_cast<int>(rowHeight));
        const QRect rowRect(0, y, w, rowIntHeight);
        const QRect bookRect(0, y, std::max(0, priceLeft + 1), rowIntHeight);
        const QRect priceRect(priceLeft, y, priceColWidth, rowIntHeight);

        // Simple side coloring based on which side is larger.
        double bidQty = lvl.bidQty;
        double askQty = lvl.askQty;
        // Extra safety: never render liquidity on the wrong side of the spread buckets.
        const bool lockedSpread =
            bestTicks.hasBid && bestTicks.hasAsk && (bestTicks.bidBucketTick == bestTicks.askBucketTick);
        if (bestTicks.hasBid && lvl.tick != 0) {
            if (lockedSpread) {
                if (lvl.tick < bestTicks.bidBucketTick) {
                    askQty = 0.0;
                }
            } else if (lvl.tick <= bestTicks.bidBucketTick) {
                askQty = 0.0;
            }
        }
        if (bestTicks.hasAsk && lvl.tick != 0) {
            if (lockedSpread) {
                if (lvl.tick > bestTicks.askBucketTick) {
                    bidQty = 0.0;
                }
            } else if (lvl.tick >= bestTicks.askBucketTick) {
                bidQty = 0.0;
            }
        }
        if (!lockedSpread && bestTicks.hasBid && bestTicks.hasAsk && lvl.tick != 0
            && lvl.tick > bestTicks.bidBucketTick && lvl.tick < bestTicks.askBucketTick) {
            bidQty = 0.0;
            askQty = 0.0;
        }
        const bool hasBid = bidQty > 0.0;
        const bool hasAsk = askQty > 0.0;
        const bool isBestBidRow =
            (bestTicks.hasBid && lvl.tick == bestTicks.bidBucketTick)
            || (m_snapshot.bestBid > 0.0 && std::abs(lvl.price - m_snapshot.bestBid) <= bestPriceTolerance);
        const bool isBestAskRow =
            (bestTicks.hasAsk && lvl.tick == bestTicks.askBucketTick)
            || (m_snapshot.bestAsk > 0.0 && std::abs(lvl.price - m_snapshot.bestAsk) <= bestPriceTolerance);

        QColor rowColor = m_style.background;
        if (hasBid || hasAsk) {
            rowColor = hasAsk && (!hasBid || lvl.askQty >= lvl.bidQty) ? m_style.ask : m_style.bid;
            if (bookRect.width() > 0) {
                QColor bg = rowColor;
                bg.setAlpha((isBestBidRow || isBestAskRow) ? 150 : 60);
                p.fillRect(bookRect, bg);
            }
        }
        if (rowColor != m_style.background) {
            QColor priceBg = rowColor;
            priceBg.setAlpha((isBestBidRow || isBestAskRow) ? 120 : 40);
            p.fillRect(priceRect, priceBg);
        }

        const bool inPositionRange =
            (positionPnlColor.isValid() && positionPnlColor.alpha() > 0) &&
            (lvl.price + bestPriceTolerance >= rangeMin && lvl.price - bestPriceTolerance <= rangeMax);
        if (inPositionRange) {
            p.fillRect(priceRect, positionRangeBgColor);
        }
        const bool isPositionPriceRow = (positionRow >= 0 && i == positionRow);
        if (isPositionPriceRow) {
            p.fillRect(priceRect, positionPriceBgColor);
            QRect markerBar = priceRect;
            markerBar.setWidth(std::min(2, priceRect.width()));
            p.fillRect(markerBar, positionPnlColor);
            QPen markerPen(positionPnlColor);
            markerPen.setWidth(1);
            p.setPen(markerPen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(priceRect.adjusted(0, 0, -1, -1));
        }

        double dominantQty = 0.0;
        bool volumeIsBid = false;
        if (bidQty > 0.0) {
            dominantQty = bidQty;
            volumeIsBid = true;
        }
        if (askQty > dominantQty) {
            dominantQty = askQty;
            volumeIsBid = false;
        }
        const double notional = dominantQty * std::abs(lvl.price);
        if (notional > 0.0 && bookRect.width() > 8) {
            const QString qtyText = formatQty(notional);
            QFont boldFont = p.font();
            boldFont.setBold(true);
            p.setFont(boldFont);
            QColor qtyColor = volumeIsBid ? m_style.bid : m_style.ask;
            qtyColor.setAlpha(220);
            QRect qtyRect(bookRect.left() + 4, y, bookRect.width() - 6, rowIntHeight);

            int matchedIndex = -1;
            double rangeMin = 0.0;
            double rangeMax = 0.0;
            for (int idx = 0; idx < m_volumeRules.size(); ++idx) {
                if (notional >= m_volumeRules[idx].threshold) {
                    matchedIndex = idx;
                } else {
                    break;
                }
            }
            if (matchedIndex >= 0) {
                const VolumeHighlightRule &matched = m_volumeRules[matchedIndex];
                rangeMin = matched.threshold;
                if (matchedIndex + 1 < m_volumeRules.size()) {
                    rangeMax = m_volumeRules[matchedIndex + 1].threshold;
                } else {
                    rangeMax = rangeMin;
                }

                QColor bg = matched.color.isValid() ? matched.color : QColor("#ffd54f");
                QColor textColor = bg.lightness() < 120 ? QColor("#f0f0f0") : QColor("#1e1e1e");
                bg.setAlpha(220);

                double ratio = 1.0;
                if (rangeMax > rangeMin) {
                    ratio = std::clamp((notional - rangeMin) / (rangeMax - rangeMin), 0.0, 1.0);
                }
                const int totalWidth = bookRect.width();
                int highlightWidth = static_cast<int>(std::round(totalWidth * ratio));
                highlightWidth = std::clamp(highlightWidth, 0, totalWidth);

                if (highlightWidth > 0) {
                    QRect fillRect(bookRect.left(), y, highlightWidth, rowIntHeight);
                    p.fillRect(fillRect, bg);
                }
                p.setPen(textColor);
            } else {
                p.setPen(qtyColor);
            }

            p.drawText(qtyRect, Qt::AlignLeft | Qt::AlignVCenter, qtyText);
            p.setFont(font());
        }

        // Price text with leading-zero compaction.
        const QString text =
            (lvl.tick != 0) ? formatPriceForDisplayFromTick(lvl.tick, m_snapshot.tickSize)
                            : formatPriceForDisplay(lvl.price, m_snapshot.tickSize);
        if (isPositionPriceRow) {
            QFont bold = font();
            bold.setBold(true);
            p.setFont(bold);
            p.setPen(positionPnlColor);
        } else if (inPositionRange) {
            p.setFont(font());
            p.setPen(positionPnlColor);
        } else {
            p.setPen(m_style.text);
            p.setFont(font());
        }
        int textWidth = fm.horizontalAdvance(text);
        int textX = priceRight - textWidth - 2;
        int textY = y + fm.ascent() + (static_cast<int>(rowHeight) - fm.height()) / 2;
        p.drawText(textX, textY, text);
        if (isPositionPriceRow) {
            p.setFont(font());
        }

        // Grid line aligned to row top (matches PrintsWidget grid spacing)
        p.setPen(m_style.grid);
        p.drawLine(QPoint(0, y), QPoint(w, y));

        if (i == m_hoverRow) {
            QColor hoverFill(40, 110, 220, 60);
            QRect hoverRect = rowRect;
            hoverRect.setLeft(std::max(0, bookRect.left()));
            hoverRect.setRight(priceRight + 1);
            p.fillRect(hoverRect, hoverFill);
        }
    }

    if (hasPosition && bestReferencePrice > 0.0 && positionRow >= 0) {
        const QRect pnlRect(0, positionRow * m_rowHeight, priceLeft + 1, m_rowHeight);
            QFont pnlFont = p.font();
            pnlFont.setBold(true);
            p.setFont(pnlFont);
            QString pnlText = QStringLiteral("%1%2%3")
                                  .arg(positionPnl >= 0.0 ? QStringLiteral("+") : QString())
                                  .arg(QString::number(std::abs(positionPnl), 'f', std::abs(positionPnl) >= 1.0 ? 2 : 4))
                                  .arg(QStringLiteral("$"));
            p.setPen(positionPnlColor);
            p.drawText(pnlRect.adjusted(16, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, pnlText);
            const int arrowSize = 12;
            const int centerY = pnlRect.center().y();
            QPolygon arrow;
            if (positionPnl >= 0.0) {
                arrow << QPoint(6, centerY + arrowSize / 2)
                      << QPoint(12, centerY + arrowSize / 2)
                      << QPoint(9, centerY - arrowSize / 2);
            } else {
                arrow << QPoint(6, centerY - arrowSize / 2)
                      << QPoint(12, centerY - arrowSize / 2)
                      << QPoint(9, centerY + arrowSize / 2);
            }
            p.setBrush(positionPnlColor);
            p.setPen(Qt::NoPen);
            p.drawPolygon(arrow);
            p.setPen(m_style.text);
            p.setFont(font());
    }

    m_exitButtonRect = QRect();
    if (m_infoAreaHeight > 0 && hasPosition) {
        const QRect infoRect(0, height() - m_infoAreaHeight, w, m_infoAreaHeight);
        // 3-column position panel (rounded, no percent).
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);

        const int pad = 6;
        const int spacing = 6;
        QRect content = infoRect.adjusted(pad, pad, -pad, -pad);
        const int cellW = std::max(1, (content.width() - spacing * 2) / 3);
        const int cellH = std::max(1, content.height());
        const int radius = 8;

        const bool isLong = (m_position.side == OrderSide::Buy);
        const double entry = m_position.averagePrice;
        const double bestPrice = bestReferencePrice > 0.0 ? bestReferencePrice : entry;
        const double qty = m_position.quantity;
        const double entryNotionalUsd = std::abs(entry * qty);
        const double signedNotional = isLong ? entryNotionalUsd : -entryNotionalUsd;
        const double pnl = (bestPrice > 0.0)
                               ? (isLong ? (bestPrice - entry) * qty : (entry - bestPrice) * qty)
                               : 0.0;
        const bool pnlPos = pnl >= 0.0;

        auto fmtTrim = [](double v, int decimals) -> QString {
            if (!std::isfinite(v)) return QStringLiteral("0");
            const int dec = std::clamp(decimals, 0, 2);
            QString s = QString::number(v, 'f', dec);
            while (s.contains(QLatin1Char('.')) && s.endsWith(QLatin1Char('0'))) s.chop(1);
            if (s.endsWith(QLatin1Char('.'))) s.chop(1);
            if (s == QStringLiteral("-0")) s = QStringLiteral("0");
            return s;
        };
        const int pricePrec = std::clamp(precisionForTick(m_snapshot.tickSize), 0, 2);
        const QString entryText = fmtTrim(entry, pricePrec);
        const QString valueText = fmtTrim(signedNotional, 2) + QStringLiteral("$");
        const QString pnlText = fmtTrim(pnl, 2) + QStringLiteral("$");

        const QColor leftBg("#0b0b0b");
        const QColor midBg = isLong ? QColor("#1e5b38") : QColor("#6a1b1b");
        const QColor rightBg = pnlPos ? QColor("#1e5b38") : QColor("#6a1b1b");
        const QColor leftBorder("#1a1a1a");
        const QColor midBorder = isLong ? QColor("#2f6c37") : QColor("#992626");
        const QColor rightBorder = pnlPos ? QColor("#2f6c37") : QColor("#992626");

        auto drawCell = [&](int idx, const QColor &bg, const QColor &border, const QString &text) {
            const int x = content.left() + idx * (cellW + spacing);
            QRect r(x, content.top(), cellW, cellH);
            QColor fill = bg;
            fill.setAlpha(235);
            QColor br = border;
            br.setAlpha(255);
            p.setPen(QPen(br, 1));
            p.setBrush(fill);
            p.drawRoundedRect(r, radius, radius);

            QFont f = font();
            f.setBold(true);
            f.setPointSize(std::max(10, font().pointSize() + 2));
            p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(r, Qt::AlignCenter, text);
        };

        drawCell(0, leftBg, leftBorder, entryText);
        drawCell(1, midBg, midBorder, valueText);
        drawCell(2, rightBg, rightBorder, pnlText);

        p.restore();
    }
}

void DomWidget::mousePressEvent(QMouseEvent *event)
{
    if (!m_snapshot.levels.isEmpty()) {
        const int rows = m_snapshot.levels.size();
        const int y = event->pos().y();
        const int ladderHeight = rows * m_rowHeight;
        const bool hasPosition = m_position.hasPosition && m_position.quantity > 0.0 && m_position.averagePrice > 0.0;
        if (m_infoAreaHeight > 0 && hasPosition && y >= ladderHeight) {
            const int buttonW = 56;
            const int rightMargin = 8;
            const int buttonH = std::max(14, m_infoAreaHeight - 6);
            const int buttonX = width() - rightMargin - buttonW;
            const int buttonY = ladderHeight + (m_infoAreaHeight - buttonH) / 2;
            const QRect exitRect(buttonX, buttonY, buttonW, buttonH);
            if (exitRect.contains(event->pos())) {
                emit exitPositionRequested();
                event->accept();
                return;
            }
        } else if (m_infoAreaHeight > 0 && y >= ladderHeight) {
            // In the QWidget-painted path we track the exit rect; keep compatibility if it exists.
            if (!m_exitButtonRect.isNull() && m_exitButtonRect.contains(event->pos())) {
                emit exitPositionRequested();
                event->accept();
                return;
            }
        }
        if (y >= 0 && y < ladderHeight) {
            int row = y / m_rowHeight;
            row = std::clamp(row, 0, rows - 1);
            const DomLevel &lvl = m_snapshot.levels[row];
            emit rowClicked(event->button(), row, lvl.price, lvl.bidQty, lvl.askQty);
        }
    }
    QWidget::mousePressEvent(event);
}

QSize DomWidget::sizeHint() const
{
    return QSize(180, 420 + m_infoAreaHeight);
}

QSize DomWidget::minimumSizeHint() const
{
    return QSize(120, 240 + m_infoAreaHeight);
}

void DomWidget::resizeEvent(QResizeEvent *event)
{
    if (m_quickWidget) {
        m_quickWidget->setGeometry(rect());
    }
    updateActionOverlayGeometry();
    QWidget::resizeEvent(event);
}

void DomWidget::ensureActionOverlayParent()
{
    QWidget *candidate = nullptr;
    QVariant ptr = property("domContainerPtr");
    if (ptr.isValid()) {
        candidate = reinterpret_cast<QWidget *>(ptr.value<quintptr>());
    }
    if (!candidate) {
        candidate = parentWidget();
    }
    if (!candidate) {
        candidate = this;
    }
    if (m_actionOverlayParent == candidate && m_actionOverlayWidget) {
        return;
    }
    m_actionOverlayParent = candidate;
    if (!m_actionOverlayWidget) {
        m_actionOverlayWidget = new QWidget(m_actionOverlayParent);
        m_actionOverlayWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_actionOverlayWidget->setVisible(false);
        m_actionOverlayWidget->setStyleSheet(QStringLiteral(
            "QWidget {"
            "  background: transparent;"
            "  border: none;"
            "}"));
        auto *overlayLayout = new QHBoxLayout(m_actionOverlayWidget);
        overlayLayout->setContentsMargins(0, 0, 0, 0);
        overlayLayout->setSpacing(0);
        m_actionOverlayLabel = new QLabel(m_actionOverlayWidget);
        m_actionOverlayLabel->setAlignment(Qt::AlignCenter);
        m_actionOverlayLabel->setStyleSheet(
            QStringLiteral(
                "QLabel {"
                "  color: #f2f6fb;"
                "  font-weight: 700;"
                "  font-size: 11px;"
                "  letter-spacing: 0.2px;"
                "  background-color: rgba(14,16,22,220);"
                "  border: 1px solid rgba(122,170,255,140);"
                "  border-radius: 8px;"
                "  padding: 2px 6px;"
                "}"));
        m_actionOverlayLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        overlayLayout->addWidget(m_actionOverlayLabel, 1);
        m_actionOverlayWidget->raise();
    } else {
        m_actionOverlayWidget->setParent(m_actionOverlayParent);
    }
    updateActionOverlayGeometry();
}

void DomWidget::updateActionOverlayGeometry()
{
    if (!m_actionOverlayWidget || !m_actionOverlayWidget->isVisible()) {
        return;
    }
    QWidget *parent = m_actionOverlayParent ? m_actionOverlayParent : this;
    const int w = parent->width();
    const int h = parent->height();
    const QSize hint = m_actionOverlayWidget->sizeHint();
    const int overlayH = std::max(hint.height(), m_rowHeight + 6);
    const int overlayW = std::clamp(hint.width() + 28, 80, std::max(80, w - 28));
    const int x = std::max(4, (w - overlayW) / 2);
    // Place just above the footer/info area, below lot bubbles.
    const int bottomMargin = std::max(4, m_infoAreaHeight - 2);
    const int y = std::clamp(h - bottomMargin - overlayH + 4, 4, std::max(4, h - overlayH - 2));
    m_actionOverlayWidget->setGeometry(x, y, overlayW, overlayH);
    m_actionOverlayWidget->raise();
}
void DomWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_snapshot.levels.isEmpty()) {
        const int rows = m_snapshot.levels.size();
        const int y = event->pos().y();
        const int ladderHeight = rows * m_rowHeight;
        int row = -1;
        if (y >= 0 && y < ladderHeight) {
            row = std::clamp(y / m_rowHeight, 0, rows - 1);
        }

        if (row != m_hoverRow) {
            m_hoverRow = row;
            updateHoverInfo(row);
            update();
            if (row >= 0) {
                const DomLevel &lvl = m_snapshot.levels[row];
                emit rowHovered(row, lvl.price, lvl.bidQty, lvl.askQty);
            } else {
                emit rowHovered(-1, 0.0, 0.0, 0.0);
            }
        }
    }

    QWidget::mouseMoveEvent(event);
}

void DomWidget::leaveEvent(QEvent *event)
{
    if (m_hoverRow != -1) {
        m_hoverRow = -1;
        updateHoverInfo(-1);
        update();
        emit rowHovered(-1, 0.0, 0.0, 0.0);
    }
    QWidget::leaveEvent(event);
}

void DomWidget::updateHoverInfo(int row)
{
    if (row < 0 || row >= m_snapshot.levels.size()) {
        m_hoverInfoText.clear();
        emit hoverInfoChanged(-1, 0.0, QString());
        if (m_quickWidget && m_quickReady) {
            if (auto *root = m_quickWidget->rootObject()) {
                root->setProperty("hoverRow", -1);
                root->setProperty("hoverText", QString());
            }
        }
        return;
    }

    const DomLevel &lvl = m_snapshot.levels[row];
    const double notional = std::max(lvl.bidQty, lvl.askQty) * std::abs(lvl.price);
    const double cumulative = cumulativeNotionalForRow(row);
    double pct = 0.0;
    QString percentText;
    if (percentFromReference(lvl.price, m_snapshot.bestBid, m_snapshot.bestAsk, pct)) {
        percentText = QString::number(pct, 'f', std::abs(pct) >= 0.1 ? 2 : 3) + QLatin1String("%");
    } else {
        percentText = QStringLiteral("-");
    }

    QStringList parts;
    parts << percentText;
    if (cumulative > 0.0) {
        parts << formatValueShort(cumulative);
    } else if (notional > 0.0) {
        parts << formatValueShort(notional);
    }
    m_hoverInfoText = parts.join(QStringLiteral(" | "));
    emit hoverInfoChanged(row, lvl.price, m_hoverInfoText);
    if (m_quickWidget && m_quickReady) {
        if (auto *root = m_quickWidget->rootObject()) {
            root->setProperty("hoverRow", m_hoverRow);
            root->setProperty("hoverText", m_hoverInfoText);
        }
    }
}

double DomWidget::cumulativeNotionalForRow(int row) const
{
    if (row < 0 || row >= m_snapshot.levels.size()) {
        return 0.0;
    }
    if (m_snapshot.bestBid <= 0.0 && m_snapshot.bestAsk <= 0.0) {
        return 0.0;
    }

    const DomLevel &target = m_snapshot.levels[row];
    const double tol = priceTolerance(m_snapshot.tickSize);
    const double targetPrice = target.price;
    double total = 0.0;

    if (m_snapshot.bestBid > 0.0 && targetPrice <= m_snapshot.bestBid + tol) {
        const double lower = std::min(targetPrice, m_snapshot.bestBid);
        const double upper = std::max(targetPrice, m_snapshot.bestBid);
        for (const DomLevel &lvl : m_snapshot.levels) {
            if (lvl.bidQty <= 0.0) {
                continue;
            }
            if (lvl.price >= lower - tol && lvl.price <= upper + tol) {
                total += lvl.bidQty * std::abs(lvl.price);
            }
        }
        return total;
    }

    if (m_snapshot.bestAsk > 0.0 && targetPrice >= m_snapshot.bestAsk - tol) {
        const double lower = std::min(targetPrice, m_snapshot.bestAsk);
        const double upper = std::max(targetPrice, m_snapshot.bestAsk);
        for (const DomLevel &lvl : m_snapshot.levels) {
            if (lvl.askQty <= 0.0) {
                continue;
            }
            if (lvl.price >= lower - tol && lvl.price <= upper + tol) {
                total += lvl.askQty * std::abs(lvl.price);
            }
        }
        return total;
    }

    return 0.0;
}

void DomWidget::setVolumeHighlightRules(const QVector<VolumeHighlightRule> &rules)
{
    m_volumeRules = rules;
    std::sort(m_volumeRules.begin(), m_volumeRules.end(), [](const VolumeHighlightRule &a, const VolumeHighlightRule &b) {
        return a.threshold < b.threshold;
    });
    m_forceFullRecalc = true;
    if (m_quickWidget && m_quickReady) {
        scheduleQuickSnapshotUpdate();
    }
    update();
}

void DomWidget::setLocalOrders(const QVector<LocalOrderMarker> &orders)
{
    m_localOrders = orders;
    m_forceFullRecalc = true;
    if (m_quickWidget && m_quickReady) {
        scheduleQuickSnapshotUpdate();
    }
    update();
}

void DomWidget::setHighlightPrices(const QVector<double> &prices)
{
    m_highlightPrices = prices;
    m_forceFullRecalc = true;
    if (m_quickWidget && m_quickReady) {
        scheduleQuickSnapshotUpdate();
    }
    update();
}

void DomWidget::setPriceTextMarkers(const QVector<PriceTextMarker> &markers)
{
    m_priceTextMarkers = markers;
    m_forceFullRecalc = true;
    if (m_quickWidget && m_quickReady) {
        scheduleQuickSnapshotUpdate();
    }
    update();
}

void DomWidget::notifyBucketTicksUpdated(const QVector<qint64> &bucketTicks)
{
    if (bucketTicks.isEmpty()) {
        return;
    }
    if (bucketTicks.size() > 4096) {
        m_dirtyBucketTicks.clear();
        m_forceFullRecalc = true;
        return;
    }
    for (qint64 t : bucketTicks) {
        if (t != 0) {
            m_dirtyBucketTicks.insert(t);
        }
    }
}

void DomWidget::setActionOverlayText(const QString &text)
{
    const QString next = text.trimmed();
    if (m_actionOverlayText == next) {
        return;
    }
    m_actionOverlayText = next;
    updateQuickOverlayProperties();
    ensureActionOverlayParent();
    if (m_actionOverlayLabel && m_actionOverlayWidget) {
        m_actionOverlayLabel->setText(m_actionOverlayText);
        m_actionOverlayWidget->setVisible(!m_actionOverlayText.isEmpty());
        if (m_actionOverlayWidget->isVisible()) {
            updateActionOverlayGeometry();
            m_actionOverlayWidget->raise();
        }
    }
    update();
}

void DomWidget::handleRowClick(int row, int button, double price, double bidQty, double askQty)
{
    emit rowClicked(static_cast<Qt::MouseButton>(button), row, price, bidQty, askQty);
}

void DomWidget::handleRowClickIndex(int row, int button)
{
    if (row < 0 || row >= m_snapshot.levels.size()) {
        return;
    }
    const auto &lvl = m_snapshot.levels[row];
    emit rowClicked(static_cast<Qt::MouseButton>(button), row, lvl.price, lvl.bidQty, lvl.askQty);
}

void DomWidget::handleRowHover(int row)
{
    if (row == m_hoverRow) {
        return;
    }
    m_hoverRow = row;
    updateHoverInfo(row);
    if (row >= 0 && row < m_snapshot.levels.size()) {
        const auto &lvl = m_snapshot.levels[row];
        emit rowHovered(row, lvl.price, lvl.bidQty, lvl.askQty);
    } else {
        emit rowHovered(-1, 0.0, 0.0, 0.0);
    }
}

int DomWidget::rowForPrice(double price) const
{
    if (m_snapshot.levels.isEmpty()) {
        return -1;
    }
    int closest = 0;
    double bestDist = std::numeric_limits<double>::max();
    for (int i = 0; i < m_snapshot.levels.size(); ++i) {
        const double dist = std::abs(m_snapshot.levels[i].price - price);
        if (dist < bestDist) {
            bestDist = dist;
            closest = i;
        }
    }
    return closest;
}

void DomWidget::ensureQuickInitialized()
{
    if (m_quickWidget) {
        return;
    }
    m_quickWidget = new QQuickWidget(this);
    m_quickWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_quickWidget->setClearColor(Qt::transparent);
    // Make mouse handling consistent: compute hover/click rows in C++ using widget coordinates.
    // QML MouseAreas can drift by 1 row due to ListView contentY/pixel rounding differences.
    m_quickWidget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    connect(m_quickWidget,
            &QQuickWidget::statusChanged,
            this,
            [this](QQuickWidget::Status status) {
                m_quickReady = (status == QQuickWidget::Ready);
                if (m_quickReady) {
                    syncQuickProperties();
                    updateQuickSnapshot();
                }
            });
    m_quickWidget->setSource(QUrl(QStringLiteral("qrc:/qml/GpuDomView.qml")));
    m_quickWidget->setGeometry(rect());
}

void DomWidget::updateQuickOverlayProperties()
{
    if (!m_quickWidget || !m_quickReady) {
        return;
    }
    if (auto *root = m_quickWidget->rootObject()) {
        root->setProperty("tickSize", m_snapshot.tickSize);
        // Disable QML overlay to avoid double background; widget overlay handles SL/TP hint.
        root->setProperty("actionOverlayText", QString());
        root->setProperty("actionOverlayVisible", false);

        const bool hasPosition = m_position.hasPosition && m_position.quantity > 0.0 && m_position.averagePrice > 0.0;
        const double entryPrice = hasPosition ? m_position.averagePrice : 0.0;
        const double markPrice = hasPosition
                                     ? ((m_position.side == OrderSide::Buy) ? m_snapshot.bestBid : m_snapshot.bestAsk)
                                     : 0.0;
        const bool active = hasPosition && entryPrice > 0.0 && markPrice > 0.0;
        root->setProperty("positionActive", active);
        root->setProperty("positionEntryPrice", entryPrice);
        root->setProperty("positionMarkPrice", markPrice);

        QColor pnlColor("#ffffff");
        if (active) {
            const double bestPrice = markPrice > 0.0 ? markPrice : entryPrice;
            const double unrealized =
                (bestPrice > 0.0)
                    ? ((m_position.side == OrderSide::Buy)
                           ? (bestPrice - entryPrice) * m_position.quantity
                           : (entryPrice - bestPrice) * m_position.quantity)
                    : 0.0;
            pnlColor = unrealized >= 0.0 ? QColor("#4caf50") : QColor("#e53935");
        }
        root->setProperty("positionPnlColor", pnlColor);

        const bool visible = hasPosition && m_infoAreaHeight > 0;
        root->setProperty("positionVisible", visible);
        root->setProperty("infoAreaHeight", m_infoAreaHeight);
        if (!visible) {
            root->setProperty("positionIsLong", true);
            root->setProperty("positionPnlPositive", true);
            root->setProperty("positionAvgText", QString());
            root->setProperty("positionValueText", QString());
            root->setProperty("positionPctText", QString());
            root->setProperty("positionPnlText", QString());
            return;
        }

        const double bestPrice = markPrice > 0.0 ? markPrice : entryPrice;
        const double unrealized =
            (bestPrice > 0.0)
                ? ((m_position.side == OrderSide::Buy)
                       ? (bestPrice - entryPrice) * m_position.quantity
                       : (entryPrice - bestPrice) * m_position.quantity)
                : 0.0;
        const double totalPnl = unrealized;
        const bool isLong = (m_position.side == OrderSide::Buy);
        const double entryNotionalUsd = std::abs(entryPrice * m_position.quantity);

        auto fmtTrim = [](double v, int decimals) -> QString {
            if (!std::isfinite(v)) return QStringLiteral("0");
            const int dec = std::clamp(decimals, 0, 2);
            QString s = QString::number(v, 'f', dec);
            while (s.contains(QLatin1Char('.')) && s.endsWith(QLatin1Char('0'))) {
                s.chop(1);
            }
            if (s.endsWith(QLatin1Char('.'))) {
                s.chop(1);
            }
            if (s == QStringLiteral("-0")) {
                s = QStringLiteral("0");
            }
            return s;
        };

        const int tickPrec = precisionForTick(m_snapshot.tickSize);
        const int pricePrec = std::clamp(tickPrec, 0, 2);

        const QString entryText = fmtTrim(entryPrice, pricePrec);
        const double signedNotional = isLong ? entryNotionalUsd : -entryNotionalUsd;
        const QString valueText = fmtTrim(signedNotional, 2) + QStringLiteral("$");
        const QString pnlText = fmtTrim(totalPnl, 2) + QStringLiteral("$");

        root->setProperty("positionIsLong", isLong);
        root->setProperty("positionPnlPositive", totalPnl >= 0.0);
        root->setProperty("positionAvgText", entryText);
        root->setProperty("positionValueText", valueText);
        root->setProperty("positionPctText", QString());
        root->setProperty("positionPnlText", pnlText);
    }
}

void DomWidget::syncQuickProperties()
{
    if (!m_quickWidget) {
        return;
    }
    if (auto *root = m_quickWidget->rootObject()) {
        root->setProperty("rowHeight", m_rowHeight);
        root->setProperty("hoverRow", m_hoverRow);
        root->setProperty("useGpuDom", m_useGpuDom);
        root->setProperty("backgroundColor", m_style.background);
        root->setProperty("gridColor", m_style.grid);
        root->setProperty("textColor", m_style.text);
        root->setProperty("bidColor", m_style.bid);
        root->setProperty("askColor", m_style.ask);
        root->setProperty("priceFontFamily", font().family());
        root->setProperty("priceFontPixelSize", font().pixelSize() > 0 ? font().pixelSize()
                                                                       : font().pointSize() + 2);
        root->setProperty("domBridge", QVariant::fromValue(static_cast<QObject *>(this)));
        root->setProperty("levelsModel", QVariant::fromValue(static_cast<QObject *>(&m_levelsModel)));
        updateQuickOverlayProperties();
        resolveGpuDomItem();
    }
}

void DomWidget::resolveGpuDomItem()
{
    if (!m_useGpuDom || !m_quickWidget || !m_quickReady) {
        m_gpuDomItem = nullptr;
        return;
    }
    if (m_gpuDomItem) {
        return;
    }
    if (auto *root = m_quickWidget->rootObject()) {
        if (QObject *obj = root->findChild<QObject *>(QStringLiteral("gpuDom"))) {
            m_gpuDomItem = qobject_cast<GpuDomItem *>(obj);
        }
    }
}

void DomWidget::scheduleQuickSnapshotUpdate()
{
    if (!m_quickAllowed || !m_quickWidget || !m_quickReady) {
        return;
    }
    if (m_quickSnapshotScheduled) {
        return;
    }
    m_quickSnapshotScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_quickSnapshotScheduled = false;
        m_snapshotThrottle.invalidate();
        updateQuickSnapshot();
    });
}

void DomWidget::updateQuickSnapshot()
{
    if (!m_quickAllowed) {
        return;
    }
    if (!m_quickWidget || !m_quickReady) {
        return;
    }
    resolveGpuDomItem();
    if (auto *root = m_quickWidget->rootObject()) {
        static const bool profile = qEnvironmentVariableIntValue("FUSION_PROFILE_DOM") > 0;
        static const int profileThresholdMs = []() {
            const int v = qEnvironmentVariableIntValue("FUSION_PROFILE_DOM_MS");
            return v > 0 ? v : 4;
        }();
        QElapsedTimer prof;
        if (profile) {
            prof.start();
        }

        if (m_snapshotThrottle.isValid()) {
            const qint64 minNs = m_useGpuDom ? 16000000 : kMinSnapshotIntervalNs; // ~60Hz for GPU DOM, ~120Hz otherwise
            if (m_snapshotThrottle.nsecsElapsed() < minNs) {
                return;
            }
            m_snapshotThrottle.restart();
        } else {
            m_snapshotThrottle.start();
        }
        const auto &levels = m_snapshot.levels;
        const int rowsCount = levels.size();
        if (rowsCount <= 0) {
            m_levelsModel.setRows(QVector<DomLevelsModel::Row>());
            if (auto *rootObj = m_quickWidget->rootObject()) {
                if (m_qmlHasOrderHighlights) {
                    m_qmlHasOrderHighlights = false;
                    rootObj->setProperty("hasOrderHighlights", false);
                }
            }
            m_orderHighlightCount = 0;
            m_cachedLayoutValid = false;
            m_cachedLevelTickSize = 0.0;
            return;
        }

        const double bestPriceTolerance = priceTolerance(m_snapshot.tickSize);
        const double tick = m_snapshot.tickSize > 0.0 ? m_snapshot.tickSize : 0.0;
        const BestBucketTicks bestTicks = bestBucketTicksForSnapshot(m_snapshot);
        const qint64 compression = std::max<qint64>(1, m_snapshot.compression);

        // Recompute price column width only when font/tick/width changes.
        const int w = std::max(1, width());
        const int fontPx = (font().pixelSize() > 0) ? font().pixelSize() : (font().pointSize() + 2);
        const QString fontKey = font().family() + QLatin1Char('#') + QString::number(fontPx);
        const bool priceNeedsRecalc =
            (m_cachedWidthForPrice != w)
            || (m_cachedTickForPrice <= 0.0 || std::abs(m_cachedTickForPrice - tick) > 1e-12)
            || (m_cachedFontKeyForPrice != fontKey);
        if (priceNeedsRecalc) {
            QFontMetrics fm(font());
            int maxPriceWidth = 0;
            const QString tinySample = QStringLiteral("(5)12345");
            maxPriceWidth = std::max(maxPriceWidth, fm.horizontalAdvance(tinySample));

            auto updateMaxWidthForLevel = [&](const DomLevel &lvl) {
                const QString text =
                    (lvl.tick != 0) ? formatPriceForDisplayFromTick(lvl.tick, tick)
                                    : formatPriceForDisplay(lvl.price, tick);
                maxPriceWidth = std::max(maxPriceWidth, fm.horizontalAdvance(text));
            };
            if (m_snapshot.bestBid > 0.0) {
                maxPriceWidth =
                    std::max(maxPriceWidth,
                             fm.horizontalAdvance(formatPriceForDisplay(m_snapshot.bestBid, tick)));
            }
            if (m_snapshot.bestAsk > 0.0) {
                maxPriceWidth =
                    std::max(maxPriceWidth,
                             fm.horizontalAdvance(formatPriceForDisplay(m_snapshot.bestAsk, tick)));
            }
            updateMaxWidthForLevel(levels.front());
            updateMaxWidthForLevel(levels.back());
            updateMaxWidthForLevel(levels[rowsCount / 2]);
            const int priceColWidth = std::clamp(maxPriceWidth + 8, 52, std::max(60, w / 3));
            if (priceColWidth != m_cachedPriceColumnWidth) {
                root->setProperty("priceColumnWidth", priceColWidth);
                m_cachedPriceColumnWidth = priceColWidth;
            }
            m_cachedWidthForPrice = w;
            m_cachedFontKeyForPrice = fontKey;
            m_cachedTickForPrice = tick;
        }

        const bool tickSizeStable = (m_cachedLevelTickSize > 0.0 && std::abs(m_cachedLevelTickSize - tick) <= 1e-12);
        const bool layoutStable =
            !m_disableIncremental
            && tickSizeStable
            && m_cachedLayoutValid
            && (m_cachedLayoutRowsCount == rowsCount)
            && (m_cachedLayoutCompression == compression)
            && (m_cachedLayoutMinTick == m_snapshot.minTick)
            && (m_cachedLayoutMaxTick == m_snapshot.maxTick);

        auto computeRow = [&](const DomLevel &lvl, const DomLevelsModel::Row *reuse) -> DomLevelsModel::Row {
            DomLevelsModel::Row row;
            double bidQty = lvl.bidQty;
            double askQty = lvl.askQty;
            // Extra safety: never render liquidity on the wrong side of the spread buckets.
            const bool lockedSpread =
                bestTicks.hasBid && bestTicks.hasAsk && (bestTicks.bidBucketTick == bestTicks.askBucketTick);
            if (bestTicks.hasBid && lvl.tick != 0) {
                if (lockedSpread) {
                    if (lvl.tick < bestTicks.bidBucketTick) {
                        askQty = 0.0;
                    }
                } else if (lvl.tick <= bestTicks.bidBucketTick) {
                    askQty = 0.0;
                }
            }
            if (bestTicks.hasAsk && lvl.tick != 0) {
                if (lockedSpread) {
                    if (lvl.tick > bestTicks.askBucketTick) {
                        bidQty = 0.0;
                    }
                } else if (lvl.tick >= bestTicks.askBucketTick) {
                    bidQty = 0.0;
                }
            }
            if (!lockedSpread && bestTicks.hasBid && bestTicks.hasAsk && lvl.tick != 0
                && lvl.tick > bestTicks.bidBucketTick && lvl.tick < bestTicks.askBucketTick) {
                bidQty = 0.0;
                askQty = 0.0;
            }
            const bool hasBid = bidQty > 0.0;
            const bool hasAsk = askQty > 0.0;
            // In GPU mode we rely on incremental bucket updates. During volatility a top-of-book
            // change can briefly be out-of-sync with the per-level deltas. If we use a fuzzy
            // price tolerance here, multiple adjacent rows can be considered "best" and appear
            // as duplicated bids/asks until a full rebuild.
            const bool isBestBidRow = bestTicks.hasBid && (lvl.tick != 0 && lvl.tick == bestTicks.bidBucketTick);
            const bool isBestAskRow = bestTicks.hasAsk && (lvl.tick != 0 && lvl.tick == bestTicks.askBucketTick);

            QColor rowColor = m_style.background;
            if (hasBid || hasAsk) {
                rowColor = hasAsk && (!hasBid || askQty >= bidQty) ? m_style.ask : m_style.bid;
            }
            QColor bookColor = Qt::transparent;
            QColor priceColor = Qt::transparent;
            if (rowColor != m_style.background) {
                bookColor = rowColor;
                bookColor.setAlpha((isBestBidRow || isBestAskRow) ? 150 : 60);
                priceColor = rowColor;
                priceColor.setAlpha((isBestBidRow || isBestAskRow) ? 120 : 40);
            }

            double dominantQty = 0.0;
            bool volumeIsBid = false;
            if (bidQty > 0.0) {
                dominantQty = bidQty;
                volumeIsBid = true;
            }
            if (askQty > dominantQty) {
                dominantQty = askQty;
                volumeIsBid = false;
            }
            const double notional = dominantQty * std::abs(lvl.price);
            QString qtyText;
            double volumeRatio = 0.0;
            QColor volumeFillColor(Qt::transparent);
            QColor volumeTextColor = volumeIsBid ? m_style.bid : m_style.ask;
            volumeTextColor.setAlpha(220);
            if (notional > 0.0) {
                qtyText = formatQty(notional);
                int matchedIndex = -1;
                double rangeMin = 0.0;
                double rangeMax = 0.0;
                for (int idx = 0; idx < m_volumeRules.size(); ++idx) {
                    if (notional >= m_volumeRules[idx].threshold) {
                        matchedIndex = idx;
                    } else {
                        break;
                    }
                }
                if (matchedIndex >= 0) {
                    const VolumeHighlightRule &matched = m_volumeRules[matchedIndex];
                    rangeMin = matched.threshold;
                    if (matchedIndex + 1 < m_volumeRules.size()) {
                        rangeMax = m_volumeRules[matchedIndex + 1].threshold;
                    } else {
                        rangeMax = rangeMin;
                    }

                    QColor bg = matched.color.isValid() ? matched.color : QColor("#ffd54f");
                    bg.setAlpha(220);
                    volumeFillColor = bg;
                    QColor textColor = bg.lightness() < 120 ? QColor("#f0f0f0") : QColor("#1e1e1e");
                    volumeTextColor = textColor;

                    if (rangeMax > rangeMin) {
                        volumeRatio =
                            std::clamp((notional - rangeMin) / (rangeMax - rangeMin), 0.0, 1.0);
                    } else {
                        volumeRatio = 1.0;
                    }
                } else {
                    volumeRatio = 0.0;
                }
            }

            if (!m_priceTextMarkers.isEmpty()) {
                for (const auto &marker : m_priceTextMarkers) {
                    if (!(marker.price > 0.0)) {
                        continue;
                    }
                    if (std::abs(lvl.price - marker.price) > bestPriceTolerance) {
                        continue;
                    }
                    const QString t = marker.text.trimmed();
                    if (t.isEmpty()) {
                        continue;
                    }
                    qtyText = t;
                    volumeRatio = 1.0;
                    QColor fill = marker.fillColor.isValid() ? marker.fillColor : QColor("#ffffff");
                    if (fill.isValid() && fill.alpha() == 255) {
                        fill.setAlpha(220);
                    }
                    volumeFillColor = fill;
                    volumeTextColor = marker.textColor.isValid() ? marker.textColor : QColor("#ffffff");
                    break;
                }
            }

            row.price = lvl.price;
            if (reuse && !reuse->priceText.isEmpty() && tickSizeStable) {
                row.priceText = reuse->priceText;
            } else {
                row.priceText =
                    (lvl.tick != 0) ? formatPriceForDisplayFromTick(lvl.tick, tick)
                                    : formatPriceForDisplay(lvl.price, tick);
            }
            row.bidQty = bidQty;
            row.askQty = askQty;
            row.bookColor = bookColor;
            row.priceBgColor = priceColor;
            row.volumeText = qtyText;
            row.volumeTextColor = volumeTextColor;
            row.volumeFillRatio = volumeRatio;
            row.volumeFillColor = volumeFillColor;

            if (!m_cachedMarkersByTick.isEmpty()) {
                const auto it = m_cachedMarkersByTick.constFind(lvl.tick);
                if (it != m_cachedMarkersByTick.constEnd() && it->notional > 0.0) {
                    row.markerText = formatQty(it->notional);
                    row.markerBuy = it->buy;
                    QColor fill = it->buy ? m_style.bid : m_style.ask;
                    fill.setAlpha(240);
                    QColor border = fill;
                    border = it->buy ? border.darker(150) : border.darker(170);
                    border.setAlpha(240);
                    row.markerFillColor = fill;
                    row.markerBorderColor = border;
                }
            }
            row.orderHighlight = (!row.markerText.isEmpty()) || (!m_cachedHighlightTicks.isEmpty() && m_cachedHighlightTicks.contains(lvl.tick));

            return row;
        };

        auto rebuildCachedMarkersIfNeeded = [&]() {
            if (tick <= 0.0 || compression <= 0) {
                m_cachedMarkersByTick.clear();
                m_cachedMarkersOrdersHash = 0;
                m_cachedMarkersCompression = compression;
                m_cachedMarkersTickSize = tick;
                return;
            }
            int h = 0;
            for (const auto &m : m_localOrders) {
                h ^= qHash(m.orderId);
                h ^= qHash(static_cast<qint64>(std::llround(m.price * 100000.0)));
                h ^= qHash(static_cast<qint64>(std::llround(m.quantity * 100000.0)));
                h ^= qHash(static_cast<int>(m.side));
            }
            if (h == m_cachedMarkersOrdersHash
                && m_cachedMarkersCompression == compression
                && (m_cachedMarkersTickSize > 0.0 && std::abs(m_cachedMarkersTickSize - tick) <= 1e-12)) {
                return;
            }
            m_cachedMarkersOrdersHash = h;
            m_cachedMarkersCompression = compression;
            m_cachedMarkersTickSize = tick;
            m_cachedMarkersByTick.clear();
            if (m_localOrders.isEmpty()) {
                return;
            }
            m_cachedMarkersByTick.reserve(m_localOrders.size());
            for (const auto &m : m_localOrders) {
                if (!(m.price > 0.0) || !(m.quantity > 0.0)) {
                    continue;
                }
                qint64 rawTick = 0;
                if (!priceToTick(m.price, tick, rawTick)) {
                    continue;
                }
                const bool buy = (m.side == OrderSide::Buy);
                const qint64 bucketTick =
                    buy ? floorBucketTick(rawTick, compression) : ceilBucketTick(rawTick, compression);
                MarkerAgg agg = m_cachedMarkersByTick.value(bucketTick, MarkerAgg{});
                agg.notional += std::abs(m.quantity);
                agg.buy = buy;
                if (agg.createdMs == 0) {
                    agg.createdMs = m.createdMs;
                } else if (m.createdMs > 0) {
                    agg.createdMs = std::min<qint64>(agg.createdMs, m.createdMs);
                }
                m_cachedMarkersByTick.insert(bucketTick, agg);
            }
        };

        auto rebuildCachedHighlightsIfNeeded = [&]() {
            if (tick <= 0.0 || compression <= 0) {
                m_cachedHighlightTicks.clear();
                m_cachedHighlightHash = 0;
                m_cachedHighlightCompression = compression;
                m_cachedHighlightTickSize = tick;
                return;
            }
            int h = 0;
            for (double p : m_highlightPrices) {
                h ^= qHash(static_cast<qint64>(std::llround(p * 100000.0)));
            }
            if (h == m_cachedHighlightHash
                && m_cachedHighlightCompression == compression
                && (m_cachedHighlightTickSize > 0.0 && std::abs(m_cachedHighlightTickSize - tick) <= 1e-12)) {
                return;
            }
            m_cachedHighlightHash = h;
            m_cachedHighlightCompression = compression;
            m_cachedHighlightTickSize = tick;
            m_cachedHighlightTicks.clear();
            if (m_highlightPrices.isEmpty()) {
                return;
            }
            m_cachedHighlightTicks.reserve(m_highlightPrices.size() * 2);
            for (const double p : m_highlightPrices) {
                if (!(p > 0.0)) continue;
                qint64 rawTick = 0;
                if (!priceToTick(p, tick, rawTick)) continue;
                m_cachedHighlightTicks.insert(floorBucketTick(rawTick, compression));
                m_cachedHighlightTicks.insert(ceilBucketTick(rawTick, compression));
            }
        };

        auto tickToIndex = [&](qint64 tickKey) -> int {
            if (!m_cachedLayoutValid || compression <= 0 || rowsCount <= 0) {
                return -1;
            }
            if (tickKey < m_snapshot.minTick || tickKey > m_snapshot.maxTick) {
                return -1;
            }
            const qint64 delta = m_snapshot.maxTick - tickKey;
            if (delta < 0) {
                return -1;
            }
            if ((delta % compression) != 0) {
                return -1;
            }
            const qint64 idx = delta / compression;
            if (idx < 0 || idx >= rowsCount) {
                return -1;
            }
            return static_cast<int>(idx);
        };

        if (!layoutStable) {
            rebuildCachedMarkersIfNeeded();
            rebuildCachedHighlightsIfNeeded();
            QVector<DomLevelsModel::Row> rows;
            rows.reserve(rowsCount);
            int highlightCount = 0;
            for (int i = 0; i < rowsCount; ++i) {
                DomLevelsModel::Row row = computeRow(levels[i], nullptr);
                if (row.orderHighlight) {
                    ++highlightCount;
                }
                rows.append(std::move(row));
            }
            m_orderHighlightCount = highlightCount;
            {
                const bool has = highlightCount > 0;
                if (has != m_qmlHasOrderHighlights) {
                    m_qmlHasOrderHighlights = has;
                    root->setProperty("hasOrderHighlights", has);
                }
            }
            m_cachedLevelTickSize = tick;
            m_cachedLayoutMinTick = m_snapshot.minTick;
            m_cachedLayoutMaxTick = m_snapshot.maxTick;
            m_cachedLayoutCompression = compression;
            m_cachedLayoutRowsCount = rowsCount;
            m_cachedLayoutValid = true;
            m_dirtyBucketTicks.clear();
            m_forceFullRecalc = false;
            if (m_gpuDomItem) {
                m_gpuDomItem->setRows(rows);
            }
            m_levelsModel.setRows(std::move(rows));
            if (profile) {
                const int elapsed = static_cast<int>(prof.elapsed());
                if (elapsed >= profileThresholdMs) {
                    qWarning() << "[DomWidget] full rebuild" << rowsCount << "rows in" << elapsed << "ms";
                }
            }
            return;
        }

        if (!m_forceFullRecalc && m_dirtyBucketTicks.isEmpty()) {
            if (profile) {
                const int elapsed = static_cast<int>(prof.elapsed());
                if (elapsed >= profileThresholdMs) {
                    qWarning() << "[DomWidget] noop frame took" << elapsed << "ms";
                }
            }
            return;
        }

        rebuildCachedMarkersIfNeeded();
        rebuildCachedHighlightsIfNeeded();

        QVector<int> changedIndices;
        QVector<DomLevelsModel::Row> changedRows;
        changedIndices.reserve(64);
        changedRows.reserve(64);
        int highlightDelta = 0;
        if (m_forceFullRecalc) {
            for (int i = 0; i < rowsCount; ++i) {
                const DomLevelsModel::Row &oldRow = m_levelsModel.rowAt(i);
                const DomLevelsModel::Row next = computeRow(levels[i], &oldRow);
                if (next != oldRow) {
                    if (oldRow.orderHighlight != next.orderHighlight) {
                        highlightDelta += next.orderHighlight ? 1 : -1;
                    }
                    changedIndices.push_back(i);
                    changedRows.push_back(next);
                }
            }
        } else {
            for (qint64 tickKey : std::as_const(m_dirtyBucketTicks)) {
                const int i = tickToIndex(tickKey);
                if (i < 0 || i >= rowsCount) {
                    continue;
                }
                const DomLevelsModel::Row &oldRow = m_levelsModel.rowAt(i);
                const DomLevelsModel::Row next = computeRow(levels[i], &oldRow);
                if (next != oldRow) {
                    if (oldRow.orderHighlight != next.orderHighlight) {
                        highlightDelta += next.orderHighlight ? 1 : -1;
                    }
                    changedIndices.push_back(i);
                    changedRows.push_back(next);
                }
            }
        }
        const int dirtyCount = m_dirtyBucketTicks.size();
        m_dirtyBucketTicks.clear();
        m_forceFullRecalc = false;
        if (highlightDelta != 0) {
            m_orderHighlightCount = std::max(0, m_orderHighlightCount + highlightDelta);
            const bool has = m_orderHighlightCount > 0;
            if (has != m_qmlHasOrderHighlights) {
                m_qmlHasOrderHighlights = has;
                root->setProperty("hasOrderHighlights", has);
            }
        }
        m_levelsModel.updateRows(changedIndices, changedRows);
        if (m_gpuDomItem) {
            m_gpuDomItem->updateRows(changedIndices, changedRows);
        }
        if (profile) {
            const int elapsed = static_cast<int>(prof.elapsed());
            if (elapsed >= profileThresholdMs) {
                qWarning() << "[DomWidget] update rowsChanged=" << changedIndices.size()
                           << "dirty=" << dirtyCount << "in" << elapsed << "ms";
            }
        }
    }
}
