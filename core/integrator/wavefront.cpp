// Path tracing with Owen-scrambled Sobol samples, filter importance sampling
// of the pixel filter, one light per bounce chosen by power, and multiple
// importance sampling (power heuristic) of light and BSDF samples; BSDF rays
// collect the lights at infinity on escape and the area lights they cross.
// Surfaces scatter through closures (core/bsdf/closure.h) built from their
// material's surface model; delta events (mirrors, smooth interfaces) skip
// light sampling and weigh the lights they reach by 1. Refractions track the
// medium a path is in (Beer absorption) and the index ratio for Russian
// roulette.

#include "core/integrator/wavefront.h"

#include "core/integrator/camera_samples.h"

#include "core/bsdf/surface.h"
#include "core/integrator/holdout.h"
#include "core/integrator/material_eval.h"
#include "core/integrator/lpe_path.h"
#include "core/integrator/path.h"
#include "core/integrator/shading_queries.h"
#include "core/lights/lights.h"
#include "core/math/frame.h"
#include "core/math/sampling.h"
#include "core/math/sobol.h"
#include "core/texture/ray_cone.h"

#include <algorithm>
#include <atomic>
#include <utility>

namespace nova {

namespace {
std::atomic<uint64_t> g_walks{0}, g_walksCapped{0};
} // namespace

void RayQueue::reserve(uint32_t capacity) {
    if (org.size() < capacity) {
        org.resize(capacity);
        dir.resize(capacity);
        tMax.resize(capacity);
        time.resize(capacity);
        path.resize(capacity);
        filterSeed.resize(capacity);
        rayType.resize(capacity);
    }
    size = 0;
}

void ShadowQueue::reserve(uint32_t capacity) {
    rays.reserve(capacity);
    if (contribution.size() < capacity) {
        contribution.resize(capacity);
        occluded.resize(capacity);
        partBegin.resize(capacity);
        partCount.resize(capacity);
        shadowCategory.resize(capacity);
        emitter.resize(capacity);
        emitterBary.resize(capacity);
        emitterClamp.resize(capacity);
    }
}

void WaveQueues::reserve(uint32_t paths) {
    if (pixel.size() < paths) {
        pixel.resize(paths);
        beta.resize(paths);
        radiance.resize(paths);
        pixelSeed.resize(paths);
        sampleIndex.resize(paths);
        bsdfPdf.resize(paths);
        shadeRay.resize(paths);
        shadeMaterial.resize(paths);
        shadeOrder.resize(paths);
        shadeOrderMaterial.resize(paths);
        diffuseShare.resize(paths);
        etaScale.resize(paths);
        sigmaA.resize(paths);
        inMedium.resize(paths);
        coneWidth.resize(paths);
        coneSpread.resize(paths);
        eventCounts.resize(paths);
        limitDither.resize(paths);
        lightOnly.resize(paths);
        afterDiffuse.resize(paths);
        pathAlpha.resize(paths);
        holdout.resize(paths);
        prevP.resize(paths);
        prevN.resize(paths);
        prevReceiver.resize(paths);
        prevSet.resize(paths);
        lpeState.resize(paths);
        hitT.resize(paths);
        hitMesh.resize(paths);
        hitPrim.resize(paths);
        hitUv.resize(paths);
        hitInstance.resize(paths);
        lightHitT.resize(paths);
        lightHitMesh.resize(paths);
        lightHitPrim.resize(paths);
        lightHitInstance.resize(paths);
        lightHitUv.resize(paths);
        mediumT.resize(paths);
        crossingBegin.resize(paths);
        crossingCount.resize(paths);
        volumeHit.resize(paths);
        volumeT.resize(paths);
        volumeId.resize(paths);
        volumePhase.resize(paths);
        mediumAlpha.resize(paths);
        volumeShade.resize(paths);
        lightT.resize(paths);
        eqA.resize(paths);
        eqB.resize(paths);
        walks.resize(paths);
    }
    rays.reserve(paths);
    nextRays.reserve(paths);
    aoRays.reserve(paths);
    if (aoOccluded.size() < paths)
        aoOccluded.resize(paths);
    // Per path and depth at most: an equiangular sample, the hit's or volume
    // event's light sample, a walk exit's, and a shadow-linked dome's BSDF
    // sample (path.h) at the event or the exit.
    shadows.reserve(4 * paths);
}

namespace {

// Adds a contribution to the path's radiance. A NaN/Inf contribution counts
// as black instead of poisoning the pixel; checked per contribution, so the
// light path expressions' parts (lpeDeposit) sum to the beauty. A holdout
// path's light is not in the beauty (core/integrator/holdout.h).
void addRadiance(WaveQueues &q, uint32_t p, const Spectrum &c) {
    if (isFinite(c) && !q.holdout[p])
        q.radiance[p] += c;
}

// --- Light path expressions (docs/design/M6.md D4-D6) ----------------------
// The events, deposits and parts are core/integrator/lpe_path.h's (shared
// with the GPU); here the site parts live in the wave's queues.

// WaveQueues' site parts as lpe_path.h's Parts.
struct QueueParts {
    WaveQueues &q;
    uint32_t size() const { return uint32_t(q.lpePartState.size()); }
    lpe::State stateAt(uint32_t i) const { return q.lpePartState[i]; }
    const Spectrum &valueAt(uint32_t i) const { return q.lpePartValue[i]; }
    void add(uint32_t i, const Spectrum &v, const Spectrum &u) {
        q.lpePartValue[i] += v;
        q.lpePartUnit[i] += u;
    }
    void push(lpe::State s, const Spectrum &v, const Spectrum &u) {
        q.lpePartState.push_back(s);
        q.lpePartValue.push_back(v);
        q.lpePartUnit.push_back(u);
    }
};

// The parts of one site, summed by final state; states without outputs are
// dropped. `unit`: the part with MIS weight 1 (light samples).
void lpePartsClear(WaveQueues &q) {
    q.lpePartState.clear();
    q.lpePartValue.clear();
    q.lpePartUnit.clear();
}

NOVA_FORCEINLINE void lpePartsAdd(const LpeView &t, WaveQueues &q, lpe::State s, const Spectrum &v,
                                  const Spectrum &unit = {0.0f, 0.0f, 0.0f}) {
    QueueParts parts{q};
    nova::lpePartsAdd(t, parts, s, v, unit);
}

void lpeDepositParts(const LpeView &t, Film &film, WaveQueues &q, uint32_t pixel, const Spectrum &factor) {
    const QueueParts parts{q};
    nova::lpeDepositParts(t, film, parts, pixel, factor);
}

// Moves the parts to the shadow ray pushed last, scaled by `scale`.
void lpeAttachParts(WaveQueues &q, float scale) {
    ShadowQueue &s = q.shadows;
    const uint32_t r = s.rays.size - 1;
    s.partBegin[r] = uint32_t(s.partState.size());
    s.partCount[r] = uint8_t(q.lpePartState.size());
    for (size_t i = 0; i < q.lpePartState.size(); ++i) {
        s.partState.push_back(q.lpePartState[i]);
        s.partValue.push_back(q.lpePartValue[i] * scale);
        s.partUnit.push_back(q.lpePartUnit[i]);
    }
}


// Closest hits for the current ray queue. Scattered rays in a scene whose
// shadow visibility differs from theirs also find where they would end as
// shadow rays: the lights they reach are tested as Karma tests them
// (Scene::shadowVisibilityDiffers).
void intersectRays(const Scene &scene, WaveQueues &q, uint32_t depth, const ShadeGlobals &globals) {
    HitBatch hits{q.hitT.data(), q.hitMesh.data(), q.hitPrim.data(), q.hitUv.data(), q.hitInstance.data()};
    scene.accel().intersect(q.rays.batch(depth, &globals), hits);
    q.lightQuery = depth > 0 && scene.shadowVisibilityDiffers();
    if (!q.lightQuery)
        return;
    thread_local std::vector<uint8_t> shadowClass;
    shadowClass.assign(q.rays.size, uint8_t(RayClass::Shadow));
    RayBatch batch = q.rays.batch(depth, &globals);
    batch.rayType = shadowClass.data();
    HitBatch light{q.lightHitT.data(), q.lightHitMesh.data(), q.lightHitPrim.data(), q.lightHitUv.data(),
                   q.lightHitInstance.data()};
    scene.accel().intersect(batch, light);
}

// Runs an external (VEX) material for prepared lanes: the shader's
// parameters per lane from the input blocks, else uniform. A failed run
// leaves black results (the bridge reports the error once).
void runExternalLanes(const ExternalBinding &x, const ExternalLane *lanes, int count,
                      const SurfaceInputs *const *inputs, ExternalResult *out) {
    const std::vector<ExternalParam> &params = x.shader->params();
    thread_local std::vector<float> scratch;
    thread_local std::vector<ExternalInput> ins;
    size_t total = 0;
    for (size_t i = 0; i < params.size(); ++i)
        if (x.slots[i] != kNoExternalSlot)
            total += size_t(params[i].width) * size_t(count);
    scratch.resize(total);
    ins.assign(params.size(), ExternalInput{});
    float *cursor = scratch.data();
    for (size_t i = 0; i < params.size(); ++i) {
        const int w = params[i].width;
        if (w == 0)
            continue;
        if (x.slots[i] != kNoExternalSlot) {
            for (int l = 0; l < count; ++l)
                for (int j = 0; j < w; ++j)
                    cursor[l * w + j] = inputs[l]->v[x.slots[i] + j];
            ins[i] = {cursor, true};
            cursor += size_t(w) * size_t(count);
        } else {
            ins[i] = {x.values[i].data(), false};
        }
    }
    if (!x.shader->run(count, lanes, ins.data(), x.strings.data(), out, nullptr))
        for (int l = 0; l < count; ++l)
            out[l] = ExternalResult{};
}

// The lane of a hit for the bridge (dPds/dPdt: a frame around N, no
// parametric derivatives).
ExternalLane externalLane(const SurfaceHit &h, Vec3f dir, float time, Vec3f wi, Vec3f u) {
    const Frame f = Frame::fromNormal(h.N);
    ExternalLane lane;
    lane.P = h.P;
    lane.I = dir;
    lane.N = h.N;
    lane.Ng = h.Ng;
    lane.dPds = f.t;
    lane.dPdt = f.b;
    lane.s = h.uv.x;
    lane.t = h.uv.y;
    lane.time = time;
    lane.wi = wi;
    lane.sx = u.x;
    lane.sy = u.y;
    return lane;
}

// Calls f(x, y, slot) for the film pixels of a wave: every pixel of `tile`
// in rows, or the listed film pixels in their order.
template <typename F>
void forEachPixel(const TileRect &tile, const uint32_t *pixels, uint32_t pixelCount, int filmWidth, F &&f) {
    if (!pixels) {
        uint32_t slot = 0;
        for (int y = tile.y0; y < tile.y1; ++y)
            for (int x = tile.x0; x < tile.x1; ++x, ++slot)
                f(x, y, slot);
        return;
    }
    for (uint32_t slot = 0; slot < pixelCount; ++slot)
        f(int(pixels[slot] % uint32_t(filmWidth)), int(pixels[slot] / uint32_t(filmWidth)), slot);
}

// First-hit AOVs (depth, normal, albedo, primId and the utility planes)
// from one ray through every pixel center, traced with the first sample.
// Keeping them out of the filtered beauty samples makes ids and depths exact
// per pixel instead of mixing neighbours, and leaves the sample sequence
// untouched. `globals`: the render's frame, as the beauty's shading.
void renderFirstHitAovs(const Scene &scene, const Camera &camera, const TileRect &tile, const uint32_t *pixels,
                        uint32_t pixelCount, int filmWidth, Film &film, WaveQueues &q, const ClosureContext &ctx,
                        float textureFilterWidth, const ShadeGlobals &globals) {
    q.rays.size = 0;
    float coneWidth0 = 0.0f, coneSpread0 = 0.0f;
    cameraCone(camera,
               {float(tile.x0 + tile.width() / 2 + camera.imageOriginX) + 0.5f,
                float(tile.y0 + tile.height() / 2 + camera.imageOriginY) + 0.5f},
               coneWidth0, coneSpread0);
    forEachPixel(tile, pixels, pixelCount, filmWidth, [&](int x, int y, uint32_t slot) {
        const int ix = x + camera.imageOriginX, iy = y + camera.imageOriginY; // image pixel
        // Lens center and frame time: sharp, stable AOVs.
        const Ray ray = camera.generateRay(camera.rasterToNdc({float(ix) + 0.5f, float(iy) + 0.5f}), {0.5f, 0.5f},
                                           camera.frameTime);
        q.pixel[slot] = uint32_t(y) * uint32_t(filmWidth) + uint32_t(x);
        // Stochastic opacity: the first sample's decision at the pixel.
        const float seed = Sampler::make(ix, iy, 0, 0).get1D(stream::bounce(0, stream::kOpacity));
        q.rays.push(ray.org, ray.dir, ray.tMax, slot, camera.frameTime, seed, RayClass::Camera);
    });
    intersectRays(scene, q, 0, globals);

    const LightData *lights = scene.lights().data();
    const uint32_t lightCount = scene.analyticLightCount();
    bool hasVisibleAreaLights = false;
    for (uint32_t i = 0; i < lightCount; ++i)
        hasVisibleAreaLights |= isAreaLight(lights[i].type) && !lights[i].delta && lights[i].cameraVisible;
    // The utility planes at a first hit (D9): the surface hit (null: a
    // camera-visible light at P), its shading and geometric normals as
    // authored (Karma's ray:hitN and hitNg are not turned to the viewer), the
    // eye ray. Planes with a sample filter take each sample instead
    // (core/integrator/camera_samples.h).
    const UtilityContext utilityCtx = UtilityContext::make(scene, camera);
    const auto writeUtility = [&](uint32_t pix, const UtilityHit &h) {
        for (UtilityPlane &plane : film.utility)
            if (plane.request.filter == SampleFilter::Center)
                utilityCtx.value(plane.request, h, plane.data.data() + size_t(pix) * plane.width);
    };

    for (uint32_t k = 0; k < q.rays.size; ++k) {
        const bool miss = q.hitMesh[k] == kInvalidId;
        const uint32_t pix = q.pixel[q.rays.path[k]];
        SurfaceHit hit;
        if (!miss)
            hit = scene.resolveHit(q.hitMesh[k], q.hitPrim[k], q.hitUv[k], q.hitT[k], q.hitInstance[k], camera.frameTime,
                                   q.rays.org[k], q.rays.dir[k]);
        uint32_t light = 0;
        float tLight = 0.0f;
        Vec3f nLight;
        bool front = false;
        if (hasVisibleAreaLights && nearestCameraLight(lights, lightCount, q.rays.org[k], q.rays.dir[k],
                                                       miss ? q.rays.tMax[k] : q.hitT[k], !miss, hit.extent, light,
                                                       tLight, nLight, front)) {
            // A camera-visible light is opaque in the beauty, so it is the
            // first-hit surface here as well (lights are not rprims: no ids).
            const Vec3f P = q.rays.org[k] + q.rays.dir[k] * tLight;
            film.depth[pix] = camera.viewDepth(P);
            film.normal[pix] = faceForward(nLight, -q.rays.dir[k]);
            film.albedo[pix] = spectrum(0.0f);
            film.primId[pix] = -1;
            film.instanceId[pix] = -1;
            if (!film.utility.empty()) {
                UtilityHit h;
                h.tHit = tLight;
                h.P = P;
                h.n = h.ng = nLight;
                h.org = q.rays.org[k];
                h.dir = q.rays.dir[k];
                h.time = camera.frameTime;
                writeUtility(pix, h);
            }
            continue;
        }
        if (miss)
            continue; // misses keep the clear values
        const Vec3f ng = faceForward(hit.Ng, -q.rays.dir[k]);
        const SurfaceInputs *inputs = nullptr;
        Vec3f fx, fy;
        coneFootprint(q.rays.dir[k], hit.Ng, (coneWidth0 + coneSpread0 * hit.t) * textureFilterWidth, fx, fy);
        HitTextureData texture;
        if (textureFilterWidth > 0.0f) {
            texture.footprintX = &fx;
            texture.footprintY = &fy;
        }
        const MaterialId material = scene.materialIdOf(hit.mesh, hit.prim);
        evaluateSurfaceInputs(scene, material, &hit, &q.rays.dir[k], &q.rays.time[k], 1, &inputs, q.materialScratch,
                              globals, texture);
        Spectrum albedo;
        if (const ExternalBinding *x = scene.material(material).desc.external.get(); x && x->shader) {
            // VEX: the bridge's albedo at wo.
            const ExternalLane lane = externalLane(hit, q.rays.dir[k], q.rays.time[k], hit.N, {0.5f, 0.5f, 0.5f});
            ExternalResult r;
            runExternalLanes(*x, &lane, 1, &inputs, &r);
            albedo = r.albedo;
        } else {
            ClosureList cl;
            buildSurfaceClosure(*inputs, hit, -q.rays.dir[k], ctx, cl);
            albedo = cl.albedo;
        }
        film.depth[pix] = camera.viewDepth(hit.P);
        film.normal[pix] = faceForward(hit.N, ng);
        film.albedo[pix] = albedo;
        film.primId[pix] = scene.meshRecord(hit.mesh).desc.userId;
        film.instanceId[pix] = int32_t(hit.instance);
        if (!film.utility.empty()) {
            UtilityHit h;
            h.hit = &hit;
            h.uv = q.hitUv[k];
            h.tHit = q.hitT[k];
            h.P = hit.P;
            h.n = hit.N;
            h.ng = hit.Ng;
            h.org = q.rays.org[k];
            h.dir = q.rays.dir[k];
            h.inputs = inputs;
            h.material = material;
            h.time = camera.frameTime;
            writeUtility(pix, h);
        }
    }
}

// Stage 1: one camera ray per tile pixel, distributed by the pixel filter.
// Sampler seeds and raster positions use image pixels, so crop and tile
// renders reproduce the full image exactly.
void generateCameraRays(const Camera &camera, const PixelFilter &filter, uint32_t seed, const TileRect &tile,
                        const uint32_t *pixels, uint32_t pixelCount, uint32_t samplesPerPixel, const Film &film,
                        WaveQueues &q, bool filterSeeds) {
    q.rays.size = 0;
    // The cone of a pixel hardly changes across a tile; Karma's footprint
    // does not depend on the sample count, so neither does this one.
    float coneWidth0 = 0.0f, coneSpread0 = 0.0f;
    cameraCone(camera,
               {float(tile.x0 + tile.width() / 2 + camera.imageOriginX) + 0.5f,
                float(tile.y0 + tile.height() / 2 + camera.imageOriginY) + 0.5f},
               coneWidth0, coneSpread0);
    forEachPixel(tile, pixels, pixelCount, film.width, [&](int x, int y, uint32_t pixelSlot) {
        // A pixel's samples are consecutive slots, in sample order.
        for (uint32_t j = 0; j < samplesPerPixel; ++j) {
            const uint32_t slot = pixelSlot * samplesPerPixel + j;
            const int ix = x + camera.imageOriginX, iy = y + camera.imageOriginY;
            const uint32_t pix = uint32_t(y) * uint32_t(film.width) + uint32_t(x);
            const uint32_t sampleIndex = film.count[pix] + j;
            const Sampler sampler = Sampler::make(ix, iy, sampleIndex, seed);
            const Vec2f offset = filter.sample(sampler.get2D(stream::kPixelFilter));
            const float time = sampler.get1D(stream::kTime); // box shutter; the whole path shares it
            const Ray ray = camera.generateRay(
                camera.rasterToNdc({float(ix) + 0.5f + offset.x, float(iy) + 0.5f + offset.y}),
                sampler.get2D(stream::kLens), time);

            q.pixel[slot] = pix;
            q.sampleIndex[slot] = sampleIndex;
            q.beta[slot] = spectrum(1.0f);
            q.radiance[slot] = spectrum(0.0f);
            q.pixelSeed[slot] = sampler.pixelSeed;
            q.bsdfPdf[slot] = 0.0f;
            q.diffuseShare[slot] = 1.0f;
            q.etaScale[slot] = 1.0f;
            q.sigmaA[slot] = spectrum(0.0f);
            q.inMedium[slot] = 0;
            q.coneWidth[slot] = coneWidth0;
            q.coneSpread[slot] = coneSpread0;
            q.eventCounts[slot] = 0;
            q.limitDither[slot] = sampler.get1D(stream::kLimit);
            q.lightOnly[slot] = 0;
            q.afterDiffuse[slot] = 0;
            q.pathAlpha[slot] = 0.0f;
            q.holdout[slot] = 0;
            // No MIS vertex yet (path.h, kPrevSetDelta): the queues are reused
            // across tiles, so the slot's earlier path must not leak in.
            q.prevP[slot] = ray.org;
            q.prevN[slot] = Vec3f{0.0f, 0.0f, 0.0f};
            q.prevReceiver[slot] = uint8_t(Receiver::None);
            q.prevSet[slot] = kPrevSetDelta;
            if (q.records) {
                CameraSampleRecords &r = q.camera;
                r.mesh[slot] = kInvalidId;
                r.eventT[slot] = kInf;
                r.org[slot] = ray.org;
                r.dir[slot] = ray.dir;
                r.time[slot] = time;
                r.alpha[slot] = 0.0f;
            }
            q.rays.push(ray.org, ray.dir, ray.tMax, slot, time,
                        filterSeeds ? sampler.get1D(stream::bounce(0, stream::kOpacity)) : 0.0f, RayClass::Camera);
        }
    });
}

// Stage 3: shading, in three passes (docs/design/M3.md section 4).
//   A  lights and escapes: absorption inside media, camera-visible lights,
//      the area lights BSDF rays crossed, lights at infinity for misses; the
//      surviving hits are listed with their material;
//   B  a stable counting sort of the survivors by material, so each material
//      is shaded as one group (the shading VM runs on such batches);
//   C  per hit: the material's closure, emission, one light sample and the
//      continuation ray.
// Every path has at most one ray per bounce, so the order of the passes
// changes no path's sum: results do not depend on the grouping.
//
// `depth` counts the scattering events before the current rays, so the
// surfaces found here are scattering event depth + 1. Every light type is cut
// by the same rule: events up to maxDepth are lit (by light samples and by
// BSDF samples that reach a light); the last iteration (depth == maxDepth)
// only gathers rays that reach a light.
struct ShadeContext {
    const Scene &scene;
    const RenderSettings &settings;
    const ClosureContext &closures;
    const LightData *lights;
    uint32_t lightCount;             // the lights before the emissive triangles (Scene::analyticLightCount)
    LightSamplerView lightSampler;   // by power (the Uniform sampling mode); pmf > 0: light-sampled at all
    const LightTreeView *lightTree;  // the light tree (null: select by power)
    LightLinks links;                // light linking (M9 D15; null: none)
    const uint32_t *shadowDomes;     // the shadow-linked dome lights (path.h)
    uint32_t shadowDomeCount;
    float tolerance;
    uint32_t depth;
    // The firefly clamps (RenderSettings::clampIndirect, clampDirect; docs/
    // design/M11-P1.md): lights reached by rays (and emitters treated as
    // lights) take the indirect clamp after two or more scattering events,
    // the direct one before; emission of other objects likewise, or with
    // clampEmissionIndirect (Karma) already after one; light samples the
    // indirect clamp from the second event on.
    float clampLight;
    float clampEmission;
    float clampNee;
    const LpeView *lpe; // null: no light path expressions
    Film &film;
    UtilityPlane *ao;     // the ambient occlusion AOV (null: none)
    // Holdouts (core/integrator/holdout.h): whether a holdout path's light
    // reaches any output (else it ends at its camera hit), and whether some
    // light has illumbackground.
    bool holdoutShading;
    bool illumBackground;
    const ShadeGlobals &globals; // the render's frame and camera (surface shading)
};

// The light path expression variants of the light and closure queries stay
// out of line, so the code without them is as compact as before.
#if defined(_MSC_VER)
#define NOVA_NOINLINE __declspec(noinline)
#else
#define NOVA_NOINLINE __attribute__((noinline))
#endif

// The lights' selection probabilities at a path's last scattering vertex
// (MIS of its BSDF sample, docs/design/M9.md D11): the light tree's there,
// or the power table's.
struct PathLightPmf {
    const ShadeContext &c;
    Vec3f p, n;
    Receiver r;
    uint32_t set; // the vertex's light-linking receiver set (D15)
    NOVA_INLINE float operator()(uint32_t i) const {
        return c.lightTree ? c.lightTree->pmf(i, p, n, r, set) : c.lightSampler.pmf[i];
    }
    // Whether light i lights that vertex at all.
    NOVA_INLINE bool links(uint32_t i) const { return c.links.links(i, set); }
};
NOVA_INLINE PathLightPmf pathPmf(const ShadeContext &c, const WaveQueues &q, uint32_t p) {
    return {c, q.prevP[p], q.prevN[p], Receiver(q.prevReceiver[p]), prevLinkSet(q.prevSet[p])};
}

// Where the ray of slot k (path p) gathers lights: their filters' site
// (gatherSite(), docs/design/M11-P4.md).
NOVA_INLINE LightFilterSite pathFilterSite(const ShadeContext &c, const WaveQueues &q, uint32_t k, uint32_t p) {
    return gatherSite(q.bsdfPdf[p], q.prevP[p], q.rays.org[k], c.links.categories(prevLinkSet(q.prevSet[p])));
}

// cameraEnvironment, each light's share to its outputs.
NOVA_NOINLINE Spectrum cameraEnvironmentLpe(const ShadeContext &c, const WaveQueues &q, uint32_t p, lpe::State state,
                                            Vec3f org, Vec3f dir, bool &visible) {
    const Spectrum beta = q.beta[p];
    const uint32_t pixel = q.pixel[p];
    return cameraEnvironment(c.lights, c.lightCount, org, dir, visible, [&](uint32_t i, const Spectrum &Li) {
        lpeDepositAll(*c.lpe, c.film, pixel, lpeLightStep(*c.lpe, state, i), beta * Li);
    });
}

// areaLightsAlongRay and escapedRadiance, each light's share as a part.
template <typename Atten>
NOVA_NOINLINE Spectrum areaLightsAlongRayLpe(const ShadeContext &c, WaveQueues &q, uint32_t p, lpe::State state,
                                             Vec3f org, Vec3f dir, float tEnd, const LightFilterSite &site,
                                             const Atten &atten) {
    lpePartsClear(q);
    return areaLightsAlongRay(
        c.lights, pathPmf(c, q, p), c.lightCount, c.lightTree, org, dir, tEnd, q.prevP[p], site, q.bsdfPdf[p],
        c.settings.lightStrategy, q.diffuseShare[p],
        [&](uint32_t i, const Spectrum &Li) { lpePartsAdd(*c.lpe, q, lpeLightStep(*c.lpe, state, i), Li); }, atten);
}

// Lights at infinity that light sampling never picks (constant domes) give
// the unoccluded; estimate at the continuation (lpeUnoccludedEnvironment)
// and the lit one here, both with weight 1. `beta`: the escaping lights'
// throughput (gatherLights': with level-dependent volumes the light
// transmittance, not the flight's weight).
NOVA_NOINLINE Spectrum escapedRadianceLpe(const ShadeContext &c, WaveQueues &q, uint32_t p, lpe::State state,
                                          Vec3f dir, const LightFilterSite &site, const Spectrum &beta) {
    lpePartsClear(q);
    return escapedRadiance(c.lights, pathPmf(c, q, p), c.lightCount, dir, site, q.bsdfPdf[p],
                           c.settings.lightStrategy, q.diffuseShare[p], [&](uint32_t i, const Spectrum &Li) {
                               const lpe::State s = lpeLightStep(*c.lpe, state, i);
                               lpePartsAdd(*c.lpe, q, s, Li);
                               if (c.lpe->unit && !(c.lightSampler.pmf[i] > 0.0f))
                                   lpeDepositUnit(*c.lpe, c.film, q.pixel[p], s, beta * Li, false, true);
                           });
}

// The unoccluded; estimate of the lights at infinity that light sampling
// never picks, along a continuation (direction, weight already in beta) of a
// non-delta event or not, gathered at `site`.
NOVA_NOINLINE void lpeUnoccludedEnvironment(const ShadeContext &c, uint32_t pixel, lpe::State state, Vec3f dir,
                                            const LightFilterSite &site, float diffuseShare, const Spectrum &beta,
                                            bool nonDelta) {
    nova::lpeUnoccludedEnvironment(*c.lpe, c.lights, c.lightCount, c.lightSampler.pmf, c.film, pixel, state, dir,
                                   site, diffuseShare, beta, nonDelta);
}

// evalClosure and sampleClosure with the lobes' values, out of line.
NOVA_NOINLINE ClosureEval evalLobesOutOfLine(const ClosureList &cl, Vec3f wo, Vec3f wi, Spectrum *lobeF,
                                             uint32_t &mask) {
    return nova::evalClosureLobes(cl, wo, wi, lobeF, mask);
}

NOVA_NOINLINE bool sampleLobesOutOfLine(const ClosureList &cl, Vec3f wo, Vec3f u, ClosureSample &s, Spectrum *lobeF,
                                        uint32_t &mask) {
    return nova::sampleClosureLobes(cl, wo, u, s, lobeF, mask);
}

// A camera ray's surface hit into its path's sample record (M11 P6).
void recordCameraHit(WaveQueues &q, uint32_t p, uint32_t k) {
    CameraSampleRecords &r = q.camera;
    r.mesh[p] = q.hitMesh[k];
    r.prim[p] = q.hitPrim[k];
    r.instance[p] = q.hitInstance[k];
    r.uv[p] = q.hitUv[k];
    r.t[p] = q.hitT[k];
}

// The cryptomatte ids of a camera sample's first surface, weight 1 each
// (docs/design/M6.md D10). Stochastic opacity already picked the surface,
// so each id's coverage is its opacity share in expectation.
void cryptoAccumulate(const Scene &scene, Film &film, uint32_t pixel, MeshId mesh, uint32_t prim, uint32_t instance) {
    const MeshRecord &rec = scene.meshRecord(mesh);
    for (CryptoPlane &plane : film.crypto) {
        switch (plane.request.source) {
        case CryptoSource::Object:
            if (rec.cryptoObject)
                plane.add(pixel, rec.cryptoObject, 1.0f);
            break;
        case CryptoSource::Material:
            plane.add(pixel, scene.material(scene.materialIdOf(mesh, prim)).desc.cryptoId, 1.0f);
            break;
        case CryptoSource::Primvar:
            for (const CryptoAttribute &a : rec.cryptoAttributes) {
                if (a.name != plane.request.primvar)
                    continue;
                size_t i = 0;
                if (a.uniform) { // per authored face; curves: per curve, not segment (M9 D3)
                    const MeshDesc &d = rec.desc;
                    i = d.isCurves() ? (prim < d.segmentCurve.size() ? size_t(d.segmentCurve[prim]) : size_t(prim))
                                     : (prim < d.triangleFaces.size() ? size_t(d.triangleFaces[prim]) : size_t(prim));
                }
                if (a.perInstance) // instance-rate primvars of the instancers (M11 P6)
                    i = instance;
                if (i < a.ids.size() && (!a.perInstance || a.ids[i] != 0)) // 0: an instance without a value
                    plane.add(pixel, a.ids[i], 1.0f);
                break;
            }
            break;
        }
    }
}

// Before pass A (docs/design/M8.md D9): the volumes along the current rays.
// Each ray's segment (to its hit, or to its end) gets a free flight that
// may end in a scattering event (D8); its weight waits in mediumT for pass
// A, with the weight in front of each area light the ray crosses (found as
// pass A finds them) and, for camera rays, the transmittance for alpha. The
// emission the ray collects is added here, clamped and split like a
// surface's emission. Light-only rays and rays past the path length only
// gather lights (Karma: they collect no emission and never scatter, measured
// research H 4.7): ratio tracking. With level-dependent volumes a
// continuation's lights get a ratio-tracked transmittance of their own
// (lightT, over the whole segment).
void pushShadowRay(const ShadeContext &c, WaveQueues &q, Vec3f o, Vec3f d, float t, uint32_t p,
                   const Spectrum &contribution, float time, float seed, uint32_t li, const LightSample &ls,
                   uint8_t flags = 0);

// The ray of ray slot k as its free flight sees the volumes (traceMedia).
MediumRay flightRay(const ShadeContext &c, const WaveQueues &q, uint32_t k) {
    MediumRay ray;
    ray.o = q.rays.org[k];
    ray.d = q.rays.dir[k];
    ray.tEnd = q.hitMesh[k] == kInvalidId ? q.rays.tMax[k] : q.hitT[k];
    ray.time = q.rays.time[k];
    ray.rayType = q.rays.rayType[k];
    ray.depth = c.depth;
    return ray;
}

// The MIS densities of equiangular sampling (docs/design/M9.md D17) of
// light li at distance t on ray k's medium segment, which both strategies
// weigh with (power heuristic): the free flight's stand-in density times
// the light's selection probability `pTree` at the point, and the density
// with which the equiangular strategy picks the light and t (0 outside the
// segment) times the stand-in's transmittance to t. The equiangular
// estimate needs that transmittance tracked, which Nova's trackers return as
// nearly 0 or 1 on a tight majorant: its useful density is the product, so
// it takes over only where the medium is thin or the light close.
void equiangularDensities(const ShadeContext &c, WaveQueues &q, uint32_t k, uint32_t li, float t, float pTree,
                          float &pEq, float &pFree) {
    pEq = pFree = 0.0f;
    const float a = q.eqA[k], b = q.eqB[k];
    MediumPart parts[4];
    Spectrum extinction;
    mediumAt(c.scene, q.volumes, ShadeGlobals{c.settings.frame, c.settings.fps}, flightRay(c, q, k), t, parts, 4,
             extinction);
    pFree = freeFlightProxy(extinction, a, t) * pTree;
    Equiangular e;
    if (t >= a && t < b && e.init(q.rays.org[k], q.rays.dir[k], a, b, c.lights[li].position)) {
        const float sigma = (extinction.x + extinction.y + extinction.z) * (1.0f / 3.0f);
        pEq = c.scene.equiangularLights().pmf[li] * e.pdf(t) * std::exp(-sigma * nova::fmax(t - a, 0.0f));
    }
}

// Equiangular sampling (D17) on ray k's medium segment [a, b), for a ray
// that may scatter: one light of those that opted in (by power), a distance
// in proportion to 1 / r^2 to it, and a light sample there with its shadow
// ray. MIS against the free flight's light samples over the distance (the
// free flight's events take the other share, shadeVolumes) and, for lights
// with an area, against the phase function's samples per volume as a free
// flight's event there would be. Its own Rng stream.
void equiangularSample(const ShadeContext &c, WaveQueues &q, uint32_t k) {
    const uint32_t p = q.rays.path[k];
    const MediumRay ray = flightRay(c, q, k);
    const Scene::EquiangularLights eq = c.scene.equiangularLights();
    Rng rng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], stream::equiangular(c.depth));
    float select = 0.0f;
    const uint32_t slot = eq.table.sample(rng.next(), select);
    if (slot == kInvalidId)
        return;
    const uint32_t li = eq.lights[slot];
    if (!c.links.links(li, 0u)) // volume events are receiver set 0 (D15)
        return;
    const LightData &light = c.lights[li];
    Equiangular e;
    if (!e.init(ray.o, ray.d, q.eqA[k], q.eqB[k], light.position))
        return;
    const float t = e.sample(rng.next());
    if (!(t >= q.eqA[k] && t < q.eqB[k]))
        return; // rounding at the ends
    const ShadeGlobals globals{c.settings.frame, c.settings.fps};
    MediumPart parts[4];
    Spectrum extinction;
    const int n = mediumAt(c.scene, q.volumes, globals, ray, t, parts, 4, extinction);
    if (n == 0)
        return;
    const Vec3f x = ray.o + ray.d * t;
    const LightSample ls = sampleLight(light, x, rng.next2());
    if (!(ls.pdf > 0.0f) || isBlack(ls.Li))
        return;
    const float pTree =
        c.lightTree ? c.lightTree->pmf(li, x, Vec3f{0.0f, 0.0f, 0.0f}, Receiver::None, 0u) : c.lightSampler.pmf[li];
    float pEq = 0.0f, pFree = 0.0f;
    equiangularDensities(c, q, k, li, t, pTree, pEq, pFree);
    const float pSample = select * e.pdf(t);
    if (!(pEq > 0.0f) || !(pSample > 0.0f))
        return;
    const float wT = pEq * pEq / (pEq * pEq + pFree * pFree);
    const float lightPdf = pTree * ls.pdf;
    Spectrum f{0.0f, 0.0f, 0.0f}, fUnit{0.0f, 0.0f, 0.0f};
    for (int i = 0; i < n; ++i) {
        const float ph = parts[i].phase.eval(ray.d, ls.wi);
        f += parts[i].scattering *
             (ph * lightSampleWeight(c.settings.lightStrategy, lightPdf, ph, ls.delta || light.neeOnly()));
        fUnit += parts[i].scattering * ph;
    }
    // The throughput to the point: the medium as the ray's own flight sees
    // it, and an absorbing refractive interior (gatherLights' Beer factor).
    MediumRay to = ray;
    to.tEnd = t;
    Spectrum beta = q.beta[p] * trackMedia(c.scene, q.volumes, globals, to, rng.next(), rng, nullptr);
    if (q.inMedium[p]) {
        const Spectrum s = q.sigmaA[p];
        beta *= Spectrum{std::exp(-s.x * t), std::exp(-s.y * t), std::exp(-s.z * t)};
    }
    const float scale = light.diffuse * wT / (pSample * ls.pdf);
    const Spectrum contribution = beta * f * ls.Li * scale;
    const LpeView *lpe = c.lpe;
    const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);
    if (lpe)
        lpePartsClear(q);
    if (state && (!isBlack(contribution) || lpe->unit)) {
        const VolumeId v = parts[0].volume;
        const uint32_t tag = v < lpe->volumeTag.size() ? lpe->volumeTag[v] : 0u;
        const lpe::State scattered = lpeStep(*lpe, state, lpe::TypeVolume, lpe::ScatterNone, tag, 0);
        if (scattered)
            lpePartsAdd(*lpe, q, lpeLightStep(*lpe, scattered, li), contribution, beta * fUnit * ls.Li * scale);
    }
    if (!isBlack(contribution) || (lpe && lpe->unit && !q.lpePartState.empty())) {
        const float seed = q.rays.filterSeed[k] + 0.75f - std::floor(q.rays.filterSeed[k] + 0.75f);
        pushShadowRay(c, q, x, ls.wi, shadowRayLength(ls.distance, c.tolerance), p, contribution, ray.time, seed, li,
                      ls);
    }
}

void traceMedia(const ShadeContext &c, WaveQueues &q) {
    q.media = true;
    const bool equiangular = c.scene.equiangularLights().table.count > 0;
    // Where volumes depend on the ray level (Karma's modifiers), the light a
    // continuation gathers sees them at its event's level, the next event
    // at its own (measured, research H 4.7); otherwise one flight does both.
    q.lightSplit = c.depth > 0 && c.scene.anyLevelDependentVolume();
    q.crossingLight.clear();
    q.crossingT.clear();
    q.crossingAtten.clear();
    bool hasAreaLights = false;
    for (uint32_t i = 0; i < c.lightCount; ++i)
        hasAreaLights |= isAreaLight(c.lights[i].type) && !c.lights[i].delta;
    const ShadeGlobals globals{c.settings.frame, c.settings.fps};
    const LpeView *lpe = c.lpe;
    constexpr int kMaxCrossings = 16;
    for (uint32_t k = 0; k < q.rays.size; ++k) {
        const uint32_t p = q.rays.path[k];
        const bool miss = q.hitMesh[k] == kInvalidId;
        const Vec3f org = q.rays.org[k], dir = q.rays.dir[k];
        q.crossingBegin[k] = uint32_t(q.crossingT.size());
        q.crossingCount[k] = 0;
        float crossT[kMaxCrossings];
        uint32_t crossLight[kMaxCrossings];
        Spectrum crossOut[kMaxCrossings];
        int crossings = 0;
        if (hasAreaLights) {
            const float tLights = miss ? q.rays.tMax[k] : lightSearchLimit(q.hitT[k], c.tolerance);
            crossings = areaLightCrossings(c.lights, c.lightCount, c.lightTree, org, dir, tLights, crossT, crossLight,
                                           kMaxCrossings);
            for (int i = 1; i < crossings; ++i) // by distance
                for (int j = i; j > 0 && crossT[j - 1] > crossT[j]; --j) {
                    std::swap(crossT[j - 1], crossT[j]);
                    std::swap(crossLight[j - 1], crossLight[j]);
                }
        }
        MediumRay ray;
        ray.o = org;
        ray.d = dir;
        ray.tEnd = miss ? q.rays.tMax[k] : q.hitT[k];
        ray.time = q.rays.time[k];
        ray.rayType = q.rays.rayType[k];
        ray.depth = c.depth;
        const uint32_t stream = stream::bounce(c.depth, stream::kMedium);
        const float u0 = Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get1D(stream);
        Rng rng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], stream);
        // Emission: of volume lights MIS-weighted against their light
        // samples, with the lights' multipliers past the camera (D13).
        EmissionGather gather;
        gather.strategy = c.settings.lightStrategy;
        gather.lightData = c.lights;
        gather.tree = c.lightTree;
        gather.tablePmf = c.lightSampler.pmf;
        gather.prevP = q.prevP[p];
        gather.prevN = q.prevN[p];
        gather.prevReceiver = Receiver(q.prevReceiver[p]);
        gather.prevSet = prevLinkSet(q.prevSet[p]);
        gather.links = c.depth > 0 ? c.links : LightLinks{}; // the camera is not a receiver
        if (c.depth > 0) {
            gather.bsdfPdf = q.bsdfPdf[p];
            gather.diffuseShare = q.diffuseShare[p];
            gather.multipliers = true;
        }
        EmissionGather lightGather = gather; // lights of the split transmittance
        q.volumeHit[k] = 0;
        q.eqA[k] = q.eqB[k] = 0.0f;
        if (!q.lightOnly[p] && c.depth < c.settings.maxDepth) {
            if (equiangular && mediumExtent(c.scene, ray, q.eqA[k], q.eqB[k]))
                equiangularSample(c, q, k);
            const int allCrossings = crossings;
            gather.lights = !q.lightSplit;
            const MediumFlight f = flyMedia(c.scene, q.volumes, globals, ray, q.beta[p], u0, rng, &gather,
                                            c.depth == 0 ? &q.mediumAlpha[k] : nullptr, crossT, crossOut, crossings);
            q.mediumT[k] = f.weight;
            if (f.scattered) {
                q.volumeHit[k] = 1;
                q.volumeT[k] = f.t;
                q.volumeId[k] = f.volume;
                q.volumePhase[k] = f.phase;
                crossings = f.crossings; // the lights in front of the event
            }
            if (q.lightSplit) {
                // Lights along the whole segment, as the light sample's
                // shadow ray of the event that spawned this ray sees them
                // (one level lower), whether or not the flight scattered.
                MediumRay light = ray;
                --light.depth;
                const uint32_t ls = stream::mediumLight(c.depth);
                Rng lrng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], ls);
                lightGather.objects = false;
                q.lightT[k] = trackMedia(c.scene, q.volumes, globals, light,
                                         Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get1D(ls), lrng, &lightGather,
                                         crossT, crossOut, allCrossings);
                crossings = allCrossings;
            }
        } else {
            // These rays only gather lights: the light sample's partner
            // under MIS, so the shaders see them like that sample's shadow
            // ray, at the level of the event that spawned them (Karma's
            // modifiers, measured research H 4.7).
            if (ray.depth > 0)
                --ray.depth;
            gather.objects = false;
            q.mediumT[k] = trackMedia(c.scene, q.volumes, globals, ray, u0, rng, &gather, crossT, crossOut, crossings);
            q.mediumAlpha[k] = q.lightT[k] = q.mediumT[k];
        }
        for (int i = 0; i < crossings; ++i) {
            q.crossingLight.push_back(crossLight[i]);
            q.crossingT.push_back(crossT[i]);
            q.crossingAtten.push_back(crossOut[i]);
        }
        q.crossingCount[k] = uint8_t(crossings);
        // The emission collected: volumes as O events, volume lights as L
        // events (seen from the camera they count for every output).
        const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);
        if (!isBlack(gather.objectSum)) {
            const Spectrum contribution = clampContribution(q.beta[p] * gather.objectSum, c.clampEmission);
            addRadiance(q, p, contribution);
            if (state)
                lpeDepositAll(*lpe, c.film, q.pixel[p], lpeStep(*lpe, state, lpe::TypeObject, lpe::ScatterNone, 0, 0),
                              contribution);
        }
        for (const EmissionGather *g : {&gather, &lightGather})
            for (int s = 0; s < g->lightCount; ++s) {
                const Spectrum contribution = clampContribution(q.beta[p] * g->lightSum[s], c.clampLight);
                addRadiance(q, p, contribution);
                if (!state)
                    continue;
                const lpe::State n = lpeLightStep(*lpe, state, g->lightIndex[s]);
                if (c.depth == 0)
                    lpeDepositAll(*lpe, c.film, q.pixel[p], n, contribution);
                else
                    lpeDeposit(*lpe, c.film, q.pixel[p], n, contribution);
            }
    }
}

// The transmittance in front of light `light` crossed at distance t by ray
// k (traceMedia); without volumes 1.
struct MediumAtten {
    const WaveQueues &q;
    uint32_t k;
    Spectrum operator()(uint32_t light, float t) const {
        if (!q.media)
            return {1.0f, 1.0f, 1.0f};
        const uint32_t b = q.crossingBegin[k], e = b + q.crossingCount[k];
        for (uint32_t i = b; i < e; ++i)
            if (q.crossingLight[i] == light && q.crossingT[i] == t)
                return q.crossingAtten[i];
        return q.lightSplit ? q.lightT[k] : q.mediumT[k];
    }
    // The first crossing of a light (camera-visible lights).
    Spectrum ofLight(uint32_t light) const {
        if (!q.media)
            return {1.0f, 1.0f, 1.0f};
        const uint32_t b = q.crossingBegin[k], e = b + q.crossingCount[k];
        for (uint32_t i = b; i < e; ++i)
            if (q.crossingLight[i] == light)
                return q.crossingAtten[i];
        return q.mediumT[k];
    }
};

// Pass A.
void gatherLights(const ShadeContext &c, Film &film, WaveQueues &q) {
    bool hasAreaLights = false, hasVisibleAreaLights = false;
    for (uint32_t i = 0; i < c.lightCount; ++i) {
        const bool area = isAreaLight(c.lights[i].type) && !c.lights[i].delta;
        hasAreaLights |= area;
        hasVisibleAreaLights |= area && c.lights[i].cameraVisible;
    }
    q.shadeCount = 0;
    q.volumeShadeCount = 0;
    const LpeView *lpe = c.lpe;
    for (uint32_t k = 0; k < q.rays.size; ++k) {
        const uint32_t p = q.rays.path[k];
        const Vec3f dir = q.rays.dir[k];
        const bool miss = q.hitMesh[k] == kInvalidId;
        const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);
        // A scattering event in a volume (traceMedia) ends the segment: the
        // surface behind it is not reached, the event goes to pass C'.
        const bool event = q.media && q.volumeHit[k];

        // Absorption along the segment inside a medium (Beer). A ray that
        // leaves the scene from inside an absorbing medium is lost.
        if (q.inMedium[p]) {
            const Spectrum s = q.sigmaA[p];
            const float t = event ? q.volumeT[k] : miss ? kInf : q.hitT[k];
            q.beta[p] *= Spectrum{s.x > 0.0f ? std::exp(-s.x * t) : 1.0f, s.y > 0.0f ? std::exp(-s.y * t) : 1.0f,
                                  s.z > 0.0f ? std::exp(-s.z * t) : 1.0f};
        }
        // Volumes along the segment (traceMedia): lights crossed on the way
        // take the transmittance in front of them, everything at the end of
        // the segment the whole.
        const MediumAtten atten{q, k};
        const Spectrum betaIn = q.beta[p];
        if (q.media)
            q.beta[p] *= q.mediumT[k];

        if (c.depth == 0) {
            // The sample's alpha into the film (and its record, M11 P6).
            const auto addAlpha = [&](float a) {
                film.alphaSum[q.pixel[p]] += a;
                if (q.records)
                    q.camera.alpha[p] += a;
            };
            // Camera rays see camera-visible lights only, which are opaque.
            if (hasVisibleAreaLights) {
                // Ties with the surface hit use the hit triangle's extent.
                const bool surface = !miss && !event;
                const float extent = !surface ? 0.0f
                                              : c.scene
                                                    .resolveHit(q.hitMesh[k], q.hitPrim[k], q.hitUv[k], q.hitT[k],
                                                                q.hitInstance[k], q.rays.time[k], q.rays.org[k], dir)
                                                    .extent;
                Spectrum Le;
                uint32_t light = 0;
                if (cameraAreaLight(c.lights, c.lightCount, q.rays.org[k], dir,
                                    event ? q.volumeT[k] : miss ? q.rays.tMax[k] : q.hitT[k], surface, extent, Le,
                                    light)) {
                    const Spectrum contribution = clampContribution(betaIn * Le * atten.ofLight(light), c.clampLight);
                    addRadiance(q, p, contribution);
                    if (state)
                        lpeDepositAll(*lpe, film, q.pixel[p], lpeLightStep(*lpe, state, light), contribution);
                    addAlpha(1.0f);
                    continue;
                }
            }
            // Alpha (D12): surfaces and visible environments are opaque,
            // otherwise the volumes' opacity over the whole camera segment
            // (Karma: density gives opacity).
            const auto volumeAlpha = [&] {
                const Spectrum T = q.mediumAlpha[k];
                return 1.0f - (T.x + T.y + T.z) * (1.0f / 3.0f);
            };
            if (event) {
                float alpha = 1.0f;
                if (miss) {
                    bool visible = false;
                    cameraEnvironment(c.lights, c.lightCount, q.rays.org[k], dir, visible);
                    alpha = visible ? 1.0f : volumeAlpha();
                } else {
                    if (!film.crypto.empty()) // volumes are transparent to cryptomattes
                        cryptoAccumulate(c.scene, film, q.pixel[p], q.hitMesh[k], q.hitPrim[k], q.hitInstance[k]);
                    if (q.records)
                        recordCameraHit(q, p, k);
                }
                if (q.records)
                    q.camera.eventT[p] = q.volumeT[k];
                addAlpha(alpha);
                q.volumeShade[q.volumeShadeCount++] = k;
                continue;
            }
            if (miss) {
                if (state) // B: the ray left the scene (D8)
                    lpeDeposit(*lpe, film, q.pixel[p], lpe->program.step(state, lpe->backgroundSymbol), q.beta[p]);
                bool visible = false;
                const Spectrum beta = q.beta[p];
                const Spectrum Le = state ? cameraEnvironmentLpe(c, q, p, state, q.rays.org[k], dir, visible)
                                          : cameraEnvironment(c.lights, c.lightCount, q.rays.org[k], dir, visible);
                addRadiance(q, p, clampContribution(beta * Le, c.clampLight));
                addAlpha(visible ? 1.0f : q.media ? volumeAlpha() : 0.0f);
                continue;
            }
            if (!film.crypto.empty())
                cryptoAccumulate(c.scene, film, q.pixel[p], q.hitMesh[k], q.hitPrim[k], q.hitInstance[k]);
            if (q.records) {
                recordCameraHit(q, p, k);
                q.camera.eventT[p] = q.hitT[k];
            }
            const HoldoutMode holdout =
                c.scene.anyHoldouts() ? c.scene.meshRecord(q.hitMesh[k]).holdout : HoldoutMode::None;
            if (holdout != HoldoutMode::None) {
                // A holdout (core/integrator/holdout.h): transparent to alpha
                // but for the volumes in front, its light to holdout; outputs.
                addAlpha(q.media ? volumeAlpha() : 0.0f);
                q.holdout[p] = uint8_t(holdout);
                if (lpe)
                    q.lpeState[p] = lpeHoldoutState(lpe->program, q.lpeState[p]);
                if (!c.holdoutShading)
                    continue; // nothing takes its light
            } else {
                addAlpha(1.0f); // surfaces are opaque to alpha (transmission included)
            }
        } else {
            // Rays scattered by a surface collect the area lights they crossed
            // and, on escape, the lights at infinity. Lights on the surface hit
            // count, with the tolerance of the shadow rays.
            // With light path expressions each light's share is a part of
            // its own (its light event); the parts take the clamp of the sum.
            // The lights' densities are measured from the MIS vertex (prevP),
            // which a fake-caustic pass-through leaves behind the ray.
            const bool split = q.media && q.lightSplit;
            // The lights end where the ray ends as a shadow ray when shadow
            // visibility differs from the ray's (intersectRays).
            const bool lightMiss = q.lightQuery ? q.lightHitMesh[k] == kInvalidId : miss;
            const float lightHitT = q.lightQuery ? q.lightHitT[k] : q.hitT[k];
            // Where the ray gathers lights, for their filters (M11-P4).
            const LightFilterSite site = pathFilterSite(c, q, k, p);
            if (hasAreaLights) {
                const float tEnd = event && !split ? q.volumeT[k]
                                   : lightMiss     ? q.rays.tMax[k]
                                                   : lightSearchLimit(lightHitT, c.tolerance);
                const Spectrum Le =
                    state ? areaLightsAlongRayLpe(c, q, p, state, q.rays.org[k], dir, tEnd, site, atten)
                          : areaLightsAlongRay(c.lights, pathPmf(c, q, p), c.lightCount, c.lightTree, q.rays.org[k], dir,
                                               tEnd, q.prevP[p], site, q.bsdfPdf[p], c.settings.lightStrategy,
                                               q.diffuseShare[p], NoLightSink{}, atten);
                if (!isBlack(Le)) {
                    const Spectrum contribution = betaIn * Le;
                    const float scale = clampScale(contribution, c.clampLight);
                    addRadiance(q, p, contribution * scale);
                    if (state)
                        lpeDepositParts(*lpe, film, q, q.pixel[p], betaIn * scale);
                }
            }
            if (lightMiss && (split || !event)) {
                const Spectrum beta = split ? betaIn * q.lightT[k] : q.beta[p];
                if (state)
                    lpeDeposit(*lpe, film, q.pixel[p], lpe->program.step(state, lpe->backgroundSymbol), beta);
                const Spectrum Le = state ? escapedRadianceLpe(c, q, p, state, dir, site, beta)
                                          : escapedRadiance(c.lights, pathPmf(c, q, p), c.lightCount, dir, site,
                                                            q.bsdfPdf[p], c.settings.lightStrategy, q.diffuseShare[p]);
                const Spectrum contribution = beta * Le;
                const float scale = clampScale(contribution, c.clampLight);
                addRadiance(q, p, contribution * scale);
                if (state)
                    lpeDepositParts(*lpe, film, q, q.pixel[p], beta * scale);
            }
            if (event) {
                q.volumeShade[q.volumeShadeCount++] = k;
                continue;
            }
            if (miss)
                continue;
            // Past a ray limit or the path length: lights only, the hit is
            // neither shaded nor emits (Karma), unless it is an emissive
            // triangle treated as a light (M9 D14), whose emission shadeHit
            // adds alone. A light-only ray crosses fake-caustic objects as a
            // shadow ray would (shadeHit).
            const bool emitter = c.scene.anyEmitters() &&
                                 c.scene.emitterLight(q.hitMesh[k], q.hitInstance[k], q.hitPrim[k]) != kInvalidId;
            if (c.depth >= c.settings.maxDepth && !emitter)
                continue;
            if (q.lightOnly[p] && !emitter && !(q.afterDiffuse[p] && c.scene.meshRecord(q.hitMesh[k]).fakeCaustics))
                continue;
        }
        q.shadeRay[q.shadeCount] = k;
        q.shadeMaterial[q.shadeCount] = c.scene.materialIdOf(q.hitMesh[k], q.hitPrim[k]);
        ++q.shadeCount;
    }
}

// Pass B: shadeOrder lists the survivors grouped by material, in ray order
// within a group.
void sortByMaterial(const Scene &scene, WaveQueues &q) {
    const uint32_t n = q.shadeCount;
    bool single = true;
    for (uint32_t i = 1; i < n && single; ++i)
        single = q.shadeMaterial[i] == q.shadeMaterial[0];
    if (single) { // the common case: nothing to reorder
        for (uint32_t i = 0; i < n; ++i) {
            q.shadeOrder[i] = q.shadeRay[i];
            q.shadeOrderMaterial[i] = q.shadeMaterial[i];
        }
        return;
    }
    std::vector<uint32_t> &start = q.materialStart;
    start.assign(size_t(scene.materialCount()) + 1, 0u);
    for (uint32_t i = 0; i < n; ++i)
        ++start[q.shadeMaterial[i] + 1];
    for (size_t m = 1; m < start.size(); ++m)
        start[m] += start[m - 1];
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t to = start[q.shadeMaterial[i]]++;
        q.shadeOrder[to] = q.shadeRay[i];
        q.shadeOrderMaterial[to] = q.shadeMaterial[i];
    }
}

// Pass C for one hit (ray k) with its surface inputs.
// The light NEE draws at a hit. shadeHit and the external pre-pass draw the
// same one: the sampler streams are deterministic. Selection (z) and
// position (xy) share a 3D stream, so they are stratified jointly.
struct LightChoice {
    uint32_t light = kInvalidId;
    float selectPdf = 0.0f;
    LightSample ls{};
};

// A point of a volume light (docs/design/M8.md D13; research A E8): a cell
// in proportion to its emission bound, a uniform point in it, the emission
// there. In the measure of the tentative collisions that collect emission
// along a ray (σ̄ per unit length), the sample is Li = ε / σ̄ with density
// P_cell / V_cell r² / σ̄, so the usual solid-angle weights combine it with
// those collisions under MIS (σ̄: the light's own cell; any partition of
// unity is unbiased).
LightSample sampleVolumeLight(const ShadeContext &c, WaveQueues &q, uint32_t p, const LightData &light, Vec3f origin,
                              float time, Vec2f u, uint32_t lightStream) {
    LightSample ls{};
    const VolumeRecord &rec = c.scene.volumeRecord(light.volume);
    const Distribution1DView cells = rec.lightCells.view();
    float pdf01 = 0.0f;
    uint32_t cell = 0;
    const float x = cells.sample(u.x, pdf01, cell);
    const float sigma = rec.majorant[cell];
    if (!(pdf01 > 0.0f) || !(sigma > 0.0f))
        return ls;
    // The third coordinate in the cell from the path's own generator.
    const float uz = Rng::make(q.pixelSeed[p], q.sampleIndex[p], lightStream).next();
    const MajorantLayout &l = rec.layout;
    const int32_t cx = int32_t(cell % uint32_t(l.res[0]));
    const int32_t cy = int32_t((cell / uint32_t(l.res[0])) % uint32_t(l.res[1]));
    const int32_t cz = int32_t(cell / (uint32_t(l.res[0]) * uint32_t(l.res[1])));
    const float k = float(l.k);
    const Vec3f index{float(l.lo[0]) + (float(cx) + (x * float(cells.count) - float(cell))) * k,
                      float(l.lo[1]) + (float(cy) + u.y) * k, float(l.lo[2]) + (float(cz) + uz) * k};
    const Vec3f object = rec.indexToObject.point(index);
    // A shell's cells reach past it; there is no medium outside (M11-P5).
    if (rec.desc.shell && !shellContains(rec.desc.shell->view(), object))
        return ls;
    const Vec3f world = c.scene.volumeToWorld(light.volume, time).point(object);
    const Vec3f d = world - origin;
    const float r2 = dot(d, d);
    if (!(r2 > 0.0f))
        return ls;
    const float r = std::sqrt(r2);
    ls.wi = d * (1.0f / r);
    ls.distance = r;
    VolumeEvaluator &eval = q.volumes.get(c.scene, light.volume, ShadeGlobals{c.settings.frame, c.settings.fps});
    ls.Li = eval(object, world, ls.wi, time, uint8_t(RayClass::Shadow), c.depth).emission * (1.0f / sigma);
    ls.pdf = pdf01 / float(cells.count) / rec.cellVolume * r2 / sigma;
    return ls;
}

// `lightStream`: the sampler stream (a walk's exit has its own). The light
// tree picks for the receiver at `origin` with normal `nr` (D11); the same
// point, normal and kind are the path's MIS vertex afterwards (prevP, ...).
LightChoice chooseLight(const ShadeContext &c, WaveQueues &q, uint32_t p, float time, const Sampler &sampler,
                        Vec3f origin, Vec3f nr, Receiver receiver, uint32_t set, bool nonDelta, uint32_t lightStream) {
    LightChoice n;
    n.light = kInvalidId;
    if (!nonDelta)
        return n; // delta events only: nothing to light-sample
    const Vec3f ul = sampler.get3D(lightStream);
    n.light = c.lightTree ? c.lightTree->sample(ul.z, origin, nr, receiver, set, n.selectPdf)
                          : c.lightSampler.sample(ul.z, n.selectPdf);
    // Light linking without the tree: an unlinked light is rejected (D15).
    if (n.light != kInvalidId && !c.links.links(n.light, set))
        n.light = kInvalidId;
    if (n.light != kInvalidId)
        n.ls = c.lights[n.light].type == LightType::Volume
                   ? sampleVolumeLight(c, q, p, c.lights[n.light], origin, time, {ul.x, ul.y}, lightStream)
                   : sampleLight(c.lights[n.light], origin, {ul.x, ul.y}, c.links.categories(set));
    return n;
}

LightChoice chooseLight(const ShadeContext &c, WaveQueues &q, uint32_t p, float time, const Sampler &sampler,
                        Vec3f origin, Vec3f nr, Receiver receiver, uint32_t set, bool nonDelta) {
    return chooseLight(c, q, p, time, sampler, origin, nr, receiver, set, nonDelta,
                       stream::bounce(c.depth, stream::kLight));
}

// Where rays leave a surface hit towards the side of `ng` (the shading
// origin of light samples): round curves from their tube (M9 D4), else off
// the surface. shadeHit and the external pre-pass agree on it.
Vec3f surfaceOrigin(const Scene &scene, const SurfaceHit &hit, Vec3f ng) {
    if (hit.curve && scene.meshRecord(hit.mesh).desc.curveStyle == CurveStyle::Round)
        return tubeRayOrigin(hit.P, hit.N, hit.tangent, hit.curveRadius, ng, hit.curveExitEps);
    return offsetRayOrigin(hit.P, ng);
}

// An external (VEX) hit: the bridge's result, the light direction it
// evaluated and the shader's bounce bits.
struct ExternalHit {
    const ExternalResult *result;
    Vec3f wiLight;
    const ExternalBounces *bounces;
};

RayClass externalClass(uint32_t bounce, const ExternalBounces &b) {
    if (bounce & b.sss)
        return RayClass::Sss;
    if (bounce & b.volume)
        return RayClass::Volume;
    if (bounce & b.refract)
        return RayClass::Refract;
    if (bounce & b.diffuse)
        return RayClass::Diffuse;
    return RayClass::Reflect;
}

// The external pre-pass (docs/design/M5.md D7): for a chunk of an external
// material, the hits, the light direction shadeHit will evaluate, the BSDF
// sample numbers and the shader's parameters (per hit from the input block,
// else uniform) go to the CVEX bridge in one run.
void runExternal(const ShadeContext &c, WaveQueues &q, uint32_t first, int count, const SurfaceHit *hits,
                 const Vec3f *dirs, const float *times, const SurfaceInputs *const *inputs, const ExternalBinding &x,
                 ExternalResult *out, Vec3f *wiLight) {
    ExternalLane lanes[kShadeChunk];
    for (int l = 0; l < count; ++l) {
        const uint32_t k = q.shadeOrder[first + uint32_t(l)];
        const Sampler sampler{q.pixelSeed[q.rays.path[k]], q.sampleIndex[q.rays.path[k]]};
        const SurfaceHit &h = hits[l];
        const Vec3f ngl = faceForward(h.Ng, -dirs[l]);
        const Vec3f origin = surfaceOrigin(c.scene, h, ngl);
        const LightChoice n = chooseLight(c, q, q.rays.path[k], times[l], sampler, origin, ngl, Receiver::TwoSided,
                                          c.scene.receiverSet(h.mesh, h.instance), true);
        wiLight[l] = n.light != kInvalidId && n.ls.pdf > 0.0f ? n.ls.wi : Vec3f{0.0f, 0.0f, 0.0f};
        // No light: any valid direction, the result is not used (evalClosure
        // matches wiLight only).
        const Vec3f wi = wiLight[l].x != 0.0f || wiLight[l].y != 0.0f || wiLight[l].z != 0.0f ? wiLight[l] : h.N;
        lanes[l] = externalLane(h, dirs[l], times[l], wi, sampler.get3D(stream::bounce(c.depth, stream::kBsdf)));
    }
    runExternalLanes(x, lanes, count, inputs, out);
}

// The light path expression steps of shadeHit, out of line (lpe_path.h).
NOVA_NOINLINE lpe::State lpeScatter(const ShadeContext &c, const LpeView &t, lpe::State s, const ClosureList &cl,
                                    ClosureSample &cs, const Spectrum *lobeF, uint32_t lobeMask, uint32_t tag,
                                    const Sampler &sampler) {
    return nova::lpeScatter(t, c.depth, s, cl, cs, lobeF, lobeMask, tag, sampler);
}

NOVA_NOINLINE void lpeLightSampleParts(const LpeView &t, WaveQueues &q, lpe::State s, uint32_t tag,
                                       const ClosureList &cl, const Spectrum *lobeF, uint32_t lobeMask,
                                       const LightData &light, uint32_t li, bool transmit, bool entering,
                                       const Spectrum &f, const Spectrum &unit, float weight) {
    QueueParts parts{q};
    nova::lpeLightSampleParts(t, parts, s, tag, cl, lobeF, lobeMask, light, li, transmit, entering, f, unit, weight);
}

// Queues a light sample's shadow ray with its contribution and, with light
// path expressions, the parts just collected. Towards an emissive triangle
// (M9 D14) the contribution lacks the emission, which traceShadowRays
// evaluates where an unoccluded ray ends; the clamp waits for it.
void pushShadowRay(const ShadeContext &c, WaveQueues &q, Vec3f o, Vec3f d, float t, uint32_t p,
                   const Spectrum &contribution, float time, float seed, uint32_t li, const LightSample &ls,
                   uint8_t flags) {
    const LightData &light = c.lights[li];
    if (light.type == LightType::Triangle) {
        q.shadows.push(o, d, t, p, contribution, time, seed, light.shadowCategory, flags);
        q.shadows.markEmitter(li, ls.bary, c.clampNee);
        if (c.lpe)
            lpeAttachParts(q, 1.0f);
        return;
    }
    q.shadows.push(o, d, t, p, clampContribution(contribution, c.clampNee), time, seed, light.shadowCategory, flags);
    if (c.lpe)
        lpeAttachParts(q, clampScale(contribution, c.clampNee));
}

// A background holdout seen by a ray other than the camera's
// (core/integrator/holdout.h): its diffuse albedo, emitted in the fraction of
// the light that reaches it (an O event that the shadow ray of a resampled
// light sample decides; none without a light), and the illumbackground
// lights' light as any object receives it (next-event estimation of weight
// 1: the path ends here).
void shadePrelit(const ShadeContext &c, WaveQueues &q, uint32_t k, const SurfaceHit &hit, const ClosureList &cl,
                 Vec3f wo, Vec3f ng, Vec3f origin, lpe::State state, uint32_t tag) {
    const uint32_t p = q.rays.path[k];
    const float time = q.rays.time[k];
    const LpeView *lpe = c.lpe;
    const uint32_t set = c.scene.receiverSet(hit.mesh, q.hitInstance[k]);
    const float seed = shadowRaySeed(q.rays.filterSeed[k]);
    const Spectrum albedo = diffuseAlbedo(cl);
    if (!isBlack(albedo)) {
        Rng rng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], stream::prelit(c.depth));
        const PathLightPmf pmf{c, origin, ng, Receiver::OneSided, set};
        const LightStrategy strategy = c.settings.lightStrategy;
        const LightFilterSite site{origin, c.links.categories(set)}; // light filters (M11 P4)
        const PrelitPick pick = prelitPick(rng, [&](Rng &r, int kind, PrelitCandidate &out) -> float {
            const Vec3f u{r.next(), r.next(), r.next()};
            if (kind == 0) { // a light sample
                float selectPdf = 0.0f;
                const uint32_t li = c.lightTree ? c.lightTree->sample(u.z, origin, ng, Receiver::OneSided, set, selectPdf)
                                                : c.lightSampler.sample(u.z, selectPdf);
                if (li == kInvalidId || !c.links.links(li, set) || !(selectPdf > 0.0f))
                    return 0.0f;
                const LightData &light = c.lights[li];
                if (light.type == LightType::Volume)
                    return 0.0f;
                const LightSample ls = sampleLight(light, origin, {u.x, u.y}, c.links.categories(set));
                if (!(ls.pdf > 0.0f) || dot(ls.wi, ng) <= 0.0f)
                    return 0.0f;
                const Spectrum Li = light.type == LightType::Triangle ? ls.Li * light.emission : ls.Li;
                const float lightPdf = selectPdf * ls.pdf;
                // MIS against the directions for the lights at infinity.
                const bool infinite = light.type == LightType::Dome || (light.type == LightType::Distant && !light.delta);
                const float w = infinite ? lightSampleWeight(strategy, lightPdf, cosineHemispherePdf(dot(ls.wi, ng)),
                                                             ls.delta)
                                         : 1.0f;
                out = {ls.wi, ls.distance, light.shadowCategory, light.illumBackground};
                return luminance(evalClosure(cl, wo, ls.wi).fcosDiffuse * Li) * light.diffuse * w / lightPdf;
            }
            // A direction towards the lights at infinity (constant domes: only so).
            const Vec3f d = Frame::fromNormal(ng).toWorld(sampleCosineHemisphere({u.x, u.y}));
            const float pdf = cosineHemispherePdf(dot(d, ng));
            if (!(pdf > 0.0f))
                return 0.0f;
            Spectrum L{0.0f, 0.0f, 0.0f}, illum{0.0f, 0.0f, 0.0f};
            escapedRadiance(c.lights, pmf, c.lightCount, d, site, pdf, strategy, 1.0f, [&](uint32_t i, const Spectrum &Li) {
                (c.lights[i].illumBackground ? illum : L) += Li;
            });
            // Only illumbackground domes there: they count as reaching it.
            out = {d, kInf, uint16_t(0xFFFFu), isBlack(L)};
            return luminance(evalClosure(cl, wo, d).fcosDiffuse * (L + illum)) / pdf;
        });
        const Spectrum value = clampContribution(q.beta[p] * albedo, c.clampEmission);
        const lpe::State emitted = state ? lpeStep(*lpe, state, lpe::TypeObject, lpe::ScatterNone, tag, 0) : state;
        if (pick.found && pick.ray.visible) { // an illumbackground light: no shadow
            addRadiance(q, p, value);
            if (emitted)
                lpeDepositAll(*lpe, c.film, q.pixel[p], emitted, value);
        } else if (pick.found) {
            if (lpe)
                lpePartsClear(q);
            if (emitted)
                lpePartsAdd(*lpe, q, emitted, value, value);
            q.shadows.push(origin, pick.ray.wi, shadowRayLength(pick.ray.distance, c.tolerance), p, value, time, seed,
                           pick.ray.category, kRayFromBackground);
            if (lpe)
                lpeAttachParts(q, 1.0f);
        }
    }
    if (!c.illumBackground)
        return;
    // The illumbackground lights, as an ordinary light sample would.
    const Sampler sampler{q.pixelSeed[p], q.sampleIndex[p]};
    const LightChoice choice = chooseLight(c, q, p, time, sampler, origin, ng, Receiver::OneSided, set, true);
    if (choice.light == kInvalidId || !(choice.ls.pdf > 0.0f) || !c.lights[choice.light].illumBackground)
        return;
    const LightData &light = c.lights[choice.light];
    const LightSample &ls = choice.ls;
    if (dot(ls.wi, ng) < 0.0f)
        return;
    const ClosureEval e = evalClosure(cl, wo, ls.wi);
    const Spectrum f = e.fcosDiffuse * light.diffuse + (e.fcos - e.fcosDiffuse) * light.specular;
    if (isBlack(f))
        return;
    const float lightPdf = choice.selectPdf * ls.pdf;
    const Spectrum contribution = q.beta[p] * f * ls.Li * (1.0f / lightPdf);
    if (lpe)
        lpePartsClear(q);
    if (state)
        lpeLightSampleParts(*lpe, q, state, tag, cl, nullptr, 0, light, choice.light, false, dot(wo, hit.Ng) > 0.0f,
                            f, q.beta[p] * ls.Li * (1.0f / lightPdf), 1.0f);
    pushShadowRay(c, q, origin, ls.wi, shadowRayLength(ls.distance, c.tolerance), p, contribution, time, seed,
                  choice.light, ls, kRayFromBackground);
}

// A shadow-linked dome's BSDF sample (path.h, M9 D15): a shadow ray of its
// own along the non-delta continuation `wi` (density `pdf`, diffuse share)
// of an event whose light selection and links `pmf` gives. `o`: where the
// event's light samples leave towards wi; `beta`: the continuation's weight;
// `state`: the path's LPE state after the event; `intoSolid`: wi enters a
// solid dielectric (no weight-1 parts, as for light samples).
void pushShadowDomeRay(const ShadeContext &c, WaveQueues &q, uint32_t p, const PathLightPmf &pmf, Vec3f o, Vec3f wi,
                       float pdf, float diffuseShare, const Spectrum &beta, float time, float seed, lpe::State state,
                       bool intoSolid) {
    const float u = c.shadowDomeCount > 1
                        ? Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get1D(stream::shadowDome(c.depth))
                        : 0.0f;
    Spectrum Le;
    float weight = 0.0f;
    const uint32_t li =
        sampleShadowLinkedDome(c.lights, c.shadowDomes, c.shadowDomeCount, pmf, wi,
                               LightFilterSite{pmf.p, c.links.categories(pmf.set)}, pdf, c.settings.lightStrategy,
                               diffuseShare, u, Le, weight);
    if (li == kInvalidId)
        return;
    const Spectrum contribution = beta * Le;
    const LpeView *lpe = c.lpe;
    if (lpe) {
        lpePartsClear(q);
        // Weight-1 parts only for a dome that light sampling never picks:
        // this ray is its only estimate (lpeUnoccludedEnvironment leaves it).
        const bool unit = lpe->unit && !(c.lightSampler.pmf[li] > 0.0f) && !intoSolid;
        if (state)
            lpePartsAdd(*lpe, q, lpeLightStep(*lpe, state, li), contribution, unit ? contribution : spectrum(0.0f));
    }
    if (isBlack(contribution) && !(lpe && lpe->unit && !q.lpePartState.empty()))
        return;
    q.shadows.push(o, wi, kInf, p, clampContribution(contribution, c.clampNee), time, seed,
                   c.lights[li].shadowCategory);
    if (lpe)
        lpeAttachParts(q, clampScale(contribution, c.clampNee));
}

void shadeHit(const ShadeContext &c, WaveQueues &q, uint32_t k, const SurfaceHit &hit, const SurfaceInputs &inputs,
              const ExternalHit *ext = nullptr) {
    const LightStrategy strategy = c.settings.lightStrategy;
    {
        const uint32_t p = q.rays.path[k];
        const float time = q.rays.time[k];
        const Vec3f wo = -q.rays.dir[k];
        const Vec3f ng = faceForward(hit.Ng, wo);
        // Where rays leave the hit towards d: round curves from the fibre's
        // far side when d enters it (M9 D4), else off the surface.
        const bool tube = hit.curve && c.scene.meshRecord(hit.mesh).desc.curveStyle == CurveStyle::Round;
        const auto leave = [&](Vec3f d) { return leaveHit(hit, ng, tube, d); };
        if (c.ao && c.depth == 0) {
            // The ambient occlusion AOV: one cosine-weighted ray around the
            // shading normal, unlimited (Karma's measured AO). Directions
            // below the geometric surface count as occluded.
            const Vec3f n = faceForward(hit.N, ng);
            const Vec2f u = Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get2D(stream::kUtilityAo);
            const Vec3f d = Frame::fromNormal(n).toWorld(sampleCosineHemisphere(u));
            const uint32_t r = q.aoRays.size;
            q.aoRays.push(leave(ng), d, kInf, p, time,
                          q.rays.filterSeed[k] + 0.25f - std::floor(q.rays.filterSeed[k] + 0.25f), RayClass::Shadow);
            q.aoOccluded[r] = dot(d, ng) <= 0.0f;
        }
        ClosureList cl;
        if (ext) {
            // An external (VEX) closure: the bridge's results for this hit.
            const ExternalResult &r = *ext->result;
            cl.clear();
            cl.ng = hit.Ng;
            ExternalClosure &x = cl.external;
            x.active = true;
            x.wiLight = ext->wiLight;
            x.lightF = r.lightF;
            x.lightPdf = r.lightPdf;
            x.dir = r.dir;
            x.weight = r.weight;
            x.pdf = r.pdf;
            x.valid = r.bounce != 0 && maxComponent(r.weight) > 0.0f;
            x.rayClass = externalClass(r.bounce, *ext->bounces);
            cl.albedo = r.albedo;
            cl.emission = r.emission;
            cl.nonDeltaMask = 1;
        } else {
            buildSurfaceClosure(inputs, hit, wo, c.closures, cl);
        }
        // The object's shading properties (docs/design/M11-P1.md): no
        // reflection inside refracting surfaces, glossy roughness at least
        // the path's (constrainRoughness) or, on true-caustic paths after a
        // diffuse event, the caustics clamp.
        const ObjectShading &shading = c.scene.meshRecord(hit.mesh).shading;
        if (!ext) {
            if (!shading.internalReflection)
                dropInternalReflection(cl, wo);
            float minAlpha = c.settings.constrainRoughness ? q.pathAlpha[p] : 0.0f;
            if (q.afterDiffuse[p] && !shading.fakeCaustics && shading.causticsRoughnessClamp > 0.0f)
                minAlpha = nova::fmax(minAlpha, sqr(nova::fmin(shading.causticsRoughnessClamp, 1.0f)));
            clampLobeRoughness(cl, minAlpha);
        }
        // Fake caustics after a diffuse event (docs/design/M4.md 3.3): glossy
        // lobes are skipped, transmission becomes a straight pass-through with
        // the shadow rays' transmittance, so NEE through the object and BSDF
        // paths through it weigh the same.
        Spectrum fakeT{0.0f, 0.0f, 0.0f};
        // (External closures have no transmittance: opaque to fake caustics, D13.)
        const bool fake = !ext && q.afterDiffuse[p] && shading.fakeCaustics;
        const LpeView *lpe = c.lpe;
        const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);
        const uint32_t tag = state ? lpe->objectTag(hit.mesh, c.scene.materialIdOf(hit.mesh, hit.prim)) : 0u;
        // Rays past a ray limit or the path length reach here only at
        // emissive triangles treated as lights and (light-only rays) at
        // fake-caustic objects (pass A).
        const bool lightsOnly = q.lightOnly[p] || (c.depth > 0 && c.depth >= c.settings.maxDepth);
        if (state && lpe->albedo && !lightsOnly)
            lpeAlbedoEvents(*lpe, c.film, q.pixel[p], state, tag, cl, q.beta[p]);

        // Emission, reached by BSDF sampling: an O event of weight 1, or for
        // an emissive triangle treated as a light (M9 D14) the light's event,
        // MIS-weighted against its light samples at the previous vertex and
        // scaled by its multipliers there (the camera sees it plain).
        const uint32_t emitter =
            c.scene.anyEmitters() ? c.scene.emitterLight(hit.mesh, q.hitInstance[k], hit.prim) : kInvalidId;
        if (!isBlack(cl.emission) && (emitter != kInvalidId || !lightsOnly)) {
            Spectrum Le = cl.emission;
            if (emitter != kInvalidId && c.depth > 0) {
                const LightData &light = c.lights[emitter];
                const float bsdfPdf = q.bsdfPdf[p];
                const float lightPdf = misNeedsPmf(strategy, bsdfPdf)
                                           ? pathPmf(c, q, p)(emitter) * emitterPdf(light, q.prevP[p], hit.P)
                                           : 0.0f;
                Le *= lightMultiplier(light, q.diffuseShare[p]) * bsdfSampleWeight(strategy, bsdfPdf, lightPdf);
            }
            const Spectrum contribution =
                clampContribution(q.beta[p] * Le, emitter != kInvalidId ? c.clampLight : c.clampEmission);
            addRadiance(q, p, contribution);
            // A treated emitter reached past the camera is MIS-weighted
            // against its light samples, whose weight-1 parts already feed
            // unoccluded; and shadow; (M6 D7): plain sums only, as area lights.
            if (state && emitter != kInvalidId && c.depth > 0)
                lpeDeposit(*lpe, c.film, q.pixel[p], lpeLightStep(*lpe, state, emitter), contribution);
            else if (state)
                lpeDepositAll(*lpe, c.film, q.pixel[p],
                              emitter != kInvalidId ? lpeLightStep(*lpe, state, emitter)
                                                    : lpeStep(*lpe, state, lpe::TypeObject, lpe::ScatterNone, tag, 0),
                              contribution);
        }
        if (lightsOnly && !(fake && q.lightOnly[p] && c.depth < c.settings.maxDepth))
            return;

        // Background holdouts (core/integrator/holdout.h): pre-lit for every
        // ray but the camera's; seen from the camera, their light samples'
        // shadow rays leave a background point.
        const HoldoutMode holdout = c.scene.anyHoldouts() ? c.scene.meshRecord(hit.mesh).holdout : HoldoutMode::None;
        if (holdout == HoldoutMode::Background && c.depth > 0 && !q.lightOnly[p]) {
            shadePrelit(c, q, k, hit, cl, wo, ng, leave(ng), state, tag);
            return;
        }
        const bool backgroundPoint = holdout == HoldoutMode::Background && c.depth == 0;

        if (fake) {
            fakeT = fakeCausticFactor(cl, wo, shading, *c.closures.tables);
            if (q.lightOnly[p]) {
                // A light-only ray: through, still gathering lights only.
                if (!isBlack(fakeT)) {
                    q.beta[p] *= fakeT;
                    q.coneWidth[p] += q.coneSpread[p] * hit.t;
                    q.nextRays.push(leave(-wo), -wo, kInf, p, time, q.rays.filterSeed[k],
                                    RayClass(q.rays.rayType[k]));
                }
                return;
            }
            keepDiffuseLobes(cl);
        }

        // With light path expressions, whether the lobes need their own
        // values (D5); most closures are one event per side.
        const bool perLobe = state && lpeNeedsLobes(*lpe, state, cl, tag);

        const Sampler sampler{q.pixelSeed[p], q.sampleIndex[p]};
        const Vec3f origin = leave(ng);
        const float shadowSeed = shadowRaySeed(q.rays.filterSeed[k]);

        // Next-event estimation with one light, chosen in proportion to power.
        // Closures made of delta events only skip it. The light is sampled
        // from the shadow ray's origin: its shortened length then ends short
        // of a light lying on a surface, wherever the offset origin is (at
        // large coordinates the offset exceeds the margin measured from
        // hit.P). areaLightsAlongRay measures from it too.
        // The receiver's normal for the light tree: the geometric one, which
        // bounds where an opaque closure can receive light (bent shading
        // normals reach below N, never below Ng).
        // Closures of delta lobes only neither light-sample nor need the MIS
        // vertex (their continuations have weight 1 at lights).
        const Vec3f nShade = ng;
        const bool nonDelta = cl.nonDeltaMask != 0;
        const Receiver receiver = nonDelta ? closureReceiver(cl) : Receiver::OneSided;
        const uint32_t receiverSet = nonDelta ? c.scene.receiverSet(hit.mesh, q.hitInstance[k]) : 0u;
        const LightChoice choice =
            chooseLight(c, q, p, time, sampler, origin, nShade, receiver, receiverSet, cl.nonDeltaMask != 0);
        const uint32_t li = choice.light;
        const float selectPdf = choice.selectPdf;
        if (li != kInvalidId) {
            const LightData &light = c.lights[li];
            const LightSample &ls = choice.ls;
            if (ls.pdf > 0.0f) {
                // With light path expressions, the lobes' values for the
                // per-lobe parts (each its own scattering event).
                Spectrum lobeF[ClosureList::kMaxLobes];
                uint32_t lobeMask = 0;
                const ClosureEval e =
                    perLobe ? evalLobesOutOfLine(cl, wo, ls.wi, lobeF, lobeMask) : evalClosure(cl, wo, ls.wi);
                const Spectrum f = e.fcosDiffuse * light.diffuse + (e.fcos - e.fcosDiffuse) * light.specular;
                if (!isBlack(f)) {
                    const float lightPdf = selectPdf * ls.pdf;
                    // Shadow-linked lights other than domes are sampled by
                    // NEE alone (D15): weight 1.
                    const float weight = lightSampleWeight(strategy, lightPdf, e.pdf, ls.delta || light.neeOnly());
                    const Spectrum contribution = q.beta[p] * f * ls.Li * (weight / lightPdf);
                    if (lpe)
                        lpePartsClear(q);
                    // With unoccluded; or shadow; outputs the parts also carry
                    // MIS weight 1, and the shadow ray is traced for them even
                    // where the weight is 0. At a background point the
                    // illumbackground lights stay out of those (holdout.h).
                    if (state && (weight > 0.0f || lpe->unit)) {
                        lpeLightSampleParts(*lpe, q, state, tag, cl, lobeF, lobeMask, light, li,
                                            dot(ls.wi, ng) < 0.0f, dot(wo, hit.Ng) > 0.0f, f,
                                            q.beta[p] * ls.Li * (1.0f / lightPdf), weight);
                        // At a background point the illumbackground lights
                        // stay out of the unoccluded; and shadow; sums.
                        if (backgroundPoint && light.illumBackground)
                            std::fill(q.lpePartUnit.begin(), q.lpePartUnit.end(), spectrum(0.0f));
                    }
                    const uint8_t flags = backgroundPoint ? kRayFromBackground : uint8_t(0);
                    if ((weight > 0.0f && !isBlack(contribution)) || (lpe && lpe->unit && !q.lpePartState.empty())) {
                        if (dot(ls.wi, ng) >= 0.0f) {
                            pushShadowRay(c, q, origin, ls.wi, shadowRayLength(ls.distance, c.tolerance), p,
                                          contribution, time, shadowSeed, li, ls, flags);
                        } else {
                            // Transmitted light: the shadow ray starts below the
                            // surface, towards the same point on the light.
                            const Vec3f below = leave(ls.wi);
                            float dist = ls.distance;
                            const Vec3f d = transmittedShadowRay(origin, below, ls.wi, dist);
                            pushShadowRay(c, q, below, d, shadowRayLength(dist, c.tolerance), p, contribution, time,
                                          shadowSeed, li, ls, flags);
                        }
                    }
                }
            }
        }

        if (fake) {
            // Pass through, or sample the remaining (diffuse) lobes.
            const float lT = luminance(fakeT);
            // Without transmittance the diffuse continuation is certain: no
            // pick probability to divide by (and no 0 / 0).
            const float pass = cl.count == 0 ? 1.0f
                               : lT > 0.0f   ? std::clamp(lT / (lT + luminance(cl.albedo)), 0.05f, 0.95f)
                                             : 0.0f;
            if (lT > 0.0f && sampler.get1D(stream::bounce(c.depth, stream::kLayer)) < pass) {
                // A null event: no class, no MIS change, the cone continues.
                q.beta[p] *= fakeT * (1.0f / pass);
                q.coneWidth[p] += q.coneSpread[p] * hit.t;
                q.nextRays.push(leave(-wo), -wo, kInf, p, time, q.rays.filterSeed[k],
                                RayClass(q.rays.rayType[k]));
                return;
            }
            if (cl.count == 0)
                return;
            q.beta[p] *= 1.0f / (1.0f - pass);
        }

        // Continue the path with a closure sample (with light path
        // expressions, the lobes' values there for the class choice).
        ClosureSample s;
        Spectrum lobeF[ClosureList::kMaxLobes];
        uint32_t lobeMask = 0;
        const Vec3f us = sampler.get3D(stream::bounce(c.depth, stream::kBsdf));
        const bool sampled = perLobe ? sampleLobesOutOfLine(cl, wo, us, s, lobeF, lobeMask) : sampleClosure(cl, wo, us, s);
        if (sampled) {
            const lpe::State next = state ? lpeScatter(c, *lpe, state, cl, s, lobeF, lobeMask, tag, sampler) : state;
            // Into the inside of a solid dielectric (as for light samples),
            // before thin-wall pass-throughs change s.
            const bool intoSolid = s.lobe >= 0 && cl.lobe[s.lobe].type == LobeType::Dielectric && refractsInto(cl) &&
                                   (dot(wo, hit.Ng) > 0.0f ? s.transmit : !s.transmit);
            if (s.rayClass == RayClass::Diffuse || s.rayClass == RayClass::Sss)
                q.afterDiffuse[p] = 1;
            if (c.settings.constrainRoughness && s.lobe >= 0)
                q.pathAlpha[p] = nova::fmax(q.pathAlpha[p], glossyAlpha(cl.lobe[s.lobe]));
            // Ray limits: within the limit of the event's class a full ray;
            // past it a light-only ray, or for refraction with thin walls a
            // straight pass-through up to twice the limit (Karma).
            bool passThrough = false;
            if (s.rayClass != RayClass::Volume) {
                const int cls = int(s.rayClass) - int(RayClass::Diffuse); // Diffuse, Reflect, Refract, Sss
                const RayLimitOverrides &o = c.scene.meshRecord(hit.mesh).rayLimits;
                const float limit = o.limit[cls] >= 0.0f ? o.limit[cls] : c.settings.limits.limit[cls];
                const uint32_t allowed = pathLimit(limit, q.limitDither[p]);
                const uint32_t n = uint32_t(q.eventCounts[p] >> (8 * cls)) & 0xFFu;
                if (n < allowed) {
                    // within the limit
                } else if (s.rayClass == RayClass::Refract && n < 2 * allowed &&
                           (s.lobe < 0 || cl.lobe[s.lobe].type != LobeType::Hair) && // glass only, not fibres
                           (o.thinWallAtRefractLimit >= 0 ? o.thinWallAtRefractLimit != 0
                                                          : c.settings.limits.thinWallAtRefractLimit)) {
                    passThrough = true;
                } else {
                    q.lightOnly[p] = 1;
                }
                if (!q.lightOnly[p] && n < 255)
                    q.eventCounts[p] += uint64_t(1) << (8 * cls);
            }
            if (s.walk) {
                // A subsurface walk (pass C''): one SSS event whose exit
                // continues the path; past the SSS limit the walk still runs
                // and its exit gathers lights only (Karma, research 4.6).
                // Russian roulette waits for the exit's throughput.
                const Spectrum beta = q.beta[p] * s.weight;
                if (!isBlack(beta) && isFinite(beta)) {
                    q.beta[p] = beta;
                    if (lpe)
                        q.lpeState[p] = next;
                    q.walks[q.walkCount++] =
                        WaveQueues::Walk{k, hit.mesh, q.hitInstance[k], hit.t, hit.P, ng, cl.lobe[s.lobe].subsurface};
                }
                return;
            }
            if (passThrough) {
                // Straight on with the event's weight, as through a thin wall:
                // no bending, no medium change, nothing for MIS to weigh.
                s.wi = -wo;
                s.delta = true;
                s.pdf = 0.0f;
                s.refract = false;
                s.diffuseShare = 0.0f;
            }
            Spectrum beta = q.beta[p] * s.weight;
            float etaScale = q.etaScale[p];
            if (s.refract)
                etaScale *= s.eta * s.eta; // the BTDF scaled radiance by 1 / eta^2
            bool alive = !isBlack(beta) && isFinite(beta);
            if (alive && c.depth + 1 >= c.settings.rrStartDepth) {
                const float survive = rouletteSurvival(beta, etaScale);
                if (sampler.get1D(stream::bounce(c.depth, stream::kRoulette)) < survive)
                    beta *= 1.0f / survive;
                else
                    alive = false;
            }
            if (alive) {
                q.beta[p] = beta;
                if (lpe)
                    q.lpeState[p] = next;
                if (next && lpe->unit && !intoSolid) { // as light samples: none into solid glass
                    // Gathered at this hit with the receiver set the
                    // continuation will carry (gatherSite()).
                    const uint32_t set = !s.delta                          ? receiverSet
                                         : (q.prevSet[p] & kPrevSetDelta) ? c.scene.receiverSet(hit.mesh, q.hitInstance[k])
                                                                          : prevLinkSet(q.prevSet[p]);
                    lpeUnoccludedEnvironment(c, q.pixel[p], next, s.wi, LightFilterSite{origin, c.links.categories(set)},
                                             s.diffuseShare, beta, !s.delta);
                }
                q.bsdfPdf[p] = s.delta ? -1.0f : s.pdf;
                q.diffuseShare[p] = s.diffuseShare;
                if (!s.delta) { // the MIS vertex: where this hit's light sample was chosen
                    q.prevP[p] = origin;
                    q.prevN[p] = nShade;
                    q.prevReceiver[p] = uint8_t(receiver);
                    q.prevSet[p] = receiverSet;
                    if (c.shadowDomeCount > 0)
                        pushShadowDomeRay(c, q, p, PathLightPmf{c, origin, nShade, receiver, receiverSet},
                                          dot(s.wi, ng) >= 0.0f ? origin : leave(s.wi), s.wi, s.pdf,
                                          s.diffuseShare, beta, time, domeRaySeed(q.rays.filterSeed[k]), next,
                                          intoSolid);
                } else if (q.prevSet[p] & kPrevSetDelta) {
                    // No MIS vertex yet: light linking asks this delta surface.
                    q.prevSet[p] = kPrevSetDelta | c.scene.receiverSet(hit.mesh, q.hitInstance[k]);
                }
                q.etaScale[p] = etaScale;
                if (s.refract) {
                    // Entering or leaving: by the authored orientation.
                    const bool entering = dot(wo, hit.Ng) > 0.0f;
                    q.inMedium[p] = entering && !isBlack(cl.sigmaA);
                    q.sigmaA[p] = entering ? cl.sigmaA : spectrum(0.0f);
                }
                // Ray cone: the width at the hit, the spread widened by the
                // curvature (relative to the side the ray came from) and the
                // sampled lobe's roughness.
                const float width = q.coneWidth[p] + q.coneSpread[p] * hit.t;
                const float alpha = s.delta ? 0.0f : s.roughness;
                if (alpha >= 1.0f) { // diffuse: the lobe's spread dominates
                    q.coneSpread[p] = nova::fmax(q.coneSpread[p], 1.0f);
                } else {
                    const float side = dot(hit.Ng, wo) >= 0.0f ? 1.0f : -1.0f;
                    q.coneSpread[p] = scatterSpread(q.coneSpread[p], width, c.scene.curvature(hit, time) * side,
                                                    -std::fabs(dot(hit.Ng, wo)), alpha);
                }
                q.coneWidth[p] = width;
                q.nextRays.push(leave(s.wi), s.wi, kInf, p, time,
                                c.scene.anyFiltered() ? sampler.get1D(stream::bounce(c.depth + 1, stream::kOpacity))
                                                      : 0.0f,
                                s.rayClass);
            }
        }
    }
}

// Pass C: per material, in chunks of up to kShadeChunk hits whose surface
// inputs one program run evaluates. Produces the shadow-ray queue and the
// ray queue of the next bounce.
void shadeHits(const ShadeContext &c, WaveQueues &q) {
    q.walkCount = 0;
    q.nextRays.size = 0;
    SurfaceHit hits[kShadeChunk];
    Vec3f dirs[kShadeChunk];
    float times[kShadeChunk];
    uint8_t rayTypes[kShadeChunk];
    const SurfaceInputs *inputs[kShadeChunk];
    Vec3f footX[kShadeChunk], footY[kShadeChunk];
    float texRnd[kShadeChunk];
    Vec3f queryRnd[kShadeChunk];
    const float filterWidth = c.settings.textureFilterWidth;
    const ShadeGlobals &globals = c.globals;
    const SceneShadingQueries queries(c.scene, c.depth, &globals);
    for (uint32_t i = 0; i < q.shadeCount;) {
        const MaterialId material = q.shadeOrderMaterial[i];
        uint32_t end = i + 1;
        while (end < q.shadeCount && end - i < uint32_t(kShadeChunk) && q.shadeOrderMaterial[end] == material)
            ++end;
        const int count = int(end - i);
        // Texture footprints and filtering samples only where a texture is
        // read or a derivative taken (bump).
        const MaterialRecord &rec = c.scene.material(material);
        const bool textured = !rec.samplers.empty() || (rec.desc.program && rec.desc.program->needsFootprints);
        const bool querying = rec.desc.program && rec.desc.program->needsQueries;
        HitTextureData texture;
        if (textured) {
            texture.rnd = texRnd;
            if (filterWidth > 0.0f) {
                texture.footprintX = footX;
                texture.footprintY = footY;
            }
        }
        for (int l = 0; l < count; ++l) {
            const uint32_t k = q.shadeOrder[i + uint32_t(l)];
            times[l] = q.rays.time[k];
            dirs[l] = q.rays.dir[k];
            rayTypes[l] = q.rays.rayType[k];
            hits[l] = c.scene.resolveHit(q.hitMesh[k], q.hitPrim[k], q.hitUv[k], q.hitT[k], q.hitInstance[k], times[l],
                                         q.rays.org[k], dirs[l]);
            if (!textured && !querying)
                continue;
            const uint32_t p = q.rays.path[k];
            if (querying)
                queryRnd[l] = Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get3D(stream::shadingQuery(c.depth));
            if (!textured)
                continue;
            if (filterWidth > 0.0f)
                coneFootprint(dirs[l], hits[l].Ng,
                              std::fabs(q.coneWidth[p] + q.coneSpread[p] * hits[l].t) * filterWidth, footX[l],
                              footY[l]);
            texRnd[l] = Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get1D(stream::bounce(c.depth, stream::kTexture));
        }
        evaluateSurfaceInputs(c.scene, material, hits, dirs, times, count, inputs, q.materialScratch, globals,
                              texture, HitRayData{rayTypes, c.depth, querying ? &queries : nullptr, queryRnd});
        if (const ExternalBinding *x = rec.desc.external.get(); x && x->shader) {
            // External (VEX) shading: one bridge run for the chunk.
            ExternalResult results[kShadeChunk];
            Vec3f wiLight[kShadeChunk];
            runExternal(c, q, i, count, hits, dirs, times, inputs, *x, results, wiLight);
            for (int l = 0; l < count; ++l) {
                const ExternalHit ext{&results[l], wiLight[l], &x->bounces};
                shadeHit(c, q, q.shadeOrder[i + uint32_t(l)], hits[l], *inputs[l], &ext);
            }
        } else {
            for (int l = 0; l < count; ++l)
                shadeHit(c, q, q.shadeOrder[i + uint32_t(l)], hits[l], *inputs[l]);
        }
        i = end;
    }
}

// Pass C' (docs/design/M8.md D9-D12): the scattering events in volumes,
// after pass C (which starts the shadow and continuation queues). One light
// sample with the phase function (MIS against the continuation; volumes
// take the lights' diffuse multiplier, Karma measured, research H 4.7) and
// a continuation sampled from it (weight 1, the event's albedo is already
// in beta), of class Volume within the volume limit and light-only past
// it. In light path expressions a volume event is V without a scattering
// class or label (Karma, measured); A takes the event's weight.
void shadeVolumes(const ShadeContext &c, WaveQueues &q) {
    const LpeView *lpe = c.lpe;
    constexpr int kVolumeClass = int(LimitClass::Volume);
    for (uint32_t i = 0; i < q.volumeShadeCount; ++i) {
        const uint32_t k = q.volumeShade[i];
        const uint32_t p = q.rays.path[k];
        const float time = q.rays.time[k];
        const Vec3f d = q.rays.dir[k];
        const float t = q.volumeT[k];
        const Vec3f x = q.rays.org[k] + d * t;
        const PhaseFunction phase = q.volumePhase[k];
        const VolumeId volume = q.volumeId[k];
        const Sampler sampler{q.pixelSeed[p], q.sampleIndex[p]};
        const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);
        const uint32_t tag = state && volume < lpe->volumeTag.size() ? lpe->volumeTag[volume] : 0u;
        const lpe::State scattered = state ? lpeStep(*lpe, state, lpe::TypeVolume, lpe::ScatterNone, tag, 0) : state;
        if (scattered && lpe->albedo && lpe->albedoAfter[scattered])
            lpeDeposit(*lpe, c.film, q.pixel[p], lpe->program.step(scattered, lpe->albedoSymbol), q.beta[p]);

        // Next-event estimation (no surface: the shadow ray starts at x).
        const LightChoice choice =
            chooseLight(c, q, p, time, sampler, x, Vec3f{0.0f, 0.0f, 0.0f}, Receiver::None, 0u, true);
        if (choice.light != kInvalidId && choice.ls.pdf > 0.0f) {
            const LightData &light = c.lights[choice.light];
            const LightSample &ls = choice.ls;
            const float f = phase.eval(d, ls.wi) * light.diffuse;
            if (f != 0.0f) {
                const float lightPdf = choice.selectPdf * ls.pdf;
                const float weight = lightSampleWeight(c.settings.lightStrategy, lightPdf, phase.eval(d, ls.wi),
                                                       ls.delta || light.neeOnly());
                // A light sampled equiangularly along this ray (D17): the
                // free flight's share of the distance.
                float share = 1.0f;
                if (light.equiangular && q.eqB[k] > q.eqA[k]) {
                    float pEq = 0.0f, pFree = 0.0f;
                    equiangularDensities(c, q, k, choice.light, t, choice.selectPdf, pEq, pFree);
                    share = pEq > 0.0f ? pFree * pFree / (pEq * pEq + pFree * pFree) : 1.0f;
                }
                const Spectrum unit = q.beta[p] * ls.Li * (f * share / lightPdf);
                const Spectrum contribution = unit * weight;
                if (lpe)
                    lpePartsClear(q);
                if (scattered && (weight > 0.0f || lpe->unit))
                    lpePartsAdd(*lpe, q, lpeLightStep(*lpe, scattered, choice.light), contribution, unit);
                if ((weight > 0.0f && !isBlack(contribution)) || (lpe && lpe->unit && !q.lpePartState.empty())) {
                    const float shadowSeed = shadowRaySeed(q.rays.filterSeed[k]);
                    pushShadowRay(c, q, x, ls.wi, shadowRayLength(ls.distance, c.tolerance), p, contribution, time,
                                  shadowSeed, choice.light, ls);
                }
            }
        }

        // The continuation.
        float pdf = 0.0f;
        const Vec3f wi = phase.sample(d, sampler.get3D(stream::bounce(c.depth, stream::kBsdf)), pdf);
        if (!(pdf > 0.0f))
            continue;
        const float volumeLimit = c.scene.volumeRecord(volume).desc.volumeLimit;
        const uint32_t allowed =
            pathLimit(volumeLimit >= 0.0f ? volumeLimit : c.settings.limits.limit[kVolumeClass], q.limitDither[p]);
        const uint32_t n = uint32_t(q.eventCounts[p] >> (8 * kVolumeClass)) & 0xFFu;
        if (n >= allowed)
            q.lightOnly[p] = 1;
        else if (n < 255)
            q.eventCounts[p] += uint64_t(1) << (8 * kVolumeClass);
        Spectrum beta = q.beta[p];
        bool alive = !isBlack(beta) && isFinite(beta);
        if (alive && c.depth + 1 >= c.settings.rrStartDepth) {
            const float survive = std::clamp(maxComponent(absolute(beta)) * q.etaScale[p], 0.05f, 0.95f);
            if (sampler.get1D(stream::bounce(c.depth, stream::kRoulette)) < survive)
                beta *= 1.0f / survive;
            else
                alive = false;
        }
        if (!alive)
            continue;
        q.beta[p] = beta;
        if (lpe)
            q.lpeState[p] = scattered;
        if (scattered && lpe->unit) // volume events are receiver set 0
            lpeUnoccludedEnvironment(c, q.pixel[p], scattered, wi, LightFilterSite{x, c.links.categories(0u)}, 1.0f,
                                     beta, true);
        q.bsdfPdf[p] = pdf;
        q.diffuseShare[p] = 1.0f;
        q.prevP[p] = x;
        q.prevN[p] = Vec3f{0.0f, 0.0f, 0.0f};
        q.prevReceiver[p] = uint8_t(Receiver::None);
        q.prevSet[p] = 0;
        if (c.shadowDomeCount > 0)
            pushShadowDomeRay(c, q, p, PathLightPmf{c, x, Vec3f{0.0f, 0.0f, 0.0f}, Receiver::None, 0u}, x, wi, pdf,
                              1.0f, beta, time, domeRaySeed(q.rays.filterSeed[k]), scattered, false);
        q.afterDiffuse[p] = 1; // fake caustics: a volume event counts as diffuse
        q.coneWidth[p] += q.coneSpread[p] * t;
        q.coneSpread[p] = nova::fmax(q.coneSpread[p], 1.0f);
        q.nextRays.push(x, wi, kInf, p, time,
                        c.scene.anyFiltered() ? sampler.get1D(stream::bounce(c.depth + 1, stream::kOpacity)) : 0.0f,
                        RayClass::Volume);
    }
}

// Pass C'' (docs/design/M9.md): the subsurface walks pass C queued, each
// run to completion against its own mesh instance (other objects are not
// there for it). At the exit a Lambert lobe on the shading normal facing out
// of the medium, weight the walk's throughput: one light sample (its own
// stream; the light's diffuse multiplier) and a cosine continuation of class
// SSS, light-only if the entry was past the SSS limit. The walk is the one
// SSS event of the entry: the exit adds no light path expression event.
void walkSubsurface(const ShadeContext &c, WaveQueues &q) {
    const LpeView *lpe = c.lpe;
    for (uint32_t i = 0; i < q.walkCount; ++i) {
        const WaveQueues::Walk &w = q.walks[i];
        const uint32_t k = w.ray;
        const uint32_t p = q.rays.path[k];
        const float time = q.rays.time[k];
        const Sampler sampler{q.pixelSeed[p], q.sampleIndex[p]};
        const uint32_t walkStream = stream::bounce(c.depth, stream::kWalk);
        Rng rng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], walkStream);
        // The walk starts on the surface itself: an offset origin would sit
        // a sizeable part of a small mean free path deep (256 ULPs are 3 % of
        // 0.001 at unit coordinates, measured -2 % in Karma's f_a1). The
        // first segment skips the entry surface with a minimum distance
        // instead, of the order of the hit point's rounding.
        const float scale = nova::fmax(nova::fmax(std::fabs(w.P.x), std::fabs(w.P.y)), nova::fmax(std::fabs(w.P.z), 1.0f));
        const float tMinEntry = 64.0f * 1.1920929e-7f * scale;
        const auto trace = [&](Vec3f org, Vec3f dir, float tMax, float &t, uint32_t &prim, Vec2f &uv) {
            const bool entry = org.x == w.P.x && org.y == w.P.y && org.z == w.P.z;
            return c.scene.intersectLocal(w.mesh, w.instance, time, org, dir, tMax, t, prim, uv,
                                          entry ? tMinEntry : 0.0f);
        };
        const WalkResult r =
            randomWalk(w.medium, w.P, -w.ng, sampler.get3D(walkStream), rng, kMaxWalkSegments, trace);
        q.walkRays += r.segments;
        ++q.walksDone;
        q.walksCapped += r.capped ? 1u : 0u;
        if (!r.exited)
            continue;
        Spectrum beta = q.beta[p] * r.weight;
        if (isBlack(beta) || !isFinite(beta))
            continue;
        const SurfaceHit hit = c.scene.resolveHit(w.mesh, r.prim, r.uv, r.t, w.instance, time, r.org, r.dir);
        const Vec3f ng = faceForward(hit.Ng, r.dir); // out of the medium
        const Vec3f n = faceForward(hit.N, ng);
        const Vec3f origin = offsetRayOrigin(hit.P, ng);
        const lpe::State state = lpe ? q.lpeState[p] : lpe::State(0);

        // Next-event estimation with the exit's Lambert lobe.
        const LightChoice choice =
            chooseLight(c, q, p, time, sampler, origin, ng, Receiver::OneSided, c.scene.receiverSet(w.mesh, w.instance),
                        true, stream::walkLight(c.depth));
        if (choice.light != kInvalidId && choice.ls.pdf > 0.0f) {
            const LightData &light = c.lights[choice.light];
            const LightSample &ls = choice.ls;
            const float cosI = dot(ls.wi, n);
            if (cosI > 0.0f && dot(ls.wi, ng) > 0.0f) {
                const float lightPdf = choice.selectPdf * ls.pdf;
                const float weight =
                    lightSampleWeight(c.settings.lightStrategy, lightPdf, cosineHemispherePdf(cosI),
                                      ls.delta || light.neeOnly());
                const Spectrum unit = beta * ls.Li * light.diffuse * (cosI * kInvPi / lightPdf);
                const Spectrum contribution = unit * weight;
                if (lpe)
                    lpePartsClear(q);
                if (state && (weight > 0.0f || lpe->unit))
                    lpePartsAdd(*lpe, q, lpeLightStep(*lpe, state, choice.light), contribution, unit);
                if ((weight > 0.0f && !isBlack(contribution)) || (lpe && lpe->unit && !q.lpePartState.empty())) {
                    const float shadowSeed = shadowRaySeed(q.rays.filterSeed[k]);
                    pushShadowRay(c, q, origin, ls.wi, shadowRayLength(ls.distance, c.tolerance), p, contribution,
                                  time, shadowSeed, choice.light, ls);
                }
            }
        }

        // The continuation: the entry's kBsdf direction numbers were not used
        // (it picked the lobe only), Russian roulette after the walk.
        const Vec3f us = sampler.get3D(stream::bounce(c.depth, stream::kBsdf));
        const Vec3f wi = Frame::fromNormal(n).toWorld(sampleCosineHemisphere({us.x, us.y}));
        const float pdf = cosineHemispherePdf(dot(wi, n));
        if (!(dot(wi, ng) > 0.0f) || !(pdf > 0.0f))
            continue;
        if (c.depth + 1 >= c.settings.rrStartDepth) {
            const float survive = rouletteSurvival(beta, q.etaScale[p]);
            if (sampler.get1D(stream::bounce(c.depth, stream::kRoulette)) >= survive)
                continue;
            beta *= 1.0f / survive;
        }
        q.beta[p] = beta;
        if (state && lpe->unit)
            lpeUnoccludedEnvironment(c, q.pixel[p], state, wi,
                                     LightFilterSite{origin, c.links.categories(c.scene.receiverSet(w.mesh, w.instance))},
                                     1.0f, beta, true);
        q.bsdfPdf[p] = pdf;
        q.diffuseShare[p] = 1.0f;
        q.prevP[p] = origin;
        q.prevN[p] = ng;
        q.prevReceiver[p] = uint8_t(Receiver::OneSided);
        q.prevSet[p] = c.scene.receiverSet(w.mesh, w.instance);
        if (c.shadowDomeCount > 0)
            pushShadowDomeRay(c, q, p, PathLightPmf{c, origin, ng, Receiver::OneSided, q.prevSet[p]}, origin, wi, pdf,
                              1.0f, beta, time, domeRaySeed(q.rays.filterSeed[k]), state, false);
        q.coneWidth[p] += q.coneSpread[p] * w.hitT;
        q.coneSpread[p] = nova::fmax(q.coneSpread[p], 1.0f);
        q.nextRays.push(origin, wi, kInf, p, time,
                        c.scene.anyFiltered() ? sampler.get1D(stream::bounce(c.depth + 1, stream::kOpacity)) : 0.0f,
                        RayClass::Sss);
    }
}

void shadeSurfaces(const Scene &scene, const RenderSettings &settings, uint32_t depth, Film &film, WaveQueues &q,
                   const ClosureContext &closures, const LpeView *lpe, UtilityPlane *ao, const ShadeGlobals &globals) {
    const LightTreeView tree = scene.lightTree();
    bool illumBackground = false;
    if (scene.anyBackground())
        for (uint32_t i = 0; i < scene.analyticLightCount(); ++i)
            illumBackground |= scene.lights()[i].illumBackground;
    const ShadeContext c{scene,
                         settings,
                         closures,
                         scene.lights().data(),
                         scene.analyticLightCount(),
                         scene.lightSampler(),
                         settings.lightTree ? &tree : nullptr,
                         scene.lightLinks(),
                         scene.shadowLinkedDomes().data(),
                         uint32_t(scene.shadowLinkedDomes().size()),
                         scene.intersectionTolerance(),
                         depth,
                         depth > 1 ? settings.clampIndirect : settings.clampDirect,
                         depth > 1 || (depth == 1 && settings.clampEmissionIndirect) ? settings.clampIndirect
                                                                                      : settings.clampDirect,
                         depth > 0 ? settings.clampIndirect : settings.clampDirect,
                         lpe,
                         film,
                         ao,
                         (lpe && lpe->program.holdoutStart != 0) || ao,
                         illumBackground,
                         globals};
    // The light samples of this depth (traceMedia's equiangular ones first).
    q.shadows.rays.size = 0;
    q.shadows.emitterCount = 0;
    q.shadows.partState.clear();
    q.shadows.partValue.clear();
    q.shadows.partUnit.clear();
    if (!scene.activeVolumes().empty())
        traceMedia(c, q);
    else
        q.media = false;
    gatherLights(c, film, q);
    sortByMaterial(scene, q);
    shadeHits(c, q);
    if (q.walkCount > 0)
        walkSubsurface(c, q);
    if (q.media)
        shadeVolumes(c, q);
}

// Fake caustics, phase 2 (docs/design/M4.md 3.3): along a shadow ray that
// no other surface blocks, the product of the fake-caustic surfaces'
// transmittances (closest first, at most 8 crossings). `category`: the
// light's shadow-link category (0xFFFF: none); objects outside its shadow
// set neither block nor tint the ray, as in phase 1 (M9 D15), they are
// stepped over. Returns the rays traced in `rays`.
Spectrum fakeCausticTransmittance(const Scene &scene, const ClosureContext &ctx, const RenderSettings &settings,
                                  uint32_t depth, Vec3f org, Vec3f dir, float tMax, float time, float seed,
                                  uint16_t category, MaterialEvalScratch &scratch, uint32_t &rays,
                                  uint8_t flags = 0) {
    Spectrum T = spectrum(1.0f);
    const uint8_t type = uint8_t(RayClass::Shadow);
    // The hit filter sees the shadow ray's flags (background points, holdout.h).
    const uint8_t filterType = uint8_t(type | flags);
    const ShadeGlobals globals{settings.frame, settings.fps};
    for (int crossing = 0; crossing < 8; ++crossing) {
        float t = 0.0f;
        uint32_t mesh = kInvalidId, prim = 0, instance = 0;
        Vec2f uv{0.0f, 0.0f};
        HitBatch hits{&t, &mesh, &prim, &uv, &instance};
        RayBatch ray{&org, &dir, &tMax, 1, &time, &seed, &filterType, depth};
        ray.globals = &globals;
        scene.accel().intersect(ray, hits);
        ++rays;
        if (mesh == kInvalidId)
            return T;
        const bool excluded = category != 0xFFFFu && !scene.castsShadow(mesh, instance, category);
        if (!excluded && !scene.meshRecord(mesh).fakeCaustics)
            return spectrum(0.0f); // not expected: phase 1 found no blocker
        const SurfaceHit hit = scene.resolveHit(mesh, prim, uv, t, instance, time, org, dir);
        if (!excluded) {
            const SurfaceInputs *inputs = nullptr;
            evaluateSurfaceInputs(scene, scene.materialIdOf(mesh, prim), &hit, &dir, &time, 1, &inputs, scratch,
                                  globals, HitTextureData{}, HitRayData{&type, depth});
            ClosureList cl;
            buildSurfaceClosure(*inputs, hit, -dir, ctx, cl);
            T *= fakeCausticFactor(cl, -dir, scene.meshRecord(mesh).shading, *ctx.tables);
            if (isBlack(T))
                return T;
        }
        const Vec3f next = offsetRayOrigin(hit.P, faceForward(hit.Ng, dir));
        if (tMax < kInf)
            tMax -= dot(next - org, dir);
        org = next;
        if (!(tMax > 0.0f))
            return T;
    }
    return T;
}

// Emissive triangles (docs/design/M9.md D14): the emission where each shadow
// ray towards one ends, for the rays that count (unoccluded, or all of them
// for unoccluded; outputs), evaluated per material in batches and seen from
// the ray's origin; the contribution and its parts take it, then the clamp
// of a light sample.
void emitterShadowRadiance(const Scene &scene, const RenderSettings &settings, const ClosureContext &ctx,
                           uint32_t depth, WaveQueues &q, bool unit) {
    ShadowQueue &s = q.shadows;
    const LightData *lights = scene.lights().data();
    const auto materialOf = [&](uint32_t i) {
        const LightData &l = lights[s.emitter[i]];
        return scene.materialIdOf(l.mesh, l.prim);
    };
    thread_local std::vector<std::pair<MaterialId, uint32_t>> order;
    order.clear();
    for (uint32_t i = 0; i < s.rays.size; ++i)
        if (s.emitter[i] != kInvalidId && (!s.occluded[i] || unit))
            order.emplace_back(materialOf(i), i);
    std::sort(order.begin(), order.end());
    SurfaceHit hits[kShadeChunk];
    Vec3f dirs[kShadeChunk];
    float times[kShadeChunk];
    uint8_t rayTypes[kShadeChunk];
    const SurfaceInputs *inputs[kShadeChunk];
    for (size_t a = 0; a < order.size();) {
        const MaterialId material = order[a].first;
        size_t b = a;
        int n = 0;
        for (; b < order.size() && n < kShadeChunk && order[b].first == material; ++b, ++n) {
            const uint32_t i = order[b].second;
            const LightData &l = lights[s.emitter[i]];
            const Vec2f bary = s.emitterBary[i];
            const Vec3f P = l.position + l.axisU * bary.x + l.axisV * bary.y;
            hits[n] = scene.resolveHit(l.mesh, l.prim, bary, length(P - s.rays.org[i]), l.instance, s.rays.time[i],
                                       s.rays.org[i], s.rays.dir[i]);
            dirs[n] = s.rays.dir[i];
            times[n] = s.rays.time[i];
            rayTypes[n] = uint8_t(RayClass::Shadow);
        }
        evaluateSurfaceInputs(scene, material, hits, dirs, times, n, inputs, q.materialScratch,
                              ShadeGlobals{settings.frame, settings.fps}, HitTextureData{},
                              HitRayData{rayTypes, depth + 1});
        for (int j = 0; j < n; ++j) {
            const uint32_t i = order[a + size_t(j)].second;
            ClosureList cl;
            buildSurfaceClosure(*inputs[j], hits[j], -dirs[j], ctx, cl);
            const Spectrum Le = isFinite(cl.emission) ? cl.emission : spectrum(0.0f);
            const Spectrum value = s.contribution[i] * Le;
            const float scale = clampScale(value, s.emitterClamp[i]);
            s.contribution[i] = value * scale;
            for (uint32_t k = s.partBegin[i], end = k + s.partCount[i]; k < end && k < s.partValue.size(); ++k) {
                s.partValue[k] = s.partValue[k] * Le * scale;
                s.partUnit[k] = s.partUnit[k] * Le;
            }
        }
        a = b;
    }
}

// Karma's bgoccludeshadow (core/integrator/holdout.h): a shadow ray from a
// background point that a background holdout occludes keeps its light (the
// plate shows that shadow already): unoccluded, and marked
// kRayBackgroundLit so that fake-caustic objects (phase 2) do not shadow it
// either. Every such ray is probed, since phase 1 skips fake-caustic
// blockers. Returns the rays traced.
uint32_t probeBackgroundOccluders(const Scene &scene, ShadowQueue &s, uint32_t depth, const ShadeGlobals &globals) {
    thread_local RayQueue probe;
    thread_local std::vector<uint32_t> index;
    thread_local std::vector<uint8_t> hit;
    probe.reserve(s.rays.size);
    index.clear();
    for (uint32_t i = 0; i < s.rays.size; ++i) {
        if (!(s.rays.rayType[i] & kRayFromBackground))
            continue;
        probe.push(s.rays.org[i], s.rays.dir[i], s.rays.tMax[i], i, s.rays.time[i], s.rays.filterSeed[i],
                   RayClass::Shadow);
        probe.rayType[probe.size - 1] |= kRayProbeBackground;
        index.push_back(i);
    }
    if (probe.size == 0)
        return 0;
    hit.resize(probe.size);
    scene.accel().occluded(probe.batch(depth, &globals), hit.data());
    for (uint32_t j = 0; j < probe.size; ++j)
        if (hit[j]) {
            s.occluded[index[j]] = 0;
            s.rays.rayType[index[j]] |= kRayBackgroundLit;
        }
    return probe.size;
}

// Stage 4: occlusion tests for the light samples; unoccluded ones add their
// radiance.
uint32_t traceShadowRays(const Scene &scene, const RenderSettings &settings, const ClosureContext &ctx, uint32_t depth,
                         WaveQueues &q, Film &film, const LpeView *lpe) {
    ShadowQueue &s = q.shadows;
    if (s.rays.size == 0)
        return 0;
    const ShadeGlobals globals{settings.frame, settings.fps};
    // Shadow rays: RayClass::Shadow, RayDepth the bounce's (M4 3.1).
    RayBatch batch = s.rays.batch(depth, &globals);
    if (scene.shadowLinking())
        batch.shadowCategory = s.shadowCategory.data(); // D15: only a light's shadow set blocks it
    scene.accel().occluded(batch, s.occluded.data());
    uint32_t rays = s.rays.size;
    if (scene.backgroundOccludesShadows() && scene.anyBackground())
        rays += probeBackgroundOccluders(scene, s, depth, globals);
    const bool fake = scene.anyFakeCaustics();
    const bool media = !scene.activeVolumes().empty();
    const bool unit = lpe && lpe->unit;
    if (s.emitterCount > 0)
        emitterShadowRadiance(scene, settings, ctx, depth, q, unit);
    for (uint32_t i = 0; i < s.rays.size; ++i) {
        const bool blocked = s.occluded[i] != 0;
        if (blocked && !unit)
            continue;
        const uint32_t p = s.rays.path[i];
        Spectrum T = spectrum(1.0f);
        if (!blocked) {
            Spectrum c = s.contribution[i];
            if (fake && !(s.rays.rayType[i] & kRayBackgroundLit)) {
                T = fakeCausticTransmittance(scene, ctx, settings, depth, s.rays.org[i], s.rays.dir[i],
                                             s.rays.tMax[i], s.rays.time[i], s.rays.filterSeed[i],
                                             scene.shadowLinking() ? s.shadowCategory[i] : uint16_t(0xFFFFu),
                                             q.materialScratch, rays,
                                             uint8_t(s.rays.rayType[i] & kRayFromBackground));
                c *= T;
            }
            if (media && (!isBlack(c) || unit)) {
                // The volumes between the point and the light (D9), seen as
                // a shadow ray by their shaders (D15).
                MediumRay ray;
                ray.o = s.rays.org[i];
                ray.d = s.rays.dir[i];
                ray.tEnd = s.rays.tMax[i];
                ray.time = s.rays.time[i];
                ray.rayType = uint8_t(RayClass::Shadow);
                ray.depth = depth;
                const uint32_t stream = stream::mediumShadow(depth);
                Rng rng = Rng::make(q.pixelSeed[p], q.sampleIndex[p], stream);
                const Spectrum Tm = trackMedia(scene, q.volumes, globals, ray,
                                               Sampler{q.pixelSeed[p], q.sampleIndex[p]}.get1D(stream), rng, nullptr);
                T *= Tm;
                c *= Tm;
            }
            addRadiance(q, p, c);
        }
        if (!lpe)
            continue;
        // unoccluded; takes the light before shadowing, shadow;'s lit sum
        // what passes (D7).
        for (uint32_t j = s.partBegin[i], end = j + s.partCount[i]; j < end; ++j) {
            if (!blocked)
                lpeDeposit(*lpe, film, q.pixel[p], s.partState[j], s.partValue[j] * T);
            if (unit) {
                lpeDepositUnit(*lpe, film, q.pixel[p], s.partState[j], s.partUnit[j], true, false);
                if (!blocked)
                    lpeDepositUnit(*lpe, film, q.pixel[p], s.partState[j], s.partUnit[j] * T, false, true);
            }
        }
    }
    return rays;
}

} // namespace

// The ambient occlusion AOV's rays (one per camera hit, D9): unoccluded
// ones add 1 to the pixel's sum. Every surface blocks them, fake-caustic
// ones too (M6: AO has no transmittance phase), by its opacity.
uint32_t traceAoRays(const Scene &scene, UtilityPlane &ao, WaveQueues &q, const ShadeGlobals &globals) {
    RayQueue &r = q.aoRays;
    if (r.size == 0)
        return 0;
    thread_local std::vector<uint8_t> occluded;
    occluded.resize(r.size);
    RayBatch batch = r.batch(0, &globals);
    batch.skipFakeCaustics = false;
    scene.accel().occluded(batch, occluded.data());
    for (uint32_t i = 0; i < r.size; ++i)
        if (!occluded[i] && !q.aoOccluded[i])
            ao.data[q.pixel[r.path[i]]] += 1.0f;
    return r.size;
}

uint64_t renderWave(const Scene &scene, const Camera &camera, const RenderSettings &settings,
                    const PixelFilter &filter, const TileRect &tile, const uint32_t *pixels, uint32_t pixelCount,
                    Film &film, WaveQueues &q, const LpeTables *tables, uint32_t samplesPerPixel) {
    const LpeView lpeView = tables ? tables->view() : LpeView{};
    const LpeView *lpe = tables ? &lpeView : nullptr;
    const uint32_t pixelPaths = pixels ? pixelCount : tile.pixelCount();
    const uint32_t paths = pixelPaths * samplesPerPixel;
    q.reserve(paths);
    uint64_t rays = 0;
    const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
    const ShadeGlobals globals = shadeGlobals(settings, camera);
    // First-hit AOVs for the pixels without a sample yet.
    q.firstHitPixels.clear();
    forEachPixel(tile, pixels, pixelCount, film.width, [&](int x, int y, uint32_t) {
        const uint32_t pix = uint32_t(y) * uint32_t(film.width) + uint32_t(x);
        if (film.count[pix] == 0)
            q.firstHitPixels.push_back(pix);
    });
    if (!q.firstHitPixels.empty()) {
        const bool all = q.firstHitPixels.size() == pixelPaths;
        renderFirstHitAovs(scene, camera, tile, all ? pixels : q.firstHitPixels.data(),
                           all ? pixelCount : uint32_t(q.firstHitPixels.size()), film.width, film, q, ctx,
                           settings.textureFilterWidth, globals);
        rays += q.firstHitPixels.size();
    }
    q.records = film.wantsSampleRecords();
    if (q.records)
        q.camera.reserve(paths);
    generateCameraRays(camera, filter, settings.seed, tile, pixels, pixelCount, samplesPerPixel, film, q,
                       scene.anyFiltered());
    if (lpe)
        std::fill(q.lpeState.begin(), q.lpeState.begin() + paths, lpe->program.start); // after the camera event

    UtilityPlane *ao = film.utility.empty() ? nullptr : film.findUtility(UtilityKind::Ao);
    for (uint32_t depth = 0; depth <= settings.maxDepth && q.rays.size > 0; ++depth) {
        rays += q.rays.size;
        intersectRays(scene, q, depth, globals);
        if (q.lightQuery)
            rays += q.rays.size;
        q.aoRays.size = 0;
        shadeSurfaces(scene, settings, depth, film, q, ctx, lpe, ao, globals);
        rays += q.walkRays;
        q.walkRays = 0;
        rays += traceShadowRays(scene, settings, ctx, depth, q, film, lpe);
        if (ao && depth == 0)
            rays += traceAoRays(scene, *ao, q, globals);
        std::swap(q.rays, q.nextRays);
    }

    for (uint32_t slot = 0; slot < paths; ++slot) {
        const Spectrum L = q.radiance[slot];
        if (isFinite(L)) // a NaN/Inf sample counts as black instead of poisoning the pixel
            film.colorSum[q.pixel[slot]] += L;
        ++film.count[q.pixel[slot]];
    }
    if (q.records) // filtered utility planes and the deep map (camera_samples.h)
        accumulateCameraSamples(scene, camera, globals, film,
                                CameraSampleView::of(q.camera, paths, q.pixel.data(), q.sampleIndex.data(),
                                                     q.radiance.data()));
    if (q.walksDone > 0) {
        g_walks.fetch_add(q.walksDone, std::memory_order_relaxed);
        g_walksCapped.fetch_add(q.walksCapped, std::memory_order_relaxed);
        q.walksDone = q.walksCapped = 0;
    }
    return rays;
}

uint64_t renderFirstHits(const Scene &scene, const Camera &camera, const RenderSettings &settings, const TileRect &tile,
                         Film &film, WaveQueues &q, const uint32_t *pixels, uint32_t pixelCount) {
    q.reserve(pixels ? pixelCount : tile.pixelCount());
    const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
    q.firstHitPixels.clear();
    forEachPixel(tile, pixels, pixelCount, film.width, [&](int x, int y, uint32_t) {
        const uint32_t pix = uint32_t(y) * uint32_t(film.width) + uint32_t(x);
        if (film.count[pix] == 0)
            q.firstHitPixels.push_back(pix);
    });
    if (q.firstHitPixels.empty())
        return 0;
    renderFirstHitAovs(scene, camera, tile, q.firstHitPixels.data(), uint32_t(q.firstHitPixels.size()), film.width, film,
                       q, ctx, settings.textureFilterWidth, shadeGlobals(settings, camera));
    return q.firstHitPixels.size();
}

void accumulateCrypto(const Scene &scene, Film &film, uint32_t pixel, MeshId mesh, uint32_t prim, uint32_t instance) {
    cryptoAccumulate(scene, film, pixel, mesh, prim, instance);
}

ShadeGlobals shadeGlobals(const RenderSettings &settings, const Camera &camera) {
    ShadeGlobals g{settings.frame, settings.fps};
    const Mat4 clip = camera.worldToView * inverse(camera.ndcToView);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            g.worldToClip[i][j] = float(clip.m[i][j]);
    g.eye = camera.viewToWorld.transformAffine({0.0f, 0.0f, 0.0f});
    g.forward = normalize(camera.viewToWorld.transformDir({0.0f, 0.0f, -1.0f}));
    g.hasCamera = 1;
    return g;
}

void subsurfaceWalkStats(uint64_t &walks, uint64_t &capped) {
    walks = g_walks.load(std::memory_order_relaxed);
    capped = g_walksCapped.load(std::memory_order_relaxed);
}

void evaluateEmission(const Scene &scene, MaterialId material, const SurfaceHit *hits, const Vec3f *dirs, int count,
                      Spectrum *out) {
    thread_local MaterialEvalScratch scratch;
    const ClosureContext ctx{&bsdfTables(), kXyzToRec709Grey};
    for (int a = 0; a < count; a += kShadeChunk) {
        const int n = std::min(kShadeChunk, count - a);
        float times[kShadeChunk] = {};
        const SurfaceInputs *inputs[kShadeChunk];
        evaluateSurfaceInputs(scene, material, hits + a, dirs + a, times, n, inputs, scratch);
        for (int j = 0; j < n; ++j) {
            ClosureList cl;
            buildSurfaceClosure(*inputs[j], hits[a + j], -dirs[a + j], ctx, cl);
            out[a + j] = isFinite(cl.emission) ? cl.emission : spectrum(0.0f);
        }
    }
}

} // namespace nova
