#pragma once

// Light filters (docs/design/M11-P4.md): Hydra's `lightFilter` sprim. Karma's
// blocker (shader id kma_lfilter_blocker) becomes a nova::LightFilterDesc
// under the prim's path; lights list the paths of their filters
// (`light:filters`), which the scene resolves at commit. Other filter kinds
// warn once and are left out.

#include <pxr/imaging/hd/sprim.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaLightFilter final : public HdSprim {
public:
    explicit HdNovaLightFilter(SdfPath const &id);
    ~HdNovaLightFilter() override = default;

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Finalize(HdRenderParam *renderParam) override;

private:
    bool _inScene = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
