#include "GpuTrailItem.h"

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
} // namespace

GpuTrailItem::GpuTrailItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

QObject *GpuTrailItem::circlesModel() const
{
    return m_model.data();
}

void GpuTrailItem::setCirclesModel(QObject *obj)
{
    auto *m = qobject_cast<QAbstractItemModel *>(obj);
    if (m_model == m) return;
    m_model = m;
    resolveRoles();
    reconnectModel();
    emit circlesModelChanged();
    update();
}

void GpuTrailItem::setSidePadding(int v)
{
    const int next = std::max(0, v);
    if (m_sidePadding == next) return;
    m_sidePadding = next;
    emit sidePaddingChanged();
    update();
}

void GpuTrailItem::setMaxSegments(int v)
{
    const int next = std::clamp(v, 0, 4096);
    if (m_maxSegments == next) return;
    m_maxSegments = next;
    emit maxSegmentsChanged();
    update();
}

void GpuTrailItem::setColor(const QColor &c)
{
    if (m_color == c) return;
    m_color = c;
    emit colorChanged();
    update();
}

void GpuTrailItem::setOpacity(qreal v)
{
    const qreal next = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(1.0 + m_opacity, 1.0 + next)) return;
    m_opacity = next;
    emit opacityChanged();
    update();
}

void GpuTrailItem::setLineWidth(qreal v)
{
    const qreal next = std::clamp(v, 0.1, 8.0);
    if (qFuzzyCompare(1.0 + m_lineWidth, 1.0 + next)) return;
    m_lineWidth = next;
    emit lineWidthChanged();
    update();
}

void GpuTrailItem::reconnectModel()
{
    if (!m_model) return;
    connect(m_model, &QAbstractItemModel::modelReset, this, &GpuTrailItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::layoutChanged, this, &GpuTrailItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &GpuTrailItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &GpuTrailItem::update, Qt::UniqueConnection);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &GpuTrailItem::update, Qt::UniqueConnection);
}

void GpuTrailItem::resolveRoles()
{
    m_roleXRatio = -1;
    m_roleY = -1;
    if (!m_model) return;
    const auto names = m_model->roleNames();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        const QByteArray n = it.value();
        if (n == "xRatio") m_roleXRatio = it.key();
        else if (n == "y") m_roleY = it.key();
    }
}

float GpuTrailItem::xForRatio(float ratio) const
{
    const float w = std::max(1.0f, static_cast<float>(width()));
    const float usable = std::max(1.0f, w - static_cast<float>(m_sidePadding) * 2.0f);
    const float r = std::clamp(ratio, 0.0f, 1.0f);
    return static_cast<float>(m_sidePadding) + r * usable;
}

QSGNode *GpuTrailItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
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
    if (!m_model || m_roleXRatio < 0 || m_roleY < 0 || m_maxSegments <= 0) {
        geom->allocate(0);
        geom->setVertexCount(0);
        node->markDirty(QSGNode::DirtyGeometry);
        return node;
    }

    QColor c = m_color;
    c.setAlphaF(std::clamp(c.alphaF() * m_opacity, 0.0, 1.0));
    const Color8 col = toColor8(c);

    const int rows = m_model->rowCount();
    const int segments = std::min(m_maxSegments, std::max(0, rows - 1));
    const int vtx = segments * 6;
    if (geom->vertexCount() != vtx) {
        geom->allocate(vtx);
    }
    auto *v = static_cast<QSGGeometry::ColoredPoint2D *>(geom->vertexData());
    int idx = 0;

    const float halfW = std::max(0.25f, static_cast<float>(m_lineWidth) * 0.5f);

    auto setPoint = [&](int at, float px, float py) {
        v[at].x = px;
        v[at].y = py;
        v[at].r = col.r;
        v[at].g = col.g;
        v[at].b = col.b;
        v[at].a = col.a;
    };

    for (int i = 0; i < segments; ++i) {
        const QModelIndex ia = m_model->index(i, 0);
        const QModelIndex ib = m_model->index(i + 1, 0);
        const float ra = static_cast<float>(m_model->data(ia, m_roleXRatio).toDouble());
        const float rb = static_cast<float>(m_model->data(ib, m_roleXRatio).toDouble());
        const float ya = static_cast<float>(m_model->data(ia, m_roleY).toDouble());
        const float yb = static_cast<float>(m_model->data(ib, m_roleY).toDouble());

        const float x1 = xForRatio(ra);
        const float y1 = ya;
        const float x2 = xForRatio(rb);
        const float y2 = yb;

        const float dx = x2 - x1;
        const float dy = y2 - y1;
        const float d2 = dx * dx + dy * dy;
        if (d2 < 0.25f) {
            continue;
        }
        const float len = std::sqrt(std::max(1e-6f, d2));
        const float nx = -dy / len;
        const float ny = dx / len;
        const float ox = nx * halfW;
        const float oy = ny * halfW;

        const float ax = x1 + ox;
        const float ay = y1 + oy;
        const float bx = x1 - ox;
        const float by = y1 - oy;
        const float cx = x2 + ox;
        const float cy = y2 + oy;
        const float dx2p = x2 - ox;
        const float dy2p = y2 - oy;

        // Two triangles: (a,b,c) and (c,b,d)
        setPoint(idx++, ax, ay);
        setPoint(idx++, bx, by);
        setPoint(idx++, cx, cy);
        setPoint(idx++, cx, cy);
        setPoint(idx++, bx, by);
        setPoint(idx++, dx2p, dy2p);
    }

    geom->setVertexCount(idx);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}
