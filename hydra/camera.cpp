#include "camera.h"

#include "renderParam.h"

PXR_NAMESPACE_OPEN_SCOPE

void HdNovaCamera::Sync(HdSceneDelegate *sd, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) {
    const HdDirtyBits bits = *dirtyBits;
    HdCamera::Sync(sd, renderParam, dirtyBits); // projection, shutter, lens; clears the bits
    if (GetId().IsEmpty() || !renderParam || !(bits & (DirtyTransform | DirtyParams)))
        return;
    auto *param = static_cast<HdNovaRenderParam *>(renderParam);
    _sampledShutter = param->GetShutter();
    HdNovaSampleTransform(sd, GetId(), _sampledShutter, param->GetMaxMotionSteps(), &_motion);
}

PXR_NAMESPACE_CLOSE_SCOPE
