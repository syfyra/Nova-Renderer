#pragma once

// Displacement of tessellated meshes (docs/design/M4.md 3.6,
// docs/research/m4/displacement.md 5.5-5.6): every point runs its material's
// displacement program once, in the shading VM with one lane per point. The
// point's owner triangle (the first that references it) decides the
// material, the barycentrics of its primvars and the texture footprints (half
// the triangle's edges). The object-space offset of the first motion step is
// added to every step (Karma's and Cycles' rule), and the normals are
// recomputed per step from the displaced triangles (area-weighted).

#include "core/math/xform.h"
#include "core/scene/mesh.h"
#include "core/shading/external.h"
#include "core/shading/program.h"
#include "core/texture/texture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nova {

// A material's displacement as the mesh builder runs it.
struct DisplacementShader {
    shading::ProgramPtr program;      // Program::displacement
    std::vector<float> uniforms;      // initUniforms() with the material's parameter values
    std::vector<TexSampler> samplers; // the program's texture slots with the material's textures
    // VEX displacement through the CVEX bridge (docs/design/M5.md D8), run
    // instead of `program` with world-space P and N: the parameters' values
    // and strings, and the primvars they read where the mesh has them (D10;
    // empty: the parameters' names), in the order of external->params().
    std::shared_ptr<const ExternalDisplacer> external;
    std::vector<std::vector<float>> externalValues;
    std::vector<std::string> externalStrings;
    std::vector<std::string> externalPrimvars;
};

struct DisplacementContext {
    const ITextureSampler *textures = nullptr;
    Mat4 objectToWorld = Mat4::identity(); // at the first motion step
    float frame = 1.0f, fps = 24.0f;       // the Time and Frame globals
};

struct DisplacementInput {
    // The shader of every triangle (index into `shaders`; -1: none).
    std::vector<int32_t> triangleShader;
    std::vector<const DisplacementShader *> shaders;
    // The object-space direction of every point (step 0; normalized here).
    // Empty: the mesh's normals of step 0.
    std::vector<Vec3f> directions;
    std::vector<int32_t> triangleFaces;             // authored face per triangle (PrimId)
    std::vector<std::pair<int32_t, int32_t>> copies; // (copy, source): copies take their source's offset
};

// Displaces `mesh` in place. Returns the number of points displaced.
size_t displaceMesh(MeshDesc &mesh, const DisplacementInput &input, const DisplacementContext &ctx);

// True if the shader's offset is the uniform zero vector for these
// parameters (no tessellation needed; Karma skips such materials too).
bool displacementIsZero(const DisplacementShader &shader);

} // namespace nova
