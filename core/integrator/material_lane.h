#pragma once

// The per-hit inputs of a material's shading program (globals, the tangent
// frame), shared by the CPU's chunked evaluation and the GPU's per-thread one
// (docs/design/M10.md D3).

#include "core/math/frame.h"
#include "core/scene/attributes.h"
#include "core/scene/hit.h"
#include "core/scene/motion.h"
#include "core/shading/ir.h"

namespace nova {

// Frame and fps for the Time and Frame globals.
struct ShadeGlobals {
    float frame = 1.0f;
    float fps = 24.0f;
    // The render camera for NDC positions (Global::Pndc; hasCamera 0: none,
    // the position is 0): world to clip space (row vectors, divided by w),
    // the eye and the unit view direction (depth).
    uint32_t hasCamera = 0;
    float worldToClip[4][4] = {};
    Vec3f eye{0.0f, 0.0f, 0.0f}, forward{0.0f, 0.0f, -1.0f};

    // A world point in NDC space: x, y in [0, 1] over the image, z its depth.
    NOVA_INLINE Vec3f ndc(Vec3f p) const {
        if (!hasCamera)
            return {0.0f, 0.0f, 0.0f};
        const auto &m = worldToClip;
        const float x = p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0];
        const float y = p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1];
        const float w = p.x * m[0][3] + p.y * m[1][3] + p.z * m[2][3] + m[3][3];
        const float iw = w != 0.0f ? 1.0f / w : 1.0f;
        return {0.5f * (x * iw + 1.0f), 0.5f * (y * iw + 1.0f), dot(p - eye, forward)};
    }
};

namespace material_lane {

// The object-space vector of world-space vector a (Cramer's rule on the
// object axes).
NOVA_INLINE Vec3f toObject(const Affine3f &xf, Vec3f a) {
    const float det = xf.determinant();
    if (det == 0.0f)
        return {0.0f, 0.0f, 0.0f};
    return Vec3f{dot(a, cross(xf.y, xf.z)), dot(a, cross(xf.z, xf.x)), dot(a, cross(xf.x, xf.y))} * (1.0f / det);
}

// The two vectors a, b in the plane of gradients gs, gt with a.gs = 1,
// a.gt = 0, b.gs = 0, b.gt = 1 (dP/ds and dP/dt of the parametrization
// whose gradients these are); false if they are degenerate.
NOVA_INLINE bool dualBasis(Vec3f gs, Vec3f gt, Vec3f &a, Vec3f &b) {
    const float ss = dot(gs, gs), tt = dot(gt, gt), st = dot(gs, gt), d = ss * tt - st * st;
    if (!(d > 1e-30f) || !isFinite(d))
        return false;
    a = (gs * tt - gt * st) * (1.0f / d);
    b = (gt * ss - gs * st) * (1.0f / d);
    return isFinite(a) && isFinite(b);
}

// dP/ds and dP/dt of the hit's tangent uv set (Karma's tangent frame
// source): from the uv at the triangle corners, or the surface parameters of
// an analytic shape. Without that set, the triangle's edges (Karma: the first
// edge); degenerate, any tangent basis.
NOVA_INLINE void tangentDerivatives(const SurfaceHit &hit, const AttributeRef *uv, const uint32_t *triangle, Vec3f &dPdu,
                                    Vec3f &dPdv) {
    Vec3f gs = hit.dudP, gt = hit.dvdP; // barycentrics: the dual basis is the triangle's edges
    if (uv && uv->source == AttributeSource::Parametric) {
        // the surface parameters are the uv set
    } else if (uv && uv->source != AttributeSource::None) {
        float c[3][2];
        const float corners[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok = ok && fetchAttribute(*uv, triangle, hit.prim, hit.instance, corners[k][0], corners[k][1], 2, c[k]);
        if (ok) {
            gs = hit.dudP * (c[1][0] - c[0][0]) + hit.dvdP * (c[2][0] - c[0][0]);
            gt = hit.dudP * (c[1][1] - c[0][1]) + hit.dvdP * (c[2][1] - c[0][1]);
        }
    }
    if (dualBasis(gs, gt, dPdu, dPdv) || dualBasis(hit.dudP, hit.dvdP, dPdu, dPdv))
        return;
    const Frame f = Frame::fromNormal(hit.N);
    dPdu = f.t;
    dPdv = f.b;
}

// What a global needs of the hit beyond SurfaceHit: the ray's direction,
// class and depth, the footprint vectors (zero without), the instance's
// object-to-world transform at the ray time (computed by `xf()` only when a
// global asks for it), the tangent uv set and the triangle's corner indices.
struct LaneHit {
    const SurfaceHit *hit;
    Vec3f dir;
    Vec3f footprintX{0.0f, 0.0f, 0.0f}, footprintY{0.0f, 0.0f, 0.0f};
    float rayType = 0.0f;
    uint32_t depth = 0;
    const AttributeRef *tangentUv = nullptr;
    const uint32_t *triangle = nullptr;
};

// The value of a global at a hit: a vector in v, a scalar in s.
template <class ObjectToWorld>
NOVA_INLINE void globalValue(shading::Global g, const LaneHit &lane, const ShadeGlobals &globals,
                             const ObjectToWorld &xf, Vec3f &v, float &s) {
    using shading::Global;
    const SurfaceHit &hit = *lane.hit;
    v = {0.0f, 0.0f, 0.0f};
    s = 0.0f;
    switch (g) {
    case Global::P:
        v = hit.P;
        break;
    case Global::Pobject:
        v = hit.Pobject;
        break;
    case Global::N:
        v = hit.N;
        break;
    case Global::Ng:
        v = hit.Ng;
        break;
    case Global::Nobject:
        v = hit.Nobject;
        break;
    case Global::I:
        v = lane.dir;
        break;
    case Global::Time:
        s = (globals.frame - 1.0f) / (globals.fps > 0.0f ? globals.fps : 24.0f);
        break;
    case Global::Frame:
        s = globals.frame;
        break;
    case Global::ObjX:
    case Global::ObjY:
    case Global::ObjZ:
    case Global::ObjT: {
        const Affine3f a = xf();
        v = g == Global::ObjX ? a.x : g == Global::ObjY ? a.y : g == Global::ObjZ ? a.z : a.t;
        break;
    }
    case Global::PDx:
    case Global::PDy:
        v = hit.P + (g == Global::PDy ? lane.footprintY : lane.footprintX);
        break;
    case Global::PobjectDx:
    case Global::PobjectDy:
        v = hit.Pobject + toObject(xf(), g == Global::PobjectDy ? lane.footprintY : lane.footprintX);
        break;
    case Global::DPdu:
    case Global::DPdv: {
        Vec3f du, dv;
        tangentDerivatives(hit, lane.tangentUv, lane.triangle, du, dv);
        v = g == Global::DPdu ? du : dv;
        break;
    }
    case Global::PrimId:
        s = float(hit.prim);
        break;
    case Global::InstanceId:
        s = float(hit.instance);
        break;
    case Global::RayType:
        s = lane.rayType;
        break;
    case Global::RayDepth:
        s = float(lane.depth);
        break;
    case Global::HitDist:
        s = hit.t;
        break;
    case Global::Backface:
        s = dot(lane.dir, hit.Ng) > 0.0f ? 1.0f : 0.0f;
        break;
    case Global::Pndc:
        v = globals.ndc(hit.P);
        break;
    default:
        break;
    }
}

// A primvar's value at the hit (uv moved by footprint vector `shift` - 1 if
// shift > 0), or the fallback.
NOVA_INLINE void primvarValue(const AttributeRef *ref, const LaneHit &lane, uint8_t shift, uint8_t width,
                              const float *fallback, float *out) {
    const SurfaceHit &hit = *lane.hit;
    Vec2f uv = hit.uv;
    if (shift) { // at the hit moved by a footprint vector
        const Vec3f f = shift - 1 ? lane.footprintY : lane.footprintX;
        uv = {uv.x + dot(hit.dudP, f), uv.y + dot(hit.dvdP, f)};
    }
    if (!ref || !fetchAttribute(*ref, lane.triangle, hit.prim, hit.instance, uv.x, uv.y, width, out))
        for (int k = 0; k < width; ++k)
            out[k] = fallback[k];
}

} // namespace material_lane

} // namespace nova
