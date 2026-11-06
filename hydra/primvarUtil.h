#pragma once

// Numeric primvars as flat float arrays: the shading attributes of meshes
// and instancers (docs/design/M3.md K12).

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// A scalar or array value of a numeric type (bool, int, float, double, half
// and their 2-4 component vectors) as `width` floats per element. Returns
// false for anything else (strings, tokens, matrices, quaternions, ...).
bool HdNovaPrimvarToFloats(VtValue const &value, int *width, std::vector<float> *out);

// Primvars that are geometry, not shading data.
bool HdNovaIsGeometryPrimvar(TfToken const &name);

class HdSceneDelegate;

// The object's tag in light path expressions: Karma's constant string
// primvar karma:object:lpetag (empty if not authored).
std::string HdNovaObjectLpeTag(HdSceneDelegate *sd, SdfPath const &id);

PXR_NAMESPACE_CLOSE_SCOPE

#include "karmaSettings.h"

#include "core/scene/mesh.h"
#include "core/scene/ray_limits.h"

PXR_NAMESPACE_OPEN_SCOPE

// Emissive meshes as lights (docs/design/M9.md D14): the constant primvars
// nova:object:lightSource (else karma:object:treat_as_lightsource; 0 no, 1
// yes, 2 auto) and karma:object:lightsource_diffusescale, _specularscale and
// _samplingquality, else the render settings' Karma values (`defaults`,
// docs/design/M11-P1.md).
nova::MeshLightSource HdNovaMeshLightSource(HdSceneDelegate *sd, SdfPath const &id,
                                            HdNovaObjectDefaults const &defaults = HdNovaObjectDefaults{});

// A prim's per-object ray limits and shading properties (constant
// primvars): Nova's names first (nova:object:*Limit, thinWallAtRefractLimit,
// fakeCaustics, internalReflection, causticsRoughnessClamp), then Karma's
// (karma:object:*limit, thinwallatrefractlimit, causticsenable inverted with
// Karma's fake-caustic factor, the fakecaustics* controls,
// internalreflectenable, causticsroughnessclamp); unset fields fall back to
// the render settings (docs/design/M11-P1.md).
nova::RayLimitOverrides HdNovaObjectOverrides(HdSceneDelegate *sd, SdfPath const &id);

// A constant primvar's value (empty if the prim has none of that name).
VtValue HdNovaConstantPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name);

PXR_NAMESPACE_CLOSE_SCOPE
