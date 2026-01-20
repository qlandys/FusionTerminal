#include "GpuDomItem.h"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

namespace {
struct Color8 {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    uchar a = 0;
};

static inline uchar premul(uchar v, uchar a)
{
    return static_cast<uchar>((static_cast<unsigned int>(v) * static_cast<unsigned int>(a) + 127u) / 255u);
}

static inline Color8 toColor8(const QColor &c)
{
    if (!c.isValid()) {
        return {};
    }
    const QColor p = c.toRgb();
    Color8 out;
    const uchar a = static_cast<uchar>(std::clamp(p.alpha(), 0, 255));
    out.a = a;
    out.r = premul(static_cast<uchar>(std::clamp(p.red(), 0, 255)), a);
    out.g = premul(static_cast<uchar>(std::clamp(p.green(), 0, 255)), a);
    out.b = premul(static_cast<uchar>(std::clamp(p.blue(), 0, 255)), a);
    return out;
}

static inline void addRect(QSGGeometry::ColoredPoint2D *v,
                           int &idx,
                           float x,
                           float y,
                           float w,
                           float h,
                           const Color8 &color)
{
    const float x2 = x + w;
    const float y2 = y + h;

    auto setPoint = [&](int at, float px, float py) {
        v[at].x = px;
        v[at].y = py;
        v[at].r = color.r;
        v[at].g = color.g;
        v[at].b = color.b;
        v[at].a = color.a;
    };

    setPoint(idx++, x, y);
    setPoint(idx++, x2, y);
    setPoint(idx++, x, y2);

    setPoint(idx++, x2, y);
    setPoint(idx++, x2, y2);
    setPoint(idx++, x, y2);
}

static inline float clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
} // namespace

GpuDomItem::GpuDomItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void GpuDomItem::setRowHeight(int v)
{
    const int next = std::max(1, v);
    if (m_rowHeight == next) {
        return;
    }
    m_rowHeight = next;
    emit rowHeightChanged();
    update();
}

void GpuDomItem::setPriceColumnWidth(int v)
{
    const int next = std::max(1, v);
    if (m_priceColumnWidth == next) {
        return;
    }
    m_priceColumnWidth = next;
    emit priceColumnWidthChanged();
    update();
}

void GpuDomItem::setBackgroundColor(const QColor &c)
{
    if (m_backgroundColor == c) {
        return;
    }
    m_backgroundColor = c;
    emit backgroundColorChanged();
    update();
}

void GpuDomItem::setGridColor(const QColor &c)
{
    if (m_gridColor == c) {
        return;
    }
    m_gridColor = c;
    emit gridColorChanged();
    update();
}

void GpuDomItem::setHoverColor(const QColor &c)
{
    if (m_hoverColor == c) {
        return;
    }
    m_hoverColor = c;
    emit hoverColorChanged();
    update();
}

void GpuDomItem::setPriceBorderColor(const QColor &c)
{
    if (m_priceBorderColor == c) {
        return;
    }
    m_priceBorderColor = c;
    emit priceBorderColorChanged();
    update();
}

void GpuDomItem::setHoverRow(int v)
{
    if (m_hoverRow == v) {
        return;
    }
    m_hoverRow = v;
    emit hoverRowChanged();
    update();
}

void GpuDomItem::setOrderHighlightPhase(qreal v)
{
    if (qFuzzyCompare(1.0 + m_orderHighlightPhase, 1.0 + v)) {
        return;
    }
    m_orderHighlightPhase = v;
    emit orderHighlightPhaseChanged();
    update();
}

void GpuDomItem::setPositionActive(bool v)
{
    if (m_positionActive == v) {
        return;
    }
    m_positionActive = v;
    emit positionActiveChanged();
    update();
}

void GpuDomItem::setPositionEntryPrice(qreal v)
{
    if (qFuzzyCompare(1.0 + m_positionEntryPrice, 1.0 + v)) {
        return;
    }
    m_positionEntryPrice = v;
    emit positionEntryPriceChanged();
    update();
}

void GpuDomItem::setPositionMarkPrice(qreal v)
{
    if (qFuzzyCompare(1.0 + m_positionMarkPrice, 1.0 + v)) {
        return;
    }
    m_positionMarkPrice = v;
    emit positionMarkPriceChanged();
    update();
}

void GpuDomItem::setTickSize(qreal v)
{
    if (qFuzzyCompare(1.0 + m_tickSize, 1.0 + v)) {
        return;
    }
    m_tickSize = v;
    emit tickSizeChanged();
    update();
}

void GpuDomItem::setPositionPnlColor(const QColor &c)
{
    if (m_positionPnlColor == c) {
        return;
    }
    m_positionPnlColor = c;
    emit positionPnlColorChanged();
    update();
}

void GpuDomItem::setRows(const QVector<DomLevelsModel::Row> &rows)
{
    m_rows = rows;
    update();
}

void GpuDomItem::updateRows(const QVector<int> &indices, const QVector<DomLevelsModel::Row> &rows)
{
    if (indices.size() != rows.size()) {
        return;
    }
    const int n = std::min(indices.size(), rows.size());
    for (int i = 0; i < n; ++i) {
        const int idx = indices[i];
        if (idx < 0 || idx >= m_rows.size()) {
            continue;
        }
        m_rows[idx] = rows[i];
    }
    update();
}

QSGNode *GpuDomItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(oldNode);
    if (!node) {
        node = new QSGGeometryNode();
        node->setFlag(QSGNode::OwnedByParent, true);

        auto *material = new QSGVertexColorMaterial();
        material->setFlag(QSGMaterial::Blending, true);
        node->setMaterial(material);
        node->setFlag(QSGNode::OwnsMaterial, true);
    }

    // Take a snapshot of the data for this frame. This avoids any lifetime issues if the GUI thread
    // updates the row buffer while the scenegraph is syncing.
    const QVector<DomLevelsModel::Row> rows = m_rows;
    const int rowsCount = rows.size();
    const float w = std::max(1.0f, static_cast<float>(width()));
    const float h = std::max(1.0f, static_cast<float>(height()));
    const float priceW = std::clamp(static_cast<float>(m_priceColumnWidth), 1.0f, w);
    const float bookW = std::max(0.0f, w - priceW);
    const float rowH = std::max(1.0f, static_cast<float>(m_rowHeight));

    const bool posActive =
        m_positionActive && (m_positionEntryPrice > 0.0) && (m_positionMarkPrice > 0.0) && (m_tickSize > 0.0);
    const double tol = std::max(1e-8, m_tickSize > 0.0 ? m_tickSize * 0.25 : 1e-8);
    const double rangeMin = std::min(m_positionEntryPrice, m_positionMarkPrice);
    const double rangeMax = std::max(m_positionEntryPrice, m_positionMarkPrice);

    const QColor gridLine = [&]() {
        QColor c = m_gridColor;
        if (c.isValid()) {
            c.setAlphaF(c.alphaF() * 0.4);
        }
        return c;
    }();
    const Color8 bg = toColor8(m_backgroundColor);
    const Color8 border = toColor8(m_priceBorderColor);
    const Color8 hover = toColor8(QColor(m_hoverColor.red(),
                                         m_hoverColor.green(),
                                         m_hoverColor.blue(),
                                         static_cast<int>(m_hoverColor.alphaF() * 0.25 * 255.0)));
    const Color8 grid = toColor8(gridLine);

    const float phase = clamp01(static_cast<float>(m_orderHighlightPhase));
    const float pulse = 0.5f - 0.5f * std::cos(phase * 6.283185307179586f);
    const float glowA = 0.020f + pulse * 0.090f;

    QColor pnl = m_positionPnlColor;
    pnl.setAlphaF(pnl.alphaF() * 0.12);
    const Color8 pnlFill = toColor8(pnl);
    const Color8 pnlMark = toColor8(m_positionPnlColor);

    // Worst-case rectangles per row:
    // bookBg, volFill, priceBg, gridLine, hover, orderGlow, priceSep, priceRight, pnlFill, pnlMark.
    const int perRow = 12; // keep some slack for safety
    const int rowsDrawn = std::clamp(static_cast<int>(std::ceil(h / rowH)) + 2, 0, rowsCount);
    const int rectCount = 1 + rowsDrawn * perRow; // + background
    const int vertexCount = rectCount * 6;

    auto *geom = node->geometry();
    if (!geom) {
        geom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geom->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geom);
        node->setFlag(QSGNode::OwnsGeometry, true);
    }
    if (geom->vertexCount() != vertexCount) {
        geom->allocate(vertexCount);
        geom->setDrawingMode(QSGGeometry::DrawTriangles);
    }

    auto *v = static_cast<QSGGeometry::ColoredPoint2D *>(geom->vertexData());
    int vi = 0;

    addRect(v, vi, 0, 0, w, h, bg);

    const int capacity = geom->vertexCount();
    for (int i = 0; i < rowsCount; ++i) {
        const float y = static_cast<float>(i) * rowH;
        if (y > h) {
            break;
        }
        const auto &r = rows[i];

        auto safeAddRect = [&](float rx, float ry, float rw, float rh, const Color8 &c) {
            if (vi + 6 > capacity) {
                return false;
            }
            addRect(v, vi, rx, ry, rw, rh, c);
            return true;
        };

        const Color8 bookColor = toColor8(r.bookColor);
        if (bookColor.a != 0 && bookW > 0.0f) {
            if (!safeAddRect(0, y, bookW, rowH, bookColor)) break;
        }
        if (r.volumeFillRatio > 0.0 && bookW > 0.0f) {
            const float vw = bookW * clamp01(static_cast<float>(r.volumeFillRatio));
            const Color8 fill = toColor8(r.volumeFillColor);
            if (fill.a != 0 && vw > 0.5f) {
                if (!safeAddRect(0, y, vw, rowH, fill)) break;
            }
        }
        const Color8 priceBg = toColor8(r.priceBgColor);
        if (priceBg.a != 0) {
            if (!safeAddRect(w - priceW, y, priceW, rowH, priceBg)) break;
        }

        if (posActive && r.price > 0.0) {
            const bool inRange = (r.price + tol >= rangeMin) && (r.price - tol <= rangeMax);
            if (inRange) {
                if (!safeAddRect(w - priceW, y, priceW, rowH, pnlFill)) break;
            }
            const bool isMarkRow = std::abs(r.price - m_positionMarkPrice) <= tol;
            if (isMarkRow) {
                if (!safeAddRect(w - priceW, y, 2.0f, rowH, pnlMark)) break;
            }
        }

        if (r.orderHighlight) {
            const bool isTopOfBookRow =
                (r.bookColor.isValid() && r.bookColor.alpha() >= 100)
                || (r.priceBgColor.isValid() && r.priceBgColor.alpha() >= 80);
            QColor base = r.markerFillColor.isValid() && r.markerFillColor.alpha() > 0
                              ? r.markerFillColor
                              : (r.bookColor.isValid() && r.bookColor.alpha() > 0 ? r.bookColor : QColor("#ffffff"));
            base = base.toRgb();
            // Make top-of-book (best bid/ask) order highlights noticeably blink.
            // Regular levels keep a subtle glow so the UI doesn't get noisy.
            const float a = isTopOfBookRow ? (0.06f + pulse * 0.22f) : glowA;
            base.setAlphaF(std::clamp(a, 0.0f, 0.35f));
            const Color8 glow = toColor8(base);
            if (!safeAddRect(0, y, w, rowH, glow)) break;
        }

        if (m_hoverRow == i) {
            if (!safeAddRect(0, y, w, rowH, hover)) break;
        }

        // Separators
        if (!safeAddRect(bookW, y, 1.0f, rowH, border)) break;
        if (!safeAddRect(w - 1.0f, y, 1.0f, rowH, border)) break;

        // Grid line (bottom)
        if (!safeAddRect(0, y + rowH - 1.0f, w, 1.0f, grid)) break;
    }

    geom->setVertexCount(vi);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
