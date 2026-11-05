#pragma once

// VEX shaders in a material's "vex" network (docs/design/M5.md D1-D4).
// Hydra leaves their node type id empty: the identity is the shader's
// sourceAsset or sourceCode, which the node type info keeps under the
// authored name ("sourceAsset", or "VEX:sourceAsset" with a source type).
// Houdini writes the Principled Shader as an opdef asset and a VEX Material
// Builder as source code (docs/research/m5/vex-materials.md 0.1).

#include <pxr/base/vt/dictionary.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

enum class HdNovaVexKind {
    NotVex,                 // an info:id shader (or nothing usable)
    PrincipledSurface,      // opdef:/Vop/principledshader::2.0?SurfaceVexCode
    PrincipledDisplacement, // opdef:/Vop/principledshader::2.0?DisplacementVexCode
    Code,                   // sourceCode (a VEX Material Builder)
    Asset,                  // any other sourceAsset
};

struct HdNovaVexShader {
    HdNovaVexKind kind = HdNovaVexKind::NotVex;
    std::string asset; // the authored sourceAsset path
    std::string code;  // the sourceCode
};

// The VEX shader a node's type info describes.
HdNovaVexShader HdNovaVexShaderOf(VtDictionary const &typeInfo);

// A short name for messages: the asset path, or "VEX source code".
std::string HdNovaDescribeVexShader(HdNovaVexShader const &shader);

PXR_NAMESPACE_CLOSE_SCOPE
