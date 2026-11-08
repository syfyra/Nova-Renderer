// OptiX device programs of the GPU backend (docs/design/M10.md D5): raygen
// programs over SoA ray queues that write Nova's hit records. Hits come from
// hit objects (optixTraverse), so no closest-hit or miss program runs
// (research oa 5.1). Compiled to OptiX-IR and embedded (core/CMakeLists.txt).

#include "core/bsdf/subsurface.h"
#include "core/gpu/device_shading.h"
#include "core/gpu/launch.h"
#include "core/math/rng.h"
#include "core/math/sobol.h"
#include "core/scene/motion.h"

#include <optix.h>

extern "C" {
__constant__ nova::gpu::TraceParams params;
}

namespace {

__forceinline__ __device__ bool rayIndex(unsigned &i) {
    i = optixGetLaunchIndex().x;
    const unsigned n = params.count ? *params.count : optixGetLaunchDimensions().x;
    return i < n;
}

__forceinline__ __device__ float3 f3(nova::Vec3f v) { return make_float3(v.x, v.y, v.z); }

__forceinline__ __device__ float rayTime(unsigned i) { return params.time ? params.time[i] : 0.0f; }

// The hit object's surface parameters: triangles their barycentrics, curves
// the segment's parameter (Embree's u); spheres and points nothing (hit.h
// works from t).
__forceinline__ __device__ nova::Vec2f hitUv() {
    const OptixPrimitiveType type = optixGetPrimitiveType(optixHitObjectGetHitKind());
    if (type == OPTIX_PRIMITIVE_TYPE_TRIANGLE) {
        const float2 b = optixHitObjectGetTriangleBarycentrics();
        return {b.x, b.y};
    }
    if (type != OPTIX_PRIMITIVE_TYPE_SPHERE && type != OPTIX_PRIMITIVE_TYPE_CUSTOM)
        return {optixHitObjectGetCurveParameter(), 0.0f};
    return {0.0f, 0.0f};
}

// A tessellated normal-oriented ribbon's triangle as Embree's curve hit
// (D5): triangle 2q of a segment spans (left, right) at u_q to right at
// u_q+1, triangle 2q + 1 (left u_q, right u_q+1, left u_q+1).
__forceinline__ __device__ void ribbonHit(unsigned mesh, unsigned &prim, nova::Vec2f &uv) {
    const unsigned quads = params.shapes[mesh].ribbonQuads;
    if (!quads)
        return;
    const unsigned local = prim % (2 * quads);
    const float along = local & 1u ? uv.x + uv.y : uv.y;
    prim /= 2 * quads;
    uv = {(float(local / 2) + along) / float(quads), 0.0f};
}

// The mesh of a custom primitive: the instance's, or in a walk (a GAS
// traced alone) the payload's.
__forceinline__ __device__ unsigned shapeMesh() { return params.walks ? optixGetPayload_0() : optixGetInstanceId(); }

} // namespace

// Closest hits (Embree's tnear 0: Nova offsets the origins).
extern "C" __global__ void __raygen__closest() {
    unsigned i;
    if (!rayIndex(i))
        return;
    optixTraverse(params.handle, f3(params.org[i]), f3(params.dir[i]), params.tMin, params.tMax[i], rayTime(i), 255,
                  OPTIX_RAY_FLAG_NONE, 0, 1, 0);
    if (!optixHitObjectIsHit()) {
        params.mesh[i] = 0xFFFFFFFFu;
        return;
    }
    const unsigned mesh = optixHitObjectGetInstanceId();
    unsigned prim = optixHitObjectGetPrimitiveIndex();
    nova::Vec2f uv = hitUv();
    if (optixGetPrimitiveType(optixHitObjectGetHitKind()) == OPTIX_PRIMITIVE_TYPE_TRIANGLE)
        ribbonHit(mesh, prim, uv);
    (params.hitT ? params.hitT : params.tMax)[i] = optixHitObjectGetRayTmax();
    params.mesh[i] = mesh;
    params.prim[i] = prim;
    params.uv[i] = uv;
    params.instance[i] = optixHitObjectGetInstanceIndex() - params.firstInstance[mesh];
}

// Occlusion: any hit before tMax.
extern "C" __global__ void __raygen__occluded() {
    unsigned i;
    if (!rayIndex(i))
        return;
    const unsigned c = params.category ? params.category[i] : 0xFFFFu;
    const unsigned mask = c < 7 ? 1u << c : 0x80u;
    if (params.probeBackground) {
        // Karma's bgoccludeshadow (core/integrator/holdout.h): a ray from a
        // background point that a background holdout occludes keeps its
        // light, unoccluded and untinted by fake-caustic objects
        // (kRayBackgroundLit); the any-hit filter sees only those holdouts.
        // Every such ray is probed: phase 1 skips fake-caustic blockers.
        if (!params.rayType || !(params.rayType[i] & nova::kRayFromBackground))
            return;
        optixTraverse(params.handle, f3(params.org[i]), f3(params.dir[i]), params.tMin, params.tMax[i], rayTime(i),
                      0xFFu, OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT, 0, 1, 0);
        if (optixHitObjectIsHit()) {
            params.occluded[i] = 0;
            params.backgroundLit[i] |= nova::kRayBackgroundLit;
        }
        return;
    }
    // Filtered meshes' any-hit decides (their instances allow it).
    optixTraverse(params.handle, f3(params.org[i]), f3(params.dir[i]), params.tMin, params.tMax[i], rayTime(i), mask,
                  OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT, 0, 1, 0);
    params.occluded[i] = optixHitObjectIsHit() ? 1 : 0;
}

// Subsurface walks (pass C'', walkSubsurface): each against its own mesh
// instance alone, the segments taken into object space (Scene::intersectLocal)
// and traced through the mesh's GAS without the filter (cut-outs do not stop
// a walk); the custom shapes' intersectors find the mesh in the payload.
extern "C" __global__ void __raygen__walk() {
    using namespace nova;
    unsigned i;
    if (!rayIndex(i))
        return;
    const auto &wk = *static_cast<const gpu::GpuWalks *>(params.walks);
    const unsigned mesh = wk.mesh[i], path = wk.path[i];
    const float time = wk.time[i];
    const gpu::MeshGas gas = params.gas[mesh];
    const Affine3f toObject = inverseAffine(instanceToWorld(wk.meshes[mesh].view, wk.instance[i], time));
    // The walk starts on the surface itself; its first segment skips the
    // entry surface with a minimum distance of the order of the hit point's
    // rounding.
    const Vec3f P = wk.P[i];
    const float scale = fmaxf(fmaxf(fabsf(P.x), fabsf(P.y)), fmaxf(fabsf(P.z), 1.0f));
    const float tMinEntry = 64.0f * 1.1920929e-7f * scale;
    const auto trace = [&](Vec3f org, Vec3f dir, float tMax, float &t, uint32_t &prim, Vec2f &uv) {
        const bool entry = org.x == P.x && org.y == P.y && org.z == P.z;
        unsigned payload = mesh;
        optixTraverse(gas.handle, f3(toObject.point(org)), f3(toObject.vector(dir)), entry ? tMinEntry : 0.0f, tMax,
                      time, 255, OPTIX_RAY_FLAG_DISABLE_ANYHIT, gas.sbtOffset, 1, 0, payload);
        if (!optixHitObjectIsHit())
            return false;
        t = optixHitObjectGetRayTmax();
        prim = optixHitObjectGetPrimitiveIndex();
        uv = hitUv();
        if (optixGetPrimitiveType(optixHitObjectGetHitKind()) == OPTIX_PRIMITIVE_TYPE_TRIANGLE)
            ribbonHit(mesh, prim, uv);
        return true;
    };
    const uint32_t walkStream = stream::bounce(wk.depth, stream::kWalk);
    const Sampler sampler{wk.pixelSeed[path], wk.sampleIndex[path]};
    Rng rng = Rng::make(wk.pixelSeed[path], wk.sampleIndex[path], walkStream);
    const WalkResult r =
        randomWalk(wk.medium[i], P, -wk.ng[i], sampler.get3D(walkStream), rng, kMaxWalkSegments, trace);
    wk.exited[i] = r.exited ? 1 : 0;
    if (!r.exited)
        return;
    wk.weight[i] = r.weight;
    wk.org[i] = r.org;
    wk.dir[i] = r.dir;
    wk.t[i] = r.t;
    wk.prim[i] = r.prim;
    wk.uv[i] = r.uv;
}

// A key array's value for primitive `prim` at the ray's time: one value for
// all (n == 1), one per primitive, or one per primitive and step (lerped as
// the keys of Embree's motion).
template <class T>
__forceinline__ __device__ T atRayTime(const T *v, uint32_t n, const nova::gpu::ShapeData &s, unsigned prim) {
    if (n == 1)
        return v[0];
    if (s.steps < 2 || n < s.count * s.steps)
        return v[prim];
    float f;
    const uint32_t k = nova::motionSegment(optixGetRayTime(), s.steps, f);
    const T a = v[size_t(k) * s.count + prim], b = v[size_t(k + 1) * s.count + prim];
    return a * (1.0f - f) + b * f;
}

// Custom spheres (analytic spheres and transmitting points, D5): the first
// intersection past tmin, the exit when the entry lies behind it (as
// Embree's sphere points, which glass needs) or when the any-hit filter
// ignored the entry (cut-out opacity sees the far side, as Embree offers
// the back hit to its filter).
extern "C" __global__ void __intersection__sphere() {
    const nova::gpu::ShapeData &s = params.shapes[shapeMesh()];
    const unsigned prim = optixGetPrimitiveIndex();
    const float3 o = optixGetObjectRayOrigin(), d = optixGetObjectRayDirection();
    const nova::Vec3f c = atRayTime(s.centers, s.count * s.steps, s, prim);
    const float r = atRayTime(s.radii, s.radiusCount, s, prim);
    const float ox = o.x - c.x, oy = o.y - c.y, oz = o.z - c.z;
    const float a = d.x * d.x + d.y * d.y + d.z * d.z;
    const float b = ox * d.x + oy * d.y + oz * d.z;
    const float cc = ox * ox + oy * oy + oz * oz - r * r;
    const float disc = b * b - a * cc;
    if (!(disc >= 0.0f) || !(a > 0.0f))
        return;
    const float q = sqrtf(disc);
    const float t0 = (-b - q) / a, t1 = (-b + q) / a;
    const float tmin = optixGetRayTmin(), tmax = optixGetRayTmax();
    if (t0 > tmin && t0 < tmax)
        optixReportIntersection(t0, 0);
    // An accepted entry lowered tmax to t0, which skips the exit; an ignored
    // one left it, so the exit is offered next.
    if (t1 > tmin && t1 < optixGetRayTmax())
        optixReportIntersection(t1, 1);
}

// Custom oriented discs (points facing their normals, D5).
extern "C" __global__ void __intersection__disc() {
    const nova::gpu::ShapeData &s = params.shapes[shapeMesh()];
    const unsigned prim = optixGetPrimitiveIndex();
    const float3 o = optixGetObjectRayOrigin(), d = optixGetObjectRayDirection();
    const nova::Vec3f c = atRayTime(s.centers, s.count * s.steps, s, prim);
    const nova::Vec3f n = atRayTime(s.normals, s.normalCount, s, prim);
    const float r = atRayTime(s.radii, s.radiusCount, s, prim);
    const float dn = d.x * n.x + d.y * n.y + d.z * n.z;
    if (dn == 0.0f)
        return;
    const float t = ((c.x - o.x) * n.x + (c.y - o.y) * n.y + (c.z - o.z) * n.z) / dn;
    if (!(t > optixGetRayTmin() && t < optixGetRayTmax()))
        return;
    const float px = o.x + d.x * t - c.x, py = o.y + d.y * t - c.y, pz = o.z + d.z * t - c.z;
    if (px * px + py * py + pz * pz <= r * r)
        optixReportIntersection(t, 0);
}

// The filter of filtered meshes (D7): Scene::OpacityFilter's decision
// (fake caustics, shadow-link categories past the masks, opacity).
extern "C" __global__ void __anyhit__filter() {
    const auto *f = static_cast<const nova::gpu::GpuFilter *>(params.filter);
    if (!f)
        return;
    const unsigned i = optixGetLaunchIndex().x;
    const unsigned mesh = optixGetInstanceId();
    unsigned prim = optixGetPrimitiveIndex();
    nova::Vec2f uv{0.0f, 0.0f};
    const OptixPrimitiveType type = optixGetPrimitiveType();
    if (type == OPTIX_PRIMITIVE_TYPE_TRIANGLE) {
        const float2 b = optixGetTriangleBarycentrics();
        uv = {b.x, b.y};
        ribbonHit(mesh, prim, uv);
    } else if (type != OPTIX_PRIMITIVE_TYPE_SPHERE && type != OPTIX_PRIMITIVE_TYPE_CUSTOM) {
        uv = {optixGetCurveParameter(), 0.0f};
    }
    const float seed = params.seed ? params.seed[i] : 0.5f;
    // The class in the low bits, the hit filter's flags in the high ones
    // (core/scene/visibility.h).
    const uint8_t raw = params.shadowClass ? uint8_t(nova::RayClass::Shadow)
                        : params.rayType   ? params.rayType[i]
                                           : uint8_t(0);
    const uint8_t rayType = raw & nova::kRayClassBits;
    const uint16_t category = params.category ? params.category[i] : uint16_t(0xFFFFu);
    // Ambient occlusion (skipFakeCaustics 0): acceptHit's occlusion rules
    // (fake caustics, shadow links) are a light sample's; its rays are
    // still shadow rays for the opacity programs, and carry no category.
    const bool occlusion = params.occlusion != 0;
    const bool lightShadow = occlusion && params.skipFakeCaustics != 0;
    if (!nova::gpu::acceptHit(f->scene, f->globals, params.depth, mesh, prim,
                              optixGetInstanceIndex() - params.firstInstance[mesh], uv, optixGetRayTmax(),
                              params.org[i], params.dir[i], rayTime(i), seed,
                              occlusion ? uint8_t(nova::RayClass::Shadow) : rayType, lightShadow, category,
                              uint8_t((raw & ~nova::kRayClassBits) |
                                      (params.probeBackground ? nova::kRayProbeBackground : 0u))))
        optixIgnoreIntersection();
}