#pragma once

#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

// Nova's renderer scene index plugin:
// - resolves velocity and acceleration motion into sampled points and
//   instance transforms as the object settings ask (HdNovaVelocityModeSceneIndex);
// - converts implicit surfaces (sphere, cube, cone, cylinder, capsule, plane)
//   to meshes. With Hydra scene indices these prims no longer arrive as
//   meshes on their own; renderers that only handle meshes must ask for it.
// - appends the HdNovaResyncSceneIndex of the render instance, through which
//   the render pass makes prims sync again (shutter and subdivision changes).
class HdNova_ImplicitSurfaceSceneIndexPlugin final : public HdSceneIndexPlugin {
public:
    HdNova_ImplicitSurfaceSceneIndexPlugin() = default;

protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(const std::string &renderInstanceId,
                                             const HdSceneIndexBaseRefPtr &inputScene,
                                             const HdContainerDataSourceHandle &inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
