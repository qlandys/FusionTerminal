#include "GpuGridItem.h"

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
    if (!c.isValid()) return {};
    const QColor p = c.toRgb();
    const uchar a = static_cast<uchar>(p.alpha());
    return {premul(static_cast<uchar>(p.red()), a),
            premul(static_cast<uchar>(p.green()), a),
            premul(static_cast<uchar>(p.blue()), a),
            a};
}

static inline void addRect(QSGGeometry::ColoredPoint2D *v,
                           int &idx,
                           float x,
                           float y,
                           float w,
                           float h,
                           const Color8 &c)
{
    const float x2 = x + w;
    const float y2 = y + h;
    auto setPoint = [&](int at, float px, float py) {
        v[at].x = px;
        v[at].y = py;
        v[at].r = c.r;
        v[at].g = c.g;
        v[at].b = c.b;
        v[at].a = c.a;
    };
    setPoint(idx++, x, y);
    setPoint(idx++, x2, y);
    setPoint(idx++, x, y2);
    setPoint(idx++, x2, y);
    setPoint(idx++, x2, y2);
    setPoint(idx++, x, y2);
}
} // namespace

GpuGridItem::GpuGridItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void GpuGridItem::setVertical(bool v)
{
    if (m_vertical == v) return;
    m_vertical = v;
    emit verticalChanged();
    update();
}

void GpuGridItem::setCount(int v)
{
    const int next = std::max(0, v);
    if (m_count == next) return;
    m_count = next;
    emit countChanged();
    update();
}

void GpuGridItem::setStep(int v)
{
    const int next = std::max(1, v);
    if (m_step == next) return;
    m_step = next;
    emit stepChanged();
    update();
}

void GpuGridItem::setColor(const QColor &c)
{
    if (m_color == c) return;
    m_color = c;
    emit colorChanged();
    update();
}

void GpuGridItem::setOpacity(qreal v)
{
    const qreal next = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(1.0 + m_opacity, 1.0 + next)) return;
    m_opacity = next;
    emit opacityChanged();
    update();
}

QSGNode *GpuGridItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGGeometryNode *>(oldNode);
    if (!node) {
        node = new QSGGeometryNode();
        node->setFlag(QSGNode::OwnedByParent, true);
        auto *mat = new QSGVertexColorMaterial();
        mat->setFlag(QSGMaterial::Blending, true);
        node->setMaterial(mat);
        node->setFlag(QSGNode::OwnsMaterial, true);
        auto *geom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        geom->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geom);
        node->setFlag(QSGNode::OwnsGeometry, true);
    }

    auto *geom = node->geometry();
    const float w = std::max(1.0f, static_cast<float>(width()));
    const float h = std::max(1.0f, static_cast<float>(height()));
    const int step = std::max(1, m_step);
    const int count = std::max(0, m_count);

    QColor c = m_color;
    c.setAlphaF(std::clamp(c.alphaF() * m_opacity, 0.0, 1.0));
    const Color8 col = toColor8(c);

    const int lineCount = count + 1;
    const int rects = lineCount;
    const int vtx = rects * 6;
    if (geom->vertexCount() != vtx) {
        geom->allocate(vtx);
    }
    auto *v = static_cast<QSGGeometry::ColoredPoint2D *>(geom->vertexData());
    int idx = 0;
    if (m_vertical) {
        for (int i = 0; i <= count; ++i) {
            const float x = std::min(w - 1.0f, static_cast<float>(i * step) + 0.5f);
            addRect(v, idx, x, 0.0f, 1.0f, h, col);
        }
    } else {
        for (int i = 0; i <= count; ++i) {
            const float y = std::min(h - 1.0f, static_cast<float>(i * step) + 0.5f);
            addRect(v, idx, 0.0f, y, w, 1.0f, col);
        }
    }
    geom->setVertexCount(idx);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
