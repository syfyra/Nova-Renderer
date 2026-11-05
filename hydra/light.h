#pragma once

#include "core/scene/light.h"

#include <pxr/imaging/hd/light.h>
#include <pxr/pxr.h>

#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaLight final : public HdLight {
public:
    HdNovaLight(SdfPath const &id, TfToken const &lightType);
    ~HdNovaLight() override = default;

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Finalize(HdRenderParam *renderParam) override;

private:
    TfToken _lightType;
    nova::LightId _lightId = nova::kInvalidId;
    std::vector<nova::LightId> _instanceLightIds; // instances after the first (M9 D16)
};

PXR_NAMESPACE_CLOSE_SCOPE
