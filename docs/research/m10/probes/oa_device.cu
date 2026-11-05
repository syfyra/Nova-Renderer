// OptiX accel probe, device programs (docs/research/m10/optix-accel.md).
// Compiled to OptiX-IR and PTX by oa_build.bat. Raygen programs read ray i
// from SoA buffers and write a hit record; nothing here shades.
#include <optix.h>

#include "oa_shared.h"

extern "C" {
__constant__ OaParams params;
}

namespace {

__forceinline__ __device__ float3 ld3(const float *p, unsigned i) { return make_float3(p[3 * i], p[3 * i + 1], p[3 * i + 2]); }
__forceinline__ __device__ float3 sub(float3 a, float3 b) { return make_float3(a.x - b.x, a.y - b.y, a.z - b.z); }
__forceinline__ __device__ float3 madd(float3 a, float3 b, float s) { return make_float3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
__forceinline__ __device__ float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Rays beyond the device-side count exit at once (wavefront queues launched
// at their capacity).
__forceinline__ __device__ bool rayIndex(unsigned &i) {
    i = optixGetLaunchIndex().x;
    const unsigned n = params.count ? *params.count : optixGetLaunchDimensions().x;
    return i < n;
}

__forceinline__ __device__ float rayTime(unsigned i) { return params.time ? params.time[i] : 0.0f; }

// Nova's hash (core/scene/scene.cpp hashMix).
__forceinline__ __device__ unsigned hashMix(unsigned h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// The two floats Nova keeps per hit: barycentrics (triangles), the curve
// parameter (curves), the ribbon parameters, or the custom attributes.
__forceinline__ __device__ float2 hitObjectUV(OptixPrimitiveType type) {
    switch (type) {
    case OPTIX_PRIMITIVE_TYPE_TRIANGLE: return optixHitObjectGetTriangleBarycentrics();
    case OPTIX_PRIMITIVE_TYPE_SPHERE: return make_float2(0.0f, 0.0f);
    case OPTIX_PRIMITIVE_TYPE_CUSTOM:
        return make_float2(__uint_as_float(optixHitObjectGetAttribute_0()), __uint_as_float(optixHitObjectGetAttribute_1()));
    case OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE: return optixHitObjectGetRibbonParameters();
    default: return make_float2(optixHitObjectGetCurveParameter(), 0.0f);
    }
}

__forceinline__ __device__ float2 hitUV(OptixPrimitiveType type) {
    switch (type) {
    case OPTIX_PRIMITIVE_TYPE_TRIANGLE: return optixGetTriangleBarycentrics();
    case OPTIX_PRIMITIVE_TYPE_SPHERE: return make_float2(0.0f, 0.0f);
    case OPTIX_PRIMITIVE_TYPE_CUSTOM:
        return make_float2(__uint_as_float(optixGetAttribute_0()), __uint_as_float(optixGetAttribute_1()));
    case OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE: return optixGetRibbonParameters();
    default: return make_float2(optixGetCurveParameter(), 0.0f);
    }
}

} // namespace

// Closest hit through a hit object: no closest-hit or miss program runs.
extern "C" __global__ void __raygen__hitobj() {
    unsigned i;
    if (!rayIndex(i))
        return;
    optixTraverse(params.handle, ld3(params.org, i), ld3(params.dir, i), params.tmin, params.tmax[i], rayTime(i),
                  params.mask, params.rayFlags, 0, 1, 0);
    if (!optixHitObjectIsHit()) {
        params.outPrim[i] = ~0u;
        return;
    }
    const float2 uv = hitObjectUV(optixGetPrimitiveType(optixHitObjectGetHitKind()));
    params.outT[i] = optixHitObjectGetRayTmax();
    params.outPrim[i] = optixHitObjectGetPrimitiveIndex();
    params.outInst[i] = optixHitObjectGetInstanceId();
    params.outUV[2 * i] = uv.x;
    params.outUV[2 * i + 1] = uv.y;
}

// Closest hit through payload registers set by a closest-hit program; no
// miss program (the payload starts as a miss).
extern "C" __global__ void __raygen__payload() {
    unsigned i;
    if (!rayIndex(i))
        return;
    unsigned p0 = 0, p1 = ~0u, p2 = 0, p3 = 0, p4 = 0;
    optixTrace(params.handle, ld3(params.org, i), ld3(params.dir, i), params.tmin, params.tmax[i], rayTime(i),
               params.mask, params.rayFlags, 0, 1, 0, p0, p1, p2, p3, p4);
    params.outPrim[i] = p1;
    if (p1 == ~0u)
        return;
    params.outT[i] = __uint_as_float(p0);
    params.outInst[i] = p2;
    params.outUV[2 * i] = __uint_as_float(p3);
    params.outUV[2 * i + 1] = __uint_as_float(p4);
}

extern "C" __global__ void __closesthit__payload() {
    const float2 uv = hitUV(optixGetPrimitiveType());
    optixSetPayload_0(__float_as_uint(optixGetRayTmax()));
    optixSetPayload_1(optixGetPrimitiveIndex());
    optixSetPayload_2(optixGetInstanceId());
    optixSetPayload_3(__float_as_uint(uv.x));
    optixSetPayload_4(__float_as_uint(uv.y));
}

// Occlusion: terminate on the first accepted hit, no closest hit; the miss
// program clears the flag.
extern "C" __global__ void __raygen__occl_trace() {
    unsigned i;
    if (!rayIndex(i))
        return;
    unsigned p0 = 1;
    optixTrace(params.handle, ld3(params.org, i), ld3(params.dir, i), params.tmin, params.tmax[i], rayTime(i),
               params.mask,
               params.rayFlags | OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT, 0, 1, 1, p0);
    params.outOccl[i] = (unsigned char)p0;
}

extern "C" __global__ void __miss__occl() { optixSetPayload_0(0); }

extern "C" __global__ void __raygen__occl_hitobj() {
    unsigned i;
    if (!rayIndex(i))
        return;
    optixTraverse(params.handle, ld3(params.org, i), ld3(params.dir, i), params.tmin, params.tmax[i], rayTime(i),
                  params.mask, params.rayFlags | OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT, 0, 1, 0);
    params.outOccl[i] = optixHitObjectIsHit() ? 1 : 0;
}

// Launch overhead: one store per thread, no trace.
extern "C" __global__ void __raygen__empty() {
    unsigned i;
    if (!rayIndex(i))
        return;
    params.outPrim[i] = i;
}

// Everything the attribute checks need, from the hit object.
extern "C" __global__ void __raygen__inspect() {
    const unsigned i = optixGetLaunchIndex().x;
    float *o = params.outInspect + size_t(i) * kInspectStride;
    for (int k = 0; k < kInspectStride; ++k)
        o[k] = 0.0f;
    optixTraverse(params.handle, ld3(params.org, i), ld3(params.dir, i), params.tmin, params.tmax[i], rayTime(i),
                  params.mask, params.rayFlags, 0, 1, 0);
    if (!optixHitObjectIsHit())
        return;
    const unsigned kind = optixHitObjectGetHitKind();
    const OptixPrimitiveType type = optixGetPrimitiveType(kind);
    const float2 uv = hitObjectUV(type);
    o[0] = 1.0f;
    o[1] = optixHitObjectGetRayTmax();
    o[2] = float(optixHitObjectGetPrimitiveIndex());
    o[3] = float(optixHitObjectGetInstanceId());
    o[4] = float(optixHitObjectGetInstanceIndex());
    o[5] = float(kind);
    o[6] = float(int(type) - int(OPTIX_PRIMITIVE_TYPE_CUSTOM));
    o[7] = optixIsFrontFaceHit(kind) ? 1.0f : 0.0f;
    o[8] = uv.x;
    o[9] = uv.y;
    if (type == OPTIX_PRIMITIVE_TYPE_FLAT_QUADRATIC_BSPLINE) {
        const float3 n = optixHitObjectGetRibbonNormal(uv);
        o[10] = n.x;
        o[11] = n.y;
        o[12] = n.z;
    }
    optixHitObjectGetObjectToWorldTransformMatrix(o + 13); // 13..24
}

// Any-hit programs: one that accepts everything, and Nova's stochastic
// opacity decision (scene.cpp OpacityFilter::accept) with a constant opacity.
extern "C" __global__ void __anyhit__nop() {}

__forceinline__ __device__ void opacityTest() {
    const unsigned i = optixGetLaunchIndex().x;
    const float seed = params.seed ? params.seed[i] : 0.5f;
    const unsigned mesh = optixGetInstanceId(), prim = optixGetPrimitiveIndex(), inst = optixGetInstanceIndex();
    const unsigned h = hashMix(__float_as_uint(seed) ^ hashMix(mesh * 0x9E3779B1u + hashMix(prim ^ hashMix(inst + 0x632BE5ABu))));
    if (float(h >> 8) * (1.0f / 16777216.0f) >= params.opacity)
        optixIgnoreIntersection();
}

extern "C" __global__ void __anyhit__opacity() { opacityTest(); }

// The same, counting the invocations per ray (not for timing).
extern "C" __global__ void __anyhit__opacity_count() {
    ++params.counter[optixGetLaunchIndex().x];
    opacityTest();
}

// Shadow linking as a filter: only instances whose bit is set in linkMask
// (by instance id, 0..63) block the ray (Nova: Scene::castsShadow).
extern "C" __global__ void __anyhit__shadowlink() {
    const unsigned id = optixGetInstanceId();
    if (id >= 64 || ((params.linkMask >> id) & 1ull) == 0)
        optixIgnoreIntersection();
}

// Custom points (Embree's point types, which OptiX lacks): a disc facing the
// ray, a disc facing its normal, or a sphere (to compare with the built-in
// sphere). Attributes: none (u = v = 0).
extern "C" __global__ void __intersection__disc() {
    const OaPointData *pd = reinterpret_cast<const OaPointData *>(optixGetSbtDataPointer());
    const unsigned prim = optixGetPrimitiveIndex();
    const float *s = pd->sphere + 4 * size_t(prim);
    const float3 center = make_float3(s[0], s[1], s[2]);
    const float r = s[3];
    const float3 o = optixGetObjectRayOrigin(), d = optixGetObjectRayDirection();
    const float3 n = pd->normal ? ld3(pd->normal, prim) : d;
    const float dn = dot(d, n);
    if (dn == 0.0f)
        return;
    const float t = dot(sub(center, o), n) / dn;
    if (!(t >= optixGetRayTmin() && t <= optixGetRayTmax()))
        return;
    const float3 p = sub(madd(o, d, t), center);
    if (dot(p, p) <= r * r)
        optixReportIntersection(t, 0, __float_as_uint(0.0f), __float_as_uint(0.0f));
}

extern "C" __global__ void __intersection__sphere() {
    const OaPointData *pd = reinterpret_cast<const OaPointData *>(optixGetSbtDataPointer());
    const float *s = pd->sphere + 4 * size_t(optixGetPrimitiveIndex());
    const float3 center = make_float3(s[0], s[1], s[2]);
    const float r = s[3];
    const float3 o = sub(optixGetObjectRayOrigin(), center), d = optixGetObjectRayDirection();
    // Hearn-Baker form: subtract the projection first for precision.
    const float dd = dot(d, d), b = dot(o, d) / dd;
    const float3 f = madd(o, d, -b);
    const float disc = r * r - dot(f, f);
    if (disc < 0.0f)
        return;
    const float q = sqrtf(disc * dd) / dd;
    const float t0 = -b - q, t1 = -b + q;
    const float tmin = optixGetRayTmin();
    if (t0 >= tmin && t0 <= optixGetRayTmax())
        optixReportIntersection(t0, 0, __float_as_uint(0.0f), __float_as_uint(0.0f));
    else if (t1 >= tmin && t1 <= optixGetRayTmax())
        optixReportIntersection(t1, 1, __float_as_uint(0.0f), __float_as_uint(0.0f));
}
