#pragma once

// VEX source code through the CVEX bridge (docs/design/M5.md D4, D10):
// nova_cvex.dll, loaded on first use from the directory of the module that
// holds this code (hdNova.dll, nova_cli.exe; NOVA_CVEX overrides the path),
// and the binding of a material's VEX shader: a small program writes every
// float parameter per hit into the input block, as the primvar of the same
// name where the mesh has one (Karma/Mantra's convention), else the
// material's value, else the shader's default.

#include "networkSource.h"
#include "registry.h"
#include "vexShaders.h"

#include "core/scene/material.h"
#include "core/shading/ir.h"

#include <memory>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// The bridge, or null with `error` (loaded once; a failure is final).
nova::ExternalBridge *HdNovaCvexBridge(std::string *error);

struct HdNovaExternalMaterial {
    nova::shading::IrGraph graph;                  // writes the per-hit parameters
    std::vector<float> params;                     // the graph's parameter values
    std::shared_ptr<nova::ExternalBinding> binding;
    uint16_t blockSize = 0;                        // floats of the input block in use
};

// Binds the VEX source-code surface `vex` of node `shader` in `src`. False
// with `error` when the bridge or the shader cannot run.
bool HdNovaBuildExternal(HdNovaNetworkSource const &src, SdfPath const &shader, HdNovaVexShader const &vex,
                         HdNovaExternalMaterial *out, std::string *error);

// The VEX source-code displacement on `src`'s displacement terminal through
// the bridge (docs/design/M5.md D8): null when the terminal holds none (the
// Principled Shader's displacement is native), null with `error` when it
// cannot run.
HdNovaDisplacementPtr HdNovaBuildExternalDisplacement(HdNovaNetworkSource const &src, std::string *error);

PXR_NAMESPACE_CLOSE_SCOPE
