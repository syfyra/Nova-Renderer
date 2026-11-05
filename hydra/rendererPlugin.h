#pragma once

#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdNovaRendererPlugin final : public HdRendererPlugin {
public:
    HdNovaRendererPlugin() = default;
    ~HdNovaRendererPlugin() override = default;

    HdRenderDelegate *CreateRenderDelegate() override;
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &settingsMap) override;
    void DeleteRenderDelegate(HdRenderDelegate *renderDelegate) override;

    using HdRendererPlugin::IsSupported; // keep the other overloads visible
    bool IsSupported(HdRendererCreateArgs const &rendererCreateArgs,
                     std::string *reasonWhyNot = nullptr) const override;
};

// "Nova XPU" (docs/design/M10.md D1): the same delegate rendering on the GPU
// (the CPU takes over without a usable device), as Karma XPU next to Karma.
class HdNovaXpuRendererPlugin final : public HdRendererPlugin {
public:
    HdNovaXpuRendererPlugin() = default;
    ~HdNovaXpuRendererPlugin() override = default;

    HdRenderDelegate *CreateRenderDelegate() override;
    HdRenderDelegate *CreateRenderDelegate(HdRenderSettingsMap const &settingsMap) override;
    void DeleteRenderDelegate(HdRenderDelegate *renderDelegate) override;

    using HdRendererPlugin::IsSupported;
    bool IsSupported(HdRendererCreateArgs const &rendererCreateArgs,
                     std::string *reasonWhyNot = nullptr) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
