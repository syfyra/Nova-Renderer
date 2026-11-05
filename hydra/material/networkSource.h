#pragma once

// Where a material's network comes from (docs/design/M3.md K11): the
// material schema of the terminal scene index, one render context at a time
// (Nova tries "nova", "kma", "mtlx", "vex", then the universal one). Reading the
// schema keeps terminal output names, color spaces and type names as
// separate fields and the nodes' type info; GetMaterialResource (the
// fallback without a scene index) loses all of them.

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <map>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

struct HdNovaParamMeta {
    TfToken colorSpace, typeName;
    bool operator==(HdNovaParamMeta const &o) const { return colorSpace == o.colorSpace && typeName == o.typeName; }
};

struct HdNovaNetworkSource {
    TfToken context; // the render context read ("" = universal)
    HdMaterialNetwork2 net; // node paths relative to the material
    std::map<SdfPath, std::map<TfToken, HdNovaParamMeta>> meta;
    std::map<SdfPath, VtDictionary> nodeTypeInfo;
    // The uv set tangent frames derive from (empty: the primary one, "st");
    // rewritten networks set it (the Principled Shader: "uv|st").
    std::string tangentUv;

    bool operator==(HdNovaNetworkSource const &o) const {
        return context == o.context && net == o.net && meta == o.meta && nodeTypeInfo == o.nodeTypeInfo &&
               tangentUv == o.tangentUv;
    }
};

// Nova's render contexts in priority order (Karma: kma > mtlx > vex > universal;
// "nova" first). The universal context ("") is the last fallback.
TfTokenVector const &HdNovaMaterialRenderContexts();

// The network of `context` from the scene index; false if there is none.
bool HdNovaReadNetwork(HdSceneIndexBaseRefPtr const &sceneIndex, SdfPath const &id, TfToken const &context,
                       HdNovaNetworkSource *out);

// Fallback without a terminal scene index: the delegate's network.
bool HdNovaReadNetworkFromDelegate(HdSceneDelegate *sd, SdfPath const &id, HdNovaNetworkSource *out);

// A readable dump for TF_DEBUG HDNOVA_MATERIAL.
std::string HdNovaDescribeNetwork(HdNovaNetworkSource const &src);

PXR_NAMESPACE_CLOSE_SCOPE
