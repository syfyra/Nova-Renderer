#pragma once

// A prim's path without what USD names anew at every load
// (docs/bug-audit-2026-11.md, section 23).
//
// The prims of native instancing reach Hydra below paths that hold USD's
// prototype names:
//   /UsdNiPropagatedPrototypes/<bindings>/__Prototype_7/UsdNiInstancer/UsdNiPrototype/geo
// and USD numbers its prototypes in the order it happens to find them: the
// same stage has another __Prototype_N for the same geometry in every
// process (measured: 30 loads, 30 assignments). An object key made of such a
// path (core/scene/object_key.h) changed from run to run, and with it
// everything keyed: the stochastic opacity of instanced geometry, the order
// of instanced lights and emissive meshes, the random numbers of strands.
//
// HdNovaStablePath() names such a prim by its first instance instead: the
// instancer's least instance location in place of everything up to
// UsdNiPrototype, so the mesh above is /World/tree_001/geo, the path of the
// first instance's proxy in the stage. Nested instances resolve level by
// level; a point instancer's prototype below such a path
// (.../ForInstancer<hash of the instancer's path>) takes the stable path of
// its instancer in place of the hash. A path without a prototype name is
// returned as it is.

#include "core/scene/object_key.h"

#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/usd/sdf/path.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

std::string HdNovaStablePath(HdSceneIndexBaseRefPtr const &sceneIndex, SdfPath const &id);
std::string HdNovaStablePath(HdSceneDelegate *sd, SdfPath const &id);

// The key of the object at `id`, and of its instance `index` (object_key.h).
inline uint64_t HdNovaObjectKey(HdSceneDelegate *sd, SdfPath const &id, uint64_t index = 0) {
    return nova::objectKey(HdNovaStablePath(sd, id), index);
}

PXR_NAMESPACE_CLOSE_SCOPE
