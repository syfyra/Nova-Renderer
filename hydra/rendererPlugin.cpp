#include "rendererPlugin.h"

#include "renderDelegate.h"

#include <pxr/imaging/hd/rendererCreateArgs.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType) {
    HdRendererPluginRegistry::Define<HdNovaRendererPlugin>();
    HdRendererPluginRegistry::Define<HdNovaXpuRendererPlugin>();
}

HdRenderDelegate *HdNovaRendererPlugin::CreateRenderDelegate() {
    return new HdNovaRenderDelegate();
}

HdRenderDelegate *HdNovaRendererPlugin::CreateRenderDelegate(HdRenderSettingsMap const &settingsMap) {
    return new HdNovaRenderDelegate(settingsMap);
}

void HdNovaRendererPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate) {
    delete renderDelegate;
}

bool HdNovaRendererPlugin::IsSupported(HdRendererCreateArgs const &, std::string *) const {
    return true; // CPU renderer: always available
}

HdRenderDelegate *HdNovaXpuRendererPlugin::CreateRenderDelegate() {
    return new HdNovaRenderDelegate(nova::RenderDevice::Xpu);
}

HdRenderDelegate *HdNovaXpuRendererPlugin::CreateRenderDelegate(HdRenderSettingsMap const &settingsMap) {
    return new HdNovaRenderDelegate(settingsMap, nova::RenderDevice::Xpu);
}

void HdNovaXpuRendererPlugin::DeleteRenderDelegate(HdRenderDelegate *renderDelegate) {
    delete renderDelegate;
}

bool HdNovaXpuRendererPlugin::IsSupported(HdRendererCreateArgs const &, std::string *) const {
    return true; // without a usable GPU it renders on the CPU (and says so once)
}

PXR_NAMESPACE_CLOSE_SCOPE
