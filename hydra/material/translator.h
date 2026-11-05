#pragma once

// MaterialX network -> shading IR (docs/design/M3.md K1, K4, K6). A pure
// function of the network and the MaterialX libraries:
//   - node ids resolve to node definitions (USD aliases, legacy renames);
//   - a native surface model at the surface terminal (Standard Surface,
//     OpenPBR, surface_unlit, UsdPreviewSurface) binds its inputs to
//     SurfaceInputs fields; any other surface (pbrlib BSDF graphs, surface
//     mixes, glTF / Disney / Lama graphs) becomes a generic closure
//     structure (M4.5, core/bsdf/closure_graph.h) over a larger input block;
//   - other nodes lower natively, or expand through their nodegraph
//     implementation (recursively), or are unknown (warning, default
//     outputs);
//   - every numeric input of the user's nodes (authored or nodedef default)
//     becomes a parameter, so value edits never change the program's
//     structure; values inside library nodegraphs are constants;
//   - unconnected inputs with a defaultgeomprop read the geometry (UV0 is the
//     "st" primvar, as in Karma).

#include "networkSource.h"
#include "../textureConvert.h"

#include "core/scene/material.h"
#include "core/shading/ir.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaTranslatedMaterial {
    bool ok = false;                  // a supported surface terminal was found and lowered
    nova::shading::IrGraph graph;     // outputs bind SurfaceInputs fields
    std::vector<float> params;        // parameter values, in the graph's layout
    nova::SurfaceInputs defaults;     // the surface model's default inputs
    std::vector<HdNovaTextureRequest> textures; // per texture slot of the graph (graph.textures)
    std::vector<std::string> warnings; // unsupported nodes or inputs (one line each)
    std::string error;                // why !ok
};

HdNovaTranslatedMaterial HdNovaTranslateMaterial(HdNovaNetworkSource const &src);

PXR_NAMESPACE_CLOSE_SCOPE
