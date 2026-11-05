#pragma once

// UsdGeomBasisCurves as Nova curve meshes (docs/design/M9.md D1-D3): the
// topology (type, basis, wrap; periodic and pinned curves expanded to open
// ones), the control points per motion step, widths as radii (times Karma's
// width scale), Karma's curve style (round tubes by default; normals only for
// oriented ribbons) and the primvars at their curve rates. Instances,
// motion, materials and the per-object settings as for meshes.

#include "motion.h"

#include "core/scene/mesh.h"

#include <pxr/imaging/hd/basisCurves.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaBasisCurves final : public HdBasisCurves {
public:
    explicit HdNovaBasisCurves(SdfPath const &id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
              TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    // The curve mesh at the shutter's steps; false if there is nothing to render.
    bool _Build(HdSceneDelegate *sceneDelegate, HdNovaShutter const &shutter, unsigned maxSteps,
                nova::MeshDesc &desc) const;

    nova::MeshId _meshId = nova::kInvalidId;
    HdNovaShutter _sampledShutter;
    unsigned _sampledMaxSteps = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
