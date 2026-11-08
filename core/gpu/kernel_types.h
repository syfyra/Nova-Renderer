#pragma once

// The GPU wavefront's data (docs/design/M10.md D4, D8, D9): the device scene,
// the wave's SoA queues and the film as plain pointers into device memory,
// shared by the host (core/gpu/gpu_renderer.cpp) and the kernels
// (core/gpu/kernels.cu). The layout follows WaveQueues (wavefront.h); the
// current rays' hit distances are their tMax (TraceParams writes them back).

#include "core/bsdf/energy.h"
#include "core/bsdf/surface.h"
#include "core/film/filter.h"
#include "core/gpu/device_texture.h"
#include "core/integrator/lpe_path.h"
#include "core/integrator/material_lane.h"
#include "core/integrator/medium_track.h"
#include "core/lights/light_sampler.h"
#include "core/lights/light_tree.h"
#include "core/lights/lights.h"
#include "core/scene/attributes.h"
#include "core/scene/camera.h"
#include "core/scene/ray_limits.h"
#include "core/scene/scene_view.h"
#include "core/scene/visibility.h"
#include "core/shading/program.h"

#include <cstdint>

namespace nova::gpu {

// The attributes a material's primvar reads find on a mesh (Scene::attributeRefs):
// one per Program::primvars entry.
struct GpuBinding {
    uint32_t material = 0;
    const AttributeRef *refs = nullptr;
};

// A mesh as the kernels see it: its geometry and instances, the material of
// each triangle (Scene::materialIdOf, the slots resolved on upload), its ray
// limits, its materials' attribute bindings, its emissive triangles treated
// as lights (MeshRecord::emitterOf) and its instances' light-linking
// receiver sets.
struct GpuMesh {
    MeshRecordView view;
    Span<uint16_t> triangleSlots; // per triangle (empty: slot 0)
    Span<uint32_t> slotMaterial;  // per slot (empty: the default material)
    RayLimitOverrides rayLimits;
    Span<GpuBinding> bindings;
    Span<uint32_t> emitterOf;
    uint32_t emitterBase = 0, emitterStride = 0;
    Span<uint32_t> receiverSets; // per instance (empty: set 0)
    Span<uint64_t> shadowMasks; // per instance: the shadow-link categories it is in
    uint8_t fakeCaustics = 0;   // shadow rays cross it with its transmittance (M4 3.3)
    // Ray visibility mask and holdout mode (MeshRecord's, M11 P2).
    uint8_t visibility = kVisibleAll;
    uint8_t holdout = 0; // HoldoutMode

    // Scene::emitterLight and Scene::receiverSet.
    NOVA_INLINE uint32_t emitterLight(uint32_t instance, uint32_t prim) const {
        if (prim >= emitterOf.size() || emitterOf[prim] == 0xFFFFFFFFu)
            return 0xFFFFFFFFu;
        return emitterBase + instance * emitterStride + emitterOf[prim];
    }
    NOVA_INLINE uint32_t receiverSet(uint32_t instance) const {
        return receiverSets.empty() ? 0u : receiverSets[instance < receiverSets.size() ? instance : 0];
    }
    // Scene::castsShadow.
    NOVA_INLINE bool castsShadow(uint32_t instance, uint16_t category) const {
        if (category >= 64)
            return true;
        return !shadowMasks.empty() &&
               ((shadowMasks[instance < shadowMasks.size() ? instance : 0] >> category) & 1u) != 0;
    }

    NOVA_INLINE const AttributeRef *refs(uint32_t material) const {
        for (size_t i = 0; i < bindings.size(); ++i)
            if (bindings[i].material == material)
                return bindings[i].refs;
        return nullptr;
    }
};

// The rows a material program may use on the GPU (one lane in local memory).
constexpr uint32_t kGpuMaxRows = nova::shading::kGpuRowLimit; // shared with the compiler's warning

// A material as the kernels see it (docs/design/M10.md G2): its inputs with
// the uniform outputs applied (MaterialRecord::resolved) and, if they vary
// per hit, its program (Program's code, rows and bindings) and uniform block.
struct GpuMaterial {
    const SurfaceInputs *resolved = nullptr;
    const shading::Instr *code = nullptr; // null: the inputs do not vary
    uint32_t codeSize = 0;
    const float *uniforms = nullptr;
    Span<shading::GlobalBinding> globals;
    Span<shading::PrimvarBinding> primvars;
    Span<shading::OutputBinding> outputs;
    uint32_t primvarNames = 0; // Program::primvars entries (refs per binding)
    int32_t tangentUv = -1;
    const DeviceTexSlot *textures = nullptr; // per texture slot (Tex2D imm)
    uint32_t textureCount = 0;
    uint8_t varying = 0;
    // Opacity (M3 K10): whether some hit may be (partly) transparent, the
    // uniform value, or the per-hit opacity program (Program::opacity, the
    // material's primvar order and textures) with its uniform block.
    uint8_t hasOpacity = 0;
    float uniformOpacity = 1.0f;
    const shading::Instr *opacityCode = nullptr;
    uint32_t opacityCodeSize = 0;
    const float *opacityUniforms = nullptr;
    Span<shading::GlobalBinding> opacityGlobals;
    Span<shading::PrimvarBinding> opacityPrimvars;
    Span<shading::OutputBinding> opacityOutputs;
    uint8_t footprints = 0; // reads textures or footprint-moved values: hits need ray-cone footprints
};

NOVA_INLINE uint32_t materialIdOf(const GpuMesh &m, uint32_t prim) {
    if (m.slotMaterial.empty())
        return 0u; // kDefaultMaterialId
    const uint32_t slot = prim < m.triangleSlots.size() ? m.triangleSlots[prim] : 0u;
    return slot < m.slotMaterial.size() ? m.slotMaterial[slot] : 0u;
}

// A volume as the kernels see it (VolumeRecord's, docs/design/M10.md G5):
// the trackers' data, its transform over the shutter, its fields (NanoVDB
// float grids with their object-to-index maps), and its material's volume
// shader (null code: the default volume, white, extinction = density).
struct GpuVolume {
    MediumVolume medium;
    Affine3f indexToObject; // the reference field's (volume lights' cells)
    Affine3f objectToWorld;
    Span<SrtTransform> motionKeys;
    uint32_t motionSteps = 1;
    Span<const void *> grids;       // per field: its grid (null: none)
    Span<Affine3f> fieldObjectToIndex;
    int32_t reference = -1;
    int32_t velocity[3] = {-1, -1, -1};
    float velocityTime[2] = {0.0f, 0.0f};
    float volumeLimit = -1.0f;
    const shading::Instr *code = nullptr;
    uint32_t codeSize = 0;
    const float *uniforms = nullptr;
    Span<shading::GlobalBinding> globals;
    Span<shading::PrimvarBinding> primvars;
    Span<shading::OutputBinding> outputs;
    const int32_t *bindings = nullptr; // per primvar binding: the fields of its 4 components (-1: fallback, -2 - i: constants[i])
    const float *constants = nullptr;  // a shell's constant primvars (VolumeRecord::constantValues)
    const VolumeInputs *defaults = nullptr; // the inputs with the uniform outputs applied
    const DeviceTexSlot *textures = nullptr;
    uint32_t textureCount = 0;
    uint8_t program = 0;

    NOVA_INLINE Affine3f toWorld(float time) const { // Scene::volumeToWorld
        return motionSteps > 1 ? srtAtTime(motionKeys.data(), motionSteps, 1, time) : objectToWorld;
    }
};

// Light crossings a ray's medium tracks (traceMedia): at most this many per ray.
constexpr int kGpuCrossings = 16;

struct GpuScene {
    const GpuMesh *meshes = nullptr;
    const GpuMaterial *materials = nullptr; // per MaterialId
    // Scene::lights(), the lights before the emissive triangles
    // (Scene::analyticLightCount), the power table over all of them, the
    // light tree (used with useTree, RenderSettings::lightTree), light
    // linking (the tree's links; null: none) and the shadow-linked domes
    // (Scene::shadowLinkedDomes).
    const LightData *lights = nullptr;
    uint32_t lightCount = 0;
    LightSamplerView lightSampler;
    LightTreeView tree;
    uint8_t useTree = 0;
    uint8_t anyEmitters = 0;
    LightLinks links;
    const uint32_t *shadowDomes = nullptr;
    uint32_t shadowDomeCount = 0;
    BsdfTables tables{};
    const float *xyzToRgb = nullptr;
    float tolerance = 0.0f;
    uint8_t hasAreaLights = 0, hasVisibleAreaLights = 0;
    uint8_t anyFiltered = 0; // some mesh is filtered: rays carry opacity seeds (Scene::anyFiltered)
    // Holdouts (core/integrator/holdout.h): some mesh is a holdout; some light
    // has illumbackground and some mesh is a background holdout.
    uint8_t anyHoldouts = 0, illumBackground = 0;
    uint8_t shadowVisibilityDiffers = 0; // Scene::shadowVisibilityDiffers
    // Light path expressions (null program.next: none), set per pass.
    LpeView lpe;
    // Volumes (per VolumeId) and the active ones; whether some depend on the
    // ray level (Karma's modifiers).
    const GpuVolume *volumes = nullptr;
    const uint32_t *activeVolumes = nullptr;
    uint32_t activeVolumeCount = 0;
    uint8_t levelDependent = 0;
    // The sky atmosphere (docs/design/M11-P7.md; volume kInvalidId: none).
    AtmosphereView atmosphere;
    NOVA_INLINE bool anyMedia() const { return activeVolumeCount > 0 || atmosphere.volume != 0xFFFFFFFFu; }
    // Equiangular sampling (M9 D17): the lights that opted in, chosen by power
    // (table slot -> light), each light's probability of being chosen.
    const uint32_t *eqLights = nullptr;
    LightSamplerView eqTable;
    const float *eqPmf = nullptr;
};

// A site's LPE parts on the GPU (lpe_path.h).
using GpuLpeParts = LpeParts<16>;

// What the OptiX programs' any-hit filter reads (TraceParams::filter).
struct GpuFilter {
    GpuScene scene;
    ShadeGlobals globals;
};

struct GpuSettings {
    uint32_t maxDepth = 6;
    uint32_t rrStartDepth = 3;
    float clampIndirect = 0.0f;
    LightStrategy lightStrategy = LightStrategy::MIS;
    RayLimits limits;
    ShadeGlobals globals; // frame and fps (MaterialX time)
    float textureFilterWidth = 0.25f;
    uint8_t ao = 0;     // the film has an ambient occlusion plane
    uint8_t crypto = 0; // the film has cryptomattes: camera hits are recorded
};

// A ray queue (RayQueue) with its length on the device.
struct GpuRays {
    Vec3f *org = nullptr;
    Vec3f *dir = nullptr;
    float *tMax = nullptr; // the hit distance after a closest-hit query
    float *time = nullptr;
    uint32_t *path = nullptr;
    float *filterSeed = nullptr;
    uint8_t *rayType = nullptr;
    uint32_t *count = nullptr;
};

// Subsurface walks (pass C'', WaveQueues::Walk): per walk the shading queued
// its ray in this depth's queue, path, time, entry (mesh, instance, the hit's
// distance, point and normal facing the ray) and medium; the walk raygen
// adds how it ended (WalkResult: exited, its weight, the last segment and
// the crossing). capacity 0: the scene has no walks (a walk ends its path,
// counted).
struct GpuWalks {
    uint32_t *ray = nullptr;
    uint32_t *path = nullptr;
    float *time = nullptr;
    uint32_t *mesh = nullptr;
    uint32_t *instance = nullptr;
    float *hitT = nullptr;
    Vec3f *P = nullptr;
    Vec3f *ng = nullptr;
    SubsurfaceLobe *medium = nullptr;
    uint8_t *exited = nullptr;
    Spectrum *weight = nullptr;
    Vec3f *org = nullptr;
    Vec3f *dir = nullptr;
    float *t = nullptr;
    uint32_t *prim = nullptr;
    Vec2f *uv = nullptr;
    uint32_t *count = nullptr;
    uint32_t capacity = 0;
    // For the walk raygen: the wave's sample numbers, the meshes, the depth.
    const uint32_t *pixelSeed = nullptr;
    const uint32_t *sampleIndex = nullptr;
    const GpuMesh *meshes = nullptr;
    uint32_t depth = 0;
};

// Path state per slot and the per-depth queues (WaveQueues).
struct GpuWave {
    uint32_t *pixel = nullptr;
    Spectrum *beta = nullptr;
    Spectrum *radiance = nullptr;
    float *alpha = nullptr; // the camera segment's alpha (added to the film at the flush)
    uint32_t *pixelSeed = nullptr;
    uint32_t *sampleIndex = nullptr;
    float *bsdfPdf = nullptr;
    float *diffuseShare = nullptr;
    float *etaScale = nullptr;
    Spectrum *sigmaA = nullptr;
    uint8_t *inMedium = nullptr;
    uint64_t *eventCounts = nullptr;
    float *limitDither = nullptr;
    uint8_t *lightOnly = nullptr;
    uint8_t *afterDiffuse = nullptr;
    uint8_t *holdout = nullptr; // the camera ray hit a holdout (HoldoutMode, core/integrator/holdout.h)
    float *coneWidth = nullptr; // the ray cone at the current ray's origin (core/texture/ray_cone.h)
    float *coneSpread = nullptr;
    // The last scattering vertex as its light sample saw it (MIS of BSDF
    // samples with the light tree, M9 D11): position, normal, receiver
    // kind, receiver set.
    Vec3f *prevP = nullptr;
    Vec3f *prevN = nullptr;
    uint8_t *prevReceiver = nullptr;
    uint32_t *prevSet = nullptr;
    // Closest hits of the current rays.
    uint32_t *hitMesh = nullptr;
    uint32_t *hitPrim = nullptr;
    Vec2f *hitUv = nullptr;
    uint32_t *hitInstance = nullptr;
    // The same rays' closest hits as shadow rays, where they find their
    // lights (WaveQueues::lightQuery; lightMesh null: none this depth).
    uint32_t *lightMesh = nullptr;
    float *lightHitT = nullptr;
    uint32_t *lightPrim = nullptr;
    Vec2f *lightUv = nullptr;
    uint32_t *lightInstance = nullptr;
    // Light samples of the current depth (ShadowQueue).
    GpuRays shadow;
    Spectrum *shadowContribution = nullptr;
    uint8_t *occluded = nullptr;
    uint16_t *shadowCategory = nullptr; // the light's shadow-link category (M9 D15)
    // Towards an emissive triangle (M9 D14): the light (kInvalidId: another
    // kind), the point's barycentrics and the clamp; the contribution waits
    // for the emission there.
    uint32_t *shadowEmitter = nullptr;
    Vec2f *shadowBary = nullptr;
    float *shadowClamp = nullptr;
    // Per shadow ray its kind (0: the hit's or event's light sample, 1: an
    // equiangular sample, 2: a walk exit's, 3: a shadow-linked dome's BSDF
    // sample, core/integrator/path.h): the shadow kernel runs once per kind,
    // so a path adds at most one ray per run (deterministic sums, no
    // atomics).
    uint8_t *shadowSlot = nullptr;
    // The ambient occlusion AOV (M6.4): per path its unoccluded rays, the
    // rays at the camera hits and whether each points below the surface.
    float *ao = nullptr;
    GpuRays aoRays;
    uint8_t *aoOccluded = nullptr;
    uint8_t *aoBelow = nullptr;
    // Cryptomattes (D9): per path its camera hit (mesh kInvalidId: none),
    // which the host accumulates.
    uint32_t *cryptoMesh = nullptr;
    uint32_t *cryptoPrim = nullptr;
    // Light path expressions: per path its state; per shadow ray its parts
    // [partBegin, + partCount) in the wave's arena (partUsed: taken so far,
    // partCapacity: its size; parts past it are dropped).
    lpe::State *lpeState = nullptr;
    uint32_t *partBegin = nullptr;
    uint8_t *partCount = nullptr;
    lpe::State *partState = nullptr;
    Spectrum *partValue = nullptr;
    Spectrum *partUnit = nullptr;
    uint32_t *partUsed = nullptr;
    uint32_t partCapacity = 0;
    // Volumes (traceMedia, per ray of this depth): the weight over its
    // segment, the alpha and light transmittances, a scattering event
    // (distance, volume, phase function), the lights crossed with the
    // transmittance in front of each; the rays with an event (pass C').
    Spectrum *mediumT = nullptr;
    Spectrum *mediumAlpha = nullptr;
    Spectrum *lightT = nullptr;
    uint8_t *volumeHit = nullptr;
    float *volumeT = nullptr;
    uint32_t *volumeId = nullptr;
    PhaseFunction *volumePhase = nullptr;
    uint8_t *crossCount = nullptr;
    uint32_t *crossLight = nullptr; // [ray * kGpuCrossings + i]
    float *crossT = nullptr;
    Spectrum *crossAtten = nullptr;
    uint32_t *volumeShade = nullptr;
    uint32_t *volumeShadeCount = nullptr;
    float *eqA = nullptr; // the medium segment equiangular sampling used (empty: none)
    float *eqB = nullptr;
    // Fake caustics (fakeCausticTransmittance, M4 3.3): per shadow ray the
    // transmittance of the fake-caustic surfaces crossed so far and the
    // length left (null: the scene has none); the closest hits of a
    // crossing's queue, whose paths are shadow rays.
    Spectrum *fakeT = nullptr;
    float *fakeLeft = nullptr;
    uint32_t *fakeMesh = nullptr;
    uint32_t *fakePrim = nullptr;
    Vec2f *fakeUv = nullptr;
    uint32_t *fakeInstance = nullptr;
    GpuWalks walks;
};

// The device film: what Film keeps per pixel for the beauty.
struct GpuFilm {
    Spectrum *colorSum = nullptr;
    float *alphaSum = nullptr;
    uint32_t *count = nullptr;
    float *ao = nullptr; // the ambient occlusion plane (null: none)
    int width = 0;
    // Light path expression sums, output-major (Film::lpe, Film::lpeLitSum).
    Spectrum *lpeSum = nullptr;
    Spectrum *lpeLit = nullptr;
    size_t pixels = 0;
    NOVA_INLINE Spectrum *lpe(uint32_t output) const { return lpeSum + size_t(output) * pixels; }
    NOVA_INLINE Spectrum *lpeLitSum(uint32_t slot) const { return lpeLit + size_t(slot) * pixels; }
};

// Kernel arguments (passed by value).
struct GenerateArgs {
    lpe::State lpeStart = 0; // the state after the camera event (0: no light path expressions)
    GpuWave wave;
    GpuRays rays;
    GpuFilm film;
    const Camera *camera = nullptr;
    const PixelFilter *filter = nullptr;
    int x0 = 0, y0 = 0, width = 0; // the region's pixels, row by row
    uint32_t firstPixel = 0;       // this wave's: [firstPixel, firstPixel + pixelCount)
    // A proxy or adaptive pass's film pixels (null: the region's), in
    // waves as the region's.
    const uint32_t *pixels = nullptr;
    // The camera cone (width, spread) per 32 x 32 tile of the region, as the
    // CPU's tiles have it; tiles per row.
    const Vec2f *tileCones = nullptr;
    int tilesX = 0;
    uint32_t pixelCount = 0;
    uint32_t spp = 1;
    uint32_t seed = 0;
    uint8_t filterSeeds = 0; // rays carry opacity seeds (some mesh is filtered)
};

struct ShadeArgs {
    GpuScene scene;
    GpuSettings settings;
    GpuFilm film;
    GpuWave wave;
    GpuRays rays; // this depth's rays and their hits
    GpuRays next; // the continuations
    uint32_t depth = 0;
};

struct ShadowArgs {
    GpuScene scene;
    GpuSettings settings;
    GpuFilm film;
    GpuWave wave;
    uint32_t depth = 0;
    uint32_t slot = 0; // the shadow rays of this kind
};

struct AoArgs {
    GpuWave wave;
};

struct FlushArgs {
    GpuWave wave;
    GpuFilm film;
    uint32_t pixelCount = 0;
    uint32_t spp = 1;
};

} // namespace nova::gpu
