#pragma once

#include "motion.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/pxr.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// HdCamera that also samples its transform over the shutter (camera motion
// blur). The render pass decides the effective shutter and dirties the camera
// when it changes.
class HdNovaCamera final : public HdCamera {
public:
    explicit HdNovaCamera(SdfPath const &id) : HdCamera(id) {}

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;

    // Camera-to-world transforms at uniform steps over the shutter of the
    // last sync; one entry if the camera does not move.
    std::vector<GfMatrix4d> const &GetMotionSteps() const { return _motion; }
    HdNovaShutter GetSampledShutter() const { return _sampledShutter; }

private:
    std::vector<GfMatrix4d> _motion;
    HdNovaShutter _sampledShutter;
};

PXR_NAMESPACE_CLOSE_SCOPE
