#include "GpuMarkerItem.h"

#include <QSGGeometryNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
struct Color8 {
    uchar r = 0;
    uchar g = 0;
    uchar b = 0;
    uchar a = 0;
};

struct Pt {
    float x = 0;
    float y = 0;
};

static inline Color8 toColor8(QColor c)
{
    if (!c.isValid()) return {};
    c = c.toRgb();
    const uchar a = static_cast<uchar>(std::clamp(c.alpha(), 0, 255));
    auto premul = [](uchar v, uchar aa) -> uchar {
        return static_cast<uchar>((static_cast<unsigned int>(v) * static_cast<unsigned int>(aa) + 127u) / 255u);
    };
    const uchar r = premul(static_cast<uchar>(std::clamp(c.red(), 0, 255)), a);
    const uchar g = premul(static_cast<uchar>(std::clamp(c.green(), 0, 255)), a);
    const uchar b = premul(static_cast<uchar>(std::clamp(c.blue(), 0, 255)), a);
    return {r, g, b, a};
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

static inline std::vector<Pt> markerPoly(float x, float y, float w, float h, float tipLen)
{
    // Keep the tip compact (the QML layout already reserves only ~tipSize px for it).
    tipLen = std::clamp(tipLen, 4.0f, std::max(4.0f, w * 0.45f));
    const float xr = x + w;
    const float yr = y + h;
    const float midY = y + h * 0.5f;
    const float tipX = xr;
    const float baseX = xr - tipLen;
    return {{x, y}, {baseX, y}, {tipX, midY}, {baseX, yr}, {x, yr}};
}

static inline float cross(const Pt &a, const Pt &b, const Pt &c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Sutherland–Hodgman clip of convex polygon `subject` against convex polygon `clip`.
static std::vector<Pt> clipConvex(const std::vector<Pt> &subject, const std::vector<Pt> &clip)
{
    std::vector<Pt> output = subject;
    if (output.empty() || clip.size() < 3) return {};

    // Clip polygon assumed CCW.
    for (size_t i = 0; i < clip.size(); ++i) {
        const Pt a = clip[i];
        const Pt b = clip[(i + 1) % clip.size()];
        std::vector<Pt> input = output;
        output.clear();
        if (input.empty()) break;

        auto inside = [&](const Pt &p) {
            return cross(a, b, p) >= 0.0f;
        };
        auto intersect = [&](const Pt &p1, const Pt &p2) -> Pt {
            // Line AB with line segment P1P2
            const float A1 = b.y - a.y;
            const float B1 = a.x - b.x;
            const float C1 = A1 * a.x + B1 * a.y;
            const float A2 = p2.y - p1.y;
            const float B2 = p1.x - p2.x;
            const float C2 = A2 * p1.x + B2 * p1.y;
            const float det = A1 * B2 - A2 * B1;
            if (std::abs(det) < 1e-6f) {
                return p2;
            }
            const float x = (B2 * C1 - B1 * C2) / det;
            const float y = (A1 * C2 - A2 * C1) / det;
            return {x, y};
        };

        Pt S = input.back();
        for (const Pt &E : input) {
            const bool Ein = inside(E);
            const bool Sin = inside(S);
            if (Ein) {
                if (!Sin) {
                    output.push_back(intersect(S, E));
                }
                output.push_back(E);
            } else if (Sin) {
                output.push_back(intersect(S, E));
            }
            S = E;
        }
    }
    return output;
}

static inline int fanTriCount(int points)
{
    return points >= 3 ? (points - 2) : 0;
}
} // namespace

GpuMarkerItem::GpuMarkerItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void GpuMarkerItem::setFillColor(const QColor &c)
{
    if (m_fillColor == c) return;
    m_fillColor = c;
    emit fillColorChanged();
    update();
}

void GpuMarkerItem::setBorderColor(const QColor &c)
{
    if (m_borderColor == c) return;
    m_borderColor = c;
    emit borderColorChanged();
    update();
}

void GpuMarkerItem::setPhase(qreal v)
{
    const qreal next = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(1.0 + m_phase, 1.0 + next)) return;
    m_phase = next;
    emit phaseChanged();
    update();
}

void GpuMarkerItem::setTipBias(qreal v)
{
    const qreal next = std::clamp(v, -1.0, 1.0);
    if (qFuzzyCompare(1.0 + m_tipBias, 1.0 + next)) return;
    m_tipBias = next;
    emit tipBiasChanged();
    update();
}

QSGNode *GpuMarkerItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *root = static_cast<QSGNode *>(oldNode);
    QSGGeometryNode *baseNode = nullptr;
    QSGGeometryNode *shineNode = nullptr;

    if (!root) {
        root = new QSGNode();
        root->setFlag(QSGNode::OwnedByParent, true);

        baseNode = new QSGGeometryNode();
        shineNode = new QSGGeometryNode();
        baseNode->setFlag(QSGNode::OwnedByParent, true);
        shineNode->setFlag(QSGNode::OwnedByParent, true);

        auto *baseMat = new QSGVertexColorMaterial();
        baseMat->setFlag(QSGMaterial::Blending, true);
        baseNode->setMaterial(baseMat);
        baseNode->setFlag(QSGNode::OwnsMaterial, true);
        auto *baseGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        baseGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        baseNode->setGeometry(baseGeom);
        baseNode->setFlag(QSGNode::OwnsGeometry, true);

        auto *shineMat = new QSGVertexColorMaterial();
        shineMat->setFlag(QSGMaterial::Blending, true);
        shineNode->setMaterial(shineMat);
        shineNode->setFlag(QSGNode::OwnsMaterial, true);
        auto *shineGeom = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
        shineGeom->setDrawingMode(QSGGeometry::DrawTriangles);
        shineNode->setGeometry(shineGeom);
        shineNode->setFlag(QSGNode::OwnsGeometry, true);

        root->appendChildNode(baseNode);
        root->appendChildNode(shineNode);
    } else {
        baseNode = static_cast<QSGGeometryNode *>(root->firstChild());
        shineNode = baseNode ? static_cast<QSGGeometryNode *>(baseNode->nextSibling()) : nullptr;
    }
    if (!baseNode || !shineNode) return root;

    const float w = std::max(1.0f, static_cast<float>(width()));
    const float h = std::max(1.0f, static_cast<float>(height()));

    // Match `GpuPrintsView.qml` layout:
    // `tipSize = min(height/2, 12)` and marker width reserves `+ tipSize`.
    // So the triangle must stay within ~tipSize to avoid "eating" text width.
    const float tipSize = std::min(h * 0.5f, 12.0f);
    const float tipBase = tipSize + static_cast<float>(m_tipBias) * 2.0f;
    const float tipMax = std::min(tipSize, w * 0.45f);
    const float tipLen0 = std::clamp(tipBase, 4.0f, tipMax);
    const float tipLen1 = tipLen0;
    const float tipLen2 = tipLen0;

    const Color8 under = toColor8(QColor::fromRgbF(QColor(Qt::black).redF(),
                                                   QColor(Qt::black).greenF(),
                                                   QColor(Qt::black).blueF(),
                                                   0.0));
    Q_UNUSED(under);

    QColor underC = m_borderColor.isValid() ? m_borderColor.darker(320) : QColor("#000000");
    underC.setAlphaF(0.55);
    const Color8 underCol = toColor8(underC);
    const Color8 borderCol = toColor8(m_borderColor);
    const Color8 fillCol = toColor8(m_fillColor);
    const Color8 leftHi = toColor8(QColor(255, 255, 255, 30));

    // Base geometry: 3 layered polygons + left highlight.
    const int polyTris = 3; // 5-point convex => 3 triangles
    const int layers = 3;
    const int baseVerts = layers * polyTris * 3 + 6; // + left highlight rect
    auto *bg = baseNode->geometry();
    if (bg->vertexCount() != baseVerts) {
        bg->allocate(baseVerts);
        bg->setDrawingMode(QSGGeometry::DrawTriangles);
    }
    auto *bv = static_cast<QSGGeometry::ColoredPoint2D *>(bg->vertexData());
    int bi = 0;

    auto emitPoly = [&](float inset, float tipLen, const Color8 &c) {
        const float x0 = inset;
        const float y0 = inset;
        const float ww = std::max(1.0f, w - inset * 2.0f);
        const float hh = std::max(1.0f, h - inset * 2.0f);
        const auto p = markerPoly(x0, y0, ww, hh, tipLen);
        // Triangulate fan from 0: (0,1,2), (0,2,3), (0,3,4)
        setPoint(bv, bi++, p[0].x, p[0].y, c);
        setPoint(bv, bi++, p[1].x, p[1].y, c);
        setPoint(bv, bi++, p[2].x, p[2].y, c);
        setPoint(bv, bi++, p[0].x, p[0].y, c);
        setPoint(bv, bi++, p[2].x, p[2].y, c);
        setPoint(bv, bi++, p[3].x, p[3].y, c);
        setPoint(bv, bi++, p[0].x, p[0].y, c);
        setPoint(bv, bi++, p[3].x, p[3].y, c);
        setPoint(bv, bi++, p[4].x, p[4].y, c);
    };

    emitPoly(0.0f, tipLen0, underCol);
    emitPoly(1.0f, tipLen1, borderCol);
    emitPoly(2.0f, tipLen2, fillCol);

    // Left highlight edge (2px).
    const float lhW = 2.0f;
    setPoint(bv, bi++, 0.0f, 0.0f, leftHi);
    setPoint(bv, bi++, lhW, 0.0f, leftHi);
    setPoint(bv, bi++, 0.0f, h, leftHi);
    setPoint(bv, bi++, lhW, 0.0f, leftHi);
    setPoint(bv, bi++, lhW, h, leftHi);
    setPoint(bv, bi++, 0.0f, h, leftHi);

    bg->setVertexCount(bi);
    baseNode->markDirty(QSGNode::DirtyGeometry);

    // Shine band: clip rotated rect against the fill polygon.
    auto *sg = shineNode->geometry();

    const float phase = std::clamp(static_cast<float>(m_phase), 0.0f, 1.0f);
    const float bandW = std::max(18.0f, w * 0.18f);
    const float startX = -bandW + (w + bandW) * phase;
    const float angle = -0.35f;

    const float rectW = bandW;
    const float rectH = h * 2.0f;
    const float cx = startX;
    const float cy = 0.0f;

    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const Pt r0{0.0f, 0.0f};
    const Pt r1{rectW, 0.0f};
    const Pt r2{rectW, rectH};
    const Pt r3{0.0f, rectH};
    auto rot = [&](Pt p) -> Pt {
        const float x = p.x * c - p.y * s;
        const float y = p.x * s + p.y * c;
        return {x + cx, y + (cy - h * 0.5f)};
    };
    std::vector<Pt> band = {rot(r0), rot(r1), rot(r2), rot(r3)};

    // Clip against the fill polygon (inset 2).
    const auto clipPoly = markerPoly(2.0f, 2.0f, std::max(1.0f, w - 4.0f), std::max(1.0f, h - 4.0f), tipLen2);
    // Ensure CCW order (markerPoly produces CCW).
    std::vector<Pt> clip = clipPoly;

    std::vector<Pt> clipped = clipConvex(band, clip);
    const int tris = fanTriCount(static_cast<int>(clipped.size()));
    const int shineVerts = tris * 3;
    if (shineVerts <= 0) {
        sg->allocate(0);
        sg->setVertexCount(0);
        shineNode->markDirty(QSGNode::DirtyGeometry);
        return root;
    }
    if (sg->vertexCount() != shineVerts) {
        sg->allocate(shineVerts);
        sg->setDrawingMode(QSGGeometry::DrawTriangles);
    }
    auto *sv = static_cast<QSGGeometry::ColoredPoint2D *>(sg->vertexData());

    QColor shineC(255, 255, 255);
    shineC.setAlphaF(0.16);
    const Color8 shineCol = toColor8(shineC);

    int si = 0;
    for (int t = 0; t < tris; ++t) {
        const Pt &p0 = clipped[0];
        const Pt &p1 = clipped[t + 1];
        const Pt &p2 = clipped[t + 2];
        setPoint(sv, si++, p0.x, p0.y, shineCol);
        setPoint(sv, si++, p1.x, p1.y, shineCol);
        setPoint(sv, si++, p2.x, p2.y, shineCol);
    }
    sg->setVertexCount(si);
    shineNode->markDirty(QSGNode::DirtyGeometry);
    return root;
}
