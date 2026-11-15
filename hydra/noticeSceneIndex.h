#pragma once

// Notices that the scene indices before Nova's delegate leave out
// (docs/bug-audit-2026-11.md, section 23). It is the last index of the chain.
//
// Light links on native instances. pxr's HdsiLightLinkingSceneIndex gives the
// instancer of native instances its instanceCategories from the instances'
// paths, but when a light's collection changes it dirties the `categories` of
// the prims the collection names: the instance prim, which no renderer syncs.
// The instancer is not dirtied, so a link that takes a native instance in or
// out reached no Hydra renderer until the stage was loaded again. Here an
// instance prim whose categories are dirtied dirties its instancer's
// instanceCategories, which the change tracker turns into DirtyInstancer on
// the prototypes' rprims.
//
// Mesh lights. A mesh with UsdLux MeshLightAPI is a mesh prim that also holds
// the light's data (`light`, and its parameters in `material`); Hydra tells an
// rprim nothing when those change. They dirty the prim's primvars here, which
// the mesh reads its light with (HdNovaMeshLight).

//
// HdNovaRelinkSceneIndex goes before pxr's light linking. That index keeps
// the category it gave a light's collection when the light is removed, and
// does not read the collections of a light that arrives without a link. A
// light that is resynced (an API schema applied to it; in Solaris a node
// that composes the prim anew) in the step that takes its link away
// therefore kept the link, and lit only what it had lit. A light that
// arrives a second time has its collections dirtied here, which makes the
// linking index read them again.

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaNoticeSceneIndex);
TF_DECLARE_WEAK_AND_REF_PTRS(HdNovaRelinkSceneIndex);

class HdNovaRelinkSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaRelinkSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input);

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

protected:
    explicit HdNovaRelinkSceneIndex(HdSceneIndexBaseRefPtr const &input);
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;

private:
    SdfPathSet _lights; // the lights and light filters that have arrived, ever
};

class HdNovaNoticeSceneIndex final : public HdSingleInputFilteringSceneIndexBase {
public:
    static HdNovaNoticeSceneIndexRefPtr New(HdSceneIndexBaseRefPtr const &input);

    HdSceneIndexPrim GetPrim(SdfPath const &primPath) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const &primPath) const override;

protected:
    explicit HdNovaNoticeSceneIndex(HdSceneIndexBaseRefPtr const &input);
    void _PrimsAdded(HdSceneIndexBase const &sender, HdSceneIndexObserver::AddedPrimEntries const &entries) override;
    void _PrimsRemoved(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::RemovedPrimEntries const &entries) override;
    void _PrimsDirtied(HdSceneIndexBase const &sender,
                       HdSceneIndexObserver::DirtiedPrimEntries const &entries) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
