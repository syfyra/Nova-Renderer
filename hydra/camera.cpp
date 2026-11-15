#include "camera.h"

#include "renderParam.h"

#include <pxr/base/gf/vec3d.h>

PXR_NAMESPACE_OPEN_SCOPE

void HdNovaCamera::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    const HdDirtyBits bits = *dirtyBits;
    HdCamera::Sync(sd, renderParam, dirtyBits); // projection, shutter, lens; clears the bits
    if (GetId().IsEmpty() || !renderParam || !(bits & (DirtyTransform | DirtyParams)))
        return;
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    _sampledShutter = param->GetShutter();
    static const TfToken tint("karma:camera:tint");
    const VtValue value = sd->GetCameraParamValue(GetId(), tint);
    _tint = GfVec3f(1.0f);
    if (value.IsHolding<GfVec3f>())
        _tint = value.UncheckedGet<GfVec3f>();
    else if (value.IsHolding<GfVec3d>())
        _tint = GfVec3f(value.UncheckedGet<GfVec3d>());
    HdNovaSampleTransform(sd, GetId(), _sampledShutter, param->GetMaxMotionSteps(), &_motion);
}

PXR_NAMESPACE_CLOSE_SCOPE
