#pragma once

// A pass-through scene index through which the delegate can make Hydra sync
// prims again. Under Hydra 2 scene index emulation, HdChangeTracker::
// MarkRprimDirty() only reaches prims of legacy (Hydra 1) scene delegates,
// not the UsdImaging scene index's, so a decision of the render pass such as
// "the shutter changed, resample all motion" has to travel as a PrimsDirtied
// notice through the scene index chain. Appended by Nova's renderer scene
// index plugin, one per render instance.

#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaResyncSceneIndex);

class HdNovaResyncSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaResyncSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input, std::string const &renderInstanceId);
    ~HdNovaResyncSceneIndex() override;

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

    // Sends PrimsDirtied with `locators` for every prim of one of `primTypes`
    // in the scene indices of the render instance. Returns false if the
    // instance has none (no scene index emulation, e.g. test rigs); the
    // caller then falls back to the change tracker.
    static bool DirtyPrims(std::string const &renderInstanceId, TfTokenVector const &primTypes,
                           HdDataSourceLocatorSet const &locators);
    // The same for the prims at `paths`.
    static bool DirtyPaths(std::string const &renderInstanceId, SdfPathVector const &paths,
                           HdDataSourceLocatorSet const &locators);

protected:
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    HdNovaResyncSceneIndex(HdSceneIndexBaseRefPtr const &input, std::string const &renderInstanceId);
    void _DirtyAll(TfTokenVector const &primTypes, HdDataSourceLocatorSet const &locators);

    std::string _renderInstanceId;
};

PXR_NAMESPACE_CLOSE_SCOPE
