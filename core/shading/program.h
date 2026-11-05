#pragma once

// Compiled shading program (docs/design/M3.md 3.3): flat bytecode over a
// register file of rows. A row holds one float per lane; a value of width w
// occupies w consecutive rows. Uniform operands index the material's uniform
// block U instead: [constants | parameters | hoisted values].
//
// Programs are immutable and shared between materials with the same
// structure (structureHash); parameter values live in the material.

#include "core/shading/ir.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nova::shading {

// The rows a program may use on the GPU (the device VM keeps one lane's rows
// in local memory, docs/design/M10.md); larger programs render there with
// their uniform inputs. CUDA-free so the compiler's warning shares the value.
constexpr uint32_t kGpuRowLimit = 512;

// Operand flags (Instr::flags): uniform operand (index into U), splat
// operand (width 1 in a wider per-component operation).
enum InstrFlags : uint8_t {
    kUniformA = 1u << 0,
    kUniformB = 1u << 1,
    kUniformC = 1u << 2,
    kSplatA = 1u << 3,
    kSplatB = 1u << 4,
    kSplatC = 1u << 5,
    kHasB = 1u << 6, // Tex2D: footprint derivatives present
};

struct Instr { // 16 bytes
    Op op;
    uint8_t width; // result width
    uint8_t flags;
    uint16_t dst, a, b, c;
    uint32_t imm;
};
static_assert(sizeof(Instr) == 16, "Instr must stay 16 bytes");

// A global or primvar the shade stage writes into rows before the code runs.
struct GlobalBinding {
    Global global;
    uint16_t row;
};
struct PrimvarBinding {
    uint32_t name;     // index into Program::primvars
    uint8_t width;
    uint8_t shift;     // 0: at the hit; 1, 2: moved by the first, second footprint vector
    uint16_t row;
    uint16_t fallback; // uniform slot of the fallback value (width floats)
};
// A surface input the program sets: from rows (varying) or from U.
struct OutputBinding {
    uint16_t field;
    uint8_t width;
    bool uniform;
    uint16_t index;
};

struct Program {
    uint64_t structureHash = 0;
    SurfaceModel model = SurfaceModel::Lambert;
    std::vector<Instr> code;     // varying, per hit
    std::vector<Instr> prologue; // uniform, once per material (rows are U slots)
    std::vector<float> constants; // U[0, constants.size())
    uint32_t paramOffset = 0;     // U[paramOffset, paramOffset + paramCount)
    uint32_t paramCount = 0;
    uint32_t uniformCount = 0;    // constants + params + hoisted
    uint32_t rowCount = 0;
    std::vector<GlobalBinding> globals;
    std::vector<PrimvarBinding> primvarBindings;
    std::vector<std::string> primvars;
    std::vector<OutputBinding> outputs;
    std::vector<TexSlot> textures; // texture slots (Tex2D imm)
    int32_t tangentUv = -1;        // primvars index DPdu/DPdv derive from (-1: none)
    std::vector<std::pair<std::string, uint16_t>> aovs; // shader AOVs: name -> input block field (3 floats)
    // The graph reduced to its opacity outputs (opacity, opacityThreshold),
    // if they vary per hit: the accelerator's hit filter runs it (K10). Its
    // parameters and texture slots are the program's.
    std::shared_ptr<const Program> opacity;
    // The displacement (M4.6): the graph reduced to the object-space offset
    // (surface_field::kDisplacement, 3 floats), run per tessellated vertex
    // when meshes are built. Its parameters and texture slots are the
    // program's. Null: the material does not displace.
    std::shared_ptr<const Program> displacement;
    // The volume shader (docs/design/M8.md D4): the graph reduced to the
    // volume outputs (core/volume/shader.h), which it writes at the volume
    // block's offsets; volume prims bound to the material run it per point.
    // Its parameters and texture slots are the program's. Null: none.
    std::shared_ptr<const Program> volume;
    // The parameter floats the program reads (offset, width), from the
    // graph's reachable Param nodes: what a value edit must change to matter.
    std::vector<std::pair<uint32_t, uint8_t>> paramReads;
    bool needsFootprints = false;  // reads globals or primvars moved by the footprint vectors
    bool needsQueries = false;     // traces ray queries (core/shading/query.h)
    std::shared_ptr<const ClosureGraph> closure; // SurfaceModel::Graph: the closure structure

    bool varyingFree() const { return code.empty() && globals.empty() && primvarBindings.empty(); }
    // The uniform block of a material with these parameter values.
    void initUniforms(const float *params, std::vector<float> &U) const;
};

using ProgramPtr = std::shared_ptr<const Program>;

} // namespace nova::shading
