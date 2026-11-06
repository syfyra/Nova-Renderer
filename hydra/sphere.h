#pragma once

// Analytic spheres (docs/design/M3.md M3.6b): UsdGeomSphere prims (also what
// the Sphere LOP makes) stay spheres instead of UsdImaging's coarse
// polygonal cage. The radius comes from the sphere schema (animated radii
// are sampled over the shutter); transforms, instancing and motion blur work
// as for meshes, so a non-uniform scale gives an exact ellipsoid. Constant
// and uniform primvars become attributes; st is the sphere's own
// parameterization unless authored. With a displacing material (M4.6) the
// sphere becomes a diced mesh: a cube cage projected onto the sphere, with
// the same st, displaced like any mesh.

#include "medium.h"
#include "motion.h"

#include "core/scene/mesh.h"

#include <pxr/imaging/hd/rprim.h>
#include <pxr/pxr.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaSphere final : public HdRprim {
public:
    explicit HdNovaSphere(SdfPath const &id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
              TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;
    TfTokenVector const &GetBuiltinPrimvarNames() const override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    nova::MeshId _meshId = nova::kInvalidId;
    HdNovaShutter _sampledShutter;
    unsigned _sampledMaxSteps = 0;
    uint64_t _builtDisplacement = 0; // signature of the displaced build (0: analytic)
    HdNovaMedium _medium;            // a medium bounded by the sphere (docs/design/M11-P5.md)
    bool _warnedMediumInstances = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
