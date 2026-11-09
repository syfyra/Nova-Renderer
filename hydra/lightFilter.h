#pragma once

// Light filters (docs/design/M11-P4.md): Hydra's `lightFilter` sprim. Karma's
// blocker (shader id kma_lfilter_blocker) becomes a nova::LightFilterDesc
// under the prim's path; lights list the paths of their filters
// (`light:filters`), which the scene resolves at commit. Other filter kinds
// warn once and are left out.

#include "core/scene/light.h"

#include <pxr/imaging/hd/sprim.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRenderParam;

class HdNovaLightFilter final : public HdSprim {
public:
    explicit HdNovaLightFilter(SdfPath const &id);
    ~HdNovaLightFilter() override = default;

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam, HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Finalize(HdRenderParam *renderParam) override;

    // A blocker whose space is another prim (a light, a camera, another
    // filter) follows that prim, which moves without this filter being
    // synced: the render pass calls this before every render (after the
    // sync). Updates the scene's filter if the space moved; true if it did.
    bool RefreshSpace(HdNovaRenderParam &param);

private:
    bool _inScene = false;
    // What the last sync built, for RefreshSpace().
    HdSceneDelegate *_sceneDelegate = nullptr;
    nova::LightFilterDesc _desc;
    std::string _space;
};

PXR_NAMESPACE_CLOSE_SCOPE
