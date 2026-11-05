#pragma once

// Hydra -> core mapping of subdivision surfaces.

#include "core/geometry/subdiv.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// Maps the subdivision scheme token; false for "none" (and unknown) schemes.
bool HdNovaGetSubdivScheme(TfToken const &scheme, nova::SubdivScheme *out);

// HdMeshTopology + PxOsdSubdivTags -> nova::SubdivMeshDesc with exactly the
// conventions of PxOsdRefinerFactory (pxOsd/refinerFactory.cpp, OpenUSD
// 26.05). `pointSteps` holds the control points of every deformation step.
// displayColor is filled by the caller.
bool HdNovaToSubdivDesc(HdMeshTopology const &topology, PxOsdSubdivTags const &tags,
                        std::vector<VtVec3fArray> const &pointSteps, SdfPath const &id, nova::SubdivMeshDesc *out);

PXR_NAMESPACE_CLOSE_SCOPE
