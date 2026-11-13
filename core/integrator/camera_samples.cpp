#include "core/integrator/camera_samples.h"

#include "core/integrator/material_eval.h"
#include "core/scene/attributes.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace nova {

void CameraSampleRecords::reserve(uint32_t paths) {
    if (mesh.size() >= paths)
        return;
    mesh.resize(paths);
    prim.resize(paths);
    instance.resize(paths);
    uv.resize(paths);
    t.resize(paths);
    eventT.resize(paths);
    org.resize(paths);
    dir.resize(paths);
    time.resize(paths);
    alpha.resize(paths);
}

CameraSampleView CameraSampleView::of(const CameraSampleRecords &r, uint32_t count, const uint32_t *pixel,
                                      const uint32_t *sampleIndex, const Spectrum *radiance) {
    CameraSampleView v;
    v.count = count;
    v.pixel = pixel;
    v.sampleIndex = sampleIndex;
    v.radiance = radiance;
    v.mesh = r.mesh.data();
    v.prim = r.prim.data();
    v.instance = r.instance.data();
    v.uv = r.uv.data();
    v.t = r.t.data();
    v.eventT = r.eventT.data();
    v.org = r.org.data();
    v.dir = r.dir.data();
    v.time = r.time.data();
    v.alpha = r.alpha.data();
    return v;
}

namespace {

// A point of a hit surface at another shutter time: the same barycentrics
// (spheres: the object-space point moved with the transform).
Vec3f hitPointAt(const Scene &scene, const SurfaceHit &hit, Vec2f uv, float t, Vec3f org, Vec3f dir, float time) {
    if (scene.meshRecord(hit.mesh).desc.isSphere())
        return scene.objectToWorld(hit.mesh, hit.instance, time).point(hit.Pobject);
    return scene.resolveHit(hit.mesh, hit.prim, uv, t, hit.instance, time, org, dir).P;
}

void set3(float *o, Vec3f v) {
    o[0] = v.x;
    o[1] = v.y;
    o[2] = v.z;
}

// The face's own parameters at a triangle hit (Karma's ray:hituv): a quad's
// two triangles map their barycentrics onto the quad's bilinear (u, v), u
// from its first vertex to the second and v on to the last; triangles keep
// theirs. The delegate fans a quad v0 v1 v2 v3 from its last vertex, as
// Karma does (hydra/meshTriangulate.h): v3 v0 v1, then v3 v1 v2, a
// left-handed mesh with the last two corners of each the other way round.
Vec2f faceParameters(const MeshDesc &d, uint32_t prim, Vec2f b) {
    if (d.isSphere() || d.isCurves() || prim >= d.triangleFaces.size())
        return b;
    const int32_t face = d.triangleFaces[prim];
    uint32_t first = prim, last = prim;
    while (first > 0 && d.triangleFaces[first - 1] == face)
        --first;
    while (last + 1 < d.triangleFaces.size() && d.triangleFaces[last + 1] == face)
        ++last;
    if (last - first != 1) // not a quad
        return b;
    if (d.leftHanded)
        b = {b.y, b.x};
    // P = v3 + b.x (v0 - v3) + b.y (v1 - v3), and v3 + b.x (v1 - v3) + b.y (v2 - v3).
    return prim == first ? Vec2f{b.y, 1.0f - b.x - b.y} : Vec2f{b.x + b.y, 1.0f - b.x};
}

} // namespace

UtilityContext UtilityContext::make(const Scene &scene, const Camera &camera) {
    const auto &pm = camera.ndcToView.m;
    UtilityContext c{scene, camera, inverse(camera.ndcToView), camera.viewToWorld.transformAffine({0.0f, 0.0f, 0.0f}),
                     pm[0][3] == 0.0 && pm[1][3] == 0.0 && pm[2][3] == 0.0};
    // The clipping planes: NDC z = -1 and 1 in view space (a far plane at
    // infinity has w = 0 there).
    const auto depthAt = [&](float z) {
        const double p[4] = {0.0, 0.0, double(z), 1.0};
        double v[4] = {0.0, 0.0, 0.0, 0.0};
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i)
                v[j] += p[i] * pm[i][j];
        return std::fabs(v[3]) > 1e-12 ? float(-v[2] / v[3]) : 0.0f;
    };
    c.nearDepth = depthAt(-1.0f);
    c.farDepth = depthAt(1.0f);
    return c;
}

Vec3f UtilityContext::lensOrigin(Vec3f org, Vec3f dir, float &toOrg) const {
    // Back along the ray to the lens plane, view z = 0.
    const Vec3f o = camera.worldToView.transformAffine(org);
    const Vec3f d = camera.worldToView.transformDir(dir);
    const float s = d.z != 0.0f ? -o.z / d.z : 0.0f; // view units along the (scaled) direction
    const Vec3f lens = org + dir * s;
    toOrg = length(org - lens);
    return lens;
}

void UtilityContext::value(const UtilityRequest &r, const UtilityHit &h, float *o) const {
    const SurfaceHit *hit = h.hit;
    switch (r.kind) {
    case UtilityKind::P:
        set3(o, h.P);
        break;
    case UtilityKind::PCamera:
        set3(o, camera.worldToView.transformAffine(h.P));
        break;
    case UtilityKind::N:
        set3(o, h.n);
        break;
    case UtilityKind::Ng:
        set3(o, h.ng);
        break;
    case UtilityKind::NCamera:
        set3(o, normalize(camera.worldToView.transformDir(h.n)));
        break;
    case UtilityKind::NgCamera:
        set3(o, normalize(camera.worldToView.transformDir(h.ng)));
        break;
    case UtilityKind::FacingN:
        o[0] = std::fabs(dot(h.n, h.dir));
        break;
    case UtilityKind::FacingNg:
        o[0] = std::fabs(dot(h.ng, h.dir));
        break;
    case UtilityKind::HitDist: // from the eye (Karma's ray:origin), not from the near plane
        o[0] = ortho ? camera.viewDepth(h.P) : length(h.P - eye);
        break;
    case UtilityKind::Face:
        if (hit) {
            // Curves: the curve, not its segment (Karma's ray:primid, M9 D3).
            const MeshDesc &d = scene.meshRecord(hit->mesh).desc;
            o[0] = d.isCurves() ? float(hit->prim < d.segmentCurve.size() ? d.segmentCurve[hit->prim] : 0u)
                   : float(hit->prim < d.triangleFaces.size() ? d.triangleFaces[hit->prim] : int32_t(hit->prim));
        }
        break;
    case UtilityKind::MotionVector:
    case UtilityKind::Velocity:
        if (hit) {
            const Vec3f P0 = hitPointAt(scene, *hit, h.uv, h.tHit, h.org, h.dir, 0.0f);
            const Vec3f P1 = hitPointAt(scene, *hit, h.uv, h.tHit, h.org, h.dir, 1.0f);
            if (r.kind == UtilityKind::Velocity) {
                set3(o, P1 - P0);
            } else {
                const Vec2f r0 = camera.worldToRaster(P0, 0.0f, viewToNdc);
                const Vec2f r1 = camera.worldToRaster(P1, 1.0f, viewToNdc);
                set3(o, {r1.x - r0.x, r1.y - r0.y, 0.0f});
            }
        }
        break;
    case UtilityKind::Primvar:
        if (hit) {
            const AttributeRef ref = scene.attributeRef(hit->mesh, r.name);
            const MeshDesc &d = scene.meshRecord(hit->mesh).desc;
            const uint32_t none[3] = {0, 0, 0};
            const uint32_t *tri = size_t(hit->prim) * 3 + 2 < d.indices.size() ? &d.indices[size_t(hit->prim) * 3] : none;
            fetchAttribute(ref, tri, hit->prim, hit->instance, hit->uv.x, hit->uv.y, 4, o);
        }
        break;
    case UtilityKind::BaseColor:
        if (h.inputs && h.inputs->model != SurfaceModel::External)
            set3(o, h.inputs->c(surface_field::baseColor));
        break;
    case UtilityKind::ShaderAov:
        if (h.inputs && hit) {
            const shading::Program *p = scene.material(h.material).desc.program.get();
            if (p)
                for (const auto &[name, field] : p->aovs)
                    if (name == r.name)
                        set3(o, h.inputs->c(field));
        }
        break;
    case UtilityKind::Ao: // per sample (traceAoRays)
        break;
    case UtilityKind::Mask:
        o[0] = 1.0f;
        break;
    case UtilityKind::HitStack:
        if (hit)
            o[0] = float(scene.meshRecord(hit->mesh).desc.userId + 1);
        break;
    case UtilityKind::Time:
        o[0] = h.time;
        break;
    case UtilityKind::Near:
    case UtilityKind::Far: {
        // The clipping planes' distances along the ray (Karma's ray:near and
        // ray:far: the planes' depths / cos for perspective cameras).
        const Vec3f d = camera.worldToView.transformDir(h.dir);
        const float len = length(d);
        const float cosine = len > 0.0f ? -d.z / len : 1.0f;
        const float depth = r.kind == UtilityKind::Near ? nearDepth : farDepth;
        o[0] = cosine > 0.0f ? depth / cosine : depth;
        break;
    }
    case UtilityKind::RayOrigin: {
        float toOrg = 0.0f;
        set3(o, lensOrigin(h.org, h.dir, toOrg));
        break;
    }
    case UtilityKind::RayDir:
        set3(o, h.dir);
        break;
    case UtilityKind::Contrib:
        set3(o, {1.0f, 1.0f, 1.0f});
        break;
    case UtilityKind::DPdz:
    case UtilityKind::Level:
        o[0] = 0.0f;
        break;
    case UtilityKind::HitUv:
        if (hit) {
            const Vec2f p = faceParameters(scene.meshRecord(hit->mesh).desc, hit->prim, h.uv);
            set3(o, {p.x, p.y, 0.0f});
        }
        break;
    case UtilityKind::NFacing:
        set3(o, faceForward(h.n, -h.dir));
        break;
    case UtilityKind::NgFacing:
        set3(o, faceForward(h.ng, -h.dir));
        break;
    case UtilityKind::PixelSamples:
        o[0] = float(h.sampleIndex + 1);
        break;
    case UtilityKind::Z:
        o[0] = camera.viewDepth(h.P);
        break;
    case UtilityKind::ObjectId:
        if (hit)
            o[0] = float(scene.meshRecord(hit->mesh).desc.userId);
        break;
    case UtilityKind::Beauty: // per sample from the path (accumulateCameraSamples)
    case UtilityKind::Lpe:    // per pass from the film (Renderer)
        break;
    default: // the texture baker's maps (core/bake/bake.h) are written by bakes, the outline planes by traceOutlines
        break;
    }
}

void accumulateCameraSamples(const Scene &scene, const Camera &camera, const ShadeGlobals &globals, Film &film,
                             const CameraSampleView &s) {
    // The planes fed here.
    thread_local std::vector<UtilityPlane *> planes;
    planes.clear();
    bool inputs = false;
    for (UtilityPlane &p : film.utility)
        if (p.request.filter != SampleFilter::Center && p.request.kind != UtilityKind::Lpe &&
            p.request.kind != UtilityKind::Ao) {
            planes.push_back(&p);
            inputs |= UtilityContext::needsInputs(p.request);
        }
    DeepPlane *deep = film.deep.empty() ? nullptr : &film.deep[0];
    if (deep)
        for (const UtilityRequest &e : deep->request.extras)
            inputs |= UtilityContext::needsInputs(e);
    if (planes.empty() && !deep)
        return;
    const UtilityContext ctx = UtilityContext::make(scene, camera);
    thread_local MaterialEvalScratch scratch;
    thread_local std::vector<float> extras;
    extras.assign(deep ? deep->stride - 5 : 0, 0.0f);

    for (uint32_t i = 0; i < s.count; ++i) {
        const uint32_t pix = s.pixel[i];
        const bool surface = s.mesh[i] != kInvalidId;
        SurfaceHit hit;
        UtilityHit h;
        h.org = s.org[i];
        h.dir = s.dir[i];
        h.time = s.time[i];
        h.sampleIndex = s.sampleIndex[i];
        HoldoutMode holdout = HoldoutMode::None;
        if (surface) {
            hit = scene.resolveHit(s.mesh[i], s.prim[i], s.uv[i], s.t[i], s.instance[i], s.time[i], s.org[i], s.dir[i]);
            h.hit = &hit;
            h.uv = s.uv[i];
            h.tHit = s.t[i];
            h.P = hit.P;
            h.n = hit.N;
            h.ng = hit.Ng;
            h.material = scene.materialIdOf(hit.mesh, hit.prim);
            holdout = scene.meshRecord(hit.mesh).holdout;
            if (inputs) {
                const SurfaceInputs *in = nullptr;
                evaluateSurfaceInputs(scene, h.material, &hit, &h.dir, &h.time, 1, &in, scratch, globals);
                h.inputs = in;
            }
        }
        const Spectrum L = isFinite(s.radiance[i]) ? s.radiance[i] : spectrum(0.0f);
        const float z = surface ? camera.viewDepth(hit.P) : 0.0f;
        for (UtilityPlane *p : planes) {
            float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (p->request.kind == UtilityKind::Beauty) {
                v[0] = L.x;
                v[1] = L.y;
                v[2] = L.z;
                v[3] = s.alpha[i];
                addFilteredSample(*p, pix, h.sampleIndex, v, true, z);
                continue;
            }
            const uint8_t only = p->request.holdouts;
            const bool counts = surface && (only == 0 || (only == 1) == (holdout == HoldoutMode::None));
            if (counts)
                ctx.value(p->request, h, v);
            addFilteredSample(*p, pix, h.sampleIndex, v, counts, z);
        }
        if (!deep)
            continue;
        // The deep sample at the first event (a surface, or a volume's
        // scatter in front of it); no event: at infinity, if it has colour
        // or coverage (a camera-visible environment).
        const DeepRequest &dr = deep->request;
        if (dr.excludeHoldouts && holdout != HoldoutMode::None)
            continue;
        const bool event = s.eventT[i] < kInf;
        const float a = s.alpha[i];
        if (!event && a == 0.0f && L.x == 0.0f && L.y == 0.0f && L.z == 0.0f)
            continue;
        float depth = FLT_MAX;
        if (event) {
            const Vec3f P = s.org[i] + s.dir[i] * s.eventT[i];
            if (dr.hitDistance) {
                float toOrg = 0.0f;
                depth = length(P - ctx.lensOrigin(s.org[i], s.dir[i], toOrg));
            } else {
                depth = camera.viewDepth(P);
            }
        }
        size_t e = 0;
        for (const UtilityRequest &r : dr.extras) {
            const int w = utilityWidth(r.kind);
            float v[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (surface)
                ctx.value(r, h, v);
            std::copy_n(v, w, extras.data() + e);
            e += size_t(w);
        }
        deep->add(pix, depth, L, a, extras.data());
    }
}

} // namespace nova
