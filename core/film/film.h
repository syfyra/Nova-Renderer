#pragma once

#include "core/film/crypto.h"
#include "core/math/spectrum.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// How a utility plane combines a pixel's samples (Karma's per-var
// karma:filter, "Sample Blending", measured in docs/design/M11-P6.md 2):
// Center is Nova's first hit through the pixel centre (docs/design/M6.md
// D9: no filter authored, or "point"); the others take one value per
// camera sample (the pixel filter's samples, filter importance sampling) at
// the sample's camera hit. Misses add 0, except where noted.
enum class SampleFilter : uint8_t {
    Center,
    Box,        // ubox: the mean, plus the default value
    Accumulate, // accumulate: the sum, plus the default value
    Min,        // minmax min/max: the sample of the smallest/largest
    Max,        // magnitude (|v|, vector length); misses count as the default value
    ZMin,       // minmax zmin/zmax (omin/omax with a PrimId var): the hit
    ZMax,       // nearest to/farthest from the camera; none: the default value
    Last,       // minmax last: the last sample
    Var,        // stats var: the unbiased sample variance
    Mse,        // stats mse: the variance of the mean (var / n)
    Even,       // stats even/odd: the mean of the even/odd-numbered samples
    Odd,
};

// Karma's karma:filter text ('["ubox",{}]', '["minmax",{"mode":"zmin"}]',
// '["stats",{"mode":"var"}]', ...) as a SampleFilter; empty: Center.
// `primIdChannel`: a render var named PrimId is bound (Karma's object
// coverage modes need it). A mode Karma reports as an error (and renders
// as ubox or min) sets `warning`.
SampleFilter parseSampleFilter(const std::string &text, bool primIdChannel, std::string *warning = nullptr);

// Utility AOVs the host asks for (docs/design/M6.md D9; Karma's ray:*
// sources, conventions measured in M6.4 and M11 P6). With the Center filter
// they are the values at the first hit through the pixel centre,
// unfiltered (ids as floats, -1 on the background); with another filter
// one value per camera sample, combined by the filter.
enum class UtilityKind : uint8_t {
    P,            // world position (ray:hitP)
    PCamera,      // camera-space position, -Z forward (ray:hitP_camera, Hydra Peye)
    N,            // shading normal as authored, not turned to the viewer (ray:hitN; Hydra's normal is turned)
    Ng,           // geometric normal as authored (ray:hitNg)
    NCamera,      // the authored shading normal in camera space (ray:hitN_camera)
    NgCamera,     // the authored geometric normal in camera space (ray:hitNg_camera)
    FacingN,      // |N . -dir| (ray:hitN_facingratio)
    FacingNg,     // |Ng . -dir| (ray:hitNg_facingratio)
    HitDist,      // distance from the ray origin (ray:hitdist)
    Face,         // authored face index (ray:primid, ray:element, Hydra elementId)
    MotionVector, // raster displacement over the shutter, pixels, x right, y up (ray:motionvector)
    Velocity,     // world displacement over the shutter (ray:velocity)
    Primvar,      // a primvar at the hit, up to 4 values (sourceType primvar, Hydra primvars:*)
    BaseColor,    // the material's base color (Karma's export_basecolor)
    ShaderAov,    // a material AOV output by name (kma_aov), 3 values
    Ao,           // ambient occlusion: per sample one cosine ray, unlimited distance (filtered)
    // Karma's outlines (docs/design/M11-P9a.md, core/integrator/outline.h),
    // per sample like Ao (the samples' mean, whatever karma:filter says):
    // the line (w colour, w); the line types (colour where the sample found
    // the type, w there); the debug planes (the camera hit's object id, its
    // outline thickness, the colour).
    Outline,
    OutlineContour,
    OutlineCrease,
    OutlineDepth,
    OutlineId,
    OutlineThickness,
    OutlineColor,
    // M11 P6: Karma's remaining raw sources (docs/design/M11-P6.md 3).
    Mask,         // 1 at a hit (ray:mask)
    HitStack,     // the hit object's index, 1 + its Hydra prim id (ray:hitstack: Karma's object index)
    Time,         // the sample's shutter position, 0 = open, 1 = close (ray:time)
    Near,         // the camera's near and far clipping distances along the ray (ray:near, ray:far)
    Far,
    RayOrigin,    // the camera ray at the lens plane: origin and direction (ray:P, ray:D)
    RayDir,
    Contrib,      // the path's throughput at the hit: 1 (ray:contrib; stochastic opacity)
    DPdz,         // 0 on surfaces (ray:dPdz: a volume step)
    HitUv,        // the face's parametric (u, v, 0) (ray:hituv)
    NFacing,      // N and Ng turned to the viewer (ray:hitN_facing, ray:hitNg_facing)
    NgFacing,
    Level,        // the ray level: 0 (ray:level)
    PixelSamples, // the sample's number + 1 at a hit (ray:pixelsamples with a filter)
    Z,            // planar camera depth (ray:hitPz with a filter; unfiltered it is Film::depth)
    ObjectId,     // the Hydra prim id (ray:objectid with a filter; unfiltered Film::primId)
    // Per-sample copies of the beauty and LPE outputs for filters other
    // than ubox: the radiance and alpha (Beauty, 4 values) or an LPE
    // output's value (Lpe, `index`; taken per pass from the film's sums).
    Beauty,
    Lpe,
    // Texture baking (core/bake/bake.h, docs/design/M11-P8.md): the maps of
    // Karma's Texture Baker, filtered (coverage averages) like every plane
    // of a bake.
    BakeNormal,          // Nt: the shading normal in the bake mesh's tangent frame
    BakeHeight,          // Ds: the distance from the bake mesh to the shading point
    BakeVector,          // Vd: the vector from the bake mesh to the shading point
    BakeVectorTangent,   // Vdt: the same in the tangent frame
    BakeOcclusion,       // Oc
    BakeCavity,          // Cv
    BakeThickness,       // Th
    BakeCurvature,       // Cu
    BakeEdge,            // Eg
    BakeRoundedNormal,   // Rn: the rounded-edge normal in the tangent frame
    BakeAlpha,           // Af: coverage
    BakePosition,        // __bake_P: the shading point, in the bake's box
    BakeShadingNormal,   // N (in a bake)
    BakeGeometricNormal, // Ng (in a bake)
    BakeExport,          // export_<name>: a Standard Surface input (or material AOV) at the shading point
};

// Whether a kind is one of the bake's maps.
inline bool isBakeKind(UtilityKind kind) { return kind >= UtilityKind::BakeNormal; }

// Utility planes summed per sample (normalized by the pixel's count) rather
// than written once at the first hit.
inline bool utilityPerSample(UtilityKind kind) {
    return kind == UtilityKind::Ao || (kind >= UtilityKind::Outline && kind <= UtilityKind::OutlineColor);
}

struct UtilityRequest {
    UtilityKind kind = UtilityKind::P;
    std::string name; // Primvar, ShaderAov
    SampleFilter filter = SampleFilter::Center;
    float defaultValue = 0.0f; // karma:defaultvalue
    // Karma's "ray:noholdout;" and "ray:holdout;" prefixes: only hits on
    // objects that are not (1) or are (2) holdouts count, others are misses.
    uint8_t holdouts = 0;
    int32_t index = -1; // Lpe: the LPE program's output
    bool operator==(const UtilityRequest &) const = default;
};

struct UtilityPlane {
    UtilityRequest request;
    uint8_t width = 3;       // floats per pixel
    std::vector<float> data; // pixel-major; with a filter its state (sums, the kept value)
    std::vector<float> aux;  // the filter's second state: sums of squares (width) or the key (1) per pixel
};

// Floats per pixel a utility plane stores.
uint8_t utilityWidth(UtilityKind kind);

// Filtered planes: one sample's value (`width` floats; `hit` false: a miss,
// `v` unused) at camera depth `z`, the sample being the pixel's
// `sampleIndex`-th; and a pixel's resolved value from `count` samples.
void addFilteredSample(UtilityPlane &plane, size_t pixel, uint32_t sampleIndex, const float *v, bool hit, float z);
void resolveFilteredSample(const UtilityPlane &plane, size_t pixel, uint32_t count, float *out);

// Deep camera maps (Karma's karma:deep products, docs/design/M11-P6.md 6):
// per camera sample its depth, radiance, alpha and the extra vars at its
// first event. Samples whose depths differ by at most zbias merge when
// compression is on (their coverage and colour add: flattening keeps the
// pixel exactly).
struct DeepRequest {
    float zbias = 0.001f;
    int compression = 5;          // 0: every sample its own
    bool hitDistance = false;     // karma:global:dcm_usehitdist: Z is the distance from the eye
    bool excludeHoldouts = false; // karma:global:dcm_excludeholdouts
    std::vector<UtilityRequest> extras; // per-sample values of other vars
    bool operator==(const DeepRequest &) const = default;
};

struct DeepPlane {
    DeepRequest request;
    uint32_t stride = 5; // floats per record: z, r, g, b, alpha sum, then the extras (premultiplied by alpha)
    // Per pixel its records; a record's colour, alpha and extras are sums
    // over its samples.
    std::vector<std::vector<float>> pixels;
    void resize(size_t pixelCount);
    void add(size_t pixel, float z, const Spectrum &c, float alpha, const float *extras);
};

// Per-pixel accumulation buffers. Rows are stored bottom-up (Hydra/OpenGL
// convention): row 0 is the bottom of the image.
struct Film {
    int width = 0;
    int height = 0;
    uint32_t samples = 0;          // completed samples per pixel

    std::vector<Spectrum> colorSum; // radiance sum
    std::vector<float> alphaSum;
    // Samples per pixel (D5): every sum below is normalized by its pixel's
    // count. `samples` counts the finished passes.
    std::vector<uint32_t> count;
    // A pixel's sample numbers are sampleOffset + its count: a film that
    // restarts while a path guiding field lives continues the sequence the
    // field learned from (docs/design/M11-P9c.md). clear() keeps it.
    uint32_t sampleOffset = 0;

    // First-hit AOVs, written by the first sample of each pixel (pixel center).
    std::vector<float> depth;       // planar camera-space depth, 0 = background
    std::vector<Vec3f> normal;      // world-space shading normal
    std::vector<Spectrum> albedo;   // surface albedo (denoiser guide)
    std::vector<int32_t> primId;    // host prim id, -1 = background
    std::vector<int32_t> instanceId; // index into the mesh's instance list, -1 = background

    // Light path expression outputs (docs/design/M6.md D6): per output a
    // radiance sum per pixel, output-major. Filtered like colorSum (filter
    // importance sampling: every sample has weight 1).
    uint32_t lpeOutputs = 0;
    std::vector<Spectrum> lpeSum;
    // shadow; outputs (D7) also sum the lit part of their weight-1 light
    // (lpeSum holds it unoccluded); one slot per such output.
    uint32_t lpeShadowOutputs = 0;
    std::vector<Spectrum> lpeLit;
    // Utility AOVs (D9), in the order the host asked for them.
    std::vector<UtilityPlane> utility;
    // Cryptomattes (D10): per pixel the ids and their summed sample weights.
    std::vector<CryptoPlane> crypto;
    // A deep camera map (empty or one plane).
    std::vector<DeepPlane> deep;

    void resize(int w, int h);
    void clear();
    // Sets the utility planes (cleared).
    void setUtility(const std::vector<UtilityRequest> &requests);
    // Sets the cryptomatte planes (cleared).
    void setCrypto(const std::vector<CryptoRequest> &requests);
    // Sets the deep camera map (null: none; cleared).
    void setDeep(const DeepRequest *request);
    // Whether camera samples feed per-sample state (filtered utility planes
    // or a deep map), so renders record each sample's camera hit.
    bool wantsSampleRecords() const;
    // Whether an LPE output has a per-sample filter (taken per pass).
    bool wantsLpeSamples() const;
    // Checkpoints (docs/design/M11-P6.md 5): the film's state as bytes and
    // back. load() fails, leaving the film unchanged, unless the layout
    // (size, planes, outputs) matches.
    void save(std::vector<uint8_t> &out) const;
    bool load(const uint8_t *data, size_t size);
    // The plane of a kind (and name), or null.
    UtilityPlane *findUtility(UtilityKind kind, const std::string &name = {});
    // Sets the number of LPE outputs and shadow; slots (the sums restart at
    // zero).
    void setLpeOutputs(uint32_t outputs, uint32_t shadowOutputs = 0);
    size_t pixelCount() const { return size_t(width) * size_t(height); }
    Spectrum *lpe(uint32_t output) { return lpeSum.data() + size_t(output) * pixelCount(); }
    const Spectrum *lpe(uint32_t output) const { return lpeSum.data() + size_t(output) * pixelCount(); }
    Spectrum *lpeLitSum(uint32_t slot) { return lpeLit.data() + size_t(slot) * pixelCount(); }
    const Spectrum *lpeLitSum(uint32_t slot) const { return lpeLit.data() + size_t(slot) * pixelCount(); }
};

} // namespace nova
