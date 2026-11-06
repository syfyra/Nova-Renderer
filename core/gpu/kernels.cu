// The GPU wavefront kernels (docs/design/M10.md D8): one thread per path or
// ray, the passes of the CPU wavefront (wavefront.cpp) over the device queues
// of core/gpu/kernel_types.h. The OptiX ray queries between them are launched
// by the host (core/gpu/optix_scene.h). Queue lengths stay on the device; a
// kernel past a queue's length returns at once.
//
// G1 (docs/design/M10.md 4): triangle meshes, the uniform surface inputs of
// each material, the lights before the emissive triangles chosen by power,
// MIS, ray limits and Russian roulette, the beauty and alpha. Each step
// mirrors its CPU counterpart; the per-path steps both share live in
// core/integrator/path.h.
//
// Compiled to a fatbin and embedded (core/CMakeLists.txt), launched through
// the driver API (core/gpu/gpu_renderer.cpp).

#include "core/gpu/device_shading.h"
#include "core/gpu/kernel_types.h"
#include "core/integrator/path.h"
#include "core/math/frame.h"
#include "core/math/sampling.h"
#include "core/math/sobol.h"
#include "core/shading/vm.h"
#include "core/texture/ray_cone.h"

#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/SampleFromVoxels.h>

using namespace nova;
using namespace nova::gpu;

namespace {

__device__ __forceinline__ uint32_t threadIndex() { return blockIdx.x * blockDim.x + threadIdx.x; }

// A NaN/Inf contribution counts as black (addRadiance in wavefront.cpp).
__device__ __forceinline__ void addRadiance(const GpuWave &w, uint32_t p, const Spectrum &c) {
    if (isFinite(c))
        w.radiance[p] += c;
}

__device__ __forceinline__ uint32_t pushRay(const GpuRays &r, Vec3f o, Vec3f d, float t, uint32_t path, float time,
                                            float seed, RayClass type) {
    const uint32_t i = atomicAdd(r.count, 1u);
    r.org[i] = o;
    r.dir[i] = d;
    r.tMax[i] = t;
    r.time[i] = time;
    r.path[i] = path;
    r.filterSeed[i] = seed;
    r.rayType[i] = uint8_t(type);
    return i;
}

// The lights' selection probabilities at a path's last scattering vertex
// (MIS of its BSDF sample, M9 D11): the light tree's there, or the power
// table's; whether light i lights that vertex (light linking, D15).
struct PathPmf {
    const GpuScene &sc;
    Vec3f p, n;
    Receiver r;
    uint32_t set;
    NOVA_INLINE float operator()(uint32_t i) const {
        return sc.useTree ? sc.tree.pmf(i, p, n, r, set) : sc.lightSampler.pmf[i];
    }
    NOVA_INLINE bool links(uint32_t i) const { return sc.links.links(i, set); }
};
__device__ __forceinline__ PathPmf pathPmf(const GpuScene &sc, const GpuWave &w, uint32_t p) {
    return {sc, w.prevP[p], w.prevN[p], Receiver(w.prevReceiver[p]), prevLinkSet(w.prevSet[p])};
}

// --- Volumes (docs/design/M10.md G5) ------------------------------------------

// Field `index` of a volume at object-space point p (VolumeEvaluator::field):
// trilinear in its NanoVDB grid.
NOVA_INLINE float volumeField(const GpuVolume &v, int32_t index, Vec3f p) {
    if (index < 0 || size_t(index) >= v.grids.size() || !v.grids[size_t(index)])
        return 0.0f;
    const auto *grid = static_cast<const nanovdb::FloatGrid *>(v.grids[size_t(index)]);
    const Vec3f q = v.fieldObjectToIndex[size_t(index)].point(p);
    auto acc = grid->getAccessor();
    const nanovdb::SampleFromVoxels<decltype(acc), 1, true> sample(acc);
    return float(sample(nanovdb::Vec3f(q.x, q.y, q.z)));
}

// A volume's coefficients at object-space point p (world point w, the ray's
// direction, normalized time and class, the scattering events before):
// VolumeEvaluator::operator() on the device.
NOVA_INLINE VolumePoint evaluateVolume(const GpuVolume &v, const ShadeGlobals &globals, Vec3f p, Vec3f w, Vec3f dir,
                                       float time, uint8_t rayType, uint32_t depth) {
    VolumePoint out;
    // Velocity blur by lookup offset (M8 D17).
    Vec3f lookup = p;
    if (v.velocity[0] >= 0) {
        const float dt = v.velocityTime[0] + time * (v.velocityTime[1] - v.velocityTime[0]);
        lookup = p - Vec3f{volumeField(v, v.velocity[0], p), volumeField(v, v.velocity[1], p),
                           volumeField(v, v.velocity[2], p)} *
                         dt;
    }
    if (!v.program) { // the default volume (M8 D6)
        const float f = volumeField(v, v.reference, lookup), d = f < 0.0f ? 0.0f : f;
        out.scattering = {d, d, d};
        return out;
    }
    using namespace shading;
    float R[kGpuMaxRows];
    const float *U = v.uniforms;
    for (size_t i = 0; i < v.globals.size(); ++i) {
        const GlobalBinding &g = v.globals[i];
        Vec3f x{0.0f, 0.0f, 0.0f};
        float s = 0.0f;
        switch (g.global) {
        case Global::P:
        case Global::PDx:
        case Global::PDy:
            x = w;
            break;
        case Global::Pobject:
        case Global::PobjectDx:
        case Global::PobjectDy:
            x = p;
            break;
        case Global::I:
            x = dir;
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
            const Affine3f xf = v.toWorld(time);
            x = g.global == Global::ObjX ? xf.x : g.global == Global::ObjY ? xf.y : g.global == Global::ObjZ ? xf.z : xf.t;
            break;
        }
        case Global::RayType:
            s = float(rayType);
            break;
        case Global::RayDepth:
            s = float(depth);
            break;
        default: // normals and surface parameters mean nothing inside a volume
            break;
        }
        if (globalWidth(g.global) == 3) {
            R[g.row] = x.x;
            R[g.row + 1] = x.y;
            R[g.row + 2] = x.z;
        } else {
            R[g.row] = s;
        }
    }
    for (size_t i = 0; i < v.primvars.size(); ++i) {
        const PrimvarBinding &b = v.primvars[i];
        for (int k = 0; k < b.width; ++k) {
            const int32_t f = v.bindings[i * 4 + size_t(k)];
            R[b.row + k] = f >= 0 ? volumeField(v, f, lookup) : U[b.fallback + k];
        }
    }
    TextureContext tex;
    tex.device = v.textures;
    tex.count = v.textureCount;
    execute<1>(v.code, v.codeSize, R, U, &tex);
    VolumeInputs in = *v.defaults;
    for (size_t i = 0; i < v.outputs.size(); ++i) {
        const OutputBinding &o = v.outputs[i];
        if (!o.uniform)
            for (int k = 0; k < o.width; ++k)
                in.v[o.field + k] = R[o.index + k];
    }
    return volumePointOf(in, depth);
}

// The scene's volumes as medium_track.h's Media on the device; its lists
// hold up to 8 volumes along a ray (more are left out).
struct DeviceMedia {
    const GpuScene &sc;
    ShadeGlobals globals;
    NOVA_INLINE uint32_t activeCount() const { return sc.activeVolumeCount; }
    NOVA_INLINE VolumeId active(uint32_t i) const { return sc.activeVolumes[i]; }
    NOVA_INLINE MediumVolume volume(VolumeId id) const { return sc.volumes[id].medium; }
    NOVA_INLINE Affine3f volumeToWorld(VolumeId id, float time) const { return sc.volumes[id].toWorld(time); }
    NOVA_INLINE VolumePoint evaluate(VolumeId id, Vec3f p, Vec3f w, Vec3f d, float time, uint8_t type, uint32_t depth) {
        return evaluateVolume(sc.volumes[id], globals, p, w, d, time, type, depth);
    }
    NOVA_INLINE void violation(const Spectrum &, float) {}
};
using GpuSpans = FixedList<VolumeSpan, 8>;
using GpuActives = FixedList<MediumActive, 8>;
using GpuHits = FixedList<MediumHit, 8>;

// The transmittance in front of light `light` crossed at distance t by ray
// k (MediumAtten): the medium pass's crossings; without volumes 1.
struct DeviceAtten {
    const GpuWave &w;
    uint32_t k;
    bool media, split;
    NOVA_INLINE Spectrum operator()(uint32_t light, float t) const {
        if (!media)
            return {1.0f, 1.0f, 1.0f};
        const size_t b = size_t(k) * kGpuCrossings;
        for (size_t i = b, e = b + w.crossCount[k]; i < e; ++i)
            if (w.crossLight[i] == light && w.crossT[i] == t)
                return w.crossAtten[i];
        return split ? w.lightT[k] : w.mediumT[k];
    }
    // The first crossing of a light (camera-visible lights).
    NOVA_INLINE Spectrum ofLight(uint32_t light) const {
        if (!media)
            return {1.0f, 1.0f, 1.0f};
        const size_t b = size_t(k) * kGpuCrossings;
        for (size_t i = b, e = b + w.crossCount[k]; i < e; ++i)
            if (w.crossLight[i] == light)
                return w.crossAtten[i];
        return w.mediumT[k];
    }
};

// A point of a volume light (sampleVolumeLight, M8 D13): a cell in
// proportion to its emission bound, a uniform point in it (the third
// coordinate from the path's own generator), the emission there; in the
// measure of the tentative collisions that collect emission along a ray.
__device__ LightSample sampleVolumeLight(const GpuScene &sc, const GpuSettings &st, const GpuWave &w, uint32_t p,
                                         const LightData &light, Vec3f origin, float time, Vec2f u,
                                         uint32_t lightStream, uint32_t depth) {
    LightSample ls{};
    const GpuVolume &v = sc.volumes[light.volume];
    const Distribution1DView &cells = v.medium.lightCells;
    float pdf01 = 0.0f;
    uint32_t cell = 0;
    const float x = cells.sample(u.x, pdf01, cell);
    const float sigma = v.medium.majorant[cell];
    if (!(pdf01 > 0.0f) || !(sigma > 0.0f))
        return ls;
    const float uz = Rng::make(w.pixelSeed[p], w.sampleIndex[p], lightStream).next();
    const MajorantLayout &l = *v.medium.layout;
    const int32_t cx = int32_t(cell % uint32_t(l.res[0]));
    const int32_t cy = int32_t((cell / uint32_t(l.res[0])) % uint32_t(l.res[1]));
    const int32_t cz = int32_t(cell / (uint32_t(l.res[0]) * uint32_t(l.res[1])));
    const float k = float(l.k);
    const Vec3f index{float(l.lo[0]) + (float(cx) + (x * float(cells.count) - float(cell))) * k,
                      float(l.lo[1]) + (float(cy) + u.y) * k, float(l.lo[2]) + (float(cz) + uz) * k};
    const Vec3f object = v.indexToObject.point(index);
    const Vec3f world = v.toWorld(time).point(object);
    const Vec3f d = world - origin;
    const float r2 = dot(d, d);
    if (!(r2 > 0.0f))
        return ls;
    const float r = std::sqrt(r2);
    ls.wi = d * (1.0f / r);
    ls.distance = r;
    ls.Li = evaluateVolume(v, st.globals, object, world, ls.wi, time, uint8_t(RayClass::Shadow), depth).emission *
            (1.0f / sigma);
    ls.pdf = pdf01 / float(cells.count) / v.medium.cellVolume * r2 / sigma;
    return ls;
}

// The light a light sample draws (chooseLight): chosen by the light tree for
// the receiver at `origin` (normal nr, kind, light-linking set) or by power
// (an unlinked light rejected), with the sampler stream `lightStream`.
struct GpuLightChoice {
    uint32_t light = 0xFFFFFFFFu;
    float selectPdf = 0.0f;
    LightSample ls{};
};
__device__ GpuLightChoice chooseLight(const GpuScene &sc, const GpuSettings &st, const GpuWave &w, uint32_t p,
                                      float time, const Sampler &sampler, Vec3f origin, Vec3f nr, Receiver receiver,
                                      uint32_t set, uint32_t depth, uint32_t lightStream) {
    GpuLightChoice n;
    const Vec3f ul = sampler.get3D(lightStream);
    n.light = sc.useTree ? sc.tree.sample(ul.z, origin, nr, receiver, set, n.selectPdf)
                         : sc.lightSampler.sample(ul.z, n.selectPdf);
    if (n.light != kInvalidId && !sc.links.links(n.light, set))
        n.light = kInvalidId;
    if (n.light != kInvalidId)
        n.ls = sc.lights[n.light].type == LightType::Volume
                   ? sampleVolumeLight(sc, st, w, p, sc.lights[n.light], origin, time, {ul.x, ul.y}, lightStream, depth)
                   : sampleLight(sc.lights[n.light], origin, {ul.x, ul.y}, sc.links.categories(set));
    return n;
}

// The MIS densities of equiangular sampling (equiangularDensities, M9 D17)
// of light li at distance t on a ray's medium segment [a, b): the free
// flight's stand-in density times the light's selection probability pTree,
// and the equiangular strategy's density times the stand-in's transmittance.
__device__ void equiangularDensities(const GpuScene &sc, DeviceMedia &media, const MediumRay &ray, float a, float b,
                                     uint32_t li, float t, float pTree, float &pEq, float &pFree) {
    pEq = pFree = 0.0f;
    MediumPart parts[4];
    Spectrum extinction;
    mediumAt(media, ray, t, parts, 4, extinction);
    pFree = freeFlightProxy(extinction, a, t) * pTree;
    Equiangular e;
    if (t >= a && t < b && e.init(ray.o, ray.d, a, b, sc.lights[li].position)) {
        const float sigma = (extinction.x + extinction.y + extinction.z) * (1.0f / 3.0f);
        pEq = sc.eqPmf[li] * e.pdf(t) * std::exp(-sigma * nova::fmax(t - a, 0.0f));
    }
}

// Moves a site's LPE parts to shadow ray i, scaled by `scale`
// (lpeAttachParts), into the wave's arena; parts past its end are dropped.
__device__ void attachParts(const GpuWave &w, uint32_t i, const GpuLpeParts &parts, float scale) {
    const uint32_t n = parts.size();
    const uint32_t begin = n ? atomicAdd(w.partUsed, n) : 0u;
    if (n == 0 || begin + n > w.partCapacity) {
        w.partCount[i] = 0;
        return;
    }
    w.partBegin[i] = begin;
    w.partCount[i] = uint8_t(n);
    for (uint32_t j = 0; j < n; ++j) {
        w.partState[begin + j] = parts.state[j];
        w.partValue[begin + j] = parts.value[j] * scale;
        w.partUnit[begin + j] = parts.unitValue[j];
    }
}

// Equiangular sampling (equiangularSample, M9 D17) on ray k's medium segment
// [eqA, eqB), for a ray that may scatter: one light of those that opted in
// (by power), a distance in proportion to 1 / r^2 to it, and a light sample
// there with its shadow ray (kind 1). MIS against the free flight's light
// samples over the distance and, for lights with an area, against the phase
// function's samples per volume. Its own Rng stream.
__device__ void equiangularSample(const ShadeArgs &a, uint32_t k, const MediumRay &ray, DeviceMedia &media,
                                  GpuSpans &spans, GpuActives &active) {
    const GpuWave &w = a.wave;
    const GpuScene &sc = a.scene;
    const GpuSettings &st = a.settings;
    const uint32_t p = a.rays.path[k];
    Rng rng = Rng::make(w.pixelSeed[p], w.sampleIndex[p], stream::equiangular(a.depth));
    float select = 0.0f;
    const uint32_t slot = sc.eqTable.sample(rng.next(), select);
    if (slot == kInvalidId)
        return;
    const uint32_t li = sc.eqLights[slot];
    if (!sc.links.links(li, 0u)) // volume events are receiver set 0
        return;
    const LightData &light = sc.lights[li];
    const float eqA = w.eqA[k], eqB = w.eqB[k];
    Equiangular e;
    if (!e.init(ray.o, ray.d, eqA, eqB, light.position))
        return;
    const float t = e.sample(rng.next());
    if (!(t >= eqA && t < eqB))
        return; // rounding at the ends
    MediumPart parts[4];
    Spectrum extinction;
    const int n = mediumAt(media, ray, t, parts, 4, extinction);
    if (n == 0)
        return;
    const Vec3f x = ray.o + ray.d * t;
    const LightSample ls = sampleLight(light, x, rng.next2());
    if (!(ls.pdf > 0.0f) || isBlack(ls.Li))
        return;
    const float pTree = sc.useTree ? sc.tree.pmf(li, x, Vec3f{0.0f, 0.0f, 0.0f}, Receiver::None, 0u)
                                   : sc.lightSampler.pmf[li];
    float pEq = 0.0f, pFree = 0.0f;
    equiangularDensities(sc, media, ray, eqA, eqB, li, t, pTree, pEq, pFree);
    const float pSample = select * e.pdf(t);
    if (!(pEq > 0.0f) || !(pSample > 0.0f))
        return;
    const float wT = pEq * pEq / (pEq * pEq + pFree * pFree);
    const float lightPdf = pTree * ls.pdf;
    Spectrum f{0.0f, 0.0f, 0.0f}, fUnit{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; ++i) {
        const float ph = parts[i].phase.eval(ray.d, ls.wi);
        f += parts[i].scattering *
             (ph * lightSampleWeight(st.lightStrategy, lightPdf, ph, ls.delta || light.neeOnly()));
        fUnit += parts[i].scattering * ph;
    }
    // The throughput to the point: the medium as the ray's own flight sees
    // it, and an absorbing refractive interior.
    MediumRay to = ray;
    to.tEnd = t;
    Spectrum beta = w.beta[p] * trackMedia(media, spans, active, to, rng.next(), rng, nullptr);
    if (w.inMedium[p]) {
        const Spectrum s = w.sigmaA[p];
        beta *= Spectrum{std::exp(-s.x * t), std::exp(-s.y * t), std::exp(-s.z * t)};
    }
    const float scale = light.diffuse * wT / (pSample * ls.pdf);
    const Spectrum contribution = beta * f * ls.Li * scale;
    const LpeView &lpe = sc.lpe;
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    GpuLpeParts lpeParts;
    if (state && (!isBlack(contribution) || lpe.unit)) {
        const VolumeId v = parts[0].volume;
        const uint32_t tag = v < lpe.volumeTag.size() ? lpe.volumeTag[v] : 0u;
        const lpe::State scattered = lpeStep(lpe, state, lpe::TypeVolume, lpe::ScatterNone, tag, 0);
        if (scattered)
            lpePartsAdd(lpe, lpeParts, lpeLightStep(lpe, scattered, li), contribution, beta * fUnit * ls.Li * scale);
    }
    if (!isBlack(contribution) || (state && lpe.unit && lpeParts.size() > 0)) {
        const float clampNee = a.depth > 0 ? st.clampIndirect : 0.0f;
        const float seed = a.rays.filterSeed[k] + 0.75f;
        const uint32_t r = pushRay(w.shadow, x, ls.wi, shadowRayLength(ls.distance, sc.tolerance), p, ray.time,
                                   seed - std::floor(seed), RayClass::Shadow);
        w.shadowCategory[r] = light.shadowCategory;
        w.shadowSlot[r] = 1;
        if (light.type == LightType::Triangle) {
            w.shadowContribution[r] = contribution;
            w.shadowEmitter[r] = li;
            w.shadowBary[r] = ls.bary;
            w.shadowClamp[r] = clampNee;
        } else {
            w.shadowContribution[r] = clampContribution(contribution, clampNee);
            w.shadowEmitter[r] = kInvalidId;
        }
        if (w.partBegin)
            attachParts(w, r, lpeParts, light.type == LightType::Triangle ? 1.0f : clampScale(contribution, clampNee));
    }
}

// A shadow-linked dome's BSDF sample (pushShadowDomeRay, path.h, M9 D15): a
// shadow ray of its own (kind 3) along the non-delta continuation wi of an
// event whose light selection and links `pmf` gives.
__device__ void pushShadowDomeRay(const ShadeArgs &a, uint32_t p, const PathPmf &pmf, Vec3f o, Vec3f wi, float pdf,
                                  float diffuseShare, const Spectrum &beta, float time, float seed, lpe::State state,
                                  bool intoSolid) {
    const GpuScene &sc = a.scene;
    const GpuWave &w = a.wave;
    const float u = sc.shadowDomeCount > 1
                        ? Sampler{w.pixelSeed[p], w.sampleIndex[p]}.get1D(stream::shadowDome(a.depth))
                        : 0.0f;
    Spectrum Le;
    float weight = 0.0f;
    const uint32_t li =
        sampleShadowLinkedDome(sc.lights, sc.shadowDomes, sc.shadowDomeCount, pmf, wi,
                               LightFilterSite{pmf.p, sc.links.categories(pmf.set)}, pdf, a.settings.lightStrategy,
                               diffuseShare, u, Le, weight);
    if (li == kInvalidId)
        return;
    const Spectrum contribution = beta * Le;
    const LpeView &lpe = sc.lpe;
    GpuLpeParts parts;
    if (state) {
        // Weight-1 parts only for a dome that light sampling never picks.
        const bool unit = lpe.unit && !(sc.lightSampler.pmf[li] > 0.0f) && !intoSolid;
        lpePartsAdd(lpe, parts, lpeLightStep(lpe, state, li), contribution, unit ? contribution : spectrum(0.0f));
    }
    if (isBlack(contribution) && !(state && lpe.unit && parts.size() > 0))
        return;
    const float clampNee = a.depth > 0 ? a.settings.clampIndirect : 0.0f;
    const uint32_t r = pushRay(w.shadow, o, wi, kInf, p, time, seed, RayClass::Shadow);
    w.shadowCategory[r] = sc.lights[li].shadowCategory;
    w.shadowSlot[r] = 3;
    w.shadowContribution[r] = clampContribution(contribution, clampNee);
    w.shadowEmitter[r] = kInvalidId;
    if (w.partBegin)
        attachParts(w, r, parts, clampScale(contribution, clampNee));
}

// Pass C for one surface hit (shadeHit).
__device__ void shadeHit(const ShadeArgs &a, uint32_t k) {
    const GpuWave &w = a.wave;
    const GpuSettings &st = a.settings;
    const LightStrategy strategy = st.lightStrategy;
    const uint32_t p = a.rays.path[k];
    const float time = a.rays.time[k];
    const Vec3f dir = a.rays.dir[k];
    const Vec3f wo = -dir;
    const uint32_t meshId = w.hitMesh[k];
    const GpuMesh &mesh = a.scene.meshes[meshId];
    const SurfaceHit hit = resolveMeshHit(mesh.view, meshId, w.hitPrim[k], w.hitUv[k], a.rays.tMax[k],
                                          w.hitInstance[k], time, a.rays.org[k], dir);
    const Vec3f ng = faceForward(hit.Ng, wo);
    // Round curves: rays leave from the fibre's far side (M9 D4).
    const bool tube = hit.curve && mesh.view.desc.curveStyle == CurveStyle::Round;
    const auto leave = [&](Vec3f d) { return leaveHit(hit, ng, tube, d); };
    if (st.ao && a.depth == 0) {
        // The ambient occlusion AOV: one cosine-weighted ray around the
        // shading normal, unlimited; directions below the geometric surface
        // count as occluded.
        const Vec3f n = faceForward(hit.N, ng);
        const Vec2f u = Sampler{w.pixelSeed[p], w.sampleIndex[p]}.get2D(stream::kUtilityAo);
        const Vec3f d = Frame::fromNormal(n).toWorld(sampleCosineHemisphere(u));
        const float seed = a.rays.filterSeed[k] + 0.25f;
        const uint32_t r = pushRay(w.aoRays, leave(ng), d, kInf, p, time, seed - std::floor(seed), RayClass::Shadow);
        w.aoBelow[r] = dot(d, ng) <= 0.0f;
    }

    const BsdfTables tables = a.scene.tables;
    const ClosureContext ctx{&tables, a.scene.xyzToRgb};
    ClosureList cl;
    {
        const uint32_t material = materialIdOf(mesh, hit.prim);
        const GpuMaterial &m = a.scene.materials[material];
        // Texture footprints only where a texture is read or a derivative
        // taken (shadeHits).
        Vec3f footX{0.0f, 0.0f, 0.0f}, footY{0.0f, 0.0f, 0.0f};
        if (m.footprints && st.textureFilterWidth > 0.0f)
            coneFootprint(dir, hit.Ng, std::fabs(w.coneWidth[p] + w.coneSpread[p] * hit.t) * st.textureFilterWidth,
                          footX, footY);
        SurfaceInputs inputs;
        surfaceInputs(st, a.depth, mesh, m, material, hit, dir, time, a.rays.rayType[k], footX, footY, inputs);
        buildSurfaceClosure(inputs, hit, wo, ctx, cl);
    }

    const GpuScene &sc = a.scene;
    // Fake caustics after a diffuse event (M4 3.3): glossy lobes are skipped,
    // transmission becomes a straight pass-through with the shadow rays'
    // transmittance, so NEE through the object and BSDF paths through it
    // weigh the same.
    const bool fake = w.afterDiffuse[p] && mesh.fakeCaustics;
    Spectrum fakeT{0.0f, 0.0f, 0.0f};
    // Rays past a ray limit or the path length reach here only at emissive
    // triangles treated as lights and (light-only rays) at fake-caustic
    // objects (the gather).
    const bool lightsOnly = w.lightOnly[p] || (a.depth > 0 && a.depth >= st.maxDepth);
    const float clampEscape = a.depth > 1 ? st.clampIndirect : 0.0f;
    const float clampNee = a.depth > 0 ? st.clampIndirect : 0.0f;
    // Light path expressions (lpe_path.h): the path's state, the object's tag.
    const LpeView &lpe = sc.lpe;
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    const uint32_t tag = state ? lpe.objectTag(meshId, materialIdOf(mesh, hit.prim)) : 0u;
    if (state && lpe.albedo && !lightsOnly)
        lpeAlbedoEvents(lpe, film, pixel, state, tag, cl, w.beta[p]);
    // Emission, reached by BSDF sampling: an O event of weight 1, or for an
    // emissive triangle treated as a light (M9 D14) the light's event,
    // MIS-weighted against its light samples at the previous vertex and
    // scaled by its multipliers there (the camera sees it plain).
    const uint32_t emitter = sc.anyEmitters ? mesh.emitterLight(hit.instance, hit.prim) : kInvalidId;
    if (!isBlack(cl.emission) && (emitter != kInvalidId || !lightsOnly)) {
        Spectrum Le = cl.emission;
        if (emitter != kInvalidId && a.depth > 0) {
            const LightData &light = sc.lights[emitter];
            const float bsdfPdf = w.bsdfPdf[p];
            const float lightPdf = misNeedsPmf(strategy, bsdfPdf)
                                       ? pathPmf(sc, w, p)(emitter) * emitterPdf(light, w.prevP[p], hit.P)
                                       : 0.0f;
            Le *= lightMultiplier(light, w.diffuseShare[p]) * bsdfSampleWeight(strategy, bsdfPdf, lightPdf);
        }
        const Spectrum contribution = clampContribution(w.beta[p] * Le, clampEscape);
        addRadiance(w, p, contribution);
        // A treated emitter reached past the camera is MIS-weighted against
        // its light samples, whose weight-1 parts already feed unoccluded;
        // and shadow; (M6 D7): plain sums only, as area lights.
        if (state && emitter != kInvalidId && a.depth > 0)
            lpeDeposit(lpe, film, pixel, lpeLightStep(lpe, state, emitter), contribution);
        else if (state)
            lpeDepositAll(lpe, film, pixel,
                          emitter != kInvalidId ? lpeLightStep(lpe, state, emitter)
                                                : lpeStep(lpe, state, lpe::TypeObject, lpe::ScatterNone, tag, 0),
                          contribution);
    }
    if (lightsOnly && !(fake && w.lightOnly[p] && a.depth < st.maxDepth))
        return;
    if (fake) {
        fakeT = closureTransmittance(cl, wo, tables);
        if (w.lightOnly[p]) {
            // A light-only ray: through, still gathering lights only.
            if (!isBlack(fakeT)) {
                w.beta[p] *= fakeT;
                w.coneWidth[p] += w.coneSpread[p] * hit.t;
                pushRay(a.next, leave(-wo), -wo, kInf, p, time, a.rays.filterSeed[k], RayClass(a.rays.rayType[k]));
            }
            return;
        }
        keepDiffuseLobes(cl);
    }
    // With light path expressions, whether the lobes need their own values.
    const bool perLobe = state && lpeNeedsLobes(lpe, state, cl, tag);

    const Sampler sampler{w.pixelSeed[p], w.sampleIndex[p]};
    const Vec3f origin = leave(ng);
    const float shadowSeed = shadowRaySeed(a.rays.filterSeed[k]);

    // Next-event estimation with one light (chooseLight): chosen by the light
    // tree for the receiver at origin with the geometric normal, or by
    // power (an unlinked light rejected). Volume lights are not sampled yet
    // (G5): their share of the selection is skipped, which keeps the
    // estimate unbiased.
    const Vec3f nShade = ng;
    const bool nonDelta = cl.nonDeltaMask != 0;
    const Receiver receiver = nonDelta ? closureReceiver(cl) : Receiver::OneSided;
    const uint32_t receiverSet = nonDelta ? mesh.receiverSet(hit.instance) : 0u;
    if (nonDelta) {
        const GpuLightChoice choice = chooseLight(sc, st, w, p, time, sampler, origin, nShade, receiver, receiverSet,
                                                  a.depth, stream::bounce(a.depth, stream::kLight));
        const uint32_t li = choice.light;
        const float selectPdf = choice.selectPdf;
        if (li != kInvalidId) {
            const LightData &light = sc.lights[li];
            const LightSample &ls = choice.ls;
            if (ls.pdf > 0.0f) {
                Spectrum lobeF[ClosureList::kMaxLobes];
                uint32_t lobeMask = 0;
                const ClosureEval e =
                    perLobe ? evalClosureLobes(cl, wo, ls.wi, lobeF, lobeMask) : evalClosure(cl, wo, ls.wi);
                const Spectrum f = e.fcosDiffuse * light.diffuse + (e.fcos - e.fcosDiffuse) * light.specular;
                if (!isBlack(f)) {
                    const float lightPdf = selectPdf * ls.pdf;
                    const float weight = lightSampleWeight(strategy, lightPdf, e.pdf, ls.delta || light.neeOnly());
                    const Spectrum contribution = w.beta[p] * f * ls.Li * (weight / lightPdf);
                    // With unoccluded; or shadow; outputs the parts also carry
                    // MIS weight 1, and the shadow ray is traced for them even
                    // where the weight is 0.
                    GpuLpeParts parts;
                    if (state && (weight > 0.0f || lpe.unit))
                        lpeLightSampleParts(lpe, parts, state, tag, cl, lobeF, lobeMask, light, li,
                                            dot(ls.wi, ng) < 0.0f, dot(wo, hit.Ng) > 0.0f, f,
                                            w.beta[p] * ls.Li * (1.0f / lightPdf), weight);
                    if ((weight > 0.0f && !isBlack(contribution)) || (state && lpe.unit && parts.size() > 0)) {
                        Vec3f o = origin, d = ls.wi;
                        float dist = ls.distance;
                        if (dot(ls.wi, ng) < 0.0f) { // transmitted light
                            o = leave(ls.wi);
                            d = transmittedShadowRay(origin, o, ls.wi, dist);
                        }
                        const uint32_t i = pushRay(w.shadow, o, d, shadowRayLength(dist, sc.tolerance), p, time,
                                                   shadowSeed, RayClass::Shadow);
                        w.shadowCategory[i] = light.shadowCategory;
                        w.shadowSlot[i] = 0;
                        if (light.type == LightType::Triangle) { // the emission waits for the ray's end
                            w.shadowContribution[i] = contribution;
                            w.shadowEmitter[i] = li;
                            w.shadowBary[i] = ls.bary;
                            w.shadowClamp[i] = clampNee;
                        } else {
                            w.shadowContribution[i] = clampContribution(contribution, clampNee);
                            w.shadowEmitter[i] = kInvalidId;
                        }
                        if (w.partBegin) // the parts go with the ray (lpeAttachParts)
                            attachParts(w, i, parts,
                                        light.type == LightType::Triangle ? 1.0f : clampScale(contribution, clampNee));
                    }
                }
            }
        }
    }

    if (fake) {
        // Pass through, or sample the remaining (diffuse) lobes.
        const float lT = luminance(fakeT);
        // Without transmittance the diffuse continuation is certain: no pick
        // probability to divide by (and no 0 / 0).
        const float pass = cl.count == 0 ? 1.0f
                           : lT > 0.0f   ? hit_detail::clampf(lT / (lT + luminance(cl.albedo)), 0.05f, 0.95f)
                                         : 0.0f;
        if (lT > 0.0f && sampler.get1D(stream::bounce(a.depth, stream::kLayer)) < pass) {
            // A null event: no class, no MIS change, the cone continues.
            w.beta[p] *= fakeT * (1.0f / pass);
            w.coneWidth[p] += w.coneSpread[p] * hit.t;
            pushRay(a.next, leave(-wo), -wo, kInf, p, time, a.rays.filterSeed[k], RayClass(a.rays.rayType[k]));
            return;
        }
        if (cl.count == 0)
            return;
        w.beta[p] *= 1.0f / (1.0f - pass);
    }

    // Continue the path with a closure sample (with light path expressions,
    // the lobes' values there for the class choice).
    ClosureSample s;
    Spectrum lobeF[ClosureList::kMaxLobes];
    uint32_t lobeMask = 0;
    const Vec3f us = sampler.get3D(stream::bounce(a.depth, stream::kBsdf));
    if (!(perLobe ? sampleClosureLobes(cl, wo, us, s, lobeF, lobeMask) : sampleClosure(cl, wo, us, s)))
        return;
    const lpe::State next = state ? lpeScatter(lpe, a.depth, state, cl, s, lobeF, lobeMask, tag, sampler) : state;
    // Into the inside of a solid dielectric (as for light samples), before
    // thin-wall pass-throughs change s.
    const bool intoSolid = s.lobe >= 0 && cl.lobe[s.lobe].type == LobeType::Dielectric && refractsInto(cl) &&
                           (dot(wo, hit.Ng) > 0.0f ? s.transmit : !s.transmit);
    if (s.rayClass == RayClass::Diffuse || s.rayClass == RayClass::Sss)
        w.afterDiffuse[p] = 1;
    // Ray limits: within the limit of the event's class a full ray; past it
    // a light-only ray, or for refraction with thin walls a straight
    // pass-through up to twice the limit (Karma).
    bool passThrough = false;
    if (s.rayClass != RayClass::Volume) {
        const int cls = int(s.rayClass) - int(RayClass::Diffuse); // Diffuse, Reflect, Refract, Sss
        const RayLimitOverrides &o = mesh.rayLimits;
        const float limit = o.limit[cls] >= 0.0f ? o.limit[cls] : st.limits.limit[cls];
        const uint32_t allowed = pathLimit(limit, w.limitDither[p]);
        const uint32_t n = uint32_t(w.eventCounts[p] >> (8 * cls)) & 0xFFu;
        if (n < allowed) {
            // within the limit
        } else if (s.rayClass == RayClass::Refract && n < 2 * allowed &&
                   (s.lobe < 0 || cl.lobe[s.lobe].type != LobeType::Hair) &&
                   (o.thinWallAtRefractLimit >= 0 ? o.thinWallAtRefractLimit != 0
                                                  : st.limits.thinWallAtRefractLimit)) {
            passThrough = true;
        } else {
            w.lightOnly[p] = 1;
        }
        if (!w.lightOnly[p] && n < 255)
            w.eventCounts[p] += uint64_t(1) << (8 * cls);
    }
    if (s.walk) {
        // A subsurface walk (pass C''): one SSS event whose exit continues
        // the path; past the SSS limit the walk still runs and its exit
        // gathers lights only. Russian roulette waits for the exit's
        // throughput.
        const Spectrum beta = w.beta[p] * s.weight;
        if (isBlack(beta) || !isFinite(beta))
            return;
        const GpuWalks &wk = w.walks;
        const uint32_t j = atomicAdd(wk.count, 1u);
        if (j >= wk.capacity)
            return; // none expected in this scene: counted, the host makes room
        w.beta[p] = beta;
        if (state)
            w.lpeState[p] = next;
        wk.ray[j] = k;
        wk.path[j] = p;
        wk.time[j] = time;
        wk.mesh[j] = meshId;
        wk.instance[j] = hit.instance;
        wk.hitT[j] = hit.t;
        wk.P[j] = hit.P;
        wk.ng[j] = ng;
        wk.medium[j] = cl.lobe[s.lobe].subsurface;
        return;
    }
    if (passThrough) {
        s.wi = -wo;
        s.delta = true;
        s.pdf = 0.0f;
        s.refract = false;
        s.diffuseShare = 0.0f;
    }
    Spectrum beta = w.beta[p] * s.weight;
    float etaScale = w.etaScale[p];
    if (s.refract)
        etaScale *= s.eta * s.eta; // the BTDF scaled radiance by 1 / eta^2
    bool alive = !isBlack(beta) && isFinite(beta);
    if (alive && a.depth + 1 >= st.rrStartDepth) {
        const float survive = rouletteSurvival(beta, etaScale);
        if (sampler.get1D(stream::bounce(a.depth, stream::kRoulette)) < survive)
            beta *= 1.0f / survive;
        else
            alive = false;
    }
    if (!alive)
        return;
    w.beta[p] = beta;
    if (state) {
        w.lpeState[p] = next;
        if (next && lpe.unit && !intoSolid) { // as light samples: none into solid glass
            // Gathered at this hit with the receiver set the continuation
            // will carry (gatherSite()).
            const uint32_t set = !s.delta                          ? receiverSet
                                 : (w.prevSet[p] & kPrevSetDelta) ? mesh.receiverSet(hit.instance)
                                                                  : prevLinkSet(w.prevSet[p]);
            lpeUnoccludedEnvironment(lpe, sc.lights, sc.lightCount, sc.lightSampler.pmf, film, pixel, next, s.wi,
                                     LightFilterSite{origin, sc.links.categories(set)}, s.diffuseShare, beta,
                                     !s.delta);
        }
    }
    w.bsdfPdf[p] = s.delta ? -1.0f : s.pdf;
    w.diffuseShare[p] = s.diffuseShare;
    if (!s.delta) { // the MIS vertex: where this hit's light sample was chosen
        w.prevP[p] = origin;
        w.prevN[p] = nShade;
        w.prevReceiver[p] = uint8_t(receiver);
        w.prevSet[p] = receiverSet;
        if (sc.shadowDomeCount > 0)
            pushShadowDomeRay(a, p, PathPmf{sc, origin, nShade, receiver, receiverSet},
                              dot(s.wi, ng) >= 0.0f ? origin : leave(s.wi), s.wi, s.pdf, s.diffuseShare, beta, time,
                              domeRaySeed(a.rays.filterSeed[k]), next, intoSolid);
    } else if (w.prevSet[p] & kPrevSetDelta) {
        // No MIS vertex yet: light linking asks this delta surface.
        w.prevSet[p] = kPrevSetDelta | mesh.receiverSet(hit.instance);
    }
    w.etaScale[p] = etaScale;
    if (s.refract) {
        // Entering or leaving: by the authored orientation.
        const bool entering = dot(wo, hit.Ng) > 0.0f;
        w.inMedium[p] = entering && !isBlack(cl.sigmaA);
        w.sigmaA[p] = entering ? cl.sigmaA : spectrum(0.0f);
    }
    // Ray cone: the width at the hit, the spread widened by the curvature
    // (relative to the side the ray came from) and the sampled lobe's
    // roughness.
    const float width = w.coneWidth[p] + w.coneSpread[p] * hit.t;
    const float alpha = s.delta ? 0.0f : s.roughness;
    if (alpha >= 1.0f) {
        w.coneSpread[p] = nova::fmax(w.coneSpread[p], 1.0f);
    } else {
        const float side = dot(hit.Ng, wo) >= 0.0f ? 1.0f : -1.0f;
        w.coneSpread[p] = scatterSpread(w.coneSpread[p], width, meshCurvature(mesh.view, hit, time) * side,
                                        -std::fabs(dot(hit.Ng, wo)), alpha);
    }
    w.coneWidth[p] = width;
    pushRay(a.next, leave(s.wi), s.wi, kInf, p, time,
            sc.anyFiltered ? sampler.get1D(stream::bounce(a.depth + 1, stream::kOpacity)) : 0.0f, s.rayClass);
}

} // namespace

// Stage 1 (generateCameraRays): one camera ray per path slot; a pixel's
// samples are consecutive slots. The host sets the ray count.
extern "C" __global__ void novaGenerate(const GenerateArgs a) {
    const uint32_t slot = threadIndex();
    if (slot >= a.pixelCount * a.spp)
        return;
    const Camera &camera = *a.camera;
    const uint32_t pixel = a.firstPixel + slot / a.spp, j = slot % a.spp;
    const uint32_t pix = a.pixels ? a.pixels[pixel]
                                  : uint32_t(a.y0 + int(pixel / uint32_t(a.width))) * uint32_t(a.film.width) +
                                        uint32_t(a.x0 + int(pixel % uint32_t(a.width)));
    const int x = int(pix % uint32_t(a.film.width)), y = int(pix / uint32_t(a.film.width));
    const int ix = x + camera.imageOriginX, iy = y + camera.imageOriginY;
    const uint32_t sampleIndex = a.film.count[pix] + j;
    const Sampler sampler = Sampler::make(ix, iy, sampleIndex, a.seed);
    const Vec2f offset = a.filter->sample(sampler.get2D(stream::kPixelFilter));
    const float time = sampler.get1D(stream::kTime);
    const Ray ray = camera.generateRay(camera.rasterToNdc({float(ix) + 0.5f + offset.x, float(iy) + 0.5f + offset.y}),
                                       sampler.get2D(stream::kLens), time);
    const GpuWave &w = a.wave;
    w.pixel[slot] = pix;
    w.sampleIndex[slot] = sampleIndex;
    w.beta[slot] = spectrum(1.0f);
    w.radiance[slot] = spectrum(0.0f);
    w.alpha[slot] = 0.0f;
    w.pixelSeed[slot] = sampler.pixelSeed;
    w.bsdfPdf[slot] = 0.0f;
    w.diffuseShare[slot] = 1.0f;
    w.etaScale[slot] = 1.0f;
    w.sigmaA[slot] = spectrum(0.0f);
    w.inMedium[slot] = 0;
    w.eventCounts[slot] = 0;
    w.limitDither[slot] = sampler.get1D(stream::kLimit);
    w.lightOnly[slot] = 0;
    w.afterDiffuse[slot] = 0;
    // No MIS vertex yet (path.h, kPrevSetDelta): device memory is not
    // cleared, and the slot's earlier path must not leak in.
    w.prevP[slot] = ray.org;
    w.prevN[slot] = Vec3f{0.0f, 0.0f, 0.0f};
    w.prevReceiver[slot] = uint8_t(Receiver::None);
    w.prevSet[slot] = kPrevSetDelta;
    w.ao[slot] = 0.0f;
    w.cryptoMesh[slot] = kInvalidId;
    if (w.lpeState)
        w.lpeState[slot] = a.lpeStart; // after the camera event
    // The cone of the pixel's tile (generateCameraRays: the tile's center).
    const Vec2f cone = a.tileCones[((y - a.y0) / 32) * a.tilesX + (x - a.x0) / 32];
    w.coneWidth[slot] = cone.x;
    w.coneSpread[slot] = cone.y;
    const GpuRays &r = a.rays;
    r.org[slot] = ray.org;
    r.dir[slot] = ray.dir;
    r.tMax[slot] = ray.tMax;
    r.time[slot] = time;
    r.path[slot] = slot;
    r.filterSeed[slot] = a.filterSeeds ? sampler.get1D(stream::bounce(0, stream::kOpacity)) : 0.0f;
    r.rayType[slot] = uint8_t(RayClass::Camera);
}

// Before pass A (traceMedia, docs/design/M8.md D9): the volumes along the
// current rays. Each ray's segment gets a free flight that may end in a
// scattering event; its weight waits in mediumT for pass A, with the weight
// in front of each area light the ray crosses and, for camera rays, the
// transmittance for alpha. The emission the ray collects is added here.
// Light-only rays and rays past the path length only gather lights: ratio
// tracking. With level-dependent volumes a continuation's lights get a
// ratio-tracked transmittance of their own (lightT).
extern "C" __global__ void novaMedia(const ShadeArgs a) {
    const uint32_t k = threadIndex();
    if (k >= *a.rays.count)
        return;
    const GpuWave &w = a.wave;
    const GpuScene &sc = a.scene;
    const GpuSettings &st = a.settings;
    const uint32_t p = a.rays.path[k];
    const bool miss = w.hitMesh[k] == kInvalidId;
    const Vec3f org = a.rays.org[k], dir = a.rays.dir[k];
    float crossT[kGpuCrossings];
    uint32_t crossLight[kGpuCrossings];
    Spectrum crossOut[kGpuCrossings];
    int crossings = 0;
    if (sc.hasAreaLights) {
        const float tLights = miss ? a.rays.tMax[k] : lightSearchLimit(a.rays.tMax[k], sc.tolerance);
        crossings = areaLightCrossings(sc.lights, sc.lightCount, sc.useTree ? &sc.tree : nullptr, org, dir, tLights,
                                       crossT, crossLight, kGpuCrossings);
        for (int i = 1; i < crossings; ++i) // by distance
            for (int j = i; j > 0 && crossT[j - 1] > crossT[j]; --j) {
                const float t = crossT[j - 1];
                crossT[j - 1] = crossT[j];
                crossT[j] = t;
                const uint32_t l = crossLight[j - 1];
                crossLight[j - 1] = crossLight[j];
                crossLight[j] = l;
            }
    }
    MediumRay ray;
    ray.o = org;
    ray.d = dir;
    ray.tEnd = a.rays.tMax[k];
    ray.time = a.rays.time[k];
    ray.rayType = a.rays.rayType[k];
    ray.depth = a.depth;
    w.eqA[k] = w.eqB[k] = 0.0f;
    const uint32_t stream = stream::bounce(a.depth, stream::kMedium);
    const float u0 = Sampler{w.pixelSeed[p], w.sampleIndex[p]}.get1D(stream);
    Rng rng = Rng::make(w.pixelSeed[p], w.sampleIndex[p], stream);
    // Emission: of volume lights MIS-weighted against their light samples,
    // with the lights' multipliers past the camera (M8 D13).
    EmissionGather gather;
    gather.strategy = st.lightStrategy;
    gather.lightData = sc.lights;
    gather.tree = sc.useTree ? &sc.tree : nullptr;
    gather.tablePmf = sc.lightSampler.pmf;
    gather.prevP = w.prevP[p];
    gather.prevN = w.prevN[p];
    gather.prevReceiver = Receiver(w.prevReceiver[p]);
    gather.prevSet = prevLinkSet(w.prevSet[p]);
    gather.links = a.depth > 0 ? sc.links : LightLinks{}; // the camera is not a receiver
    if (a.depth > 0) {
        gather.bsdfPdf = w.bsdfPdf[p];
        gather.diffuseShare = w.diffuseShare[p];
        gather.multipliers = true;
    }
    EmissionGather lightGather = gather; // lights of the split transmittance
    DeviceMedia media{sc, st.globals};
    GpuSpans spans;
    GpuActives active;
    w.volumeHit[k] = 0;
    const bool split = a.depth > 0 && sc.levelDependent;
    if (!w.lightOnly[p] && a.depth < st.maxDepth) {
        if (sc.eqTable.count > 0 && mediumExtent(media, spans, ray, w.eqA[k], w.eqB[k]))
            equiangularSample(a, k, ray, media, spans, active);
        const int allCrossings = crossings;
        gather.lights = !split;
        GpuHits hits;
        Spectrum alphaT;
        const MediumFlight f = flyMedia(media, spans, active, hits, ray, w.beta[p], u0, rng, &gather,
                                        a.depth == 0 ? &alphaT : nullptr, crossT, crossOut, crossings);
        w.mediumT[k] = f.weight;
        if (a.depth == 0)
            w.mediumAlpha[k] = alphaT;
        if (f.scattered) {
            w.volumeHit[k] = 1;
            w.volumeT[k] = f.t;
            w.volumeId[k] = f.volume;
            w.volumePhase[k] = f.phase;
            crossings = f.crossings; // the lights in front of the event
        }
        if (split) {
            // Lights along the whole segment, as the light sample's shadow
            // ray of the event that spawned this ray sees them (one level
            // lower), whether or not the flight scattered.
            MediumRay light = ray;
            --light.depth;
            const uint32_t ls = stream::mediumLight(a.depth);
            Rng lrng = Rng::make(w.pixelSeed[p], w.sampleIndex[p], ls);
            lightGather.objects = false;
            w.lightT[k] = trackMedia(media, spans, active, light, Sampler{w.pixelSeed[p], w.sampleIndex[p]}.get1D(ls),
                                     lrng, &lightGather, crossT, crossOut, allCrossings);
            crossings = allCrossings;
        }
    } else {
        // These rays only gather lights: the light sample's partner under
        // MIS, so the shaders see them like that sample's shadow ray, at the
        // level of the event that spawned them.
        if (ray.depth > 0)
            --ray.depth;
        gather.objects = false;
        const Spectrum T = trackMedia(media, spans, active, ray, u0, rng, &gather, crossT, crossOut, crossings);
        w.mediumT[k] = w.mediumAlpha[k] = w.lightT[k] = T;
    }
    w.crossCount[k] = uint8_t(crossings);
    for (int i = 0; i < crossings; ++i) {
        const size_t c = size_t(k) * kGpuCrossings + size_t(i);
        w.crossLight[c] = crossLight[i];
        w.crossT[c] = crossT[i];
        w.crossAtten[c] = crossOut[i];
    }
    // The emission collected: volumes as O events, volume lights as L events
    // (seen from the camera they count for every output).
    const float clampEscape = a.depth > 1 ? st.clampIndirect : 0.0f;
    const LpeView &lpe = sc.lpe;
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    if (!isBlack(gather.objectSum)) {
        const Spectrum contribution = clampContribution(w.beta[p] * gather.objectSum, clampEscape);
        addRadiance(w, p, contribution);
        if (state)
            lpeDepositAll(lpe, film, pixel, lpeStep(lpe, state, lpe::TypeObject, lpe::ScatterNone, 0, 0), contribution);
    }
    const EmissionGather *gathers[2] = {&gather, &lightGather};
    for (const EmissionGather *g : gathers)
        for (int s = 0; s < g->lightCount; ++s) {
            const Spectrum contribution = clampContribution(w.beta[p] * g->lightSum[s], clampEscape);
            addRadiance(w, p, contribution);
            if (!state)
                continue;
            const lpe::State n = lpeLightStep(lpe, state, g->lightIndex[s]);
            if (a.depth == 0)
                lpeDepositAll(lpe, film, pixel, n, contribution);
            else
                lpeDeposit(lpe, film, pixel, n, contribution);
        }
}

// Stage 3 (gatherLights, then shadeHit for the survivors): per ray of this
// depth, the lights it reaches and the shading of its hit.
extern "C" __global__ void novaShade(const ShadeArgs a) {
    const uint32_t k = threadIndex();
    if (k >= *a.rays.count)
        return;
    const GpuWave &w = a.wave;
    const GpuScene &sc = a.scene;
    const uint32_t p = a.rays.path[k];
    const Vec3f dir = a.rays.dir[k];
    const bool miss = w.hitMesh[k] == kInvalidId;
    const float tHit = a.rays.tMax[k]; // the ray's tMax at a miss
    // A scattering event in a volume (novaMedia) ends the segment: the
    // surface behind it is not reached, the event goes to pass C'.
    const bool media = sc.activeVolumeCount > 0;
    const bool event = media && w.volumeHit[k];
    // Absorption along the segment inside a medium (Beer). A ray that leaves
    // the scene from inside an absorbing medium is lost.
    if (w.inMedium[p]) {
        const Spectrum s = w.sigmaA[p];
        const float t = event ? w.volumeT[k] : miss ? kInf : tHit;
        w.beta[p] *= Spectrum{s.x > 0.0f ? std::exp(-s.x * t) : 1.0f, s.y > 0.0f ? std::exp(-s.y * t) : 1.0f,
                              s.z > 0.0f ? std::exp(-s.z * t) : 1.0f};
    }
    // Volumes along the segment: lights crossed on the way take the
    // transmittance in front of them, everything at its end the whole.
    const bool split = media && a.depth > 0 && sc.levelDependent;
    const DeviceAtten atten{w, k, media, split};
    const Spectrum betaIn = w.beta[p];
    if (media)
        w.beta[p] *= w.mediumT[k];
    const auto pushEvent = [&] { w.volumeShade[atomicAdd(w.volumeShadeCount, 1u)] = k; };
    // Light path expressions: each light's share goes to its own event.
    const LpeView &lpe = sc.lpe;
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    if (a.depth == 0) {
        // Camera rays see camera-visible lights only, which are opaque.
        if (sc.hasVisibleAreaLights) {
            // Ties with the surface hit use the hit triangle's extent.
            const bool surface = !miss && !event;
            const float extent = !surface ? 0.0f
                                          : resolveMeshHit(sc.meshes[w.hitMesh[k]].view, w.hitMesh[k], w.hitPrim[k],
                                                           w.hitUv[k], tHit, w.hitInstance[k], a.rays.time[k],
                                                           a.rays.org[k], dir)
                                                .extent;
            Spectrum Le;
            uint32_t light = 0;
            if (cameraAreaLight(sc.lights, sc.lightCount, a.rays.org[k], dir, event ? w.volumeT[k] : tHit, surface,
                                extent, Le, light)) {
                const Spectrum contribution = betaIn * Le * atten.ofLight(light);
                addRadiance(w, p, contribution);
                if (state)
                    lpeDepositAll(lpe, film, pixel, lpeLightStep(lpe, state, light), contribution);
                w.alpha[p] += 1.0f;
                return;
            }
        }
        // Alpha: surfaces and visible environments are opaque, otherwise the
        // volumes' opacity over the whole camera segment.
        const auto volumeAlpha = [&] {
            const Spectrum T = w.mediumAlpha[k];
            return 1.0f - (T.x + T.y + T.z) * (1.0f / 3.0f);
        };
        if (event) {
            float alpha = 1.0f;
            if (miss) {
                bool visible = false;
                cameraEnvironment(sc.lights, sc.lightCount, a.rays.org[k], dir, visible);
                alpha = visible ? 1.0f : volumeAlpha();
            } else if (a.settings.crypto) { // volumes are transparent to cryptomattes
                w.cryptoMesh[p] = w.hitMesh[k];
                w.cryptoPrim[p] = w.hitPrim[k];
            }
            w.alpha[p] += alpha;
            pushEvent();
            return;
        }
        if (miss) {
            const Spectrum beta = w.beta[p];
            if (state) // B: the ray left the scene (D8)
                lpeDeposit(lpe, film, pixel, lpe.program.step(state, lpe.backgroundSymbol), beta);
            bool visible = false;
            const Spectrum Le = state ? cameraEnvironment(sc.lights, sc.lightCount, a.rays.org[k], dir, visible,
                                                          [&](uint32_t i, const Spectrum &Li) {
                                                              lpeDepositAll(lpe, film, pixel,
                                                                            lpeLightStep(lpe, state, i), beta * Li);
                                                          })
                                      : cameraEnvironment(sc.lights, sc.lightCount, a.rays.org[k], dir, visible);
            addRadiance(w, p, beta * Le);
            w.alpha[p] += visible ? 1.0f : media ? volumeAlpha() : 0.0f;
            return;
        }
        w.alpha[p] += 1.0f; // surfaces are opaque to alpha (transmission included)
        if (a.settings.crypto) {
            w.cryptoMesh[p] = w.hitMesh[k];
            w.cryptoPrim[p] = w.hitPrim[k];
        }
    } else {
        // Rays scattered by a surface collect the area lights they crossed
        // and, on escape, the lights at infinity.
        // With light path expressions each light's share is a part of its
        // own (its light event); the parts take the clamp of the sum.
        const float clampEscape = a.depth > 1 ? a.settings.clampIndirect : 0.0f;
        const PathPmf pmf = pathPmf(sc, w, p);
        // Where the ray gathers lights, for their filters (M11-P4).
        const LightFilterSite site =
            gatherSite(w.bsdfPdf[p], w.prevP[p], a.rays.org[k], sc.links.categories(pmf.set));
        GpuLpeParts parts;
        const auto addPart = [&](uint32_t i, const Spectrum &Li) {
            lpePartsAdd(lpe, parts, lpeLightStep(lpe, state, i), Li);
        };
        if (sc.hasAreaLights) {
            const float tEnd = event && !split ? w.volumeT[k] : miss ? tHit : lightSearchLimit(tHit, sc.tolerance);
            const LightTreeView *tree = sc.useTree ? &sc.tree : nullptr;
            // The lights' densities from the MIS vertex (prevP), which a
            // fake-caustic pass-through leaves behind the ray.
            const Spectrum Le =
                state ? areaLightsAlongRay(sc.lights, pmf, sc.lightCount, tree, a.rays.org[k], dir, tEnd, w.prevP[p],
                                           site, w.bsdfPdf[p], a.settings.lightStrategy, w.diffuseShare[p], addPart,
                                           atten)
                      : areaLightsAlongRay(sc.lights, pmf, sc.lightCount, tree, a.rays.org[k], dir, tEnd, w.prevP[p],
                                           site, w.bsdfPdf[p], a.settings.lightStrategy, w.diffuseShare[p],
                                           NoLightSink{}, atten);
            if (!isBlack(Le)) {
                const Spectrum contribution = betaIn * Le;
                const float scale = clampScale(contribution, clampEscape);
                addRadiance(w, p, contribution * scale);
                if (state)
                    lpeDepositParts(lpe, film, parts, pixel, betaIn * scale);
            }
        }
        if (miss && (split || !event)) {
            const Spectrum beta = split ? betaIn * w.lightT[k] : w.beta[p];
            Spectrum Le;
            if (state) {
                lpeDeposit(lpe, film, pixel, lpe.program.step(state, lpe.backgroundSymbol), beta);
                // Lights at infinity that light sampling never picks give the
                // lit shadow; estimate here with weight 1.
                parts.clear();
                Le = escapedRadiance(sc.lights, pmf, sc.lightCount, dir, site, w.bsdfPdf[p], a.settings.lightStrategy,
                                     w.diffuseShare[p], [&](uint32_t i, const Spectrum &Li) {
                                         const lpe::State s = lpeLightStep(lpe, state, i);
                                         lpePartsAdd(lpe, parts, s, Li);
                                         if (lpe.unit && !(sc.lightSampler.pmf[i] > 0.0f))
                                             lpeDepositUnit(lpe, film, pixel, s, beta * Li, false, true);
                                     });
            } else {
                Le = escapedRadiance(sc.lights, pmf, sc.lightCount, dir, site, w.bsdfPdf[p], a.settings.lightStrategy,
                                     w.diffuseShare[p]);
            }
            const Spectrum contribution = beta * Le;
            const float scale = clampScale(contribution, clampEscape);
            addRadiance(w, p, contribution * scale);
            if (state)
                lpeDepositParts(lpe, film, parts, pixel, beta * scale);
        }
        if (event) {
            pushEvent();
            return;
        }
        if (miss)
            return;
        // Past the path length or a ray limit: lights only, the hit is neither
        // shaded nor emits, unless it is an emissive triangle treated as a
        // light (M9 D14), whose emission shadeHit adds alone. A light-only
        // ray crosses fake-caustic objects as a shadow ray would (shadeHit).
        const GpuMesh &hitMesh = sc.meshes[w.hitMesh[k]];
        const bool emitter = sc.anyEmitters && hitMesh.emitterLight(w.hitInstance[k], w.hitPrim[k]) != kInvalidId;
        if (a.depth >= a.settings.maxDepth && !emitter)
            return;
        if (w.lightOnly[p] && !emitter && !(w.afterDiffuse[p] && hitMesh.fakeCaustics))
            return;
    }
    shadeHit(a, k);
}

// Pass C'' (walkSubsurface): at a walk's exit (the walk raygen ran it) a
// Lambert lobe on the shading normal facing out of the medium, weight the
// walk's throughput: one light sample (its own stream; the light's diffuse
// multiplier; shadow slot 2) and a cosine continuation of class SSS. The
// walk is the one SSS event of the entry: the exit adds no light path
// expression event.
extern "C" __global__ void novaWalkExit(const ShadeArgs a) {
    const uint32_t i = threadIndex();
    const GpuWave &w = a.wave;
    const GpuWalks &wk = w.walks;
    if (i >= *wk.count || i >= wk.capacity || !wk.exited[i])
        return;
    const uint32_t k = wk.ray[i], p = wk.path[i];
    const float time = wk.time[i];
    Spectrum beta = w.beta[p] * wk.weight[i];
    if (isBlack(beta) || !isFinite(beta))
        return;
    const GpuScene &sc = a.scene;
    const GpuSettings &st = a.settings;
    const uint32_t meshId = wk.mesh[i], instance = wk.instance[i];
    const GpuMesh &mesh = sc.meshes[meshId];
    const Vec3f dir = wk.dir[i];
    const SurfaceHit hit =
        resolveMeshHit(mesh.view, meshId, wk.prim[i], wk.uv[i], wk.t[i], instance, time, wk.org[i], dir);
    const Vec3f ng = faceForward(hit.Ng, dir); // out of the medium
    const Vec3f n = faceForward(hit.N, ng);
    const Vec3f origin = offsetRayOrigin(hit.P, ng);
    const Sampler sampler{w.pixelSeed[p], w.sampleIndex[p]};
    const LpeView &lpe = sc.lpe;
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    const uint32_t receiverSet = mesh.receiverSet(instance);
    const float clampNee = a.depth > 0 ? st.clampIndirect : 0.0f;

    // Next-event estimation with the exit's Lambert lobe.
    const GpuLightChoice choice = chooseLight(sc, st, w, p, time, sampler, origin, ng, Receiver::OneSided, receiverSet,
                                              a.depth, stream::walkLight(a.depth));
    if (choice.light != kInvalidId && choice.ls.pdf > 0.0f) {
        const uint32_t li = choice.light;
        const LightData &light = sc.lights[li];
        const LightSample &ls = choice.ls;
        const float cosI = dot(ls.wi, n);
        if (cosI > 0.0f && dot(ls.wi, ng) > 0.0f) {
            const float lightPdf = choice.selectPdf * ls.pdf;
            const float weight = lightSampleWeight(st.lightStrategy, lightPdf, cosineHemispherePdf(cosI),
                                                   ls.delta || light.neeOnly());
            const Spectrum unit = beta * ls.Li * light.diffuse * (cosI * kInvPi / lightPdf);
            const Spectrum contribution = unit * weight;
            GpuLpeParts parts;
            if (state && (weight > 0.0f || lpe.unit))
                lpePartsAdd(lpe, parts, lpeLightStep(lpe, state, li), contribution, unit);
            if ((weight > 0.0f && !isBlack(contribution)) || (state && lpe.unit && parts.size() > 0)) {
                const uint32_t r = pushRay(w.shadow, origin, ls.wi, shadowRayLength(ls.distance, sc.tolerance), p,
                                           time, shadowRaySeed(a.rays.filterSeed[k]), RayClass::Shadow);
                w.shadowCategory[r] = light.shadowCategory;
                w.shadowSlot[r] = 2;
                if (light.type == LightType::Triangle) {
                    w.shadowContribution[r] = contribution;
                    w.shadowEmitter[r] = li;
                    w.shadowBary[r] = ls.bary;
                    w.shadowClamp[r] = clampNee;
                } else {
                    w.shadowContribution[r] = clampContribution(contribution, clampNee);
                    w.shadowEmitter[r] = kInvalidId;
                }
                if (w.partBegin)
                    attachParts(w, r, parts,
                                light.type == LightType::Triangle ? 1.0f : clampScale(contribution, clampNee));
            }
        }
    }

    // The continuation: the entry's kBsdf direction numbers were not used
    // (they picked the lobe only), Russian roulette after the walk.
    const Vec3f us = sampler.get3D(stream::bounce(a.depth, stream::kBsdf));
    const Vec3f wi = Frame::fromNormal(n).toWorld(sampleCosineHemisphere({us.x, us.y}));
    const float pdf = cosineHemispherePdf(dot(wi, n));
    if (!(dot(wi, ng) > 0.0f) || !(pdf > 0.0f))
        return;
    if (a.depth + 1 >= st.rrStartDepth) {
        const float survive = rouletteSurvival(beta, w.etaScale[p]);
        if (sampler.get1D(stream::bounce(a.depth, stream::kRoulette)) >= survive)
            return;
        beta *= 1.0f / survive;
    }
    w.beta[p] = beta;
    if (state && lpe.unit)
        lpeUnoccludedEnvironment(lpe, sc.lights, sc.lightCount, sc.lightSampler.pmf, film, pixel, state, wi,
                                 LightFilterSite{origin, sc.links.categories(receiverSet)}, 1.0f, beta, true);
    w.bsdfPdf[p] = pdf;
    w.diffuseShare[p] = 1.0f;
    w.prevP[p] = origin;
    w.prevN[p] = ng;
    w.prevReceiver[p] = uint8_t(Receiver::OneSided);
    w.prevSet[p] = receiverSet;
    if (sc.shadowDomeCount > 0)
        pushShadowDomeRay(a, p, PathPmf{sc, origin, ng, Receiver::OneSided, receiverSet}, origin, wi, pdf, 1.0f, beta,
                          time, domeRaySeed(a.rays.filterSeed[k]), state, false);
    w.coneWidth[p] += w.coneSpread[p] * wk.hitT[i];
    w.coneSpread[p] = nova::fmax(w.coneSpread[p], 1.0f);
    pushRay(a.next, origin, wi, kInf, p, time,
            sc.anyFiltered ? sampler.get1D(stream::bounce(a.depth + 1, stream::kOpacity)) : 0.0f, RayClass::Sss);
}

// Pass C' (shadeVolumes, docs/design/M8.md D9-D12): the scattering events
// in volumes. One light sample with the phase function (MIS against the
// continuation; volumes take the lights' diffuse multiplier) and a
// continuation sampled from it (weight 1, the event's albedo is already in
// beta), of class Volume within the volume limit and light-only past it. In
// light path expressions a volume event is V without a scattering class or
// label; A takes the event's weight. Volume lights and equiangular shares
// come with G5's second part.
extern "C" __global__ void novaVolumes(const ShadeArgs a) {
    const uint32_t i = threadIndex();
    const GpuWave &w = a.wave;
    if (i >= *w.volumeShadeCount)
        return;
    const GpuScene &sc = a.scene;
    const GpuSettings &st = a.settings;
    const uint32_t k = w.volumeShade[i];
    const uint32_t p = a.rays.path[k];
    const float time = a.rays.time[k];
    const Vec3f d = a.rays.dir[k];
    const float t = w.volumeT[k];
    const Vec3f x = a.rays.org[k] + d * t;
    const PhaseFunction phase = w.volumePhase[k];
    const VolumeId volume = w.volumeId[k];
    const Sampler sampler{w.pixelSeed[p], w.sampleIndex[p]};
    const LpeView &lpe = sc.lpe;
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    const lpe::State state = w.lpeState ? w.lpeState[p] : lpe::State(0);
    const uint32_t tag = state && volume < lpe.volumeTag.size() ? lpe.volumeTag[volume] : 0u;
    const lpe::State scattered = state ? lpeStep(lpe, state, lpe::TypeVolume, lpe::ScatterNone, tag, 0) : state;
    if (scattered && lpe.albedo && lpe.albedoAfter[scattered])
        lpeDeposit(lpe, film, pixel, lpe.program.step(scattered, lpe.albedoSymbol), w.beta[p]);
    const float clampNee = a.depth > 0 ? st.clampIndirect : 0.0f;

    // Next-event estimation (no surface: the shadow ray starts at x).
    {
        const GpuLightChoice choice = chooseLight(sc, st, w, p, time, sampler, x, Vec3f{0.0f, 0.0f, 0.0f},
                                                  Receiver::None, 0u, a.depth, stream::bounce(a.depth, stream::kLight));
        const uint32_t li = choice.light;
        if (li != kInvalidId && choice.ls.pdf > 0.0f) {
            const LightData &light = sc.lights[li];
            const LightSample &ls = choice.ls;
            const float f = phase.eval(d, ls.wi) * light.diffuse;
            if (f != 0.0f) {
                const float lightPdf = choice.selectPdf * ls.pdf;
                const float weight = lightSampleWeight(st.lightStrategy, lightPdf, phase.eval(d, ls.wi),
                                                       ls.delta || light.neeOnly());
                // A light sampled equiangularly along this ray (M9 D17): the
                // free flight's share of the distance.
                float share = 1.0f;
                if (light.equiangular && w.eqB[k] > w.eqA[k]) {
                    MediumRay ray;
                    ray.o = a.rays.org[k];
                    ray.d = d;
                    ray.tEnd = a.rays.tMax[k];
                    ray.time = time;
                    ray.rayType = a.rays.rayType[k];
                    ray.depth = a.depth;
                    DeviceMedia media{sc, st.globals};
                    float pEq = 0.0f, pFree = 0.0f;
                    equiangularDensities(sc, media, ray, w.eqA[k], w.eqB[k], li, t, choice.selectPdf, pEq, pFree);
                    share = pEq > 0.0f ? pFree * pFree / (pEq * pEq + pFree * pFree) : 1.0f;
                }
                const Spectrum unit = w.beta[p] * ls.Li * (f * share / lightPdf);
                const Spectrum contribution = unit * weight;
                GpuLpeParts parts;
                if (scattered && (weight > 0.0f || lpe.unit))
                    lpePartsAdd(lpe, parts, lpeLightStep(lpe, scattered, li), contribution, unit);
                if ((weight > 0.0f && !isBlack(contribution)) || (state && lpe.unit && parts.size() > 0)) {
                    const uint32_t r = pushRay(w.shadow, x, ls.wi, shadowRayLength(ls.distance, sc.tolerance), p, time,
                                               shadowRaySeed(a.rays.filterSeed[k]), RayClass::Shadow);
                    w.shadowCategory[r] = light.shadowCategory;
                    w.shadowSlot[r] = 0;
                    if (light.type == LightType::Triangle) {
                        w.shadowContribution[r] = contribution;
                        w.shadowEmitter[r] = li;
                        w.shadowBary[r] = ls.bary;
                        w.shadowClamp[r] = clampNee;
                    } else {
                        w.shadowContribution[r] = clampContribution(contribution, clampNee);
                        w.shadowEmitter[r] = kInvalidId;
                    }
                    if (w.partBegin)
                        attachParts(w, r, parts,
                                    light.type == LightType::Triangle ? 1.0f : clampScale(contribution, clampNee));
                }
            }
        }
    }

    // The continuation.
    float pdf = 0.0f;
    const Vec3f wi = phase.sample(d, sampler.get3D(stream::bounce(a.depth, stream::kBsdf)), pdf);
    if (!(pdf > 0.0f))
        return;
    constexpr int kVolumeClass = int(LimitClass::Volume);
    const float volumeLimit = sc.volumes[volume].volumeLimit;
    const uint32_t allowed =
        pathLimit(volumeLimit >= 0.0f ? volumeLimit : st.limits.limit[kVolumeClass], w.limitDither[p]);
    const uint32_t n = uint32_t(w.eventCounts[p] >> (8 * kVolumeClass)) & 0xFFu;
    if (n >= allowed)
        w.lightOnly[p] = 1;
    else if (n < 255)
        w.eventCounts[p] += uint64_t(1) << (8 * kVolumeClass);
    Spectrum beta = w.beta[p];
    bool alive = !isBlack(beta) && isFinite(beta);
    if (alive && a.depth + 1 >= st.rrStartDepth) {
        const float s = maxComponent(absolute(beta)) * w.etaScale[p];
        const float survive = s < 0.05f ? 0.05f : (0.95f < s ? 0.95f : s); // as std::clamp
        if (sampler.get1D(stream::bounce(a.depth, stream::kRoulette)) < survive)
            beta *= 1.0f / survive;
        else
            alive = false;
    }
    if (!alive)
        return;
    w.beta[p] = beta;
    if (state) {
        w.lpeState[p] = scattered;
        if (scattered && lpe.unit) // volume events are receiver set 0
            lpeUnoccludedEnvironment(lpe, sc.lights, sc.lightCount, sc.lightSampler.pmf, film, pixel, scattered, wi,
                                     LightFilterSite{x, sc.links.categories(0u)}, 1.0f, beta, true);
    }
    w.bsdfPdf[p] = pdf;
    w.diffuseShare[p] = 1.0f;
    w.prevP[p] = x;
    w.prevN[p] = Vec3f{0.0f, 0.0f, 0.0f};
    w.prevReceiver[p] = uint8_t(Receiver::None);
    w.prevSet[p] = 0;
    if (sc.shadowDomeCount > 0)
        pushShadowDomeRay(a, p, PathPmf{sc, x, Vec3f{0.0f, 0.0f, 0.0f}, Receiver::None, 0u}, x, wi, pdf, 1.0f, beta,
                          time, domeRaySeed(a.rays.filterSeed[k]), scattered, false);
    w.afterDiffuse[p] = 1; // fake caustics: a volume event counts as diffuse
    w.coneWidth[p] += w.coneSpread[p] * t;
    w.coneSpread[p] = nova::fmax(w.coneSpread[p], 1.0f);
    pushRay(a.next, x, wi, kInf, p, time,
            sc.anyFiltered ? sampler.get1D(stream::bounce(a.depth + 1, stream::kOpacity)) : 0.0f, RayClass::Volume);
}

// Stage 4 (traceShadowRays): unoccluded light samples add their radiance;
// towards an emissive triangle (emitterShadowRadiance) the material's
// emission where the ray ends, seen from its origin, then the clamp.
extern "C" __global__ void novaShadow(const ShadowArgs a) {
    const uint32_t i = threadIndex();
    const GpuWave &w = a.wave;
    if (i >= *w.shadow.count || w.shadowSlot[i] != a.slot)
        return;
    // unoccluded; and shadow; outputs need the blocked rays as well (D7).
    const LpeView &lpe = a.scene.lpe;
    const bool parts = w.partBegin && w.partCount[i] > 0;
    const bool unit = parts && lpe.unit;
    const bool blocked = w.occluded[i] != 0;
    if (blocked && !unit)
        return;
    Spectrum c = w.shadowContribution[i];
    float emitterScale = 1.0f;
    Spectrum Le{1.0f, 1.0f, 1.0f};
    if (const uint32_t li = w.shadowEmitter[i]; li != kInvalidId) {
        const GpuScene &sc = a.scene;
        const LightData &l = sc.lights[li];
        const Vec3f o = w.shadow.org[i], d = w.shadow.dir[i];
        const float time = w.shadow.time[i];
        const Vec2f bary = w.shadowBary[i];
        const Vec3f P = l.position + l.axisU * bary.x + l.axisV * bary.y;
        const GpuMesh &mesh = sc.meshes[l.mesh];
        const SurfaceHit hit = resolveMeshHit(mesh.view, l.mesh, l.prim, bary, length(P - o), l.instance, time, o, d);
        const uint32_t material = materialIdOf(mesh, l.prim);
        SurfaceInputs inputs;
        surfaceInputs(a.settings, a.depth + 1, mesh, sc.materials[material], material, hit, d, time,
                      uint8_t(RayClass::Shadow), Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 0.0f}, inputs);
        const BsdfTables tables = sc.tables;
        const ClosureContext ctx{&tables, sc.xyzToRgb};
        ClosureList cl;
        buildSurfaceClosure(inputs, hit, -d, ctx, cl);
        Le = isFinite(cl.emission) ? cl.emission : spectrum(0.0f);
        const Spectrum value = c * Le;
        emitterScale = clampScale(value, w.shadowClamp[i]);
        c = value * emitterScale;
    }
    const uint32_t p = w.shadow.path[i];
    Spectrum T{1.0f, 1.0f, 1.0f};
    if (!blocked && w.fakeT) { // the fake-caustic surfaces in front of the light (novaFakeStep)
        T = w.fakeT[i];
        c *= T;
    }
    if (!blocked && a.scene.activeVolumeCount > 0 && (!isBlack(c) || unit)) {
        // The volumes between the point and the light, seen as a shadow ray
        // by their shaders.
        MediumRay ray;
        ray.o = w.shadow.org[i];
        ray.d = w.shadow.dir[i];
        ray.tEnd = w.shadow.tMax[i];
        ray.time = w.shadow.time[i];
        ray.rayType = uint8_t(RayClass::Shadow);
        ray.depth = a.depth;
        const uint32_t stream = stream::mediumShadow(a.depth);
        Rng rng = Rng::make(w.pixelSeed[p], w.sampleIndex[p], stream);
        DeviceMedia media{a.scene, a.settings.globals};
        GpuSpans spans;
        GpuActives active;
        const Spectrum Tm =
            trackMedia(media, spans, active, ray, Sampler{w.pixelSeed[p], w.sampleIndex[p]}.get1D(stream), rng, nullptr);
        T *= Tm;
        c *= Tm;
    }
    if (!blocked)
        addRadiance(w, p, c);
    if (!parts)
        return;
    // The parts (traceShadowRays): plain sums what passes, unoccluded; the
    // light before shadowing, shadow;'s lit sum what passes (D7). Towards an
    // emissive triangle they take its emission and the clamp first.
    GpuFilm film = a.film;
    const uint32_t pixel = w.pixel[p];
    for (uint32_t j = w.partBegin[i], end = j + w.partCount[i]; j < end; ++j) {
        const Spectrum value = w.partValue[j] * Le * emitterScale, unitValue = w.partUnit[j] * Le;
        if (!blocked)
            lpeDeposit(lpe, film, pixel, w.partState[j], value * T);
        if (unit) {
            lpeDepositUnit(lpe, film, pixel, w.partState[j], unitValue, true, false);
            if (!blocked)
                lpeDepositUnit(lpe, film, pixel, w.partState[j], unitValue * T, false, true);
        }
    }
}

// Fake caustics, phase 2 (fakeCausticTransmittance, M4 3.3): along a shadow
// ray that no other surface blocks, the product of the fake-caustic
// surfaces' transmittances, closest first. The unoccluded rays start their
// crossings (queue entries whose paths are the shadow rays).
extern "C" __global__ void novaFakeStart(const ShadeArgs a) {
    const uint32_t i = threadIndex();
    const GpuWave &w = a.wave;
    if (i >= *w.shadow.count)
        return;
    w.fakeT[i] = spectrum(1.0f);
    if (w.occluded[i])
        return;
    w.fakeLeft[i] = w.shadow.tMax[i];
    pushRay(a.next, w.shadow.org[i], w.shadow.dir[i], w.shadow.tMax[i], i, w.shadow.time[i], w.shadow.filterSeed[i],
            RayClass::Shadow);
}

// One crossing (at most 8): the closest surface along each ray ends it
// (none), blocks it (another kind: not expected, phase 1 found no blocker)
// or takes its transmittance, and the ray goes on beyond it. Objects
// outside the light's shadow set (its shadow-link category) neither block
// nor tint the ray, as in phase 1 (M9 D15): it steps over them.
extern "C" __global__ void novaFakeStep(const ShadeArgs a) {
    const uint32_t j = threadIndex();
    const GpuWave &w = a.wave;
    if (j >= *a.rays.count)
        return;
    const uint32_t meshId = w.fakeMesh[j];
    if (meshId == kInvalidId)
        return;
    const uint32_t i = a.rays.path[j];
    const GpuScene &sc = a.scene;
    const GpuMesh &mesh = sc.meshes[meshId];
    const uint16_t category = w.shadowCategory[i];
    const bool excluded = category != 0xFFFFu && !mesh.castsShadow(w.fakeInstance[j], category);
    if (!excluded && !mesh.fakeCaustics) {
        w.fakeT[i] = spectrum(0.0f);
        return;
    }
    const Vec3f org = a.rays.org[j], dir = a.rays.dir[j];
    const float time = a.rays.time[j];
    const SurfaceHit hit = resolveMeshHit(mesh.view, meshId, w.fakePrim[j], w.fakeUv[j], a.rays.tMax[j],
                                          w.fakeInstance[j], time, org, dir);
    if (!excluded) {
        const uint32_t material = materialIdOf(mesh, hit.prim);
        SurfaceInputs inputs;
        surfaceInputs(a.settings, a.depth, mesh, sc.materials[material], material, hit, dir, time,
                      uint8_t(RayClass::Shadow), Vec3f{0.0f, 0.0f, 0.0f}, Vec3f{0.0f, 0.0f, 0.0f}, inputs);
        const BsdfTables tables = sc.tables;
        const ClosureContext ctx{&tables, sc.xyzToRgb};
        ClosureList cl;
        buildSurfaceClosure(inputs, hit, -dir, ctx, cl);
        Spectrum T = w.fakeT[i];
        T *= closureTransmittance(cl, -dir, tables);
        w.fakeT[i] = T;
        if (isBlack(T))
            return;
    }
    const Vec3f next = offsetRayOrigin(hit.P, faceForward(hit.Ng, dir));
    float left = w.fakeLeft[i];
    if (left < kInf)
        left -= dot(next - org, dir);
    if (!(left > 0.0f))
        return;
    w.fakeLeft[i] = left;
    pushRay(a.next, next, dir, left, i, time, a.rays.filterSeed[j], RayClass::Shadow);
}

// The ambient occlusion AOV's rays (traceAoRays): unoccluded ones count.
extern "C" __global__ void novaAo(const AoArgs a) {
    const uint32_t i = threadIndex();
    const GpuWave &w = a.wave;
    if (i < *w.aoRays.count && !w.aoOccluded[i] && !w.aoBelow[i])
        w.ao[w.aoRays.path[i]] += 1.0f;
}

// The wave's samples into the film (renderWave's end): per pixel its
// samples in order.
extern "C" __global__ void novaFlush(const FlushArgs a) {
    const uint32_t pixelSlot = threadIndex();
    if (pixelSlot >= a.pixelCount)
        return;
    const GpuWave &w = a.wave;
    const uint32_t first = pixelSlot * a.spp;
    const uint32_t pix = w.pixel[first];
    Spectrum sum = a.film.colorSum[pix];
    float alpha = a.film.alphaSum[pix];
    for (uint32_t j = 0; j < a.spp; ++j) {
        const Spectrum L = w.radiance[first + j];
        if (isFinite(L))
            sum += L;
        alpha += w.alpha[first + j];
    }
    a.film.colorSum[pix] = sum;
    a.film.alphaSum[pix] = alpha;
    if (a.film.ao) {
        float ao = a.film.ao[pix];
        for (uint32_t j = 0; j < a.spp; ++j)
            ao += w.ao[first + j];
        a.film.ao[pix] = ao;
    }
    a.film.count[pix] += a.spp;
}
