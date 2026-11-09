#pragma once

#include "core/film/crypto.h"
#include "core/math/spectrum.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nova {

// Utility AOVs the host asks for (docs/design/M6.md D9; Karma's ray:*
// sources, conventions measured in M6.4). All but Ao are the values at the
// first hit through the pixel centre, unfiltered; ids are stored as floats,
// -1 on the background.
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
    // per sample like Ao (filtered): the line (w colour, w); the line types
    // (colour where the sample found the type, w there); the debug planes
    // (the camera hit's object id, its outline thickness, the colour).
    Outline,
    OutlineContour,
    OutlineCrease,
    OutlineDepth,
    OutlineId,
    OutlineThickness,
    OutlineColor,
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
    bool operator==(const UtilityRequest &) const = default;
};

struct UtilityPlane {
    UtilityRequest request;
    uint8_t width = 3;       // floats per pixel
    std::vector<float> data; // pixel-major
};

// Floats per pixel a utility plane stores.
uint8_t utilityWidth(UtilityKind kind);

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

    void resize(int w, int h);
    void clear();
    // Sets the utility planes (cleared).
    void setUtility(const std::vector<UtilityRequest> &requests);
    // Sets the cryptomatte planes (cleared).
    void setCrypto(const std::vector<CryptoRequest> &requests);
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
