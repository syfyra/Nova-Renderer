#pragma once

// The material sprim (docs/design/M3.md section 6). Every edit arrives as
// AllDirty and bound meshes are never re-synced, so the sprim diffs the
// network itself: an unchanged network is a no-op, a value edit only
// changes the parameters (the program, shared by structure, stays), and the
// meshes keep their MaterialId whatever happens to the material.

#include "networkSource.h"

#include "core/scene/material.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaMaterial final : public HdMaterial {
public:
    explicit HdNovaMaterial(SdfPath const &id);

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override { return HdMaterial::AllDirty; }
    void Finalize(HdRenderParam *renderParam) override;

private:
    HdNovaNetworkSource _source; // the network last translated
    bool _haveSource = false;
    nova::MaterialDesc _applied; // what the scene has
    bool _hasApplied = false;
    uint64_t _appliedVexDisplacement = 0; // hash of the applied VEX displacement (0: none)
};

PXR_NAMESPACE_CLOSE_SCOPE
