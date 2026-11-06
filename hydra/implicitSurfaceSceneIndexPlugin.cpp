#include "implicitSurfaceSceneIndexPlugin.h"

#include "resyncSceneIndex.h"
#include "velocitySceneIndex.h"
#include "volumeDependencies.h"

#include <pxr/imaging/hd/dependencyForwardingSceneIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens, ((sceneIndexPluginName, "HdNova_ImplicitSurfaceSceneIndexPlugin")));

// Must match the renderers' "displayName"s in plugInfo.json.
static const char *const _rendererDisplayNames[] = {"Nova", "Nova XPU"};

TF_REGISTRY_FUNCTION(TfType) {
    HdSceneIndexPluginRegistry::Define<HdNova_ImplicitSurfaceSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin) {
    const HdDataSourceBaseHandle toMesh =
        HdRetainedTypedSampledDataSource<TfToken>::New(HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    // Spheres stay spheres: Nova renders them analytically (HdNovaSphere).
    const HdContainerDataSourceHandle inputArgs = HdRetainedContainerDataSource::New(
        HdPrimTypeTokens->cube, toMesh, HdPrimTypeTokens->cone, toMesh, HdPrimTypeTokens->cylinder, toMesh,
        HdPrimTypeTokens->capsule, toMesh, HdPrimTypeTokens->plane, toMesh);

    for (const char *renderer : _rendererDisplayNames)
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            renderer, _tokens->sceneIndexPluginName, inputArgs, 0, HdSceneIndexPluginRegistry::InsertionOrderAtStart);

    // Light linking (docs/design/M9.md D15): pxr's light-linking scene index
    // turns the lights' UsdLux collections into categories on lights and
    // geometry, as Karma's own plugin does for Karma (research lights.md 4.2).
    // Not `instancer` among the geometry: per-instance categories would be lost.
    // The generic `light` is Karma's sky dome (docs/design/M11-P3.md).
    VtArray<TfToken> lights{HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                            HdPrimTypeTokens->sphereLight,  HdPrimTypeTokens->rectLight,
                            HdPrimTypeTokens->diskLight,    HdPrimTypeTokens->cylinderLight,
                            HdPrimTypeTokens->meshLight,    HdPrimTypeTokens->light};
    VtArray<TfToken> geometry{HdPrimTypeTokens->mesh, HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->points,
                              HdPrimTypeTokens->sphere, HdPrimTypeTokens->volume};
    const HdContainerDataSourceHandle linkArgs = HdRetainedContainerDataSource::New(
        HdsiLightLinkingSceneIndexTokens->lightPrimTypes, HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(lights),
        HdsiLightLinkingSceneIndexTokens->geometryPrimTypes,
        HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(geometry));
    for (const char *renderer : _rendererDisplayNames)
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            renderer,
            [](const std::string &, const HdSceneIndexBaseRefPtr &input, const HdContainerDataSourceHandle &args) {
                return HdSceneIndexBaseRefPtr(HdsiLightLinkingSceneIndex::New(input, args));
            },
            linkArgs, 0, HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexBaseRefPtr HdNova_ImplicitSurfaceSceneIndexPlugin::_AppendSceneIndex(
    const std::string &renderInstanceId, const HdSceneIndexBaseRefPtr &inputScene,
    const HdContainerDataSourceHandle &inputArgs) {
    // Velocity and acceleration blur (docs/design/M8.md D19): velocities
    // resolved into sampled points and instance positions per the settings.
    HdSceneIndexBaseRefPtr scene = HdNovaVelocityModeSceneIndex::Append(inputScene);
    scene = HdsiImplicitSurfaceSceneIndex::New(scene, inputArgs);
    // Volumes depend on their fields (docs/design/M8.md D3), forwarded as Storm does.
    scene = HdDependencyForwardingSceneIndex::New(HdNovaVolumeDependencySceneIndex::New(scene));
    return HdNovaResyncSceneIndex::New(scene, renderInstanceId);
}

PXR_NAMESPACE_CLOSE_SCOPE
