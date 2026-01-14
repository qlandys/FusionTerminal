#include "GpuCirclesItem.h"

#include <QAbstractItemModel>
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

static inline void setPoint(QSGGeometry::ColoredPoint2D *v, int at, float x, float y, const Color8 &c)
{
    v[at].x = x;
    v[at].y = y;
    v[at].r = c.r;
    v[at].g = c.g;
    v[at].b = c.b;
    v[at].a = c.a;
}

static inline float clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}
} // namespace

GpuCirclesItem::GpuCirclesItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

QObject *GpuCirclesItem::circlesModel() const
{
    return m_model.data();
}

void GpuCirclesItem::setCirclesModel(QObject *obj)
{
    auto *m = qobject_cast<QAbstractItemModel *>(obj);
    if (m_model == m) return;
    m_model = m;
    resolveRoles();
    reconnectModel();
    emit circlesModelChanged();
    update();
}

void GpuCirclesItem::setSidePadding(int v)
{
    const int next = std::max(0, v);
    if (m_sidePadding == next) return;
    m_sidePadding = next;
    emit sidePaddingChanged();
    update();
}

void GpuCirclesItem::setPrintsSquare(bool v)
{
    if (m_printsSquare == v) return;
    m_printsSquare = v;
    emit printsSquareChanged();
    update();
}

void GpuCirclesItem::setMaxCircles(int v)
{
    const int next = std::clamp(v, 0, 4096);
    if (m_maxCircles == next) return;
    m_maxCircles = next;
    emit maxCirclesChanged();
    update();
}

void GpuCirclesItem::setCircleSegments(int v)
{
    const int next = std::clamp(v, 6, 96);
    if (m_circleSegments == next) return;
    m_circleSegments = next;
    emit circleSegmentsChanged();
    update();
}

void GpuCirclesItem::reconnectModel()
{
    if (!m_model) return;
    connect(m_model, &QAbstractItemModel::modelReset, this, &GpuCirclesItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::layoutChanged, this, &GpuCirclesItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &GpuCirclesItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &GpuCirclesItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &GpuCirclesItem::update, Qt::UniqueConnection);
}

void GpuCirclesItem::resolveRoles()
{
    m_roleXRatio = -1;
    m_roleY = -1;
    m_roleRadius = -1;
    m_roleFillColor = -1;
    m_roleBorderColor = -1;
    if (!m_model) return;
    const auto names = m_model->roleNames();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QByteArray n = it.value();
        if (n == "xRatio") m_roleXRatio = it.key();
        else if (n == "y") m_roleY = it.key();
        else if (n == "radius") m_roleRadius = it.key();
        else if (n == "fillColor") m_roleFillColor = it.key();
        else if (n == "borderColor") m_roleBorderColor = it.key();
    }
}

float GpuCirclesItem::xForRatio(float ratio) const
{
    const float w = std::max(1.0f, static_cast<float>(width()));
    const float usable = std::max(1.0f, w - static_cast<float>(m_sidePadding) * 2.0f);
    const float r = clamp01(ratio);
    return static_cast<float>(m_sidePadding) + r * usable;
}

QSGNode *GpuCirclesItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGNode *>(oldNode);
    QSGGeometryNode *borderNode = nullptr;
    QSGGeometryNode *fillNode = nullptr;

    if (!root) {
        root = new QSGNode();
        root->setFlag(QSGNode::OwnedByParent, true);

        borderNode = new QSGGeometryNode();
        fillNode = new QSGGeometryNode();
        borderNode->setFlag(QSGNode::OwnedByParent, true);
        fillNode->setFlag(QSGNode::OwnedByParent, true);

        auto *borderMat = new QSGVertexColorMaterial();
        borderMat->setFlag(QSGMaterial::Blending, true);
        borderNode->setMaterial(borderMat);
        borderNode->setFlag(QSGNode::OwnsMaterial, true);
        auto *borderGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        borderGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        borderNode->setGeometry(borderGeom);
        borderNode->setFlag(QSGNode::OwnsGeometry, true);

        auto *fillMat = new QSGVertexColorMaterial();
        fillMat->setFlag(QSGMaterial::Blending, true);
        fillNode->setMaterial(fillMat);
        fillNode->setFlag(QSGNode::OwnsMaterial, true);
        auto *fillGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        fillGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        fillNode->setGeometry(fillGeom);
        fillNode->setFlag(QSGNode::OwnsGeometry, true);

        root->appendChildNode(borderNode);
        root->appendChildNode(fillNode);
    } else {
        borderNode = static_cast<QSGGeometryNode *>(root->firstChild());
        fillNode = borderNode ? static_cast<QSGGeometryNode *>(borderNode->nextSibling()) : nullptr;
    }

    if (!borderNode || !fillNode) {
        return root;
    }

    auto *borderGeom = borderNode->geometry();
    auto *fillGeom = fillNode->geometry();

    if (!m_model
        || m_roleXRatio < 0
        || m_roleY < 0
        || m_roleRadius < 0
        || m_roleFillColor < 0
        || m_roleBorderColor < 0
        || m_maxCircles <= 0) {
        borderGeom->allocate(0);
        borderGeom->setVertexCount(0);
        fillGeom->allocate(0);
        fillGeom->setVertexCount(0);
        borderNode->markDirty(QSGNode::DirtyGeometry);
        fillNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }

    const int rows = m_model->rowCount();
    const int n = std::min(m_maxCircles, rows);
    const int seg = std::max(6, m_circleSegments);

    // Border:
    // - circle: ring made of 2 triangles per segment (6 vertices)
    // - square: 2 rects (outer border + inner fill) handled in their respective nodes
    const int borderVertsPerCircle = m_printsSquare ? 6 : (seg * 6);
    const int fillVertsPerCircle = m_printsSquare ? 6 : (seg * 3);

    const int borderVtx = n * borderVertsPerCircle;
    const int fillVtx = n * fillVertsPerCircle;
    if (borderGeom->vertexCount() != borderVtx) {
        borderGeom->allocate(borderVtx);
        borderGeom->setDrawingMode(QSGGeometry::DrawTriangles);
    }
    if (fillGeom->vertexCount() != fillVtx) {
        fillGeom->allocate(fillVtx);
        fillGeom->setDrawingMode(QSGGeometry::DrawTriangles);
    }

    auto *bv = static_cast<QSGGeometry::ColoredPoint2D *>(borderGeom->vertexData());
    auto *fv = static_cast<QSGGeometry::ColoredPoint2D *>(fillGeom->vertexData());

    int bi = 0;
    int fi = 0;

    const float borderW = 2.0f;

    for (int i = 0; i < n; ++i) {
        const QModelIndex idx = m_model->index(i, 0);
        const float xRatio = static_cast<float>(m_model->data(idx, m_roleXRatio).toDouble());
        const float cy = static_cast<float>(m_model->data(idx, m_roleY).toDouble());
        const float radius = static_cast<float>(m_model->data(idx, m_roleRadius).toDouble());
        const QColor fillC = m_model->data(idx, m_roleFillColor).value<QColor>();
        const QColor borderC = m_model->data(idx, m_roleBorderColor).value<QColor>();
        const Color8 fill = toColor8(fillC);
        const Color8 border = toColor8(borderC);

        if (radius <= 0.5f) {
            continue;
        }
        const float cx = xForRatio(xRatio);

        if (m_printsSquare) {
            const float w = radius * 2.0f;
            const float h = w;
            const float x = cx - radius;
            const float y = cy - radius;
            // Border rect
            setPoint(bv, bi++, x, y, border);
            setPoint(bv, bi++, x + w, y, border);
            setPoint(bv, bi++, x, y + h, border);
            setPoint(bv, bi++, x + w, y, border);
            setPoint(bv, bi++, x + w, y + h, border);
            setPoint(bv, bi++, x, y + h, border);

            // Fill rect inset
            const float inset = borderW;
            const float w2 = std::max(0.0f, w - inset * 2.0f);
            const float h2 = std::max(0.0f, h - inset * 2.0f);
            const float x2 = x + inset;
            const float y2 = y + inset;
            setPoint(fv, fi++, x2, y2, fill);
            setPoint(fv, fi++, x2 + w2, y2, fill);
            setPoint(fv, fi++, x2, y2 + h2, fill);
            setPoint(fv, fi++, x2 + w2, y2, fill);
            setPoint(fv, fi++, x2 + w2, y2 + h2, fill);
            setPoint(fv, fi++, x2, y2 + h2, fill);
            continue;
        }

        const float rOuter = radius;
        const float rInner = std::max(0.0f, radius - borderW);

        // Fill: triangle fan (center + seg triangles)
        for (int s = 0; s < seg; ++s) {
            const float a0 = (static_cast<float>(s) / static_cast<float>(seg)) * 6.283185307179586f;
            const float a1 = (static_cast<float>(s + 1) / static_cast<float>(seg)) * 6.283185307179586f;
            const float x0 = cx + std::cos(a0) * rInner;
            const float y0 = cy + std::sin(a0) * rInner;
            const float x1 = cx + std::cos(a1) * rInner;
            const float y1 = cy + std::sin(a1) * rInner;
            setPoint(fv, fi++, cx, cy, fill);
            setPoint(fv, fi++, x0, y0, fill);
            setPoint(fv, fi++, x1, y1, fill);
        }

        // Border ring: 2 triangles per segment
        for (int s = 0; s < seg; ++s) {
            const float a0 = (static_cast<float>(s) / static_cast<float>(seg)) * 6.283185307179586f;
            const float a1 = (static_cast<float>(s + 1) / static_cast<float>(seg)) * 6.283185307179586f;
            const float ox0 = cx + std::cos(a0) * rOuter;
            const float oy0 = cy + std::sin(a0) * rOuter;
            const float ox1 = cx + std::cos(a1) * rOuter;
            const float oy1 = cy + std::sin(a1) * rOuter;
            const float ix0 = cx + std::cos(a0) * rInner;
            const float iy0 = cy + std::sin(a0) * rInner;
            const float ix1 = cx + std::cos(a1) * rInner;
            const float iy1 = cy + std::sin(a1) * rInner;

            // (outer0, inner0, outer1)
            setPoint(bv, bi++, ox0, oy0, border);
            setPoint(bv, bi++, ix0, iy0, border);
            setPoint(bv, bi++, ox1, oy1, border);
            // (outer1, inner0, inner1)
            setPoint(bv, bi++, ox1, oy1, border);
            setPoint(bv, bi++, ix0, iy0, border);
            setPoint(bv, bi++, ix1, iy1, border);
        }
    }

    borderGeom->setVertexCount(bi);
    fillGeom->setVertexCount(fi);
    borderNode->markDirty(QSGNode::DirtyGeometry);
    fillNode->markDirty(QSGNode::DirtyGeometry);
    return root;
}
