#pragma once

// IR -> Program (docs/design/M3.md 2.1): reachability (dead code), constant
// folding by running the operation itself, exact identities only (x * 1,
// x / 1, identity swizzles and copies), common subexpressions, hoisting of
// uniform subgraphs into the prologue, Sethi-Ullman ordering of the varying
// code and first-fit row allocation. Parameter values never fold.
//
// Texture footprints (K7): the varying part of every Tex2D's coordinates is
// cloned twice with P, Pobject and the primvars moved by the hit's footprint
// vectors; the differences are the lookup's derivatives (any operation
// differentiates, the clones share all uniform work).

#include "core/shading/program.h"

#include <string>

namespace nova::shading {

struct CompileOptions {
    bool optimize = true;   // folding, identities and CSE (off: differential tests)
    bool derivatives = true; // Tex2D footprint derivatives
    bool opacityProgram = true; // also build Program::opacity
    bool displacementProgram = true; // build Program::displacement from the kDisplacement output
    bool volumeProgram = true; // build Program::volume from the volume outputs (core/volume/shader.h)
    // The material's parameter values: the volume program folds them as
    // constants (it runs per tentative collision, and its majorants need the
    // values anyway, so a value edit recompiles it). Null: they stay parameters.
    const std::vector<float> *volumeParams = nullptr;
    uint32_t maxRows = 1024;
    bool gpuRowWarning = true; // warn above kGpuRowLimit rows (off: host-only programs such as displacement)
};

struct CompileResult {
    ProgramPtr program;     // null on failure
    std::string error;
    std::string warning;    // e.g. more rows than the GPU limit (kGpuRowLimit)
};

CompileResult compile(const IrGraph &graph, const CompileOptions &options = {});

} // namespace nova::shading
