#include "core/bake/bake.h"

#include "core/bake/mikktspace.h"
#include "core/bsdf/surface.h"
#include "core/integrator/material_eval.h"
#include "core/integrator/settings.h"
#include "core/integrator/wavefront.h"
#include "core/math/frame.h"
#include "core/math/sampling.h"
#include "core/math/sobol.h"
#include "core/scene/ray.h"
#include "core/shading/program.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nova {
namespace {

// The bake's random numbers: one 3D sample per map and pixel sample, in a
// stream block of their own.
constexpr uint32_t kBakeStream = 0x100000u;
enum BakeQuery : uint32_t { qOcclusion, qCavity, qThickness, qCurvature, qEdge, qRounded };

// Sample i of a pixel sample's pattern: the additive recurrence of the
// plastic constant (R3), shifted by the sample's random numbers (as the
// shading queries, core/integrator/shading_queries.cpp).
Vec3f patternPoint(Vec3f rnd, int i) {
    const float x = rnd.x + float(i) * 0.81917251f, y = rnd.y + float(i) * 0.67104361f,
                z = rnd.z + float(i) * 0.54970048f;
    return {x - std::floor(x), y - std::floor(y), z - std::floor(z)};
}

Vec3f normalizeOr(Vec3f v, Vec3f fallback) {
    const float l = length(v);
    return l > 0.0f && std::isfinite(l) ? v * (1.0f / l) : fallback;
}

// The Standard Surface inputs Karma's baker exports (export_<input>), by
// name: the field and its width; base_combined is base * base_color.
struct ExportField {
    const char *name;
    uint16_t field;
    uint8_t width;
};
const ExportField kExports[] = {
    {"base", surface_field::baseWeight, 1},
    {"base_color", surface_field::baseColor, 3},
    {"diffuse_roughness", surface_field::diffuseRoughness, 1},
    {"metalness", surface_field::metalness, 1},
    {"specular", surface_field::specularWeight, 1},
    {"specular_color", surface_field::specularColor, 3},
    {"specular_roughness", surface_field::specularRoughness, 1},
    {"specular_IOR", surface_field::specularIor, 1},
    {"specular_anisotropy", surface_field::specularAnisotropy, 1},
    {"specular_rotation", surface_field::specularRotation, 1},
    {"transmission", surface_field::transmissionWeight, 1},
    {"transmission_color", surface_field::transmissionColor, 3},
    {"transmission_depth", surface_field::transmissionDepth, 1},
    {"transmission_scatter", surface_field::transmissionScatter, 3},
    {"transmission_scatter_anisotropy", surface_field::transmissionScatterAnisotropy, 1},
    {"transmission_dispersion", surface_field::transmissionDispersion, 1},
    {"transmission_extra_roughness", surface_field::transmissionExtraRoughness, 1},
    {"subsurface", surface_field::subsurfaceWeight, 1},
    {"subsurface_color", surface_field::subsurfaceColor, 3},
    {"subsurface_radius", surface_field::subsurfaceRadius, 3},
    {"subsurface_scale", surface_field::subsurfaceScale, 1},
    {"subsurface_anisotropy", surface_field::subsurfaceAnisotropy, 1},
    {"sheen", surface_field::sheenWeight, 1},
    {"sheen_color", surface_field::sheenColor, 3},
    {"sheen_roughness", surface_field::sheenRoughness, 1},
    {"coat", surface_field::coatWeight, 1},
    {"coat_color", surface_field::coatColor, 3},
    {"coat_roughness", surface_field::coatRoughness, 1},
    {"coat_anisotropy", surface_field::coatAnisotropy, 1},
    {"coat_rotation", surface_field::coatRotation, 1},
    {"coat_IOR", surface_field::coatIor, 1},
    {"coat_affect_color", surface_field::coatAffectColor, 1},
    {"coat_affect_roughness", surface_field::coatAffectRoughness, 1},
    {"thin_film_thickness", surface_field::thinFilmThickness, 1},
    {"thin_film_IOR", surface_field::thinFilmIor, 1},
    {"emission", surface_field::emissionWeight, 1},
    {"emission_color", surface_field::emissionColor, 3},
    {"opacity", surface_field::opacity, 3},
    {"thin_walled", surface_field::thinWalled, 1},
    {"coat_normal", surface_field::coatNormal, 3},
    {"normal", surface_field::normal, 3},
    {"tangent", surface_field::tangent, 3},
};

// An export's value at a shading point (3 floats; scalars splat).
void exportValue(const std::string &name, const SurfaceInputs &in, const SurfaceHit &hit, const shading::Program *p,
                 float *o) {
    o[0] = o[1] = o[2] = 0.0f;
    if (name == "base_combined") {
        const Spectrum c = in.c(surface_field::baseColor) * in.f(surface_field::baseWeight);
        o[0] = c.x, o[1] = c.y, o[2] = c.z;
        return;
    }
    for (const ExportField &e : kExports)
        if (name == e.name) {
            if (e.width == 1) {
                o[0] = o[1] = o[2] = in.f(e.field);
            } else {
                Vec3f v{in.v[e.field], in.v[e.field + 1], in.v[e.field + 2]};
                // Unconnected normals are the shading normal.
                if ((e.field == surface_field::normal || e.field == surface_field::coatNormal) && length(v) == 0.0f)
                    v = hit.N;
                o[0] = v.x, o[1] = v.y, o[2] = v.z;
            }
            return;
        }
    if (p) // a material AOV (kma_aov) of that name
        for (const auto &[aov, field] : p->aovs)
            if (aov == name) {
                o[0] = in.v[field], o[1] = in.v[field + 1], o[2] = in.v[field + 2];
                return;
            }
}

} // namespace

// ---------------------------------------------------------------------------
// The bake meshes

void Baker::Surface::index(Vec2f origin) {
    const size_t n = tris.size();
    grid = std::clamp(int(std::sqrt(double(n))), 1, 2048);
    const int g = grid;
    cellStart.assign(size_t(g) * size_t(g) + 1, 0);
    const auto cells = [&](const Tri &t, int &x0, int &y0, int &x1, int &y1) {
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 3; ++k) {
            const float c[2] = {t.uv[k].x - origin.x, t.uv[k].y - origin.y};
            for (int a = 0; a < 2; ++a) {
                lo[a] = std::min(lo[a], c[a]);
                hi[a] = std::max(hi[a], c[a]);
            }
        }
        if (!(hi[0] >= 0.0f && hi[1] >= 0.0f && lo[0] <= 1.0f && lo[1] <= 1.0f))
            return false;
        x0 = std::clamp(int(std::floor(lo[0] * float(g))), 0, g - 1);
        y0 = std::clamp(int(std::floor(lo[1] * float(g))), 0, g - 1);
        x1 = std::clamp(int(std::floor(hi[0] * float(g))), 0, g - 1);
        y1 = std::clamp(int(std::floor(hi[1] * float(g))), 0, g - 1);
        return true;
    };
    for (const Tri &t : tris) {
        int x0, y0, x1, y1;
        if (cells(t, x0, y0, x1, y1))
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    ++cellStart[size_t(y) * size_t(g) + size_t(x) + 1];
    }
    for (size_t c = 0; c + 1 < cellStart.size(); ++c)
        cellStart[c + 1] += cellStart[c];
    cellTris.assign(cellStart.back(), 0);
    std::vector<uint32_t> fill(cellStart.begin(), cellStart.end() - 1);
    for (uint32_t i = 0; i < uint32_t(n); ++i) {
        int x0, y0, x1, y1;
        if (cells(tris[i], x0, y0, x1, y1))
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    cellTris[fill[size_t(y) * size_t(g) + size_t(x)]++] = i;
    }
}

bool Baker::Surface::find(Vec2f st, Vec2f origin, uint32_t &tri, Vec3f &b) const {
    if (grid <= 0 || !(st.x >= 0.0f && st.y >= 0.0f && st.x <= 1.0f && st.y <= 1.0f))
        return false;
    const int g = grid;
    const int cx = std::clamp(int(st.x * float(g)), 0, g - 1), cy = std::clamp(int(st.y * float(g)), 0, g - 1);
    const size_t c = size_t(cy) * size_t(g) + size_t(cx);
    const Vec2f p{st.x + origin.x, st.y + origin.y};
    for (uint32_t k = cellStart[c]; k < cellStart[c + 1]; ++k) {
        const Tri &t = tris[cellTris[k]];
        const float e1x = t.uv[1].x - t.uv[0].x, e1y = t.uv[1].y - t.uv[0].y;
        const float e2x = t.uv[2].x - t.uv[0].x, e2y = t.uv[2].y - t.uv[0].y;
        const float px = p.x - t.uv[0].x, py = p.y - t.uv[0].y;
        const float d = e1x * e2y - e2x * e1y;
        if (std::fabs(d) < 1e-20f)
            continue;
        const float b1 = (px * e2y - e2x * py) / d, b2 = (e1x * py - px * e1y) / d, b0 = 1.0f - b1 - b2;
        constexpr float eps = -1e-6f;
        if (b0 >= eps && b1 >= eps && b2 >= eps) {
            tri = cellTris[k];
            b = {std::max(b0, 0.0f), std::max(b1, 0.0f), std::max(b2, 0.0f)};
            const float s = b.x + b.y + b.z;
            b = b * (1.0f / s);
            return true;
        }
    }
    return false;
}

void Baker::addMesh(const Scene &scene, MeshId id, Surface &out, bool frames) const {
    if (id >= scene.meshCount())
        return;
    const MeshRecord &rec = scene.meshRecord(id);
    const MeshDesc &d = rec.desc;
    if (!rec.alive || d.shape != MeshShape::Triangles || rec.instanceCount() == 0)
        return;
    const MeshAttribute *uv = d.findAttribute(m_settings.uv);
    if (!uv || uv->width < 2 || (uv->rate != AttributeRate::Vertex && uv->rate != AttributeRate::Corner))
        return;
    const MeshAttribute *biasAttr =
        m_settings.traceBiasAttribute.empty() ? nullptr : d.findAttribute(m_settings.traceBiasAttribute);
    const MeshAttribute *maxAttr =
        m_settings.traceMaxAttribute.empty() ? nullptr : d.findAttribute(m_settings.traceMaxAttribute);
    const Affine3f xf = scene.objectToWorld(id, 0, 0.0f);
    const float det = xf.determinant();
    const size_t triCount = d.triangleCount(), pointCount = d.pointCount();
    const bool vertexNormals = d.normals.size() >= pointCount && pointCount > 0;
    const size_t first = out.tris.size();
    const auto attr = [&](const MeshAttribute *a, size_t t, int k, uint32_t vi, float fallback) {
        if (!a || a->data.empty())
            return fallback;
        const size_t i = a->rate == AttributeRate::Corner    ? t * 3 + size_t(k)
                         : a->rate == AttributeRate::Vertex  ? size_t(vi)
                         : a->rate == AttributeRate::Uniform ? t
                                                             : 0;
        return i * a->width < a->data.size() ? a->data[i * a->width] : fallback;
    };
    for (size_t t = 0; t < triCount; ++t) {
        Tri tri;
        tri.mesh = id;
        tri.prim = uint32_t(t);
        uint32_t vi[3];
        for (int k = 0; k < 3; ++k) {
            vi[k] = d.indices[t * 3 + size_t(k)];
            tri.P[k] = xf.point(d.points[vi[k] < pointCount ? vi[k] : 0]);
            const size_t ui = uv->rate == AttributeRate::Corner ? t * 3 + size_t(k) : size_t(vi[k]);
            tri.uv[k] = ui * uv->width + 1 < uv->data.size()
                            ? Vec2f{uv->data[ui * uv->width], uv->data[ui * uv->width + 1]}
                            : Vec2f{0.0f, 0.0f};
            tri.bias[k] = attr(biasAttr, t, k, vi[k], m_settings.traceBias);
            tri.maxT[k] = attr(maxAttr, t, k, vi[k], m_settings.traceMax);
        }
        const Vec3f ng = normalizeOr(cross(tri.P[1] - tri.P[0], tri.P[2] - tri.P[0]), Vec3f{0.0f, 0.0f, 1.0f});
        for (int k = 0; k < 3; ++k) {
            Vec3f n = vertexNormals ? xf.normal(d.normals[vi[k]]) : ng;
            if (vertexNormals && det < 0.0f)
                n = -n;
            tri.N[k] = normalizeOr(n, ng);
        }
        // Texture-coordinate derivatives of the position.
        const Vec3f e1 = tri.P[1] - tri.P[0], e2 = tri.P[2] - tri.P[0];
        const float du1 = tri.uv[1].x - tri.uv[0].x, dv1 = tri.uv[1].y - tri.uv[0].y;
        const float du2 = tri.uv[2].x - tri.uv[0].x, dv2 = tri.uv[2].y - tri.uv[0].y;
        const float dd = du1 * dv2 - du2 * dv1;
        if (std::fabs(dd) > 1e-30f) {
            tri.dPdu = (e1 * dv2 - e2 * dv1) * (1.0f / dd);
            tri.dPdv = (e2 * du1 - e1 * du2) * (1.0f / dd);
        } else {
            tri.dPdu = normalizeOr(e1, Vec3f{1.0f, 0.0f, 0.0f});
            tri.dPdv = cross(ng, tri.dPdu);
        }
        out.tris.push_back(tri);
    }
    if (!frames || out.tris.size() == first)
        return;
    // The authored faces: a face's two triangles as a quad (MikkTSpace
    // takes quads; the trace surface splits them as Karma does, below).
    const size_t n = out.tris.size() - first;
    MikkMesh mm;
    mm.P.resize(n * 3);
    mm.N.resize(n * 3);
    mm.uv.resize(n * 3);
    for (size_t t = 0; t < n; ++t)
        for (int k = 0; k < 3; ++k) {
            const Tri &tr = out.tris[first + t];
            mm.P[t * 3 + size_t(k)] = tr.P[k];
            mm.N[t * 3 + size_t(k)] = tr.N[k];
            mm.uv[t * 3 + size_t(k)] = tr.uv[k];
        }
    const auto same = [&](size_t a, size_t b) {
        return mm.P[a].x == mm.P[b].x && mm.P[a].y == mm.P[b].y && mm.P[a].z == mm.P[b].z && mm.uv[a].x == mm.uv[b].x &&
               mm.uv[a].y == mm.uv[b].y;
    };
    std::vector<std::pair<uint32_t, uint32_t>> copies; // (corner, its twin in the quad)
    for (size_t t = 0; t < n; ++t) {
        const int32_t face = t < d.triangleFaces.size() ? d.triangleFaces[t] : -1;
        const bool pair = face >= 0 && t + 1 < n && t + 1 < d.triangleFaces.size() && d.triangleFaces[t + 1] == face &&
                          (t + 2 >= d.triangleFaces.size() || d.triangleFaces[t + 2] != face) &&
                          (t == 0 || d.triangleFaces[t - 1] != face);
        const uint32_t a = uint32_t(t * 3), b = uint32_t((t + 1) * 3);
        if (pair && same(b, a) && same(b + 2, a + 1)) { // (p q r) + (p s q): quad p s q r
            MikkFace f;
            f.count = 4;
            f.corner[0] = a, f.corner[1] = b + 1, f.corner[2] = a + 1, f.corner[3] = a + 2;
            mm.faces.push_back(f);
            copies.push_back({b, a});
            copies.push_back({b + 2, a + 1});
            ++t;
            continue;
        }
        if (pair && same(b, a) && same(b + 1, a + 2)) { // (p q r) + (p r s): quad p q r s
            MikkFace f;
            f.count = 4;
            f.corner[0] = a, f.corner[1] = a + 1, f.corner[2] = a + 2, f.corner[3] = b + 2;
            mm.faces.push_back(f);
            copies.push_back({b, a});
            copies.push_back({b + 1, a + 2});
            ++t;
            continue;
        }
        MikkFace f;
        f.count = 3;
        f.corner[0] = a, f.corner[1] = a + 1, f.corner[2] = a + 2;
        mm.faces.push_back(f);
    }
    if (m_settings.mikkt) {
        std::vector<MikkTangent> tangents = mikkTangents(mm);
        for (const auto &[corner, twin] : copies)
            tangents[corner] = tangents[twin];
        // Left-handed meshes were reversed into the triangles: the authored
        // winding's sign is the opposite.
        const float flip = d.leftHanded ? -1.0f : 1.0f;
        for (size_t t = 0; t < n; ++t) {
            Tri &tr = out.tris[first + t];
            for (int k = 0; k < 3; ++k)
                tr.T[k] = tangents[t * 3 + size_t(k)].t;
            tr.sign = tangents[t * 3].sign * flip;
        }
    }
    if (m_settings.mode != BakeMode::Trace)
        return; // the shading point is on these triangles (Single): keep the host's
    // Karma splits a quad along its second and fourth corners (measured with
    // trapezoids, either winding, whichever diagonal is shorter); the host's
    // fan uses the first and third. Texture positions inside non-affine
    // quads depend on it.
    const auto cornerOf = [&](uint32_t c, Tri &dst, int k) {
        const Tri &src = out.tris[first + c / 3];
        const int j = int(c % 3);
        dst.P[k] = src.P[j];
        dst.N[k] = src.N[j];
        dst.T[k] = src.T[j];
        dst.uv[k] = src.uv[j];
        dst.bias[k] = src.bias[j];
        dst.maxT[k] = src.maxT[j];
    };
    for (const MikkFace &f : mm.faces) {
        if (f.count != 4)
            continue;
        const uint32_t t0 = f.corner[0] / 3;
        Tri a = out.tris[first + t0], b = a;
        cornerOf(f.corner[0], a, 0), cornerOf(f.corner[1], a, 1), cornerOf(f.corner[3], a, 2);
        cornerOf(f.corner[1], b, 0), cornerOf(f.corner[2], b, 1), cornerOf(f.corner[3], b, 2);
        for (Tri *t : {&a, &b}) {
            const Vec3f e1 = t->P[1] - t->P[0], e2 = t->P[2] - t->P[0];
            const float du1 = t->uv[1].x - t->uv[0].x, dv1 = t->uv[1].y - t->uv[0].y;
            const float du2 = t->uv[2].x - t->uv[0].x, dv2 = t->uv[2].y - t->uv[0].y;
            const float dd = du1 * dv2 - du2 * dv1;
            if (std::fabs(dd) > 1e-30f) {
                t->dPdu = (e1 * dv2 - e2 * dv1) * (1.0f / dd);
                t->dPdv = (e2 * du1 - e1 * du2) * (1.0f / dd);
            }
        }
        out.tris[first + t0] = a;
        out.tris[first + t0 + 1] = b;
    }
}

void Baker::build(const Scene &scene, const BakeSettings &settings) {
    m_settings = settings;
    m_low = Surface{};
    m_cage = Surface{};
    m_high = Surface{};
    m_cagePoints.clear();
    const int udim = std::clamp(settings.udim, 1001, 9999) - 1001;
    m_origin = {float(udim % 10), float(udim / 10)};
    for (MeshId id : settings.low)
        addMesh(scene, id, m_low, true);
    m_low.index(m_origin);
    if (settings.mode == BakeMode::UvMatch) {
        for (MeshId id : settings.high)
            addMesh(scene, id, m_high, false);
        m_high.index(m_origin);
    }
    if (settings.mode == BakeMode::Trace && settings.cageMode == BakeCage::Uv) {
        for (MeshId id : settings.cage)
            addMesh(scene, id, m_cage, false);
        m_cage.index(m_origin);
    }
    if (settings.mode == BakeMode::Trace && settings.cageMode == BakeCage::Topology) {
        // The cage's triangles in the low meshes' order (same topology).
        Surface cage;
        for (MeshId id : settings.cage)
            addMesh(scene, id, cage, true); // split like the low mesh
        if (cage.tris.size() == m_low.tris.size()) {
            m_cagePoints.resize(cage.tris.size() * 3);
            for (size_t t = 0; t < cage.tris.size(); ++t)
                for (int k = 0; k < 3; ++k)
                    m_cagePoints[t * 3 + size_t(k)] = cage.tris[t].P[k];
        }
    }
    // The position map's box.
    Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    const auto grow = [&](Vec3f p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    switch (settings.box) {
    case BakeBox::World:
        lo = {0.0f, 0.0f, 0.0f}, hi = {1.0f, 1.0f, 1.0f};
        break;
    case BakeBox::User:
        lo = {settings.boxMin[0], settings.boxMin[1], settings.boxMin[2]};
        hi = {settings.boxMax[0], settings.boxMax[1], settings.boxMax[2]};
        break;
    case BakeBox::BakeMesh:
        for (const Tri &t : m_low.tris)
            for (int k = 0; k < 3; ++k)
                grow(t.P[k]);
        break;
    case BakeBox::Scene:
        // The visible geometry (measured: the hidden low mesh is not in it).
        for (MeshId id = 0; id < scene.meshCount(); ++id) {
            const MeshRecord &rec = scene.meshRecord(id);
            if (!rec.alive || !rec.desc.visible || rec.visibility == 0 || rec.instanceCount() == 0)
                continue;
            for (uint32_t i = 0; i < rec.instanceCount(); ++i) {
                const Affine3f xf = scene.objectToWorld(id, i, 0.0f);
                const size_t points = rec.desc.pointCount();
                for (size_t p = 0; p < points; ++p)
                    grow(xf.point(rec.desc.points[p]));
            }
        }
        break;
    }
    if (!(lo.x <= hi.x))
        lo = hi = {0.0f, 0.0f, 0.0f};
    Vec3f size = hi - lo;
    if (settings.box != BakeBox::World && settings.boxUniform) {
        // A cube of the largest extent around the box's centre.
        const float s = std::max({size.x, size.y, size.z});
        const Vec3f c = (lo + hi) * 0.5f;
        lo = c - Vec3f{s, s, s} * 0.5f;
        size = {s, s, s};
    }
    m_boxMin = lo;
    m_boxSize = size;
}

bool Baker::lowPoint(uint32_t tri, Vec3f b, BakePoint &out) const {
    const Tri &t = m_low.tris[tri];
    out.tri = tri;
    out.b = b;
    out.P = t.P[0] * b.x + t.P[1] * b.y + t.P[2] * b.z;
    const Vec3f ng = normalizeOr(cross(t.P[1] - t.P[0], t.P[2] - t.P[0]), Vec3f{0.0f, 0.0f, 1.0f});
    out.N = normalizeOr(t.N[0] * b.x + t.N[1] * b.y + t.N[2] * b.z, ng);
    if (m_settings.mikkt) {
        const Vec3f T = t.T[0] * b.x + t.T[1] * b.y + t.T[2] * b.z;
        out.T = normalizeOr(T - out.N * dot(out.N, T), Frame::fromNormal(out.N).t);
        out.B = cross(out.N, out.T) * t.sign;
    } else {
        // Karma's own frame (measured): the tangent along dP/du, the
        // bitangent along dP/dv, whatever the winding.
        out.T = normalizeOr(t.dPdu - out.N * dot(out.N, t.dPdu), Frame::fromNormal(out.N).t);
        out.B = cross(out.N, out.T);
        if (dot(out.B, t.dPdv) < 0.0f)
            out.B = -out.B;
    }
    out.bias = t.bias[0] * b.x + t.bias[1] * b.y + t.bias[2] * b.z;
    out.maxT = t.maxT[0] * b.x + t.maxT[1] * b.y + t.maxT[2] * b.z;
    return true;
}

bool Baker::lookup(Vec2f st, BakePoint &out) const {
    uint32_t tri;
    Vec3f b;
    return m_low.find(st, m_origin, tri, b) && lowPoint(tri, b, out);
}

// ---------------------------------------------------------------------------
// Samples and shading points

uint64_t Baker::generate(const Scene &scene, const Camera &camera, const PixelFilter &filter, uint32_t seed,
                         const TileRect &tile, const uint32_t *pixels, uint32_t pixelCount, uint32_t spp,
                         const Film &film, BakeScratch &s) const {
    s.samples.clear();
    const float resX = camera.rasterToNdcScale.x > 0.0f ? 2.0f / camera.rasterToNdcScale.x : float(film.width);
    const float resY = camera.rasterToNdcScale.y > 0.0f ? 2.0f / camera.rasterToNdcScale.y : float(film.height);
    const auto pixelSample = [&](int x, int y) {
        for (uint32_t j = 0; j < spp; ++j) {
            const int ix = x + camera.imageOriginX, iy = y + camera.imageOriginY;
            const uint32_t pix = uint32_t(y) * uint32_t(film.width) + uint32_t(x);
            const Sampler sampler = Sampler::make(ix, iy, film.count[pix] + j, seed);
            const Vec2f o = filter.sample(sampler.get2D(stream::kPixelFilter));
            const Vec2f st{(float(ix) + 0.5f + o.x) / resX, (float(iy) + 0.5f + o.y) / resY};
            BakeSample b;
            b.pixel = pix;
            b.index = film.count[pix] + j;
            b.hit = lookup(st, b.low);
            s.samples.push_back(b);
        }
    };
    if (!pixels) {
        for (int y = tile.y0; y < tile.y1; ++y)
            for (int x = tile.x0; x < tile.x1; ++x)
                pixelSample(x, y);
    } else {
        for (uint32_t i = 0; i < pixelCount; ++i)
            pixelSample(int(pixels[i] % uint32_t(film.width)), int(pixels[i] / uint32_t(film.width)));
    }
    const float time = camera.frameTime;
    const auto finish = [&](BakeSample &b, MeshId mesh, uint32_t prim, Vec2f uv, float t, uint32_t inst, Vec3f org,
                            Vec3f dir) {
        b.shade = scene.resolveHit(mesh, prim, uv, t, inst, time, org, dir);
        b.prim = prim;
        b.uv = uv;
        b.t = t;
        // The view of the lighting outputs (measured): the shading point's
        // front, whichever way the ray went.
        b.dir = dot(dir, b.shade.Ng) > 0.0f ? -dir : dir;
        b.org = b.shade.P - b.dir * t;
    };
    uint64_t rays = 0;
    const BakeSettings &bs = m_settings;
    if (bs.mode == BakeMode::Single) {
        for (BakeSample &b : s.samples)
            if (b.hit) {
                const Tri &tr = m_low.tris[b.low.tri];
                finish(b, tr.mesh, tr.prim, {b.low.b.y, b.low.b.z}, 1e-3f, 0, b.low.P + b.low.N * 1e-3f, -b.low.N);
            }
        return 0;
    }
    if (bs.mode == BakeMode::UvMatch) {
        for (BakeSample &b : s.samples) {
            if (!b.hit)
                continue;
            const Tri &lt = m_low.tris[b.low.tri];
            const Vec2f st = lt.uv[0] * b.low.b.x + lt.uv[1] * b.low.b.y + lt.uv[2] * b.low.b.z;
            uint32_t tri;
            Vec3f bc;
            b.hit = m_high.find({st.x - m_origin.x, st.y - m_origin.y}, m_origin, tri, bc);
            if (b.hit) {
                const Tri &ht = m_high.tris[tri];
                const Vec3f P = ht.P[0] * bc.x + ht.P[1] * bc.y + ht.P[2] * bc.z;
                const Vec3f n = normalizeOr(ht.N[0] * bc.x + ht.N[1] * bc.y + ht.N[2] * bc.z, b.low.N);
                finish(b, ht.mesh, ht.prim, {bc.y, bc.z}, 1e-3f, 0, P + n * 1e-3f, -n);
            }
        }
        return 0;
    }
    // Trace: both ways along the low normal from P +- bias N (measured: the
    // nearer hit along its own ray wins), or one ray from a cage.
    s.org.clear();
    s.dir.clear();
    s.tMax.clear();
    s.owner.clear();
    for (uint32_t i = 0; i < uint32_t(s.samples.size()); ++i) {
        BakeSample &b = s.samples[i];
        if (!b.hit)
            continue;
        const BakePoint &p = b.low;
        const float maxT = p.maxT > 0.0f ? p.maxT : kInf;
        const auto push = [&](Vec3f o, Vec3f d, float t) {
            s.org.push_back(o);
            s.dir.push_back(d);
            s.tMax.push_back(std::min(t, kInf));
            s.owner.push_back(i);
        };
        switch (bs.cageMode) {
        case BakeCage::None:
            push(p.P + p.N * p.bias, -p.N, maxT);
            push(p.P - p.N * p.bias, p.N, maxT);
            break;
        case BakeCage::Offset:
            push(p.P + p.N * p.bias, -p.N, maxT);
            break;
        case BakeCage::Topology:
        case BakeCage::Uv: {
            Vec3f c{0.0f, 0.0f, 0.0f};
            bool have = false;
            if (bs.cageMode == BakeCage::Topology && m_cagePoints.size() == m_low.tris.size() * 3) {
                const Vec3f *cp = &m_cagePoints[size_t(p.tri) * 3];
                c = cp[0] * p.b.x + cp[1] * p.b.y + cp[2] * p.b.z;
                have = true;
            } else if (bs.cageMode == BakeCage::Uv) {
                const Tri &lt = m_low.tris[p.tri];
                const Vec2f st = lt.uv[0] * p.b.x + lt.uv[1] * p.b.y + lt.uv[2] * p.b.z;
                uint32_t tri;
                Vec3f bc;
                if (m_cage.find({st.x - m_origin.x, st.y - m_origin.y}, m_origin, tri, bc)) {
                    const Tri &ct = m_cage.tris[tri];
                    c = ct.P[0] * bc.x + ct.P[1] * bc.y + ct.P[2] * bc.z;
                    have = true;
                }
            }
            const Vec3f d = c - p.P;
            if (have && length(d) > 0.0f)
                push(c, normalizeOr(p.P - c, -p.N), maxT);
            else
                push(p.P + p.N * p.bias, -p.N, maxT); // no cage point: the offset cage
            break;
        }
        }
    }
    const uint32_t count = uint32_t(s.org.size());
    if (count == 0)
        return 0;
    s.time.assign(count, time);
    s.seed.assign(count, 0.5f);
    s.type.assign(count, uint8_t(RayClass::Camera));
    s.t.resize(count);
    s.mesh.resize(count);
    s.prim.resize(count);
    s.instance.resize(count);
    s.uv.resize(count);
    RayBatch batch{s.org.data(), s.dir.data(), s.tMax.data(), count, s.time.data(), s.seed.data(), s.type.data(), 0};
    HitBatch hits{s.t.data(), s.mesh.data(), s.prim.data(), s.uv.data(), s.instance.data()};
    scene.accel().intersect(batch, hits);
    rays += count;
    // The nearest hit of each sample's rays.
    std::vector<int32_t> best(s.samples.size(), -1);
    for (uint32_t r = 0; r < count; ++r) {
        if (s.mesh[r] == kInvalidId)
            continue;
        int32_t &k = best[s.owner[r]];
        if (k < 0 || s.t[r] < s.t[size_t(k)])
            k = int32_t(r);
    }
    for (size_t i = 0; i < s.samples.size(); ++i) {
        BakeSample &b = s.samples[i];
        if (!b.hit)
            continue;
        const int32_t r = best[i];
        if (r < 0) {
            b.hit = false;
            continue;
        }
        finish(b, s.mesh[size_t(r)], s.prim[size_t(r)], s.uv[size_t(r)], s.t[size_t(r)], s.instance[size_t(r)],
               s.org[size_t(r)], s.dir[size_t(r)]);
    }
    return rays;
}

// ---------------------------------------------------------------------------
// The maps

uint64_t Baker::accumulate(const Scene &scene, const RenderSettings &settings, const Camera &camera, uint32_t seed,
                           Film &film, BakeScratch &s, bool own) const {
    uint64_t rays = 0;
    const size_t n = s.samples.size();
    if (own)
        for (const BakeSample &b : s.samples) {
            ++film.count[b.pixel];
            if (b.hit)
                film.alphaSum[b.pixel] += 1.0f;
        }
    if (film.utility.empty() || n == 0)
        return 0;
    const BakeSettings &bs = m_settings;
    const float time = camera.frameTime;
    bool want[6] = {false, false, false, false, false, false};
    bool exports = false;
    for (const UtilityPlane &pl : film.utility) {
        switch (pl.request.kind) {
        case UtilityKind::BakeOcclusion:
            want[qOcclusion] = true;
            break;
        case UtilityKind::BakeCavity:
            want[qCavity] = true;
            break;
        case UtilityKind::BakeThickness:
            want[qThickness] = true;
            break;
        case UtilityKind::BakeCurvature:
            want[qCurvature] = true;
            break;
        case UtilityKind::BakeEdge:
            want[qEdge] = true;
            break;
        case UtilityKind::BakeRoundedNormal:
            want[qRounded] = true;
            break;
        case UtilityKind::BakeExport:
        case UtilityKind::BaseColor:
        case UtilityKind::ShaderAov:
            exports = true;
            break;
        default:
            break;
        }
    }
    // Each sample's random numbers per query.
    const auto rnd = [&](const BakeSample &b, uint32_t q) {
        const int x = int(b.pixel % uint32_t(film.width)) + camera.imageOriginX;
        const int y = int(b.pixel / uint32_t(film.width)) + camera.imageOriginY;
        return Sampler::make(x, y, b.index, seed).get3D(kBakeStream + q);
    };
    // Query results per sample: [occlusion, cavity, thickness, curvature,
    // edge, rounded normal xyz].
    constexpr int kOut = 8;
    s.values.assign(n * kOut, 0.0f);
    const auto setRays = [&](uint32_t count, uint8_t type) {
        s.time.assign(count, time);
        s.seed.assign(count, 0.5f);
        s.type.assign(count, type);
    };
    const auto clearRays = [&] {
        s.org.clear();
        s.dir.clear();
        s.tMax.clear();
        s.owner.clear();
    };
    const auto push = [&](uint32_t owner, Vec3f o, Vec3f d, float t) {
        s.org.push_back(o);
        s.dir.push_back(d);
        s.tMax.push_back(t);
        s.owner.push_back(owner);
    };
    const auto occlude = [&] {
        const uint32_t count = uint32_t(s.org.size());
        setRays(count, uint8_t(RayClass::Shadow));
        s.occluded.resize(count);
        RayBatch b{s.org.data(), s.dir.data(), s.tMax.data(), count, s.time.data(), s.seed.data(), s.type.data(), 0};
        b.skipFakeCaustics = false;
        if (count)
            scene.accel().occluded(b, s.occluded.data());
        rays += count;
    };
    const auto intersect = [&] {
        const uint32_t count = uint32_t(s.org.size());
        setRays(count, uint8_t(RayClass::Shadow));
        s.t.resize(count);
        s.mesh.resize(count);
        s.prim.resize(count);
        s.instance.resize(count);
        s.uv.resize(count);
        RayBatch b{s.org.data(), s.dir.data(), s.tMax.data(), count, s.time.data(), s.seed.data(), s.type.data(), 0};
        HitBatch h{s.t.data(), s.mesh.data(), s.prim.data(), s.uv.data(), s.instance.data()};
        if (count)
            scene.accel().intersect(b, h);
        rays += count;
    };
    const auto clampSamples = [](int v) { return std::clamp(v, 1, 1024); };

    // Occlusion: cosine-weighted around N, blocked within the maximum
    // distance (unlimited if <= 0); the unblocked fraction.
    if (want[qOcclusion]) {
        clearRays();
        const int m = clampSamples(bs.ocSamples);
        const float maxT = bs.ocMaxDist > 0.0f ? bs.ocMaxDist : kInf;
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            const BakeSample &b = s.samples[i];
            if (!b.hit)
                continue;
            const Frame f = Frame::fromNormal(b.shade.N);
            const Vec3f r = rnd(b, qOcclusion);
            const Vec3f o = offsetRayOrigin(b.shade.P, faceForward(b.shade.Ng, b.shade.N)) + b.shade.N * bs.ocBias;
            for (int k = 0; k < m; ++k) {
                const Vec3f u = patternPoint(r, k);
                push(i, o, f.toWorld(sampleCosineHemisphere({u.x, u.y})), maxT);
            }
        }
        occlude();
        std::vector<int> open(n, 0);
        for (size_t r = 0; r < s.owner.size(); ++r)
            open[s.owner[r]] += s.occluded[r] ? 0 : 1;
        for (size_t i = 0; i < n; ++i)
            s.values[i * kOut + 0] = float(open[i]) / float(m);
    }
    // Cavity: uniform directions into the surface (around -N) from 0.001
    // below it; the fraction that leaves within the distance, blended to 1
    // by the bias (measured).
    if (want[qCavity]) {
        clearRays();
        const int m = clampSamples(bs.cvSamples);
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            const BakeSample &b = s.samples[i];
            if (!b.hit)
                continue;
            const Frame f = Frame::fromNormal(-b.shade.N);
            const Vec3f r = rnd(b, qCavity);
            for (int k = 0; k < m; ++k) {
                const Vec3f u = patternPoint(r, k);
                push(i, b.shade.P - b.shade.N * 0.001f, f.toWorld(sampleUniformCone({u.x, u.y}, 1.0f)),
                     std::max(bs.cvDist, 0.0f));
            }
        }
        occlude();
        std::vector<int> open(n, 0);
        for (size_t r = 0; r < s.owner.size(); ++r)
            open[s.owner[r]] += s.occluded[r] ? 0 : 1;
        for (size_t i = 0; i < n; ++i) {
            const float c = float(open[i]) / float(m);
            s.values[i * kOut + 1] = c + (1.0f - c) * std::clamp(bs.cvBias, 0.0f, 1.0f);
        }
    }
    // Thickness: cosine-weighted directions into the surface from 0.001
    // below it; the mean distance to the first hit (misses add 0).
    if (want[qThickness]) {
        clearRays();
        const int m = clampSamples(bs.thSamples);
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            const BakeSample &b = s.samples[i];
            if (!b.hit)
                continue;
            const Frame f = Frame::fromNormal(-b.shade.N);
            const Vec3f r = rnd(b, qThickness);
            for (int k = 0; k < m; ++k) {
                const Vec3f u = patternPoint(r, k);
                push(i, b.shade.P - b.shade.N * 0.001f, f.toWorld(sampleCosineHemisphere({u.x, u.y})), kInf);
            }
        }
        intersect();
        for (size_t r = 0; r < s.owner.size(); ++r)
            if (s.mesh[r] != kInvalidId)
                s.values[size_t(s.owner[r]) * kOut + 2] += s.t[r] / float(m);
    }
    // Curvature: uniform hemispheres into (convex) and out of (concave) the
    // surface from 0.002 off it; per ray max(0, 1 - t / d); Karma's response
    // 0.5 + 0.5 sign(c) schlickBias(min(1, scale |c|), bias), c = convex -
    // concave (measured).
    if (want[qCurvature]) {
        clearRays();
        const int m = clampSamples(bs.cuSamples);
        const float d = std::max(bs.cuDist, 1e-12f);
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            const BakeSample &b = s.samples[i];
            if (!b.hit)
                continue;
            const Frame f = Frame::fromNormal(b.shade.N);
            const Vec3f r = rnd(b, qCurvature);
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < m; ++k) {
                    const Vec3f u = patternPoint(r, k);
                    Vec3f dir = f.toWorld(sampleUniformCone({u.x, u.y}, 1.0f));
                    const float sgn = side == 0 ? -1.0f : 1.0f;
                    push(i, b.shade.P + b.shade.N * (0.002f * sgn), dir * sgn, d);
                }
        }
        intersect();
        std::vector<float> conv(n, 0.0f), conc(n, 0.0f);
        for (size_t r = 0; r < s.owner.size(); ++r)
            if (s.mesh[r] != kInvalidId) {
                const float w = std::max(0.0f, 1.0f - s.t[r] / d);
                (dot(s.dir[r], s.samples[s.owner[r]].shade.N) < 0.0f ? conv : conc)[s.owner[r]] += w;
            }
        for (size_t i = 0; i < n; ++i) {
            const float c = (conv[i] - conc[i]) / float(m);
            const float x = std::min(1.0f, std::fabs(bs.cuScale * c));
            s.values[i * kOut + 3] = 0.5f + 0.5f * std::copysign(schlickBias(x, bs.cuBias), c);
        }
    }
    // Rounded edges (the shading query's estimator, M4.4): the normal
    // averaged over the surfaces within the radius (weighted sqrt(1 - (d / r)^2) here),
    // found by rays along the normal and the tangents through disks of the
    // radius; surfaces within the cusp angle, and edges the mode excludes,
    // count with the normal itself. Edge (measured): the other surfaces'
    // weight over the normal's own.
    for (const uint32_t q : {uint32_t(qEdge), uint32_t(qRounded)}) {
        if (!want[q])
            continue;
        clearRays();
        const bool edge = q == qEdge;
        const int m = clampSamples(edge ? bs.egSamples : bs.rnSamples);
        const float radius = std::max(edge ? bs.egRadius : bs.rnRadius, 1e-12f);
        const int mode = edge ? bs.egMode : bs.rnMode;
        const float cusp = std::cos(std::clamp(edge ? bs.egCusp : bs.rnCusp, 0.0f, 180.0f) * (kPi / 180.0f));
        for (uint32_t i = 0; i < uint32_t(n); ++i) {
            const BakeSample &b = s.samples[i];
            if (!b.hit)
                continue;
            const Frame f = Frame::fromNormal(b.shade.N);
            const Vec3f r = rnd(b, q);
            for (int k = 0; k < m; ++k) {
                const Vec3f u = patternPoint(r, k);
                const float rr = radius * std::sqrt(u.x), phi = kTwoPi * u.y;
                const float x = rr * std::cos(phi), y = rr * std::sin(phi);
                for (int a = 0; a < 3; ++a) {
                    const Vec3f axis = a == 0 ? f.n : a == 1 ? f.t : f.b;
                    const Vec3f e1 = a == 0 ? f.t : a == 1 ? f.b : f.n, e2 = a == 0 ? f.b : a == 1 ? f.n : f.t;
                    push(i, b.shade.P + e1 * x + e2 * y + axis * radius, -axis, 2.0f * radius);
                }
            }
        }
        intersect();
        std::vector<Vec3f> sum(n, Vec3f{0.0f, 0.0f, 0.0f});
        std::vector<float> wOwn(n, 0.0f), wOther(n, 0.0f);
        for (size_t r = 0; r < s.owner.size(); ++r) {
            if (s.mesh[r] == kInvalidId)
                continue;
            const BakeSample &b = s.samples[s.owner[r]];
            const Frame f = Frame::fromNormal(b.shade.N);
            const SurfaceHit h =
                scene.resolveHit(s.mesh[r], s.prim[r], s.uv[r], s.t[r], s.instance[r], time, s.org[r], s.dir[r]);
            const Vec3f toHit = h.P - b.shade.P;
            const float d2 = dot(toHit, toHit) / (radius * radius);
            if (d2 >= 1.0f)
                continue;
            Vec3f hn = h.N;
            bool changed = false;
            if (dot(h.N, f.n) >= cusp) {
                hn = f.n;
            } else {
                const bool convex = dot(h.N - f.n, toHit) > 0.0f;
                if ((mode == 1 && convex) || (mode == 2 && !convex))
                    hn = f.n;
                else
                    changed = true;
            }
            const float pdf = std::fabs(dot(h.Ng, f.n)) + std::fabs(dot(h.Ng, f.t)) + std::fabs(dot(h.Ng, f.b));
            if (pdf <= 0.0f)
                continue;
            // The baker's profile is wider than kma_roundededge's: weight
            // sqrt(1 - (d / r)^2) fits Karma's 90 and 135 degree edges within
            // 0.03 in the normal (1 - (d / r)^2: up to 0.056).
            const float w = std::sqrt(1.0f - d2) / pdf;
            sum[s.owner[r]] += hn * w;
            (changed ? wOther : wOwn)[s.owner[r]] += w;
        }
        for (size_t i = 0; i < n; ++i) {
            if (!s.samples[i].hit)
                continue;
            if (edge) {
                s.values[i * kOut + 4] = wOwn[i] > 0.0f ? wOther[i] / wOwn[i] : (wOther[i] > 0.0f ? 1.0f : 0.0f);
            } else {
                const Vec3f nn = wOther[i] > 0.0f ? normalizeOr(sum[i], s.samples[i].shade.N) : s.samples[i].shade.N;
                s.values[i * kOut + 5] = nn.x;
                s.values[i * kOut + 6] = nn.y;
                s.values[i * kOut + 7] = nn.z;
            }
        }
    }

    // Material values at the shading points, in runs of one material: the
    // values of each material plane, 3 per sample and plane.
    const size_t planes = film.utility.size();
    const auto isExport = [](UtilityKind k) {
        return k == UtilityKind::BakeExport || k == UtilityKind::BaseColor || k == UtilityKind::ShaderAov;
    };
    if (exports) {
        s.exported.assign(n * planes * 3, 0.0f);
        thread_local MaterialEvalScratch scratch;
        const ShadeGlobals globals = shadeGlobals(settings, camera);
        static const SurfaceInputs kDefaults = standardSurfaceDefaults();
        const auto store = [&](size_t i, const SurfaceInputs &in, const shading::Program *p) {
            if (in.model == SurfaceModel::External)
                return;
            for (size_t q = 0; q < planes; ++q) {
                const UtilityRequest &req = film.utility[q].request;
                if (isExport(req.kind))
                    exportValue(req.kind == UtilityKind::BaseColor ? std::string("base_color") : req.name, in,
                                s.samples[i].shade, p, &s.exported[(i * planes + q) * 3]);
            }
        };
        for (size_t i = 0; i < n;) {
            if (!s.samples[i].hit) {
                ++i;
                continue;
            }
            const MaterialId mat = scene.materialIdOf(s.samples[i].shade.mesh, s.samples[i].prim);
            size_t e = i;
            SurfaceHit hits[kShadeChunk];
            Vec3f dirs[kShadeChunk];
            float times[kShadeChunk];
            size_t idx[kShadeChunk];
            int c = 0;
            while (e < n && c < kShadeChunk) {
                if (s.samples[e].hit) {
                    if (scene.materialIdOf(s.samples[e].shade.mesh, s.samples[e].prim) != mat)
                        break;
                    hits[c] = s.samples[e].shade;
                    dirs[c] = s.samples[e].dir;
                    times[c] = time;
                    idx[c] = e;
                    ++c;
                }
                ++e;
            }
            if (mat == kDefaultMaterialId) {
                // Unbound geometry: Karma bakes its default Standard Surface.
                for (int k = 0; k < c; ++k)
                    store(idx[k], kDefaults, nullptr);
            } else {
                const SurfaceInputs *out[kShadeChunk];
                evaluateSurfaceInputs(scene, mat, hits, dirs, times, c, out, scratch, globals);
                const shading::Program *p = scene.material(mat).desc.program.get();
                for (int k = 0; k < c; ++k)
                    store(idx[k], *out[k], p);
            }
            i = e;
        }
    }

    // Into the planes.
    const Vec3f boxInv{m_boxSize.x != 0.0f ? 1.0f / m_boxSize.x : 0.0f, m_boxSize.y != 0.0f ? 1.0f / m_boxSize.y : 0.0f,
                       m_boxSize.z != 0.0f ? 1.0f / m_boxSize.z : 0.0f};
    const auto box = [&](float p, float lo, float size, float inv) {
        if (bs.box == BakeBox::World)
            return p;
        if (size == 0.0f) // measured: a flat box maps above it to 1, onto it to 0
            return p > lo + 1e-5f * std::max(1.0f, std::fabs(lo)) ? 1.0f : 0.0f;
        return std::clamp((p - lo) * inv, 0.0f, 1.0f);
    };
    for (size_t i = 0; i < n; ++i) {
        const BakeSample &b = s.samples[i];
        if (!b.hit)
            continue;
        const SurfaceHit &h = b.shade;
        const BakePoint &l = b.low;
        const auto tangent = [&](Vec3f v) { return Vec3f{dot(v, l.T), dot(v, l.B), dot(v, l.N)}; };
        const Vec3f vd = h.P - l.P;
        for (size_t q = 0; q < planes; ++q) {
            UtilityPlane &pl = film.utility[q];
            float *o = pl.data.data() + size_t(b.pixel) * pl.width;
            float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            int w = 3;
            const auto set = [&](Vec3f x) { v[0] = x.x, v[1] = x.y, v[2] = x.z; };
            switch (pl.request.kind) {
            case UtilityKind::BakeNormal:
                set(tangent(h.N));
                break;
            case UtilityKind::BakeHeight:
                v[0] = v[1] = v[2] = length(vd);
                break;
            case UtilityKind::BakeVector:
                set(vd);
                break;
            case UtilityKind::BakeVectorTangent:
                set(tangent(vd));
                break;
            case UtilityKind::BakeOcclusion:
                v[0] = v[1] = v[2] = s.values[i * kOut + 0];
                break;
            case UtilityKind::BakeCavity:
                v[0] = v[1] = v[2] = s.values[i * kOut + 1];
                break;
            case UtilityKind::BakeThickness:
                v[0] = v[1] = v[2] = s.values[i * kOut + 2];
                break;
            case UtilityKind::BakeCurvature:
                v[0] = v[1] = v[2] = s.values[i * kOut + 3];
                break;
            case UtilityKind::BakeEdge:
                v[0] = v[1] = v[2] = s.values[i * kOut + 4];
                break;
            case UtilityKind::BakeRoundedNormal:
                set(tangent({s.values[i * kOut + 5], s.values[i * kOut + 6], s.values[i * kOut + 7]}));
                break;
            case UtilityKind::BakeAlpha:
                v[0] = 1.0f;
                w = 1;
                break;
            case UtilityKind::BakePosition:
            case UtilityKind::P:
                set({box(h.P.x, m_boxMin.x, m_boxSize.x, boxInv.x), box(h.P.y, m_boxMin.y, m_boxSize.y, boxInv.y),
                     box(h.P.z, m_boxMin.z, m_boxSize.z, boxInv.z)});
                if (pl.request.kind == UtilityKind::P)
                    set(h.P);
                break;
            case UtilityKind::BakeShadingNormal:
            case UtilityKind::N:
                set(h.N);
                break;
            case UtilityKind::BakeGeometricNormal:
            case UtilityKind::Ng:
                set(h.Ng);
                break;
            case UtilityKind::FacingN:
                v[0] = std::fabs(dot(h.N, b.dir));
                break;
            case UtilityKind::FacingNg:
                v[0] = std::fabs(dot(h.Ng, b.dir));
                break;
            case UtilityKind::HitDist:
                v[0] = b.t;
                break;
            case UtilityKind::Face: {
                const MeshDesc &d = scene.meshRecord(h.mesh).desc;
                v[0] = float(b.prim < d.triangleFaces.size() ? d.triangleFaces[b.prim] : int32_t(b.prim));
                break;
            }
            case UtilityKind::Primvar: {
                const AttributeRef ref = scene.attributeRef(h.mesh, pl.request.name);
                const MeshDesc &d = scene.meshRecord(h.mesh).desc;
                if (size_t(b.prim) * 3 + 2 < d.indices.size())
                    fetchAttribute(ref, &d.indices[size_t(b.prim) * 3], b.prim, h.instance, b.uv.x, b.uv.y, 4, v);
                w = 4;
                break;
            }
            case UtilityKind::BaseColor:
            case UtilityKind::BakeExport:
            case UtilityKind::ShaderAov:
                for (int c = 0; c < 3; ++c)
                    v[c] = s.exported[(i * planes + q) * 3 + size_t(c)];
                break;
            default:
                break;
            }
            for (int c = 0; c < std::min(int(pl.width), w); ++c)
                o[c] += v[c];
        }
    }
    return rays;
}

} // namespace nova
