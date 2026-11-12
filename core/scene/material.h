#pragma once

// Materials of the scene (docs/design/M3.md 3.3). A material is a surface
// model with default input values, plus optionally a compiled shading
// program that sets inputs per hit, and the program's parameter values.
// Programs are shared between materials of the same structure; editing a
// parameter value only changes the material's uniform block.

#include "core/bsdf/surface.h"
#include "core/shading/external.h"
#include "core/shading/program.h"
#include "core/volume/shader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova {

using MaterialId = uint32_t;
constexpr MaterialId kDefaultMaterialId = 0;

// An external (VEX) material's shader (docs/design/M5.md D4, D7): per shader
// parameter, where its per-hit value sits in the program's input block
// (kNoExternalSlot: the uniform value below), its uniform value, and the
// string value of string parameters.
struct ExternalBinding {
    std::shared_ptr<const ExternalShader> shader;
    std::vector<uint16_t> slots;
    std::vector<std::vector<float>> values;
    std::vector<std::string> strings;
    ExternalBounces bounces;
};
constexpr uint16_t kNoExternalSlot = 0xFFFF;

struct MaterialDesc {
    SurfaceInputs inputs = lambertDefaults(); // model and the values of inputs the program does not set
    shading::ProgramPtr program;              // optional
    std::vector<float> params;                // the program's parameter values
    std::vector<TextureId> textures;          // per texture slot of the program (kNoTexture: fallback)
    std::shared_ptr<const ExternalBinding> external; // SurfaceModel::External
    std::string lpeTag; // karma:lpetag: the tag of objects without their own in light path expressions
    uint32_t cryptoId = 0; // the cryptomatte id of the material's name (core/film/crypto.h)
};

// What rendering reads per material (built by Scene::updateMaterial).
struct MaterialRecord {
    MaterialDesc desc;
    std::vector<float> uniforms; // the program's uniform block
    SurfaceInputs resolved;      // inputs with the program's uniform outputs applied
    bool varying = false;        // the program has per-hit work (rows, code or primvars)
    std::vector<TexSampler> samplers; // the program's texture slots with the material's textures
    // Opacity (K10): the uniform value (mean of the opacity color, after the
    // threshold), or a per-hit program (Program::opacity) with its uniforms.
    bool hasOpacity = false;     // some hit may be (partly) transparent
    float uniformOpacity = 1.0f;
    std::vector<float> opacityUniforms; // non-empty: the opacity varies per hit
    bool externalOpacity = false;       // the external shader's opacity varies per hit (D9)
    // The volume shader (docs/design/M8.md D4; Program::volume): its uniform
    // block and the inputs with its uniform outputs applied.
    std::vector<float> volumeUniforms;
    VolumeInputs volume = volumeDefaults();
    bool hasVolume() const { return desc.program && desc.program->volume; }
    // Emission (docs/design/M9.md D14, emissive meshes as lights): Uniform,
    // the uniform inputs emit (uniformEmission, seen along the normal), or
    // Varying, an input the emission depends on varies per hit. External
    // (VEX) materials count as None: their emission stays on BSDF paths.
    enum class Emission : uint8_t { None, Uniform, Varying };
    Emission emission = Emission::None;
    Spectrum uniformEmission{0.0f, 0.0f, 0.0f};
};

// Opacity from the opacity color and opacityThreshold (UsdPreviewSurface: a
// threshold > 0 cuts out below it and is opaque above it).
NOVA_INLINE float surfaceOpacity(Spectrum opacity, float threshold) {
    const float o = (opacity.x + opacity.y + opacity.z) * (1.0f / 3.0f);
    if (threshold > 0.0f)
        return o >= threshold ? 1.0f : 0.0f;
    return o < 0.0f ? 0.0f : (o > 1.0f ? 1.0f : o);
}

MaterialRecord makeMaterialRecord(const MaterialDesc &desc);

// The default material (id 0, always present): Lambert with the
// displayColor attribute as base color, 0.18 where there is none. Unbound
// geometry renders with it, as in M2.
MaterialDesc defaultMaterial();

// Karma's material for geometry without one: Houdini's plastic with the
// displayColor attribute as its colour, white where there is none
// (plasticDefaults()). The Hydra delegate renders unbound geometry with it
// unless nova:defaultMaterial says "lambert".
MaterialDesc plasticDefaultMaterial();

} // namespace nova
