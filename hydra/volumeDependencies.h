#pragma once

// Volume field dependencies (docs/design/M8.md D3). Hydra re-syncs a volume
// when a field it binds changes (a new file, a SOP recook's new "&t=" path)
// only if the volume declares the dependency, and only through a dependency
// forwarding scene index; USD's Storm adds both for itself
// (HdSt_DependencySceneIndexPlugin), other delegates must do the same. This
// scene index gives every volume prim a __dependencies entry per binding:
// its volumeFieldBinding locator depends on the field's volumeField locator
// (which the change tracker turns into DirtyVolumeField). Nova's scene index
// plugin puts an HdDependencyForwardingSceneIndex after it.

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaVolumeDependencySceneIndex);

class HdNovaVolumeDependencySceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaVolumeDependencySceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input);

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

protected:
    explicit HdNovaVolumeDependencySceneIndex(HdSceneIndexBaseRefPtr const &input);
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
