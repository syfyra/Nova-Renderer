#pragma once

// UsdGeomPoints as Nova point meshes (docs/design/M9.md D1): the points per
// motion step, widths as radii (times Karma's width scale), Karma's point
// style (spheres by default, discs facing the ray, discs facing the normals)
// and the primvars per point. Instances, motion, materials and the
// per-object settings as for meshes.

#include "karmaSettings.h"
#include "motion.h"

#include "core/scene/mesh.h"

#include <pxr/imaging/hd/points.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaPoints final : public HdPoints {
public:
    explicit HdNovaPoints(SdfPath const &id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits,
              TfToken const &reprToken) override;
    void Finalize(HdRenderParam *renderParam) override;

protected:
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
    void _InitRepr(TfToken const &reprToken, HdDirtyBits *dirtyBits) override;

private:
    // The point mesh at the shutter's steps; false if there is nothing to render.
    // `defaults`: the render settings' Karma style and width scale.
    bool _Build(HdSceneDelegate *sceneDelegate, HdNovaShutter const &shutter, unsigned maxSteps,
                HdNovaObjectDefaults const &defaults, nova::MeshDesc &desc) const;

    nova::MeshId _meshId = nova::kInvalidId;
    HdNovaShutter _sampledShutter;
    unsigned _sampledMaxSteps = 0;
};

PXR_NAMESPACE_CLOSE_SCOPE
