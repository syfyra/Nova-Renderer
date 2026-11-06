#pragma once

#include "core/lights/sky.h"
#include "core/scene/light.h"

#include <pxr/imaging/hd/light.h>
#include <pxr/pxr.h>

#include <memory>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// UsdLux lights and, as Hydra's generic "light" sprim, Karma's sky dome
// (KarmaSkyDomeLight, docs/design/M11-P3.md), which becomes a dome light
// with a baked sky texture. Other generic lights are ignored with a warning.
class HdNovaLight final : public HdLight {
public:
    HdNovaLight(SdfPath const &id, TfToken const &lightType);
    ~HdNovaLight() override = default;

    void Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *renderParam,
              HdDirtyBits *dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Finalize(HdRenderParam *renderParam) override;

private:
    // Turns the description of a generic light into Karma's sky: a dome with
    // the baked sky as its texture.
    void _SyncKarmaSky(HdSceneDelegate *sd, nova::LightDesc &desc);
    void _RemoveLights(HdRenderParam *renderParam);

    TfToken _lightType;
    nova::LightId _lightId = nova::kInvalidId;
    std::vector<nova::LightId> _instanceLightIds; // instances after the first (M9 D16)
    // Karma sky: the baked texture and what it was baked from, so edits of
    // other inputs (transform, visibility, linking) keep it.
    nova::KarmaSkyDesc _sky;
    int _skyWidth = 0;
    std::shared_ptr<const nova::Image> _skyImage;
    bool _warnedUnsupported = false;
};

PXR_NAMESPACE_CLOSE_SCOPE
