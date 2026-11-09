#pragma once

// Wavefront path tracer. A wave is one sample for every pixel of a tile; it
// runs through the stages Generate -> Intersect -> Shade -> Shadow once per
// bounce, and every stage works on SoA queues. The CUDA backend (M10) keeps
// the same stages and data layout, only the queues live in device memory and
// each stage becomes a kernel launch.

#include "core/accel/accel.h"
#include "core/bsdf/closure.h"
#include "core/film/film.h"
#include "core/film/filter.h"
#include "core/film/lpe.h"
#include "core/integrator/camera_samples.h"
#include "core/integrator/lpe_path.h"
#include "core/integrator/material_eval.h"
#include "core/integrator/material_lane.h"
#include "core/integrator/nested.h"
#include "core/integrator/settings.h"
#include "core/integrator/volume_media.h"
#include "core/math/spectrum.h"
#include "core/scene/camera.h"
#include "core/scene/scene.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace nova {

class Baker;

// Film pixels [x0, x1) x [y0, y1).
struct TileRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    uint32_t pixelCount() const { return uint32_t(width() * height()); }
};

struct RayQueue {
    std::vector<Vec3f> org;
    std::vector<Vec3f> dir;
    std::vector<float> tMax;
    std::vector<float> time;    // normalized shutter time; every ray of a path shares it
    std::vector<uint32_t> path; // owning path slot
    std::vector<float> filterSeed; // stochastic opacity sample (the hit filter)
    std::vector<uint8_t> rayType;  // RayClass of the event that spawned the ray
    uint32_t size = 0;

    void reserve(uint32_t capacity);
    void push(Vec3f o, Vec3f d, float t, uint32_t pathSlot, float rayTime, float seed, RayClass type) {
        org[size] = o;
        dir[size] = d;
        tMax[size] = t;
        time[size] = rayTime;
        path[size] = pathSlot;
        filterSeed[size] = seed;
        rayType[size] = uint8_t(type);
        ++size;
    }
    // `depth`: scattering events before these rays; `globals`: the render's
    // frame for the hit filter's opacity programs (RayBatch::globals).
    RayBatch batch(uint32_t depth = 0, const ShadeGlobals *globals = nullptr) const {
        RayBatch b{org.data(), dir.data(), tMax.data(), size, time.data(), filterSeed.data(), rayType.data(), depth};
        b.globals = globals;
        return b;
    }
};

struct ShadowQueue {
    RayQueue rays;
    std::vector<Spectrum> contribution; // radiance added if unoccluded
    std::vector<uint8_t> occluded;
    // Light path expressions: the contribution split by final LPE state,
    // parts [partBegin, partBegin + partCount) of the part buffers.
    std::vector<uint32_t> partBegin;
    std::vector<uint8_t> partCount;
    std::vector<lpe::State> partState;
    std::vector<Spectrum> partValue;
    std::vector<Spectrum> partUnit; // the part with MIS weight 1, unclamped (unoccluded;, shadow;)
    std::vector<uint16_t> shadowCategory; // per ray: its light's shadow-link category (M9 D15)
    // Emissive triangles (M9 D14): per ray its light (kInvalidId: another
    // kind), the point's barycentrics and the clamp; the contribution and
    // its parts wait for the emission there (traceShadowRays). How many.
    std::vector<uint32_t> emitter;
    std::vector<Vec2f> emitterBary;
    std::vector<float> emitterClamp;
    uint32_t emitterCount = 0;

    void reserve(uint32_t capacity);
    // More room, keeping the rays pushed (a path's light samples per depth
    // are bounded, WaveQueues::reserve; this keeps a miscount safe).
    void grow();
    // `flags`: the hit filter's ray flags (kRayFromBackground, core/scene/visibility.h).
    void push(Vec3f o, Vec3f d, float t, uint32_t pathSlot, Spectrum c, float rayTime, float seed,
              uint16_t category = 0xFFFFu, uint8_t flags = 0) {
        if (rays.size >= contribution.size())
            grow();
        contribution[rays.size] = c;
        shadowCategory[rays.size] = category;
        emitter[rays.size] = kInvalidId;
        rays.push(o, d, t, pathSlot, rayTime, seed, RayClass::Shadow);
        rays.rayType[rays.size - 1] |= flags;
    }
    // The last ray pushed goes to emissive triangle `light`.
    void markEmitter(uint32_t light, Vec2f bary, float clamp) {
        emitter[rays.size - 1] = light;
        emitterBary[rays.size - 1] = bary;
        emitterClamp[rays.size - 1] = clamp;
        ++emitterCount;
    }
};

// Light path expressions for one pass (docs/design/M6.md D3, D4): the
// program and the tag and label ids of the scene's objects, lights and lobe
// labels (Renderer builds them when the program or the scene changes).
struct LpeTables {
    static constexpr uint16_t kMaterialTag = 0xFFFF; // the object has no tag: its material's

    lpe::Program program;
    std::vector<uint16_t> meshTag;     // per MeshId
    std::vector<uint16_t> materialTag; // per MaterialId
    std::vector<uint16_t> volumeTag;   // per VolumeId (its own or its material's)
    std::vector<uint16_t> lightTag;    // per Scene::lights() index
    uint16_t label[size_t(LobeLabel::kCount)] = {};
    // A events (D8): per state whether one leads to outputs; any at all.
    std::vector<uint8_t> albedoAfter;
    bool albedo = false;
    uint32_t albedoSymbol = 0, backgroundSymbol = 0;
    // unoccluded; and shadow; outputs (D7): weight-1 light is summed; the
    // lit sum's slot of each output (Film::lpeLit, -1: none).
    bool unit = false;
    std::vector<int32_t> shadowSlot;
    // Per state its outputs by kind, [begin[s], begin[s + 1]): the plain
    // sums (D6), the sums of weight-1 light before shadowing (unoccluded; and
    // shadow;, D7) and shadow;'s lit slots; holdout; outputs by their other
    // flags (only holdout paths reach their states).
    std::vector<uint32_t> plainBegin, unitBegin, litBegin;
    std::vector<uint16_t> plainOuts, unitOuts, litSlots;
    // Per unitOuts entry: a shadow; output (not unoccluded;), whose sums
    // background points treat apart (docs/design/M11-P2.md D14); the lit
    // slot of their whole unoccluded light (0xFFFF: no holdout shadow;
    // outputs).
    std::vector<uint8_t> unitShadow;
    uint16_t holdoutTotalSlot = 0xFFFF;

    uint32_t objectTag(MeshId mesh, MaterialId material) const {
        const uint16_t t = mesh < meshTag.size() ? meshTag[mesh] : 0;
        if (t != kMaterialTag)
            return t;
        return material < materialTag.size() ? materialTag[material] : 0u;
    }
    // The tables as plain arrays (core/integrator/lpe_path.h).
    LpeView view() const {
        LpeView v;
        v.program = program;
        v.meshTag = spanOf(meshTag);
        v.materialTag = spanOf(materialTag);
        v.volumeTag = spanOf(volumeTag);
        v.lightTag = spanOf(lightTag);
        for (size_t i = 0; i < size_t(LobeLabel::kCount); ++i)
            v.label[i] = label[i];
        v.albedoAfter = spanOf(albedoAfter);
        v.albedo = albedo;
        v.albedoSymbol = albedoSymbol;
        v.backgroundSymbol = backgroundSymbol;
        v.unit = unit;
        v.plainBegin = spanOf(plainBegin);
        v.unitBegin = spanOf(unitBegin);
        v.litBegin = spanOf(litBegin);
        v.plainOuts = spanOf(plainOuts);
        v.unitOuts = spanOf(unitOuts);
        v.litSlots = spanOf(litSlots);
        v.unitShadow = spanOf(unitShadow);
        v.holdoutTotalSlot = holdoutTotalSlot;
        return v;
    }
};

// The BSDF label names of native lobes in light path expressions (Karma's:
// coats and SSS are labelled; sheen is a plain D event, measured: Karma's
// C<...'sheen'>.*L of a Standard Surface sheen is 0).
inline const char *lpeLabelName(LobeLabel label) {
    switch (label) {
    case LobeLabel::Coat:
        return "coat";
    case LobeLabel::Sss:
        return "sss";
    default:
        return "";
    }
}

// Per-thread storage for one wave, reused across tiles and passes.
struct WaveQueues {
    // Path state, indexed by path slot (one slot per tile pixel).
    std::vector<uint32_t> pixel;      // film index
    std::vector<Spectrum> beta;       // path throughput
    std::vector<Spectrum> radiance;   // radiance gathered so far
    std::vector<uint32_t> pixelSeed;  // sampler seed of the path's image pixel
    std::vector<uint32_t> sampleIndex; // the path's sample number in its pixel (the pixel's count)
    std::vector<float> bsdfPdf;       // density of the last BSDF sample (for MIS); < 0 after a delta event
    std::vector<float> diffuseShare;  // diffuse-class share of the last BSDF sample (light multipliers)
    std::vector<float> etaScale;      // product of squared relative indices of refractions (Russian roulette)
    std::vector<Spectrum> sigmaA;     // absorption of the medium the path is in
    std::vector<uint8_t> inMedium;    // the path is inside a refractive, absorbing material
    std::vector<NestedMedia> nested;  // the media by priority (nested dielectrics, core/integrator/nested.h)
    std::vector<float> coneWidth;     // ray cone at the current ray's origin (core/texture/ray_cone.h)
    std::vector<float> coneSpread;
    // Ray limits (core/scene/ray_limits.h): events per LimitClass so far,
    // a byte each (diffuse, reflect, refract, sss, volume); the dither of
    // fractional limits; a light-only ray (it gathers lights, then the path
    // ends).
    std::vector<uint64_t> eventCounts;
    std::vector<float> limitDither;
    std::vector<uint8_t> lightOnly;
    std::vector<uint8_t> afterDiffuse; // a diffuse or SSS event happened (fake caustics)
    std::vector<float> pathAlpha;      // the roughest glossy event so far (GGX alpha; constrainRoughness)
    // The camera ray hit a holdout (HoldoutMode; docs/design/M11-P2.md): the
    // path's light goes to holdout; outputs only, not to the beauty.
    std::vector<uint8_t> holdout;
    // The last scattering vertex, as its light sample saw it (the light
    // tree's selection probabilities for MIS of BSDF samples, M9 D11):
    // position, normal and receiver kind (Receiver).
    std::vector<Vec3f> prevP, prevN;
    std::vector<uint8_t> prevReceiver;
    std::vector<uint32_t> prevSet; // its light-linking receiver set (M9 D15, path.h)
    std::vector<lpe::State> lpeState;  // the path's events so far (light path expressions)
    std::vector<uint32_t> firstHitPixels; // the wave's pixels without samples yet (first-hit AOVs)
    // Per-sample records of the camera hits (filtered utility planes, deep
    // maps; docs/design/M11-P6.md 4), kept when the film wants them.
    bool records = false;
    CameraSampleRecords camera;

    // Karma's outlines (core/integrator/outline.h): per path its camera
    // sample's line; per camera ray its hit as the stencil compares with
    // (valid: the hit's object has outlines), its tan radius and the
    // stencil's flagged rays and line types so far; the stencil's rays (one
    // per valid camera ray and pass, outlineSource: that ray) and their hits.
    std::vector<OutlineSample> outline;
    std::vector<OutlineMain> outlineMain;
    std::vector<float> outlineRadius;
    std::vector<uint8_t> outlineValid;
    std::vector<uint16_t> outlineFlagged;
    std::vector<uint8_t> outlineTypes;
    RayQueue outlineRays;
    std::vector<uint32_t> outlineSource;
    std::vector<float> outlineHitT;
    std::vector<uint32_t> outlineHitMesh, outlineHitPrim, outlineHitInstance;
    std::vector<Vec2f> outlineHitUv;

    RayQueue rays;
    RayQueue nextRays;
    RayQueue aoRays; // the ambient occlusion AOV's rays at the camera hits (M6.4)
    std::vector<uint8_t> aoOccluded;
    std::vector<float> hitT;
    std::vector<uint32_t> hitMesh;
    std::vector<uint32_t> hitPrim;
    std::vector<Vec2f> hitUv;
    std::vector<uint32_t> hitInstance;
    // Where scattered rays find their lights when shadow visibility differs
    // from theirs (Scene::shadowVisibilityDiffers): the closest hit of the
    // same rays as shadow rays (lightQuery set for this depth).
    bool lightQuery = false;
    std::vector<float> lightHitT;
    std::vector<uint32_t> lightHitMesh, lightHitPrim, lightHitInstance;
    std::vector<Vec2f> lightHitUv;

    // At most three light samples per path and bounce: at the hit (or the
    // volume event), at the exit of a subsurface walk that starts there, and
    // the equiangular sample of the ray's medium segment (M9 D17).
    ShadowQueue shadows;

    // Shading order: the hits that reach the shading pass with their
    // material, and the same hits grouped by material (stable).
    std::vector<uint32_t> shadeRay;      // ray index
    std::vector<uint32_t> shadeMaterial; // MaterialId
    std::vector<uint32_t> shadeOrder;    // ray indices, grouped by material
    std::vector<uint32_t> shadeOrderMaterial;
    std::vector<uint32_t> materialStart; // counting-sort offsets
    uint32_t shadeCount = 0;
    MaterialEvalScratch materialScratch; // shade-stage program rows and surface inputs
    // Contributions of one site by final LPE state (several lights).
    std::vector<lpe::State> lpePartState;
    std::vector<Spectrum> lpePartValue;
    std::vector<Spectrum> lpePartUnit;

    // Volumes (docs/design/M8.md D9): this thread's evaluators; per ray of
    // the current queue (with `media`) the volumes' weight over its segment
    // (transmittance, or the free flight's weight up to its event), per area
    // light it crosses the weight in front of it ([crossingBegin, +
    // crossingCount) of the crossing buffers), a scattering event (distance,
    // volume, phase function) and camera rays' alpha transmittance; the rays
    // with an event, for pass C'.
    VolumeEvalCache volumes;
    bool media = false;
    std::vector<Spectrum> mediumT;
    std::vector<uint32_t> crossingBegin;
    std::vector<uint8_t> crossingCount;
    std::vector<uint32_t> crossingLight;
    std::vector<float> crossingT;
    std::vector<Spectrum> crossingAtten;
    std::vector<uint8_t> volumeHit;
    std::vector<float> volumeT;
    std::vector<VolumeId> volumeId;
    std::vector<PhaseFunction> volumePhase;
    std::vector<Spectrum> mediumAlpha;
    // Camera rays with holdout volumes: the other volumes' opacity (flyMedia's
    // alphaV, docs/design/M11-P2.md D15).
    std::vector<Spectrum> mediumAlphaV;
    std::vector<uint32_t> volumeShade;
    uint32_t volumeShadeCount = 0;
    // Level-dependent volumes (traceMedia): the transmittance continuations
    // gather lights with, over their whole segment.
    bool lightSplit = false;
    std::vector<Spectrum> lightT;
    // Equiangular sampling (docs/design/M9.md D17): per ray the medium
    // segment [eqA, eqB) it was done on (empty: none), for the free flight's
    // MIS weights at its event.
    std::vector<float> eqA, eqB;

    // Subsurface walks (docs/design/M9.md): the hits whose continuation is a
    // random walk, for pass C''; the walks' closest-hit queries and the
    // walks killed at the segment cap (render statistics).
    struct Walk {
        uint32_t ray;
        MeshId mesh;
        uint32_t instance;
        float hitT;
        Vec3f P;
        Vec3f ng; // the geometric normal on the side the path came from
        SubsurfaceLobe medium;
    };
    std::vector<Walk> walks;
    uint32_t walkCount = 0;
    uint64_t walkRays = 0;
    uint64_t walksDone = 0, walksCapped = 0;

    void reserve(uint32_t paths);
};

// Traces `samplesPerPixel` samples for every pixel of `tile` (or for the
// `pixelCount` film pixels of `pixels`, all inside the tile) and accumulates
// them into `film`. Each pixel's sample number is its count (docs/design/M7.md D5); a
// pixel's first sample also writes its first-hit AOVs. Tiles never overlap,
// so concurrent calls on different tiles are safe. Returns the number of
// rays traced (closest-hit and occlusion queries). With `lpe` (a program
// with outputs, film.lpeOutputs of them), the light path expressions as well.
// Subsurface walks run and killed at the segment cap since the process
// started (render statistics, docs/design/M9.md).
void subsurfaceWalkStats(uint64_t &walks, uint64_t &capped);

// With `baker` (texture baking, core/bake/bake.h), the camera rays are the
// bake's: every sample's path starts at its shading point (samples without
// one add nothing), and the bake's maps accumulate alongside.
uint64_t renderWave(const Scene &scene, const Camera &camera, const RenderSettings &settings, const PixelFilter &filter,
                    const TileRect &tile, const uint32_t *pixels, uint32_t pixelCount, Film &film, WaveQueues &queues,
                    const LpeTables *lpe = nullptr, uint32_t samplesPerPixel = 1, const Baker *baker = nullptr);

// renderWave's first part alone: the first-hit AOVs of the tile's pixels
// (or of pixels in it) without a sample yet, for passes whose samples
// another device renders (Nova XPU, docs/design/M10.md G4). Returns the
// rays traced.
uint64_t renderFirstHits(const Scene &scene, const Camera &camera, const RenderSettings &settings, const TileRect &tile,
                         Film &film, WaveQueues &queues, const uint32_t *pixels = nullptr, uint32_t pixelCount = 0);

// One sample's cryptomatte ids at a camera hit (mesh, primitive, instance)
// into the film's planes (the GPU gathers the hits, the host accumulates, D9).
void accumulateCrypto(const Scene &scene, Film &film, uint32_t pixel, MeshId mesh, uint32_t prim, uint32_t instance);

// The shading globals of a render: its frame and fps, and the camera for
// NDC positions (Global::Pndc, Karma's space:ndc).
ShadeGlobals shadeGlobals(const RenderSettings &settings, const Camera &camera);

} // namespace nova
