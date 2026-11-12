#pragma once

// The surface models Nova renders natively (docs/design/M3.md K1): Autodesk
// Standard Surface 1.0.1 (and 1.0.0 defaults), OpenPBR 1.1.1, MaterialX
// surface_unlit and (M3.12) UsdPreviewSurface. A material's shading program
// fills one SurfaceInputs block per hit: the union of all models' inputs,
// each in the units its model authors (the build functions apply each
// model's semantics, docs/research/m3/surface-models.md 2.3 and 3.4). The
// build functions turn it into a ClosureList for the fixed outgoing
// direction.

#include "core/bsdf/closure.h"
#include "core/bsdf/closure_graph.h"
#include "core/bsdf/energy.h"

#include <cstring>

namespace nova {

// Graph: a generic pbrlib BSDF graph or a mix of surface shaders
// (SurfaceInputs::graph, core/bsdf/closure_graph.h).
// External: VEX through the CVEX bridge (docs/design/M5.md D7); the block holds
// the shader's parameters, the integrator fills the closure.
// Hair: Karma Hair's controls over the Chiang fibre (docs/design/M9.md D6).
// Plastic: Houdini's plastic, baseWeight baseColor diffuse() + specularWeight
// specularColor phong(1 / specularRoughness): Karma's material for geometry
// without one (docs/bug-audit-2026-11.md section 17).
enum class SurfaceModel : uint8_t {
    Lambert,
    StandardSurface,
    OpenPBR,
    Unlit,
    UsdPreview,
    Graph,
    External,
    Hair,
    Plastic
};

// Every input: name and float count. The order defines SurfaceField and the
// layout of SurfaceInputs::v. Vectors are world space; a zero normal or
// tangent means "the geometric default".
#define NOVA_SURFACE_FIELDS(X)                                                                                         \
    X(baseWeight, 1) X(baseColor, 3) X(diffuseRoughness, 1) X(metalness, 1)                                            \
    X(specularWeight, 1) X(specularColor, 3) X(specularRoughness, 1) X(specularIor, 1) X(specularAnisotropy, 1)        \
    X(specularRotation, 1)                                                                                             \
    X(transmissionWeight, 1) X(transmissionColor, 3) X(transmissionDepth, 1) X(transmissionScatter, 3)                 \
    X(transmissionScatterAnisotropy, 1) X(transmissionDispersion, 1) X(transmissionExtraRoughness, 1)                  \
    X(subsurfaceWeight, 1) X(subsurfaceColor, 3) X(subsurfaceRadius, 3) X(subsurfaceScale, 1)                          \
    X(subsurfaceAnisotropy, 1)                                                                                         \
    X(sheenWeight, 1) X(sheenColor, 3) X(sheenRoughness, 1)                                                            \
    X(coatWeight, 1) X(coatColor, 3) X(coatRoughness, 1) X(coatAnisotropy, 1) X(coatRotation, 1) X(coatIor, 1)         \
    X(coatDarkening, 1) X(coatAffectColor, 1) X(coatAffectRoughness, 1)                                                \
    X(thinFilmWeight, 1) X(thinFilmThickness, 1) X(thinFilmIor, 1)                                                     \
    X(emissionWeight, 1) X(emissionColor, 3) X(opacity, 3) X(opacityThreshold, 1)                                      \
    X(normal, 3) X(coatNormal, 3) X(tangent, 3) X(coatTangent, 3)                                                      \
    X(thinWalled, 1) X(useSpecularWorkflow, 1)                                                                         \
    X(houdiniPrincipled, 1) /* Standard Surface with the Principled Shader's sheen and metal (M5.1) */             \
    X(hairBase, 1) X(hairBaseColor, 3) X(hairMelanin, 1) X(hairMelaninRedness, 1) X(hairMelaninRandomize, 1)         \
    X(hairThicknessScale, 1) X(hairTint, 3) X(hairShift, 1) X(hairIor, 1) X(hairAbsorption, 3) X(hairRoughness, 1)    \
    X(hairRoughnessAzimuthal, 1) X(hairRoughnessAnisotropic, 1) X(hairRoughnessRandomize, 1)                          \
    X(hairRoughnessAdvanced, 1) X(hairTtScale, 1) X(hairTrtScale, 1) X(hairDiffuse, 1) X(hairDiffuseColor, 3)

namespace surface_detail {
enum FieldWidths : uint16_t {
#define NOVA_SURFACE_WIDTH(name, width) w_##name = width,
    NOVA_SURFACE_FIELDS(NOVA_SURFACE_WIDTH)
#undef NOVA_SURFACE_WIDTH
};
constexpr uint16_t kWidths[] = {
#define NOVA_SURFACE_WIDTH_LIST(name, width) width,
    NOVA_SURFACE_FIELDS(NOVA_SURFACE_WIDTH_LIST)
#undef NOVA_SURFACE_WIDTH_LIST
};
enum FieldIndex : uint16_t {
#define NOVA_SURFACE_INDEX(name, width) i_##name,
    NOVA_SURFACE_FIELDS(NOVA_SURFACE_INDEX)
#undef NOVA_SURFACE_INDEX
    kFieldCount
};
constexpr uint16_t offsetOf(uint16_t index) {
    uint16_t o = 0;
    for (uint16_t i = 0; i < index; ++i)
        o += kWidths[i];
    return o;
}
} // namespace surface_detail

// Float offsets into SurfaceInputs::v, e.g. kSurfaceBaseColor.
namespace surface_field {
#define NOVA_SURFACE_OFFSET(name, width)                                                                               \
    constexpr uint16_t name = surface_detail::offsetOf(surface_detail::i_##name);
NOVA_SURFACE_FIELDS(NOVA_SURFACE_OFFSET)
#undef NOVA_SURFACE_OFFSET
constexpr uint16_t kCount = surface_detail::offsetOf(surface_detail::kFieldCount);
// The whole block: the native layout first, then what generic graphs and
// further segments use (docs/design/M4.md 3.4).
constexpr uint16_t kBlockSize = 384;
// Not a block field: the output of a material's displacement, the
// object-space offset (3), compiled into Program::displacement (M4.6).
constexpr uint16_t kDisplacement = 0xFFF0;
} // namespace surface_field

struct SurfaceInputs {
    float v[surface_field::kBlockSize];
    SurfaceModel model;
    uint16_t size = surface_field::kCount;  // floats in use (copies take only these)
    const ClosureGraph *graph = nullptr;    // SurfaceModel::Graph: the structure (owned by the program)

    NOVA_INLINE float f(uint16_t field) const { return v[field]; }
    NOVA_INLINE Spectrum c(uint16_t field) const { return {v[field], v[field + 1], v[field + 2]}; }
    NOVA_INLINE void set(uint16_t field, float x) { v[field] = x; }
    NOVA_INLINE void set(uint16_t field, Spectrum x) {
        v[field] = x.x;
        v[field + 1] = x.y;
        v[field + 2] = x.z;
    }
};

// Copies the inputs in use (a graph material's block is larger than a native one's).
NOVA_INLINE void copyInputs(SurfaceInputs &dst, const SurfaceInputs &src) {
    dst.model = src.model;
    dst.size = src.size;
    dst.graph = src.graph;
    std::memcpy(dst.v, src.v, sizeof(float) * src.size);
}

// The node definitions' defaults (MaterialX 1.39.5 as shipped with Houdini
// 22): Standard Surface 1.0.1 (base 1, base_color 0.8) or 1.0.0 (0.8, 1),
// OpenPBR 1.1.1, surface_unlit, and Lambert (the default material:
// displayColor goes into baseColor).
SurfaceInputs standardSurfaceDefaults(bool version100 = false);
SurfaceInputs openPbrDefaults();
SurfaceInputs unlitDefaults();
SurfaceInputs usdPreviewDefaults();
SurfaceInputs lambertDefaults(Spectrum color = spectrum(0.18f));
SurfaceInputs plasticDefaults(); // as Karma renders geometry without a material
SurfaceInputs hairDefaults(); // Karma Hair's (kma_hair)

// The geometry a closure is built for (world space, unit vectors).
struct ShadingGeometry {
    Vec3f ng; // geometric normal in the authored orientation
    Vec3f n;  // interpolated shading normal (not face-forwarded)
    Vec3f t;  // tangent perpendicular to n (anisotropy); any if hasTangent is false
    bool hasTangent;
    float random = 0.5f; // per strand in [0, 1) (hair's randomize controls; curves: per curve)
};

// Context shared by all closures of a render.
struct ClosureContext {
    const BsdfTables *tables;
    const float *xyzToRgb; // thin film: XYZ to the rendering space, rows normalized (kXyzToRec709Grey)
};

} // namespace nova

// buildClosure(): header-only, so the CUDA backend compiles the same code.
#include "core/bsdf/surface_models.h"
